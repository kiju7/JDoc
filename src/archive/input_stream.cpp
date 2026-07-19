// input_stream.cpp - Sequential byte streams (file, memory, gzip/bzip2 inflate)
// License: MIT

#include "archive/input_stream.h"
#include <zlib.h>
#include <cstring>

#ifdef JDOC_WITH_BZIP2
#include <bzlib.h>
#endif

namespace jdoc {

// ── FileStream ──────────────────────────────────────────

FileStream::FileStream(const std::string& path) {
    fp_ = fopen(path.c_str(), "rb");
}

FileStream::~FileStream() {
    if (fp_) fclose(fp_);
}

size_t FileStream::read(void* buf, size_t len) {
    if (!fp_) return 0;
    return fread(buf, 1, len, fp_);
}

// ── MemoryStream ────────────────────────────────────────

size_t MemoryStream::read(void* buf, size_t len) {
    if (!data_ || pos_ >= size_) return 0;
    size_t n = len < size_ - pos_ ? len : size_ - pos_;
    memcpy(buf, data_ + pos_, n);
    pos_ += n;
    return n;
}

// ── GzInflateStream ─────────────────────────────────────

static constexpr size_t GZ_BUF_SIZE = 65536;

GzInflateStream::GzInflateStream(InputStream& src)
    : src_(src), in_buf_(GZ_BUF_SIZE) {
    auto* zs = new z_stream();
    memset(zs, 0, sizeof(*zs));
    // 15+16: zlib decodes the gzip wrapper (header + CRC trailer)
    if (inflateInit2(zs, 15 + 16) != Z_OK) {
        delete zs;
        return;
    }
    zs_ = zs;
}

GzInflateStream::~GzInflateStream() {
    if (zs_) {
        inflateEnd(zs_);
        delete zs_;
    }
}

size_t GzInflateStream::read(void* buf, size_t len) {
    if (!zs_ || error_ || finished_ || len == 0) return 0;

    zs_->next_out = static_cast<unsigned char*>(buf);
    zs_->avail_out = static_cast<uInt>(len);

    while (zs_->avail_out > 0) {
        if (zs_->avail_in == 0 && !src_eof_) {
            size_t n = src_.read(in_buf_.data(), in_buf_.size());
            if (n == 0) {
                src_eof_ = true;
            } else {
                compressed_in_ += n;
                zs_->next_in = in_buf_.data();
                zs_->avail_in = static_cast<uInt>(n);
            }
        }

        int ret = inflate(zs_, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) {
            finished_ = true;
            break;
        }
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            error_ = true;
            break;
        }
        // Z_BUF_ERROR with an exhausted source means a truncated stream
        if (zs_->avail_in == 0 && src_eof_) {
            if (zs_->avail_out > 0) error_ = true;
            break;
        }
    }

    size_t produced = len - zs_->avail_out;
    uncompressed_out_ += produced;
    return produced;
}

// ── BzInflateStream ─────────────────────────────────────

#ifdef JDOC_WITH_BZIP2

bool BzInflateStream::supported() { return true; }

BzInflateStream::BzInflateStream(InputStream& src)
    : src_(src), in_buf_(GZ_BUF_SIZE) {
    auto* bs = new bz_stream();
    memset(bs, 0, sizeof(*bs));
    if (BZ2_bzDecompressInit(bs, 0, 0) != BZ_OK) {
        delete bs;
        return;
    }
    bs_ = bs;
}

BzInflateStream::~BzInflateStream() {
    if (bs_) {
        auto* bs = static_cast<bz_stream*>(bs_);
        BZ2_bzDecompressEnd(bs);
        delete bs;
    }
}

size_t BzInflateStream::read(void* buf, size_t len) {
    if (!bs_ || error_ || finished_ || len == 0) return 0;
    auto* bs = static_cast<bz_stream*>(bs_);

    bs->next_out = static_cast<char*>(buf);
    bs->avail_out = static_cast<unsigned>(len);

    while (bs->avail_out > 0) {
        if (bs->avail_in == 0 && !src_eof_) {
            size_t n = src_.read(in_buf_.data(), in_buf_.size());
            if (n == 0) {
                src_eof_ = true;
            } else {
                compressed_in_ += n;
                bs->next_in = in_buf_.data();
                bs->avail_in = static_cast<unsigned>(n);
            }
        }

        int ret = BZ2_bzDecompress(bs);
        if (ret == BZ_STREAM_END) {
            finished_ = true;
            break;
        }
        if (ret != BZ_OK) {
            error_ = true;
            break;
        }
        // BZ_OK with an exhausted source means a truncated stream
        if (bs->avail_in == 0 && src_eof_) {
            if (bs->avail_out > 0) error_ = true;
            break;
        }
    }

    size_t produced = len - bs->avail_out;
    uncompressed_out_ += produced;
    return produced;
}

#else  // !JDOC_WITH_BZIP2 — stub so callers always link; is_open() stays false

bool BzInflateStream::supported() { return false; }

BzInflateStream::BzInflateStream(InputStream& src) : src_(src) {}

BzInflateStream::~BzInflateStream() = default;

size_t BzInflateStream::read(void*, size_t) { return 0; }

#endif  // JDOC_WITH_BZIP2

} // namespace jdoc
