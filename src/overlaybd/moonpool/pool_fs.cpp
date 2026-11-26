/* Copyright The Overlaybd Authors. Licensed under the Apache License, Version 2.0. */
#include "pool_fs.h"
#include "uri.h"
#include "limits.h"
#include <new>
#include <stdexcept>
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
    std::vector<char> data;
    std::vector<ReadRequest> reads;
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
                    state->stats->batches++;
                    state->stats->batch_blocks += job->reads.size();
                    LOG_DEBUG("moonpool batch blocks=`", job->reads.size());
                    state->backend->get_batch(job->reads);
                    for (const auto &read : job->reads)
                        if (read.result == Result::Error)
                            state->retry_after = milliseconds() + state->options.retry_ms;
                }
            } catch (const std::bad_alloc &) {
                if (job->write)
                    state->stats->put_errors++;
                for (auto &read : job->reads)
                    read.result = Result::ResourceError;
            } catch (const std::length_error &) {
                if (job->write)
                    state->stats->put_errors++;
                for (auto &read : job->reads)
                    read.result = Result::ResourceError;
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

    void get_batch(std::vector<ReadRequest> &requests) {
        auto job = std::make_shared<Job>();
        job->reads = requests;
        if (!submit(job))
            return;
        auto deadline = Clock::now() + std::chrono::milliseconds(state_->options.timeout_ms);
        while (!job->done.load(std::memory_order_acquire)) {
            if (Clock::now() >= deadline) {
                job->abandoned = true;
                state_->retry_after = milliseconds() + state_->options.retry_ms;
                state_->stats->timeouts++;
                return;
            }
            yield();
        }
        requests = std::move(job->reads);
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

// Open the registry only on an actual miss. Layer size comes from the validated
// snapshot configuration; a warm pool must not require an origin HEAD/GET.
class LazyOrigin final : public photon::fs::VirtualReadOnlyFile {
    IFileSystem *fs_;
    std::string path_;
    uint64_t size_;
    std::unique_ptr<IFile> file_;
    photon::mutex mutex_;

public:
    LazyOrigin(IFileSystem *fs, const URI &uri)
        : fs_(fs), path_("/" + uri.origin), size_(uri.size) {
    }
    IFileSystem *filesystem() override {
        return fs_;
    }
    int close() override {
        return 0;
    }
    int fstat(struct stat *st) override {
        memset(st, 0, sizeof(*st));
        st->st_size = size_;
        st->st_mode = S_IFREG | 0444;
        st->st_blksize = 4096;
        return 0;
    }
    ssize_t pread(void *buf, size_t count, off_t offset) override {
        {
            photon::scoped_lock lock(mutex_);
            if (!file_) {
                std::unique_ptr<IFile> candidate(fs_->open(path_.c_str(), O_RDONLY));
                if (!candidate)
                    return -1;
                struct stat st;
                if (candidate->fstat(&st) < 0)
                    return -1;
                if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) != size_) {
                    errno = EIO;
                    return -1;
                }
                file_ = std::move(candidate);
            }
        }
        return file_->pread(buf, count, offset);
    }
};

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
             std::shared_ptr<Stats> stats, const Options &options, const std::string &prefix)
        : source_(source), store_(std::move(store)), stats_(std::move(stats)), options_(options),
          prefix_(prefix), fs_(fs) {
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

    ssize_t pread(void *buf, size_t count, off_t offset) override try {
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
            const auto first = (static_cast<uint64_t>(offset) + copied) / options_.block_size;
            const auto last = (static_cast<uint64_t>(offset) + count - 1) / options_.block_size;
            const auto blocks = std::min<uint64_t>(
                read_batch_blocks(options_.block_size, options_.batch_blocks), last - first + 1);
            std::vector<ReadRequest> requests(blocks);
            for (size_t i = 0; i < blocks; ++i) {
                requests[i].key = prefix_ + std::to_string(first + i);
                requests[i].capacity = HEADER + std::min<uint64_t>(
                    options_.block_size, size - (first + i) * options_.block_size);
            }
            store_->get_batch(requests);
            for (size_t i = 0; i < blocks; ++i) {
                const auto block_offset = (first + i) * options_.block_size;
                const auto block_size = requests[i].capacity - HEADER;
                const auto &key = requests[i].key;
                auto &data = requests[i].data;
                auto result = requests[i].result;
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
                    if (options_.fallback == "none" || result == Result::ResourceError) {
                        stats_->denied++;
                        // A missing object is absence; a corrupt object or a failed RPC
                        // is an I/O error. Neither permits origin access or a put.
                        errno = result == Result::Miss ? ENOENT : EIO;
                        LOG_ERROR("moonpool denied key=`, reason=`, errno=`", key,
                                  static_cast<int>(result), errno);
                        return -1;
                    }
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
                const auto begin = static_cast<uint64_t>(offset) + copied - block_offset;
                const auto n = std::min<uint64_t>(count - copied, block_size - begin);
                memcpy(static_cast<char *>(buf) + copied, data.data() + HEADER + begin, n);
                copied += n;
                if (!hit)
                    store_->put(key, std::move(data));
            }
        }
        return copied;
    } catch (const std::bad_alloc &) {
        errno = ENOMEM;
        return -1;
    } catch (const std::length_error &) {
        errno = EOVERFLOW;
        return -1;
    }

    ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override try {
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
            if (iov[i].iov_len && !iov[i].iov_base) {
                errno = EFAULT;
                return -1;
            }
            total += iov[i].iov_len;
        }
        if (total > static_cast<uint64_t>(INT64_MAX - offset)) {
            errno = EINVAL;
            return -1;
        }
        std::vector<char> buffer(std::min(total, BATCH_PAYLOAD_TARGET));
        size_t copied = 0, within = 0;
        int index = 0;
        while (copied < total) {
            const auto wanted = std::min(buffer.size(), total - copied);
            auto n = pread(buffer.data(), wanted, offset + copied);
            if (n < 0)
                return options_.fallback == "none" || !copied ? -1 : static_cast<ssize_t>(copied);
            size_t consumed = 0;
            while (consumed < static_cast<size_t>(n)) {
                while (index < iovcnt && within == iov[index].iov_len) {
                    ++index;
                    within = 0;
                }
                const auto bytes = std::min(static_cast<size_t>(n) - consumed,
                                            iov[index].iov_len - within);
                memcpy(static_cast<char *>(iov[index].iov_base) + within,
                       buffer.data() + consumed, bytes);
                consumed += bytes;
                within += bytes;
            }
            copied += n;
            if (static_cast<size_t>(n) < wanted)
                break;
        }
        return copied;
    } catch (const std::bad_alloc &) {
        errno = ENOMEM;
        return -1;
    } catch (const std::length_error &) {
        errno = EOVERFLOW;
        return -1;
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
          store_(options.enable ? std::make_shared<AsyncStore>(options, std::move(backend), stats_)
                                : nullptr) {
    }
    IFile *open(const char *path, int flags) override {
        return open(path, flags, 0);
    }
    IFile *open(const char *path, int flags, mode_t mode) override {
        if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_TRUNC | O_CREAT))) {
            errno = EROFS;
            return nullptr;
        }
        if (!path) {
            errno = EINVAL;
            return nullptr;
        }
        if (is_pool_uri(path)) {
            URI uri;
            if (!parse_uri(path, uri, true))
                return nullptr;
            // The URI remains authoritative even when the global default differs.
            auto source = new LazyOrigin(m_fs, uri);
            if (!options_.enable)
                return source;
            auto options = options_;
            options.block_size = uri.block_size;
            return new PoolFile(source, this, store_, stats_, options,
                                uri.key_prefix(options.name_space));
        }
        // Bare registry URLs do not carry a trusted size. Opening them probes
        // the origin, which is forbidden in cache-only mode; use moonpool URIs.
        if (options_.fallback == "none") {
            errno = ENOTSUP;
            return nullptr;
        }
        auto source = m_fs->open(path, flags, mode);
        if (!source)
            return nullptr;
        auto digest = digest_of(path);
        if (digest.empty() || !options_.enable || !options_.cache_registry)
            return source;
        auto prefix = options_.name_space + "/blob-v1/" + digest + "/" +
                      std::to_string(options_.block_size) + "/";
        return new PoolFile(source, this, store_, stats_, options_, prefix);
    }
};
} // namespace

