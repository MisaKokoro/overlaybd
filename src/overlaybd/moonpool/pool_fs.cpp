/* Copyright The Overlaybd Authors. Licensed under the Apache License, Version 2.0. */
#include "pool_fs.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <sys/stat.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <openssl/sha.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/fs/forwardfs.h>
#include <photon/fs/virtual-file.h>
#include <photon/thread/thread.h>

namespace MoonPool {
namespace {
using Clock = std::chrono::steady_clock;
using photon::fs::IFile;
using photon::fs::IFileSystem;
constexpr size_t HEADER = 64;
constexpr char MAGIC[] = "OBDPOOL1";

int64_t milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
}

void yield() {
    if (photon::CURRENT)
        photon::thread_usleep(1000);
    else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

struct Job {
    bool write = false;
    std::string key;
    size_t capacity = 0;
    std::vector<char> data;
    Result result = Result::Error;
    std::atomic<bool> done{false};
    std::atomic<bool> abandoned{false};
};

class AsyncStore {
    struct State {
        Options options;
        std::unique_ptr<Backend> backend;
        std::shared_ptr<Stats> stats;
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<std::shared_ptr<Job>> queue;
        std::atomic<int64_t> retry_after{0};
        bool stop = false;
    };
    std::shared_ptr<State> state_;

    static void run(std::shared_ptr<State> state) {
        try {
            state->backend->initialize();
        } catch (...) {
        }
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->wake.wait(lock, [&] { return state->stop || !state->queue.empty(); });
                if (state->stop)
                    break;
                job = std::move(state->queue.front());
                state->queue.pop_front();
            }
            if (job->abandoned.load() || milliseconds() < state->retry_after.load()) {
                if (job->write)
                    state->stats->put_errors++;
                job->done.store(true, std::memory_order_release);
                continue;
            }
            try {
                if (job->write) {
                    if (state->backend->put(job->key, job->data))
                        state->stats->puts++;
                    else {
                        state->stats->put_errors++;
                        state->retry_after = milliseconds() + state->options.retry_ms;
                    }
                } else {
                    job->result = state->backend->get(job->key, job->capacity, job->data);
                    if (job->result == Result::Error)
                        state->retry_after = milliseconds() + state->options.retry_ms;
                }
            } catch (...) {
                if (job->write)
                    state->stats->put_errors++;
                state->retry_after = milliseconds() + state->options.retry_ms;
            }
            job->done.store(true, std::memory_order_release);
        }
        // Destruction also stays on this native thread. A tardy RPC retains only
        // State/Job-owned memory, never an IFile, Photon thread or user buffer.
        state->backend.reset();
    }

    bool submit(const std::shared_ptr<Job> &job) {
        if (milliseconds() < state_->retry_after.load())
            return false;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->stop || state_->queue.size() >= state_->options.queue_depth) {
            state_->stats->queue_full++;
            return false;
        }
        state_->queue.push_back(job);
        state_->wake.notify_one();
        return true;
    }

public:
    AsyncStore(const Options &options, std::unique_ptr<Backend> backend,
               std::shared_ptr<Stats> stats)
        : state_(std::make_shared<State>()) {
        state_->options = options;
        state_->backend = std::move(backend);
        state_->stats = std::move(stats);
        std::thread(run, state_).detach();
    }
    ~AsyncStore() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stop = true;
        state_->queue.clear();
        state_->wake.notify_one();
    }

    Result get(const std::string &key, size_t capacity, std::vector<char> &data) {
        auto job = std::make_shared<Job>();
        job->key = key;
        job->capacity = capacity;
        if (!submit(job))
            return Result::Error;
        auto deadline = Clock::now() + std::chrono::milliseconds(state_->options.timeout_ms);
        while (!job->done.load(std::memory_order_acquire)) {
            if (Clock::now() >= deadline) {
                job->abandoned = true;
                state_->retry_after = milliseconds() + state_->options.retry_ms;
                state_->stats->timeouts++;
                return Result::Error;
            }
            yield();
        }
        data = std::move(job->data);
        return job->result;
    }

    void put(const std::string &key, std::vector<char> data) {
        auto job = std::make_shared<Job>();
        job->write = true;
        job->key = key;
        job->data = std::move(data);
        if (!submit(job))
            state_->stats->put_errors++;
    }
};

