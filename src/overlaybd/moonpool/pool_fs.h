/* Copyright The Overlaybd Authors. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <photon/fs/filesystem.h>

namespace MoonPool {

struct Options {
    bool enable = true;
    bool cache_registry = true;
    std::string master = "127.0.0.1:15051";
    std::string metadata_server = "http://127.0.0.1:18080/metadata";
    std::string local_hostname = "127.0.0.1";
    std::string name_space = "overlaybd";
    size_t block_size = 262144;
    unsigned timeout_ms = 200;
    unsigned retry_ms = 1000;
    size_t queue_depth = 32;
};

struct Stats {
    std::atomic<uint64_t> hits{0}, misses{0}, errors{0}, corrupt{0};
    std::atomic<uint64_t> origin_bytes{0}, timeouts{0}, queue_full{0};
    std::atomic<uint64_t> puts{0}, put_errors{0};
};

enum class Result { Hit, Miss, Error };

// Called only on the pool's native worker thread. No Photon or caller buffers
// may be retained by an implementation. get() must respect the capacity limit.
class Backend {
public:
    virtual ~Backend() = default;
    virtual void initialize() {
    }
    virtual Result get(const std::string &key, size_t capacity, std::vector<char> &value) = 0;
    virtual bool put(const std::string &key, const std::vector<char> &value) = 0;
};

std::unique_ptr<Backend> new_mooncake_backend(const Options &options);

// On success the returned filesystem owns source. On failure ownership remains
// with the caller. Non-sha256 paths bypass the pool. Only O_RDONLY is accepted.
photon::fs::IFileSystem *new_pool_fs(photon::fs::IFileSystem *source, const Options &options,
                                     std::unique_ptr<Backend> backend = nullptr,
                                     std::shared_ptr<Stats> stats = nullptr);

} // namespace MoonPool
