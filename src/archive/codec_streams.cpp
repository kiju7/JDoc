// codec_streams.cpp - Streaming decompressors over an InputStream
// License: MIT

#include "archive/codec_streams.h"

#include <zlib.h>
#include <algorithm>
#include <cstring>
#include <vector>

#ifdef JDOC_WITH_BZIP2
#include <bzlib.h>
#endif

#include "LzmaDec.h"
#include "7zAlloc.h"

namespace jdoc {
namespace {

constexpr size_t kChunk = 64 * 1024;

void set_err(std::string* err, const char* msg) {
    if (err) *err = msg;
}

} // anonymous namespace

bool discard_stream(InputStream& src, uint64_t len) {
    char buf[kChunk];
    while (len > 0) {
        size_t n = src.read(buf, static_cast<size_t>(std::min<uint64_t>(len, sizeof(buf))));
        if (n == 0) return false;
        len -= n;
    }
    return true;
}

bool copy_stored_stream(InputStream& src, uint64_t comp_size,
                        const CodecSink& sink, std::string* err) {
    char buf[kChunk];
    uint64_t left = comp_size;
    while (left > 0) {
        size_t n = src.read(buf, static_cast<size_t>(std::min<uint64_t>(left, sizeof(buf))));
        if (n == 0) {
            set_err(err, "truncated member data");
            return false;
        }
        left -= n;
        if (!sink(buf, n)) {
            discard_stream(src, left);
            return false;
        }
    }
    return true;
}

bool inflate_raw_stream(InputStream& src, uint64_t comp_size,
                        const CodecSink& sink, std::string* err) {
    z_stream zs = {};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        discard_stream(src, comp_size);
        set_err(err, "cannot initialize inflate");
        return false;
    }

    std::vector<unsigned char> in(kChunk);
    std::vector<unsigned char> out(kChunk);
    uint64_t left = comp_size;
    bool done = false;

    while (!done) {
        if (zs.avail_in == 0) {
            if (left == 0) break;  // input exhausted before stream end
            size_t n = src.read(in.data(),
                                static_cast<size_t>(std::min<uint64_t>(left, in.size())));
            if (n == 0) {
                inflateEnd(&zs);
                set_err(err, "truncated member data");
                return false;
            }
            left -= n;
            zs.next_in = in.data();
            zs.avail_in = static_cast<uInt>(n);
        }
        zs.next_out = out.data();
        zs.avail_out = static_cast<uInt>(out.size());
        int rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            inflateEnd(&zs);
            discard_stream(src, left);
            set_err(err, "corrupt deflate data");
            return false;
        }
        size_t produced = out.size() - zs.avail_out;
        if (produced > 0 &&
            !sink(reinterpret_cast<const char*>(out.data()), produced)) {
            inflateEnd(&zs);
            discard_stream(src, left);
            return false;
        }
        if (rc == Z_STREAM_END) done = true;
        if (rc == Z_BUF_ERROR && zs.avail_in == 0 && left == 0) break;
    }
    inflateEnd(&zs);
    if (!done) {
        set_err(err, "corrupt deflate data (unexpected end)");
        return false;
    }
    return discard_stream(src, left);  // trailing slack, if any
}

bool bzip2_stream(InputStream& src, uint64_t comp_size,
                  const CodecSink& sink, std::string* err) {
#ifndef JDOC_WITH_BZIP2
    (void)sink;
    discard_stream(src, comp_size);
    set_err(err, "bzip2 member unsupported (build without JDOC_WITH_BZIP2)");
    return false;
#else
    bz_stream bs = {};
    if (BZ2_bzDecompressInit(&bs, 0, 0) != BZ_OK) {
        discard_stream(src, comp_size);
        set_err(err, "cannot initialize bzip2 decompressor");
        return false;
    }

    std::vector<char> in(kChunk);
    std::vector<char> out(kChunk);
    uint64_t left = comp_size;
    bool done = false;

    while (!done) {
        if (bs.avail_in == 0) {
            if (left == 0) break;
            size_t n = src.read(in.data(),
                                static_cast<size_t>(std::min<uint64_t>(left, in.size())));
            if (n == 0) {
                BZ2_bzDecompressEnd(&bs);
                set_err(err, "truncated member data");
                return false;
            }
            left -= n;
            bs.next_in = in.data();
            bs.avail_in = static_cast<unsigned>(n);
        }
        bs.next_out = out.data();
        bs.avail_out = static_cast<unsigned>(out.size());
        int rc = BZ2_bzDecompress(&bs);
        if (rc != BZ_OK && rc != BZ_STREAM_END) {
            BZ2_bzDecompressEnd(&bs);
            discard_stream(src, left);
            set_err(err, "corrupt bzip2 data");
            return false;
        }
        size_t produced = out.size() - bs.avail_out;
        if (produced > 0 && !sink(out.data(), produced)) {
            BZ2_bzDecompressEnd(&bs);
            discard_stream(src, left);
            return false;
        }
        if (rc == BZ_STREAM_END) done = true;
    }
    BZ2_bzDecompressEnd(&bs);
    if (!done) {
        set_err(err, "corrupt bzip2 data (unexpected end)");
        return false;
    }
    return discard_stream(src, left);
#endif
}

