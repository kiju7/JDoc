// input_stream.cpp - Sequential byte streams (file, memory, gzip inflate)
// License: MIT

#include "archive/input_stream.h"
#include <zlib.h>
#include <cstring>

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

} // namespace jdoc