void encode64(char *p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = (value >> (i * 8)) & 255;
}
uint64_t decode64(const char *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= uint64_t(static_cast<unsigned char>(p[i])) << (i * 8);
    return value;
}
void checksum(const std::string &key, const std::vector<char> &value, unsigned char *out) {
    SHA256_CTX context;
    SHA256_Init(&context);
    SHA256_Update(&context, key.data(), key.size());
    SHA256_Update(&context, value.data(), 24);
    SHA256_Update(&context, value.data() + HEADER, value.size() - HEADER);
    SHA256_Final(out, &context);
}

bool valid(const std::string &key, const std::vector<char> &value, uint64_t offset, size_t size) {
    if (value.size() != HEADER + size || memcmp(value.data(), MAGIC, 8) != 0 ||
        decode64(value.data() + 8) != size || decode64(value.data() + 16) != offset)
        return false;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    checksum(key, value, hash);
    return memcmp(hash, value.data() + 24, sizeof(hash)) == 0;
}

std::string digest_of(const std::string &path) {
    auto pos = path.rfind("/sha256:");
    if (pos == std::string::npos || path.size() != pos + 8 + 64)
        return {};
    auto digest = path.substr(pos + 1);
    for (size_t i = 7; i < digest.size(); ++i)
        if (!((digest[i] >= '0' && digest[i] <= '9') || (digest[i] >= 'a' && digest[i] <= 'f')))
            return {};
    return digest;
}

class PoolFile final : public photon::fs::VirtualReadOnlyFile {
    std::unique_ptr<IFile> source_;
    std::shared_ptr<AsyncStore> store_;
    std::shared_ptr<Stats> stats_;
    Options options_;
    std::string prefix_;
    IFileSystem *fs_;
    std::atomic<int64_t> size_{-1};

public:
    PoolFile(IFile *source, IFileSystem *fs, std::shared_ptr<AsyncStore> store,
             std::shared_ptr<Stats> stats, const Options &options, const std::string &digest)
        : source_(source), store_(std::move(store)), stats_(std::move(stats)), options_(options),
          prefix_(options.name_space + "/blob-v1/" + digest + "/" +
                  std::to_string(options.block_size) + "/"),
          fs_(fs) {
    }

    IFileSystem *filesystem() override {
        return fs_;
    }
    int close() override {
        return source_->close();
    }
    int vioctl(int request, va_list args) override {
        return source_->vioctl(request, args);
    }
    int fadvise(off_t offset, off_t len, int advice) override {
        return source_->fadvise(offset, len, advice);
    }
    int fstat(struct stat *st) override {
        auto ret = source_->fstat(st);
        if (ret == 0)
            size_ = st->st_size;
        return ret;
    }
    ssize_t pwrite(const void *, size_t, off_t) override {
        errno = EROFS;
        return -1;
    }
    ssize_t pwritev(const struct iovec *, int, off_t) override {
        errno = EROFS;
        return -1;
    }
    int ftruncate(off_t) override {
        errno = EROFS;
        return -1;
    }

