/* Copyright The Overlaybd Authors. Licensed under the Apache License, Version 2.0. */
#include "../pool_fs.h"
#include "../uri.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <openssl/sha.h>
#include <photon/photon.h>
#include <photon/common/utility.h>
#include <photon/fs/forwardfs.h>
#include <photon/fs/localfs.h>
#include <photon/fs/virtual-file.h>
#include <photon/thread/thread.h>

using namespace MoonPool;
using namespace photon::fs;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(std::string(__func__) + ": " #x);                             \
    } while (0)

template <typename F>
void wait_for(F done) {
    for (int i = 0; i < 10000; ++i) {
        if (done())
            return;
        photon::thread_usleep(1000);
    }
    CHECK(done());
}

struct SourceState {
    std::vector<char> data;
    std::atomic<int> reads{0};
    std::atomic<int> opens{0};
    bool fail_open = false;
    bool fail = false;
    size_t short_read = SIZE_MAX;
    SourceState() : data(3 * 4096 + 137) {
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = (i * 37 + i / 251) % 256;
    }
};
class SourceFile : public VirtualReadOnlyFile {
    std::shared_ptr<SourceState> state_;

public:
    explicit SourceFile(std::shared_ptr<SourceState> state) : state_(std::move(state)) {
    }
    IFileSystem *filesystem() override {
        return nullptr;
    }
    int close() override {
        return 0;
    }
    int fstat(struct stat *st) override {
        memset(st, 0, sizeof(*st));
        st->st_size = state_->data.size();
        st->st_mode = S_IFREG | 0444;
        return 0;
    }
    ssize_t pread(void *buf, size_t n, off_t off) override {
        state_->reads++;
        if (state_->fail) {
            errno = EIO;
            return -1;
        }
        if (off < 0) {
            errno = EINVAL;
            return -1;
        }
        if (static_cast<size_t>(off) >= state_->data.size())
            return 0;
        n = std::min({n, state_->data.size() - off, state_->short_read});
        memcpy(buf, state_->data.data() + off, n);
        return n;
    }
};
class SourceFS : public ForwardFS_Ownership {
    std::shared_ptr<SourceState> state_;

public:
    explicit SourceFS(std::shared_ptr<SourceState> state)
        : ForwardFS_Ownership(new_localfs_adaptor(), true), state_(std::move(state)) {
    }
    IFile *open(const char *, int) override {
        state_->opens++;
        if (state_->fail_open) {
            errno = EIO;
            return nullptr;
        }
        return new SourceFile(state_);
    }
    IFile *open(const char *, int, mode_t) override {
        return open("", O_RDONLY);
    }
};
struct FakeState {
    std::mutex mutex;
    std::map<std::string, std::vector<char>> objects;
    std::atomic<bool> fail_get{false}, fail_put{false}, destroyed{false}, active{false};
    std::atomic<unsigned> delay_ms{0};
};
class FakeBackend : public Backend {
    std::shared_ptr<FakeState> state_;

public:
    explicit FakeBackend(std::shared_ptr<FakeState> state) : state_(std::move(state)) {
    }
    ~FakeBackend() override {
        state_->destroyed = true;
    }
    Result get(const std::string &key, size_t, std::vector<char> &data) override {
        state_->active = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(state_->delay_ms.load()));
        if (state_->fail_get)
            return Result::Error;
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto it = state_->objects.find(key);
        if (it == state_->objects.end())
            return Result::Miss;
        data = it->second;
        return Result::Hit;
    }
    bool put(const std::string &key, const std::vector<char> &data) override {
        if (state_->fail_put)
            return false;
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->objects.emplace(key, data);
        return true;
    }
};

std::string path_for(const SourceState &source) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(source.data.data()), source.data.size(), hash);
    const char *hex = "0123456789abcdef";
    std::string path = "/http://registry.test/v2/image/blobs/sha256:";
    for (auto c : hash) {
        path += hex[c >> 4];
        path += hex[c & 15];
    }
    return path;
}

