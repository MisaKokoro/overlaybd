/* Copyright The Overlaybd Authors. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <cstdint>
#include <string>

namespace MoonPool {
struct URI {
    std::string digest;
    std::string tenant;
    bool private_layer = false;
    uint32_t block_size = 0;
    // Internal open context, supplied by ImageFile, never by the layer URI.
    std::string origin;
    uint64_t size = 0;
    std::string canonical() const;
    std::string key_prefix(const std::string &name_space) const;
};
bool is_pool_uri(const std::string &path);
bool valid_name(const std::string &name);
bool parse_uri(const std::string &path, URI &out, bool internal = false);
// Validate the layer contract before any local cache or registry access.
std::string resolve_uri(const std::string &uri, const std::string &repo_blob_url,
                        const std::string &digest, uint64_t size, uint32_t zfile_block_size);
// Fixed-length name safe for file caches; includes namespace and all open context.
std::string local_cache_name(const std::string &path, const std::string &name_space);
} // namespace MoonPool