IFileSystem *new_pool_fs(IFileSystem *source, const Options &options,
                         std::unique_ptr<Backend> backend, std::shared_ptr<Stats> stats) {
    TransportConfig transport;
    if (!resolve_transport(options.protocol, transport)) {
        LOG_ERROR("unsupported moonpool protocol: `", options.protocol);
        return nullptr;
    }
    bool valid_namespace = !options.name_space.empty() && options.name_space.size() <= 64;
    for (auto c : options.name_space)
        valid_namespace &= (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!source || !valid_namespace ||
        (options.fallback != "direct" && options.fallback != "none") ||
        (!options.enable && options.fallback == "none") ||
        options.batch_blocks < 1 || options.batch_blocks > 64 ||
        !valid_block_size(options.block_size) || !options.timeout_ms ||
        options.timeout_ms > 60000 || !options.retry_ms || !options.queue_depth ||
        options.queue_depth > 256) {
        errno = EINVAL;
        return nullptr;
    }
    if (!backend)
        backend = new_mooncake_backend(options);
    if (options.enable && !backend) {
        LOG_ERROR_RETURN(ENOTSUP, nullptr,
                         "Mooncake backend unavailable; rebuild with ENABLE_MOONCAKE=ON");
    }
    if (!stats)
        stats = std::make_shared<Stats>();
    return new PoolFS(source, options, std::move(backend), std::move(stats));
}
} // namespace MoonPool
