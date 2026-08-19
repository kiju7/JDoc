#pragma once

#include <libdeflate.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace jdoc { namespace util {

// libdeflate 1.21 lazily replaces several process-global CPU-dispatch function
// pointers on their first call. The library treats the resulting same-value
// writes as benign, but ThreadSanitizer correctly sees concurrent first calls
// as a data race. Warm every dispatch point JDoc can reach under one call_once
// before normal compression/decompression is allowed to run concurrently.
inline void ensure_libdeflate_runtime_initialized() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        const uint8_t input = 0;
        uint8_t output = 0;
        (void)libdeflate_adler32(1, &input, 0);
        (void)libdeflate_crc32(0, &input, 0);

        libdeflate_decompressor* decompressor =
            libdeflate_alloc_decompressor();
        if (decompressor) {
            size_t actual = 0;
            (void)libdeflate_deflate_decompress(
                decompressor, &input, 0, &output, sizeof(output), &actual);
            libdeflate_free_decompressor(decompressor);
        }
    });
}

}} // namespace jdoc::util
