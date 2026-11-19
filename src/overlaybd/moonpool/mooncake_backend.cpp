/* Copyright The Overlaybd Authors. Licensed under the Apache License, Version 2.0. */
#include "pool_fs.h"
#include <dlfcn.h>
#include <cstring>
#include <cstdlib>
#include <chrono>

namespace MoonPool {
namespace {

// Use only the public C ABI. In particular, no Mooncake C++/YLT/ASIO headers
// enter OverlayBD's C++14/Photon build or its symbol namespace.
class MooncakeBackend final : public Backend {
    Options options_;
    void *library_ = nullptr;
    void *store_ = nullptr;
    void *buffer_ = nullptr;
    size_t capacity_ = 0;
    bool registered_ = false;
    std::chrono::steady_clock::time_point retry_after_{};

    void *(*create_)() = nullptr;
    void (*destroy_)(void *) = nullptr;
    int (*setup_)(void *, const char *, const char *, uint64_t, uint64_t, const char *,
                  const char *, const char *) = nullptr;
    int64_t (*get_)(void *, const char *, void *, size_t) = nullptr;
    int (*put_)(void *, const char *, const void *, size_t, const void *) = nullptr;
    int (*register_)(void *, void *, size_t) = nullptr;
    int (*unregister_)(void *, void *) = nullptr;

    template <typename T>
    bool symbol(T &function, const char *name) {
        function = reinterpret_cast<T>(dlsym(library_, name));
        return function != nullptr;
    }

    void cleanup() {
        if (registered_)
            unregister_(store_, buffer_);
        registered_ = false;
        if (store_)
            destroy_(store_);
        store_ = nullptr;
        free(buffer_);
        buffer_ = nullptr;
        if (library_)
            dlclose(library_);
        library_ = nullptr;
    }

    bool ready() {
        if (registered_)
            return true;
        auto now = std::chrono::steady_clock::now();
        if (now < retry_after_)
            return false;
        retry_after_ = now + std::chrono::milliseconds(options_.retry_ms);
        cleanup();
        library_ = dlopen(options_.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!library_)
            return false;
        if (!symbol(create_, "mooncake_store_create") ||
            !symbol(destroy_, "mooncake_store_destroy") ||
            !symbol(setup_, "mooncake_store_setup") || !symbol(get_, "mooncake_store_get_into") ||
            !symbol(put_, "mooncake_store_put") ||
            !symbol(register_, "mooncake_store_register_buffer") ||
            !symbol(unregister_, "mooncake_store_unregister_buffer"))
            return false;
        store_ = create_();
        if (!store_)
            return false;
        // This process is a cache consumer, not a storage owner. Objects survive
        // OverlayBD shutdown in the independently managed Mooncake storage node.
        if (setup_(store_, options_.local_hostname.c_str(), options_.metadata_server.c_str(), 0,
                   16 * 1024 * 1024, "tcp", "", options_.master.c_str()) != 0)
            return false;
        capacity_ = options_.block_size + 64;
        if (posix_memalign(&buffer_, 4096, capacity_) != 0)
            return false;
        registered_ = register_(store_, buffer_, capacity_) == 0;
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
        auto size = get_(store_, key.c_str(), buffer_, capacity);
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
        auto ret = put_(store_, key.c_str(), value.data(), value.size(), nullptr);
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