struct Fixture {
    Options options;
    std::shared_ptr<SourceState> source = std::make_shared<SourceState>();
    std::shared_ptr<FakeState> cache = std::make_shared<FakeState>();
    std::shared_ptr<Stats> stats = std::make_shared<Stats>();
    std::unique_ptr<IFileSystem> fs;
    Fixture() {
        options.block_size = 4096;
        options.timeout_ms = 1000;
    }
    void start(bool real = false) {
        std::unique_ptr<Backend> backend;
        if (!real)
            backend.reset(new FakeBackend(cache));
        fs.reset(new_pool_fs(new SourceFS(source), options, std::move(backend), stats));
        CHECK(fs);
    }
    std::unique_ptr<IFile> open() {
        auto file = fs->open(path_for(*source).c_str(), O_RDONLY);
        CHECK(file);
        return std::unique_ptr<IFile>(file);
    }
    void read_all(IFile *file) {
        std::vector<char> data(source->data.size() + 16, 'x');
        auto n = file->pread(data.data(), data.size(), 0);
        CHECK(n == static_cast<ssize_t>(source->data.size()));
        CHECK(std::equal(source->data.begin(), source->data.end(), data.begin()));
        CHECK(data.back() == 'x');
    }
};

void basic(bool real = false) {
    Fixture f;
    if (real) {
        f.options.timeout_ms = 5000;
        f.options.name_space =
            "obd-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }
    f.start(real);
    auto file = f.open();
    f.read_all(file.get());
    wait_for([&] { return f.stats->puts.load() == 4; });
    CHECK(f.stats->misses == 4);
    auto reads = f.source->reads.load();
    file.reset();
    file = f.open(); // a new file object must reuse the remote blocks
    f.source->fail = true;
    f.read_all(file.get());
    CHECK(f.source->reads == reads);
    CHECK(f.stats->hits == 4);
    std::vector<char> data(8000);
    iovec iov[] = {{data.data(), 2001}, {data.data() + 2001, 5999}};
    CHECK(file->preadv(iov, 2, 4000) == 8000);
    CHECK(std::equal(data.begin(), data.end(), f.source->data.begin() + 4000));
    CHECK(file->preadv2_mutable(iov, 2, 4000, 0) == 8000);
    CHECK(file->pread(data.data(), 1, f.source->data.size()) == 0);
    CHECK(file->pread(data.data(), 1, f.source->data.size() + 100) == 0);
    CHECK(file->pread(nullptr, 0, 0) == 0);
    CHECK(file->pread(data.data(), 1, -1) == -1 && errno == EINVAL);
    CHECK(file->preadv(iov, -1, 0) == -1 && errno == EINVAL);
    CHECK(file->pwrite(data.data(), 1, 0) == -1 && errno == EROFS);
    CHECK(file->ftruncate(0) == -1 && errno == EROFS);
    CHECK(file->lseek(4000, SEEK_SET) == 4000);
    CHECK(file->read(data.data(), 8000) == 8000);
    CHECK(std::equal(data.begin(), data.end(), f.source->data.begin() + 4000));
    struct stat st;
    CHECK(file->fstat(&st) == 0 && st.st_size == static_cast<off_t>(f.source->data.size()));
    CHECK(!f.fs->open(path_for(*f.source).c_str(), O_RDWR) && errno == EROFS);
    std::cout << (real ? "real Mooncake C ABI cache hit"
                       : "miss/refill/hit, preadv, EOF, read-only")
              << ": PASS\n";
}

void failures() {
    Fixture f;
    f.source->short_read = 173;
    f.start();
    auto file = f.open();
    f.read_all(file.get());
    wait_for([&] { return f.stats->puts == 4; });
    {
        std::lock_guard<std::mutex> lock(f.cache->mutex);
        for (auto &item : f.cache->objects)
            item.second.back() ^= 1;
    }
    f.read_all(file.get());
    CHECK(f.stats->corrupt == 4);
    f.source->fail = true;
    char out;
    CHECK(file->pread(&out, 1, 0) == -1 && errno == EIO);

    Fixture broken;
    broken.cache->fail_get = true;
    broken.start();
    auto bf = broken.open();
    broken.read_all(bf.get());
    CHECK(broken.stats->errors == 4);

    Fixture write_fail;
    write_fail.cache->fail_put = true;
    write_fail.start();
    auto wf = write_fail.open();
    write_fail.read_all(wf.get());
    wait_for([&] { return write_fail.stats->put_errors > 0; });
    std::cout << "short reads, corrupt blocks, origin failure, pool get/put failures: PASS\n";
}

void boundaries() {
    Fixture f;
    f.start();
    auto file = f.open();
    char buf[100];
    iovec iov[] = {{buf, 30}, {buf + 30, 70}};
    CHECK(file->preadv(iov, 2, f.source->data.size() - 45) == 45);
    CHECK(std::equal(buf, buf + 45, f.source->data.end() - 45));
    auto raw = std::unique_ptr<IFile>(f.fs->open("/not-a-digest", O_RDONLY));
    CHECK(raw && raw->pread(buf, 10, 0) == 10);
    Options invalid;
    invalid.block_size = 0;
    std::unique_ptr<SourceFS> source(new SourceFS(f.source));
    CHECK(!new_pool_fs(source.get(), invalid));
    std::cout << "scatter EOF, non-blob bypass, invalid configuration: PASS\n";
}

void isolation_and_truncation() {
    Fixture first;
    first.start();
    auto a = first.open();
    first.read_all(a.get());
    wait_for([&] { return first.stats->puts == 4; });
    Fixture other;
    other.options.name_space = "other-namespace";
    other.cache = first.cache;
    other.start();
    auto b = other.open();
    other.read_all(b.get());
    CHECK(other.stats->misses == 4 && other.stats->hits == 0);
    Fixture size;
    size.options.block_size = 8192;
    size.cache = first.cache;
    size.start();
    auto c = size.open();
    size.read_all(c.get());
    CHECK(size.stats->misses == 2 && size.stats->hits == 0);
    {
        std::lock_guard<std::mutex> lock(first.cache->mutex);
        for (auto &item : first.cache->objects)
            item.second.resize(3);
    }
    first.read_all(a.get());
    CHECK(first.stats->corrupt == 4);
    std::cout << "namespace/block-size separation and truncated objects: PASS\n";
}

struct Ticker {
    bool stop = false;
    unsigned ticks = 0;
};
void *tick(void *arg) {
    auto state = static_cast<Ticker *>(arg);
    while (!state->stop) {
        state->ticks++;
        photon::thread_usleep(1000);
    }
    return nullptr;
}
void late_completion() {
    Fixture f;
    f.options.timeout_ms = 30;
    f.options.retry_ms = 1;
    f.options.queue_depth = 1;
    f.cache->delay_ms = 500;
    f.start();
    auto file = f.open();
    Ticker ticker;
    auto thread = photon::thread_enable_join(photon::thread_create(tick, &ticker));
    DEFER(ticker.stop = true; photon::thread_join(thread));
    char data[20];
    auto start = std::chrono::steady_clock::now();
    CHECK(file->pread(data, sizeof(data), 0) == sizeof(data));
    auto duration = std::chrono::steady_clock::now() - start;
    CHECK(duration < std::chrono::milliseconds(300));
    CHECK(f.stats->timeouts == 1 && ticker.ticks >= 5);
    photon::thread_usleep(2000);
    CHECK(file->pread(data, sizeof(data), 4096) == sizeof(data));
    photon::thread_usleep(2000);
    CHECK(file->pread(data, sizeof(data), 8192) == sizeof(data));
    CHECK(f.stats->queue_full > 0);
    ticker.stop = true;
    file.reset();
    f.fs.reset(); // RPC still running: owner destruction must not wait or free its memory
    CHECK(!f.cache->destroyed);
    memset(data, 42, sizeof(data));
    wait_for([&] { return f.cache->destroyed.load(); });
    for (auto c : data)
        CHECK(c == 42);
    std::cout
        << "Photon progress, bounded queue, timeout and late completion after destruction: PASS\n";
}

// The external harness changes only the backend services. Keep this filesystem,
// worker and file alive throughout the outage to exercise client reconnection.
void real_recovery(const std::string &directory) {
    Fixture f;
    f.options.timeout_ms = 200;
    f.options.name_space =
        "obd-recovery-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    f.options.cache_registry = false;
    f.start(true);
    auto digest = path_for(*f.source).substr(path_for(*f.source).rfind('/') + 1);
    auto uri = "moonpool://v1/b/" + digest + "?bs=4096&tenant=mooncake";
    auto path = resolve_uri(uri, "http://registry.test/v2/image/blobs", digest,
                            f.source->data.size(), 4096);
    auto file = std::unique_ptr<IFile>(f.fs->open(path.c_str(), O_RDONLY));
    CHECK(file);
    auto signal = [&](const char *name) {
        std::ofstream out(directory + "/" + name);
        CHECK(out.good());
        out << "ready\n";
    };
    auto await_signal = [&](const char *name) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (access((directory + "/" + name).c_str(), F_OK) != 0) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            photon::thread_usleep(10000);
        }
    };
    auto refill = [&](uint64_t minimum_puts) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (f.stats->puts < minimum_puts) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            f.read_all(file.get());
            photon::thread_usleep(100000);
        }
    };
    refill(4);
    signal("ready");
    await_signal("down");
    auto reads = f.source->reads.load();
    f.read_all(file.get());
    CHECK(f.stats->errors > 0 && f.source->reads > reads);
    signal("fallback");
    await_signal("up");
    auto puts = f.stats->puts.load();
    refill(puts + 4);
    reads = f.source->reads.load();
    auto hits = f.stats->hits.load();
    f.source->fail = true;
    f.read_all(file.get());
    CHECK(f.source->reads == reads && f.stats->hits == hits + 4);
    std::cout
        << "real Mooncake outage, fallback, reconnect and warm hit without client restart: PASS\n";
}