bool lzma_egg_stream(InputStream& src, uint64_t comp_size,
                     uint64_t uncomp_size, const CodecSink& sink,
                     std::string* err) {
    // Block payload: 2 reserved bytes, u16 property size, the LZMA
    // properties, then the raw stream (layout per ALZip's decoder).
    if (comp_size < 9) {
        discard_stream(src, comp_size);
        set_err(err, "corrupt lzma block (too short)");
        return false;
    }
    unsigned char head[4 + LZMA_PROPS_SIZE];
    size_t got = 0;
    while (got < 4) {
        size_t n = src.read(head + got, 4 - got);
        if (n == 0) {
            set_err(err, "truncated member data");
            return false;
        }
        got += n;
    }
    uint16_t prop_size = static_cast<uint16_t>(head[2] | (head[3] << 8));
    if (prop_size != LZMA_PROPS_SIZE || comp_size < 4u + prop_size) {
        discard_stream(src, comp_size - 4);
        set_err(err, "unsupported lzma properties");
        return false;
    }
    while (got < 4u + prop_size) {
        size_t n = src.read(head + got, 4u + prop_size - got);
        if (n == 0) {
            set_err(err, "truncated member data");
            return false;
        }
        got += n;
    }
    uint64_t left = comp_size - 4 - prop_size;

    ISzAlloc alloc = {SzAlloc, SzFree};
    CLzmaDec dec;
    LzmaDec_Construct(&dec);
    if (LzmaDec_Allocate(&dec, head + 4, prop_size, &alloc) != SZ_OK) {
        discard_stream(src, left);
        set_err(err, "invalid lzma properties");
        return false;
    }
    LzmaDec_Init(&dec);

    std::vector<unsigned char> in(kChunk);
    std::vector<unsigned char> out(kChunk);
    size_t in_pos = 0, in_len = 0;
    uint64_t out_total = 0;
    bool ok = true;

    while (out_total < uncomp_size) {
        if (in_pos == in_len) {
            if (left == 0) {
                set_err(err, "corrupt lzma data (unexpected end)");
                ok = false;
                break;
            }
            in_len = src.read(in.data(),
                              static_cast<size_t>(std::min<uint64_t>(left, in.size())));
            if (in_len == 0) {
                set_err(err, "truncated member data");
                ok = false;
                break;
            }
            left -= in_len;
            in_pos = 0;
        }
        SizeT dest_len = static_cast<SizeT>(
            std::min<uint64_t>(out.size(), uncomp_size - out_total));
        SizeT src_len = in_len - in_pos;
        ELzmaStatus status;
        SRes res = LzmaDec_DecodeToBuf(&dec, out.data(), &dest_len,
                                       in.data() + in_pos, &src_len,
                                       LZMA_FINISH_ANY, &status);
        in_pos += src_len;
        if (res != SZ_OK) {
            set_err(err, "corrupt lzma data");
            ok = false;
            break;
        }
        if (dest_len > 0) {
            out_total += dest_len;
            if (!sink(reinterpret_cast<const char*>(out.data()), dest_len)) {
                ok = false;  // sink abort: err stays empty
                break;
            }
        } else if (src_len == 0) {
            set_err(err, "corrupt lzma data (no progress)");
            ok = false;
            break;
        }
    }
    LzmaDec_Free(&dec, &alloc);
    if (!discard_stream(src, left) && ok) {
        set_err(err, "truncated member data");
        ok = false;
    }
    return ok;
}

} // namespace jdoc
