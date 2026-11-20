/* Copyright The Overlaybd Authors. Licensed under the Apache License, Version 2.0. */
#include "pool_fs.h"
#include <algorithm>
#include <store_c.h>
#include <cstring>
#include <cstdlib>
#include <chrono>

namespace MoonPool {
namespace {

// Include the SDK declaration and link its C shared library at build time.
// SDK calls and resource destruction remain on the native worker thread.
class MooncakeBackend final : public Backend {
    Options options_;
    mooncake_store_t store_ = nullptr;
    void *buffer_ = nullptr;
    size_t capacity_ = 0;
    bool registered_ = false;
    std::chrono::steady_clock::time_point retry_after_{};

    void cleanup() {
        if (registered_)
            mooncake_store_unregister_buffer(store_, buffer_);
        registered_ = false;
        if (store_)
            mooncake_store_destroy(store_);
        store_ = nullptr;
        free(buffer_);
        buffer_ = nullptr;
    }

    bool ready() {
        if (registered_)
            return true;
        auto now = std::chrono::steady_clock::now();
        if (now < retry_after_)
            return false;
        retry_after_ = now + std::chrono::milliseconds(options_.retry_ms);
        cleanup();
        store_ = mooncake_store_create();
        if (!store_)
            return false;
        // This process is a cache consumer, not a storage owner. Objects survive
        // OverlayBD shutdown in the independently managed Mooncake storage node.
        if (mooncake_store_setup(store_, options_.local_hostname.c_str(),
                                 options_.metadata_server.c_str(), 0, 16 * 1024 * 1024, "tcp", "",
                                 options_.master.c_str()) != 0)
            return false;
        // Explicit layer URIs may use a different ZFile block size.
        capacity_ = std::max<size_t>(options_.block_size, 65536) + 64;
        if (posix_memalign(&buffer_, 4096, capacity_) != 0)
            return false;
        registered_ = mooncake_store_register_buffer(store_, buffer_, capacity_) == 0;
        return registered_;
    }

    void invalidate() {
        retry_after_ =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.retry_ms);
        cleanup();
    }

public:
    explicit MooncakeBackend(const Options &options) : options_(options) {
    }
    ~MooncakeBackend() override {
        cleanup();
    }
    void initialize() override {
        ready();
    }

    Result get(const std::string &key, size_t capacity, std::vector<char> &value) override {
        if (!ready() || capacity > capacity_)
            return Result::Error;
        auto size = mooncake_store_get_into(store_, key.c_str(), buffer_, capacity);
        if (size == -704)
            return Result::Miss; // Mooncake ErrorCode::OBJECT_NOT_FOUND
        if (size < 0 || static_cast<uint64_t>(size) > capacity) {
            invalidate();
            return Result::Error;
        }
        value.assign(static_cast<char *>(buffer_), static_cast<char *>(buffer_) + size);
        return Result::Hit;
    }

    bool put(const std::string &key, const std::vector<char> &value) override {
        if (!ready())
            return false;
        auto ret = mooncake_store_put(store_, key.c_str(), value.data(), value.size(), nullptr);
        // Immutable content can be inserted concurrently. Existing objects are
        // validated on every read; never remove a shared object to repair it.
        if (ret == 0 || ret == -705)
            return true; // OBJECT_ALREADY_EXISTS
        invalidate();
        return false;
    }
};
} // namespace

std::unique_ptr<Backend> new_mooncake_backend(const Options &options) {
    return std::unique_ptr<Backend>(new MooncakeBackend(options));
}
} // namespace MoonPool
