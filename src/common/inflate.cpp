// inflate.cpp - libdeflate-backed whole-buffer decompression. License: MIT
#include "common/inflate.h"
#include "common/libdeflate_init.h"
#include <libdeflate.h>
#include <limits>
#include <zlib.h>

namespace jdoc {

namespace {

// One decompressor per thread, reused across calls (libdeflate decompressors
// are stateless between calls) and released when the thread exits.
libdeflate_decompressor* thread_decompressor() {
    util::ensure_libdeflate_runtime_initialized();
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

std::vector<uint8_t> inflate_zlib_lenient(const uint8_t* src, size_t src_len) {
    std::vector<uint8_t> out;
    if (src_len == 0) return out;

    // Fast path: libdeflate decodes a well-formed FlateDecode stream whole.
    auto whole = inflate_zlib(src, src_len, src_len * 3);
    if (!whole.empty()) return whole;

    // Fallback: truncated/malformed streams (missing the adler32 trailer, a
    // wrong length) are common in PDFs and PowerPoint's embedded objects.
    // zlib's streaming inflate recovers the PARTIAL output that precedes the
    // damage, where libdeflate returns nothing.
    out.reserve(src_len * 3);

    z_stream zs = {};
    if (inflateInit(&zs) != Z_OK) return {};
    zs.next_in = const_cast<Bytef*>(src);
    zs.avail_in = static_cast<uInt>(src_len);

    uint8_t buf[8192];
    int ret;
    do {
        zs.next_out = buf;
        zs.avail_out = sizeof(buf);
        uInt prev_in = zs.avail_in;
        ret = inflate(&zs, Z_NO_FLUSH);
        size_t have = sizeof(buf) - zs.avail_out;
        out.insert(out.end(), buf, buf + have);
        if (ret == Z_STREAM_END) break;
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR
            || ret == Z_NEED_DICT) {
            inflateEnd(&zs);
            return out; // return partial
        }
        // Z_BUF_ERROR or Z_OK with no progress (e.g. truncated stream missing
        // adler32 trailer) — bail out instead of looping forever.
        if (have == 0 && zs.avail_in == prev_in) {
            inflateEnd(&zs);
            return out;
        }
    } while (true);
    inflateEnd(&zs);
    return out;
}

} // namespace jdoc
