// inflate.cpp - libdeflate-backed whole-buffer decompression. License: MIT
#include "common/inflate.h"
#include <libdeflate.h>

namespace jdoc {

namespace {

// One decompressor per thread, reused across calls (libdeflate decompressors
// are stateless between calls). Leaks one object per thread at exit — harmless.
libdeflate_decompressor* thread_decompressor() {
    thread_local libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    return d;
}

// Grow-and-retry driver for the unknown-output-size helpers. `fn` is one of
// libdeflate's *_decompress functions (they share a signature).
template <typename Fn>
std::vector<uint8_t> grow_inflate(const uint8_t* in, size_t in_size,
                                  size_t hint, Fn fn) {
    libdeflate_decompressor* d = thread_decompressor();
    if (!d || in_size == 0) return {};
    std::vector<uint8_t> out(hint ? hint : in_size * 4 + 64);
    for (;;) {
        size_t actual = 0;
        libdeflate_result r = fn(d, in, in_size, out.data(), out.size(), &actual);
        if (r == LIBDEFLATE_SUCCESS) {
            out.resize(actual);
            return out;
        }
        if (r == LIBDEFLATE_INSUFFICIENT_SPACE) {
            if (out.size() > (size_t{1} << 34)) return {};  // 16 GiB guard
            out.resize(out.size() * 2);
            continue;
        }
        return {};  // BAD_DATA / SHORT_INPUT
    }
}

} // namespace

bool inflate_raw_known(const uint8_t* in, size_t in_size,
                       uint8_t* out, size_t out_size) {
    libdeflate_decompressor* d = thread_decompressor();
    if (!d) return false;
    size_t actual = 0;
    libdeflate_result r =
        libdeflate_deflate_decompress(d, in, in_size, out, out_size, &actual);
    return r == LIBDEFLATE_SUCCESS && actual == out_size;
}

std::vector<uint8_t> inflate_raw(const uint8_t* in, size_t in_size, size_t hint) {
    return grow_inflate(in, in_size, hint, libdeflate_deflate_decompress);
}

std::vector<uint8_t> inflate_zlib(const uint8_t* in, size_t in_size, size_t hint) {
    return grow_inflate(in, in_size, hint, libdeflate_zlib_decompress);
}

} // namespace jdoc