void scheme(bool real = false) {
    Fixture f;
    f.options.cache_registry = false;
    f.options.block_size = 8192; // explicit URI size overrides the legacy default
    if (real) {
        f.options.timeout_ms = 5000;
        f.options.name_space =
            "scheme-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }
    f.start(real);
    auto digest = path_for(*f.source).substr(path_for(*f.source).rfind('/') + 1);
    auto uri = "moonpool://v1/b/" + digest + "?bs=4096&tenant=team-a";
    URI parsed;
    CHECK(parse_uri(uri, parsed) && parsed.block_size == 4096 && !parsed.private_layer);
    auto resolved = resolve_uri(uri, "http://registry.test/v2/image/blobs", digest,
                                f.source->data.size(), 4096);
    CHECK(!resolved.empty());
    CHECK(parse_uri("/" + resolved, parsed, true));
    CHECK(parsed.origin == "http://registry.test/v2/image/blobs/" + digest);
    CHECK(!local_cache_name(resolved, "ns").empty());
    CHECK(local_cache_name(resolved, "ns") != local_cache_name(resolved, "other"));
    CHECK(!f.fs->open(uri.c_str(), O_RDONLY) && errno == EINVAL); // missing trusted open context
    CHECK(f.source->opens == 0);
    auto file = std::unique_ptr<IFile>(f.fs->open(resolved.c_str(), O_RDONLY));
    CHECK(file && f.source->opens == 0);
    f.read_all(file.get());
    wait_for([&] { return f.stats->puts == 4; });
    CHECK(f.source->opens == 1);
    auto reads = f.source->reads.load();
    file.reset();
    f.source->fail = f.source->fail_open = true;
    file.reset(f.fs->open(resolved.c_str(), O_RDONLY));
    CHECK(file);
    f.read_all(file.get());
    CHECK(f.stats->hits == 4 && f.source->reads == reads && f.source->opens == 1);
    for (const auto &bad : std::vector<std::string>{
             "moonpool://v2/b/" + digest + "?bs=4096&tenant=x",
             "moonpool://v1/b/sha256:bad?bs=4096&tenant=x", uri + "&bs=4096", uri + "&unknown=x",
             uri + "&", uri + "#fragment", "moonpool://v1/b/" + digest + "?bs=524288&tenant=x",
             "moonpool://v1/b/" + digest + "?bs=4097&tenant=x",
             "moonpool://v1/b/" + digest + "?bs=999999999999999999999&tenant=x",
             "moonpool://v1/b/" + digest + "?bs=4096&tenant=../x",
             "moonpool://v1/b/" + digest + "?bs=4096&tenant=x%2Fy",
             "moonpool://v1/t/a/" + digest + "?bs=4096&tenant=b"}) {
        CHECK(!parse_uri(bad, parsed) && errno == EINVAL);
    }
    CHECK(resolve_uri(uri, "http://host/blobs", digest, 10, 8192).empty());
    CHECK(resolve_uri(uri, "file:///tmp", digest, 10, 4096).empty());
    CHECK(resolve_uri(uri, "http://user:pass@host/blobs", digest, 10, 4096).empty());
    CHECK(resolve_uri(uri, "http://host/blobs", "sha256:bad", 10, 4096).empty());
    CHECK(resolve_uri(uri, "http://host/blobs", digest, 0, 4096).empty());

    if (!real) {
        f.source->fail = f.source->fail_open = false;
        auto priv = "moonpool://v1/t/team-a/" + digest + "?bs=4096&tenant=team-a";
        auto priv_path = resolve_uri(priv, "http://registry.test/v2/image/blobs", digest,
                                     f.source->data.size(), 4096);
        CHECK(local_cache_name(priv_path, "ns") != local_cache_name(resolved, "ns"));
        file.reset(f.fs->open(priv_path.c_str(), O_RDONLY));
        f.read_all(file.get());
        CHECK(f.stats->misses == 8); // public and private pools never alias
        wait_for([&] { return f.stats->puts == 8; });
        auto other = "moonpool://v1/t/team-b/" + digest + "?bs=4096&tenant=team-b";
        auto other_path = resolve_uri(other, "http://registry.test/v2/image/blobs", digest,
                                      f.source->data.size(), 4096);
        file.reset(f.fs->open(other_path.c_str(), O_RDONLY));
        f.read_all(file.get());
        CHECK(f.stats->misses == 12);

        Fixture disabled;
        disabled.options.enable = false;
        disabled.start(true);
        auto direct = std::unique_ptr<IFile>(disabled.fs->open(resolved.c_str(), O_RDONLY));
        CHECK(direct);
        disabled.read_all(direct.get());
        CHECK(disabled.stats->hits == 0 && disabled.stats->misses == 0 &&
              disabled.source->reads > 0);
        auto before = f.stats->hits.load() + f.stats->misses.load();
        file = f.open();
        f.read_all(file.get());
        CHECK(f.stats->hits.load() + f.stats->misses.load() == before);
    }
    std::cout
        << "moonpool URI parsing, metadata validation, lazy origin, isolation and routing: PASS\n";
}

int main(int argc, char **argv) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT) != 0)
        return 1;
    int result = 0;
    try {
        if (argc == 3 && std::string(argv[1]) == "--real-recovery")
            real_recovery(argv[2]);
        else if (argc == 2 && std::string(argv[1]) == "--real") {
            basic(true);
            scheme(true);
        } else if (argc == 1) {
            basic();
            failures();
            boundaries();
            isolation_and_truncation();
            late_completion();
            scheme();
        } else {
            throw std::runtime_error("usage: moonpool_test [--real | --real-recovery <directory>]");
        }
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    photon::fini();
    return result;
}
