/* Copyright The Overlaybd Authors. Licensed under the Apache License, Version 2.0. */
#include "pool_fs.h"
#include <cerrno>

namespace MoonPool {
bool resolve_transport(const std::string &protocol, TransportConfig &config) {
    if (protocol == "tcp") {
        config = {"tcp", ""};
        return true;
    }
    errno = (protocol == "rdma" || protocol == "ub") ? ENOTSUP : EINVAL;
    return false;
}
} // namespace MoonPool