    ssize_t pread(void *buf, size_t count, off_t offset) override {
        if (offset < 0 || count > static_cast<size_t>(SSIZE_MAX)) {
            errno = EINVAL;
            return -1;
        }
        if (!count)
            return 0;
        if (!buf) {
            errno = EFAULT;
            return -1;
        }
        if (size_.load() < 0) {
            struct stat st;
            if (fstat(&st) < 0)
                return -1;
        }
        auto size = size_.load();
        if (offset >= size)
            return 0;
        count = std::min<uint64_t>(count, size - offset);
        size_t copied = 0;
        while (copied < count) {
            auto pos = static_cast<uint64_t>(offset) + copied;
            auto block_offset = pos / options_.block_size * options_.block_size;
            auto block_size = std::min<uint64_t>(options_.block_size, size - block_offset);
            auto key = prefix_ + std::to_string(pos / options_.block_size);
            std::vector<char> data;
            auto result = store_->get(key, HEADER + block_size, data);
            bool hit = result == Result::Hit && valid(key, data, block_offset, block_size);
            if (hit) {
                stats_->hits++;
                LOG_DEBUG("moonpool hit key=`, bytes=`", key, block_size);
            } else {
                if (result == Result::Hit)
                    stats_->corrupt++;
                else if (result == Result::Miss)
                    stats_->misses++;
                else
                    stats_->errors++;
                LOG_DEBUG("moonpool fallback key=`, reason=`", key, static_cast<int>(result));
                data.assign(HEADER + block_size, 0);
                size_t read = 0;
                while (read < block_size) {
                    auto ret = source_->pread(data.data() + HEADER + read, block_size - read,
                                              block_offset + read);
                    if (ret < 0 && errno == EINTR)
                        continue;
                    if (ret <= 0 || static_cast<size_t>(ret) > block_size - read) {
                        if (ret >= 0)
                            errno = EIO;
                        return copied ? static_cast<ssize_t>(copied) : -1;
                    }
                    read += ret;
                    stats_->origin_bytes += ret;
                }
                memcpy(data.data(), MAGIC, 8);
                encode64(data.data() + 8, block_size);
                encode64(data.data() + 16, block_offset);
                checksum(key, data, reinterpret_cast<unsigned char *>(data.data() + 24));
            }
            auto begin = pos - block_offset;
            auto n = std::min<uint64_t>(count - copied, block_size - begin);
            memcpy(static_cast<char *>(buf) + copied, data.data() + HEADER + begin, n);
            copied += n;
            if (!hit)
                store_->put(key, std::move(data));
        }
        return copied;
    }

    ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override {
        if (iovcnt < 0 || iovcnt > IOV_MAX || offset < 0 || (iovcnt && !iov)) {
            errno = EINVAL;
            return -1;
        }
        size_t total = 0;
        for (int i = 0; i < iovcnt; ++i) {
            if (iov[i].iov_len > static_cast<size_t>(SSIZE_MAX) - total) {
                errno = EINVAL;
                return -1;
            }
            total += iov[i].iov_len;
        }
        if (total > static_cast<uint64_t>(INT64_MAX - offset)) {
            errno = EINVAL;
            return -1;
        }
        size_t copied = 0;
        for (int i = 0; i < iovcnt; ++i) {
            auto n = pread(iov[i].iov_base, iov[i].iov_len, offset + copied);
            if (n < 0)
                return copied ? static_cast<ssize_t>(copied) : n;
            copied += n;
            if (static_cast<size_t>(n) < iov[i].iov_len)
                break;
        }
        return copied;
    }
};

class PoolFS final : public photon::fs::ForwardFS_Ownership {
    Options options_;
    std::shared_ptr<Stats> stats_;
    std::shared_ptr<AsyncStore> store_;

public:
    PoolFS(IFileSystem *source, const Options &options, std::unique_ptr<Backend> backend,
           std::shared_ptr<Stats> stats)
        : photon::fs::ForwardFS_Ownership(source, true), options_(options),
          stats_(std::move(stats)),
          store_(std::make_shared<AsyncStore>(options, std::move(backend), stats_)) {
    }
    IFile *open(const char *path, int flags) override {
        return open(path, flags, 0);
    }
    IFile *open(const char *path, int flags, mode_t mode) override {
        if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_TRUNC | O_CREAT))) {
            errno = EROFS;
            return nullptr;
        }
        auto source = m_fs->open(path, flags, mode);
        if (!source)
            return nullptr;
        auto digest = digest_of(path);
        if (digest.empty())
            return source;
        return new PoolFile(source, this, store_, stats_, options_, digest);
    }
};
} // namespace

IFileSystem *new_pool_fs(IFileSystem *source, const Options &options,
                         std::unique_ptr<Backend> backend, std::shared_ptr<Stats> stats) {
    bool valid_namespace = !options.name_space.empty() && options.name_space.size() <= 64;
    for (auto c : options.name_space)
        valid_namespace &= (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!source || !valid_namespace || options.block_size < 4096 ||
        options.block_size > 4 * 1024 * 1024 || options.block_size % 4096 || !options.timeout_ms ||
        options.timeout_ms > 60000 || !options.retry_ms || !options.queue_depth ||
        options.queue_depth > 256) {
        errno = EINVAL;
        return nullptr;
    }
    if (!backend)
        backend = new_mooncake_backend(options);
    if (!stats)
        stats = std::make_shared<Stats>();
    return new PoolFS(source, options, std::move(backend), std::move(stats));
}
} // namespace MoonPool
