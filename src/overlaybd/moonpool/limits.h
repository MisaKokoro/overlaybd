/* Copyright The Overlaybd Authors. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace MoonPool {
// These are arithmetic limits, not a deployment-specific block-size ceiling.
// Reserve room for the 64-byte header and 4096-byte alignment; SDK lengths and
// file offsets are signed 64-bit values. Supported deployments are 64-bit.
constexpr uint64_t MAX_BLOCK_SIZE = (uint64_t(INT64_MAX) - 4096) / 4096 * 4096;
constexpr size_t BATCH_PAYLOAD_TARGET = 64 * 1024 * 1024;
inline bool valid_block_size(uint64_t size) {
    return size >= 4096 && size <= MAX_BLOCK_SIZE &&
           size <= uint64_t(std::numeric_limits<ptrdiff_t>::max()) - 4096 && size % 4096 == 0;
}
// Keep ordinary batches near 64 MiB. A larger object still needs one full GET.
inline size_t read_batch_blocks(size_t block_size, size_t requested) {
    return std::min(requested, std::max(size_t(1), BATCH_PAYLOAD_TARGET / block_size));
}
inline bool buffer_layout(size_t slot_size, size_t count, size_t &stride, size_t &capacity) {
    const auto limit = size_t(std::numeric_limits<ptrdiff_t>::max());
    if (!count || slot_size < 64 || slot_size > limit - 4095)
        return false;
    stride = (slot_size + 4095) / 4096 * 4096;
    if (stride > limit / count)
        return false;
    capacity = stride * count;
    return true;
}
} // namespace MoonPool
