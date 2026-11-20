/* Copyright The Overlaybd Authors. Licensed under the Apache License, Version 2.0. */
#include "pool_fs.h"

namespace MoonPool {
std::unique_ptr<Backend> new_mooncake_backend(const Options &) {
    return nullptr;
}
} // namespace MoonPool
