/* Copyright The Overlaybd Authors. Licensed under the Apache License, Version 2.0. */
#include "uri.h"
#include "limits.h"
#include <cerrno>
#include <climits>
#include <map>
#include <openssl/sha.h>

namespace MoonPool {
namespace {
bool fail() {
    errno = EINVAL;
    return false;
}
bool digest_valid(const std::string &s) {
    if (s.size() != 71 || s.compare(0, 7, "sha256:") != 0)
        return false;
    return s.find_first_not_of("0123456789abcdef", 7) == std::string::npos;
}
bool number(const std::string &s, uint64_t &n) {
    n = 0;
    if (s.empty() || (s.size() > 1 && s[0] == '0'))
        return false;
    for (auto c : s) {
        if (c < '0' || c > '9' || n > (uint64_t(INT64_MAX) - (c - '0')) / 10)
            return false;
        n = n * 10 + c - '0';
    }
    return n > 0;
}
std::string encode(const std::string &s) {
    const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        out += '%';
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}
bool decode(const std::string &s, std::string &out) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    if (s.size() % 3)
        return false;
    for (size_t i = 0; i < s.size(); i += 3) {
        int a = hex(s[i + 1]), b = hex(s[i + 2]);
        if (s[i] != '%' || a < 0 || b < 0)
            return false;
        char c = a * 16 + b;
        if (static_cast<unsigned char>(c) <= 32 || c == 127)
            return false;
        out += c;
    }
    return true;
}
bool origin_valid(const std::string &s, const std::string &digest) {
    size_t start = s.compare(0, 7, "http://") == 0 ? 7 : s.compare(0, 8, "https://") == 0 ? 8 : 0;
    auto slash = s.find('/', start);
    if (!start || slash == std::string::npos || slash == start ||
        s.find_first_of("?#\\\r\n\t ") != std::string::npos ||
        s.substr(start, slash - start).find('@') != std::string::npos)
        return false;
    return s.size() > digest.size() &&
           s.compare(s.size() - digest.size() - 1, digest.size() + 1, "/" + digest) == 0;
}
} // namespace

bool valid_name(const std::string &s) {
    return !s.empty() && s.size() <= 64 && s != "." && s != ".." &&
           s.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
               std::string::npos;
}
bool is_pool_uri(const std::string &s) {
    return s.compare(0, 11, "moonpool://") == 0 || s.compare(0, 12, "/moonpool://") == 0;
}
std::string URI::canonical() const {
    return "moonpool://v1/" + (private_layer ? "t/" + tenant + "/" : "b/") + digest +
           "?bs=" + std::to_string(block_size) + "&tenant=" + tenant;
}
std::string URI::key_prefix(const std::string &ns) const {
    return ns + "/v1/" + (private_layer ? "t/" + tenant + "/" : "b/") + digest + "/" +
           std::to_string(block_size) + "/";
}
bool parse_uri(const std::string &path, URI &out, bool internal) {
    URI result;
    auto s = path;
    if (internal && !s.empty() && s[0] == '/')
        s.erase(0, 1);
    if (s.size() > 16384 || s.compare(0, 14, "moonpool://v1/") != 0)
        return fail();
    auto q = s.find('?');
    if (q == std::string::npos)
        return fail();
    auto resource = s.substr(14, q - 14);
    std::string path_tenant;
    if (resource.compare(0, 2, "b/") == 0)
        result.digest = resource.substr(2);
    else if (resource.compare(0, 2, "t/") == 0) {
        auto sep = resource.find('/', 2);
        if (sep == std::string::npos)
            return fail();
        result.private_layer = true;
        path_tenant = resource.substr(2, sep - 2);
        result.digest = resource.substr(sep + 1);
    } else
        return fail();
    if (!digest_valid(result.digest))
        return fail();
    std::map<std::string, std::string> args;
    for (size_t begin = q + 1; begin <= s.size();) {
        auto end = s.find('&', begin);
        if (end == std::string::npos)
            end = s.size();
        auto arg = s.substr(begin, end - begin);
        auto eq = arg.find('=');
        if (eq == std::string::npos || !args.emplace(arg.substr(0, eq), arg.substr(eq + 1)).second)
            return fail();
        if (end == s.size())
            break;
        begin = end + 1;
    }
    if (args.size() != (internal ? 4 : 2) || !args.count("bs") || !args.count("tenant"))
        return fail();
    uint64_t bs;
    if (!number(args["bs"], bs) || !valid_block_size(bs))
        return fail();
    result.block_size = bs;
    result.tenant = args["tenant"];
    if (!valid_name(result.tenant) || (result.private_layer && path_tenant != result.tenant))
        return fail();
    if (internal) {
        if (!args.count("origin") || !args.count("size") || !number(args["size"], result.size) ||
            !decode(args["origin"], result.origin) || !origin_valid(result.origin, result.digest))
            return fail();
    }
    out = std::move(result);
    return true;
}
std::string resolve_uri(const std::string &path, const std::string &repo, const std::string &digest,
                        uint64_t size) {
    URI uri;
    if (!parse_uri(path, uri) || (!digest.empty() && uri.digest != digest) || !size ||
        size > uint64_t(INT64_MAX) || repo.empty()) {
        errno = EINVAL;
        return {};
    }
    auto origin = repo + (repo.back() == '/' ? "" : "/") + uri.digest;
    if (!origin_valid(origin, uri.digest)) {
        errno = EINVAL;
        return {};
    }
    return uri.canonical() + "&size=" + std::to_string(size) + "&origin=" + encode(origin);
}
std::string local_cache_name(const std::string &path, const std::string &ns) {
    URI uri;
    if (!parse_uri(path, uri, true))
        return {};
    auto identity = ns + "/" + uri.canonical() + "/" + std::to_string(uri.size) + "/" + uri.origin;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(identity.data()), identity.size(), hash);
    std::string name = "/moonpool-";
    const char *hex = "0123456789abcdef";
    for (auto c : hash) {
        name += hex[c >> 4];
        name += hex[c & 15];
    }
    return name;
}
} // namespace MoonPool
