// inflate.cpp - libdeflate-backed whole-buffer decompression. License: MIT
#include "common/inflate.h"
#include <libdeflate.h>
#include <limits>

namespace jdoc {

namespace {

// One decompressor per thread, reused across calls (libdeflate decompressors
// are stateless between calls) and released when the thread exits.
libdeflate_decompressor* thread_decompressor() {
    struct Holder {
        Holder() : ptr(libdeflate_alloc_decompressor()) {}
        ~Holder() { libdeflate_free_decompressor(ptr); }
        libdeflate_decompressor* ptr;
    };
    thread_local Holder decompressor;
    return decompressor.ptr;
}

// Grow-and-retry driver for the unknown-output-size helpers. `fn` is one of
// libdeflate's *_decompress functions (they share a signature).
template <typename Fn>
std::vector<uint8_t> grow_inflate(const uint8_t* in, size_t in_size,
                                  size_t hint, Fn fn) {
    libdeflate_decompressor* d = thread_decompressor();
    if (!d || in_size == 0) return {};
    size_t initial_size = hint;
    if (initial_size == 0) {
        const size_t max_size = std::numeric_limits<size_t>::max();
        initial_size = in_size > (max_size - 64) / 4
            ? in_size : in_size * 4 + 64;
    }
    std::vector<uint8_t> out(initial_size);
    for (;;) {
        size_t actual = 0;
        libdeflate_result r = fn(d, in, in_size, out.data(), out.size(), &actual);
        if (r == LIBDEFLATE_SUCCESS) {
            out.resize(actual);
            return out;
        }
        if (r == LIBDEFLATE_INSUFFICIENT_SPACE) {
            if (out.size() > out.max_size() / 2) return {};
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
