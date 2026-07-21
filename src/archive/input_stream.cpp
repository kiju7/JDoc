// input_stream.cpp - Sequential byte streams (file, memory, gzip/bzip2 inflate)
// License: MIT

#include "archive/input_stream.h"
#include <zlib.h>
#include <cstdint>
#include <cstring>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef JDOC_WITH_BZIP2
#include <bzlib.h>
#endif

namespace jdoc {

// ── FileStream ──────────────────────────────────────────

#if defined(__unix__) || defined(__APPLE__)

FileStream::FileStream(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) return;
    struct stat st;
    if (::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd_);
        fd_ = -1;
        return;
    }
    size_ = static_cast<uint64_t>(st.st_size);
    map_file();
}

FileStream::~FileStream() {
    if (map_) ::munmap(map_, static_cast<size_t>(size_));
    if (fd_ >= 0) ::close(fd_);
}

bool FileStream::is_open() const { return fd_ >= 0; }

void FileStream::map_file() {
#ifndef JDOC_NO_STREAM_MMAP
    // An empty file has nothing to map; a file larger than the address space
    // cannot be mapped whole on 32-bit builds.
    if (size_ == 0 || size_ > static_cast<uint64_t>(SIZE_MAX)) return;
    void* p = ::mmap(nullptr, static_cast<size_t>(size_), PROT_READ,
                     MAP_PRIVATE, fd_, 0);
    if (p == MAP_FAILED) return;  // read() falls back to pread
    map_ = static_cast<uint8_t*>(p);
#ifdef MADV_SEQUENTIAL
    // A tar container is walked front-to-back: headers and member bytes are
    // touched in order.
    ::madvise(map_, static_cast<size_t>(size_), MADV_SEQUENTIAL);
#endif
#endif
}

// Every page a view() hands out stays resident for the life of the mapping,
// so a multi-gigabyte tar would report a multi-gigabyte RSS. The pages are
// clean and file-backed — not a leak — but a container memory limit counts
// them, so drop them once enough has been served. Anything still in use is
// re-read from the file on next touch; MADV_DONTNEED on a read-only
// MAP_PRIVATE mapping does not lose data.
void FileStream::trim_resident() {
#if defined(MADV_DONTNEED) && !defined(JDOC_NO_STREAM_MMAP)
    static constexpr uint64_t kTrimAfterBytes = 256ull << 20;
    if (served_ < kTrimAfterBytes) return;
    served_ = 0;
    ::madvise(map_, static_cast<size_t>(size_), MADV_DONTNEED);
#endif
}

size_t FileStream::read(void* buf, size_t len) {
    // pread, never the mapping: serving reads from the map would leave every
    // byte resident, and the compressed containers layered on FileStream read
    // their whole content this way for no benefit (see header).
    if (fd_ < 0 || pos_ >= size_) return 0;
    size_t avail = static_cast<size_t>(size_ - pos_);
    if (len > avail) len = avail;
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::pread(fd_, static_cast<char*>(buf) + got, len - got,
                            static_cast<off_t>(pos_ + got));
        if (n <= 0) break;  // EOF or error; report the short read
        got += static_cast<size_t>(n);
    }
    pos_ += got;
    return got;
}

const uint8_t* FileStream::view(size_t len) {
    if (!map_ || len > size_ - pos_) return nullptr;
    const uint8_t* p = map_ + pos_;
    pos_ += len;
    served_ += len;
    trim_resident();
    return p;
}

#else  // non-POSIX: buffered FILE*, no mapping (view() falls back to read())

FileStream::FileStream(const std::string& path) {
    fp_ = fopen(path.c_str(), "rb");
}

FileStream::~FileStream() {
    if (fp_) fclose(fp_);
}

bool FileStream::is_open() const { return fp_ != nullptr; }

size_t FileStream::read(void* buf, size_t len) {
    if (!fp_) return 0;
    return fread(buf, 1, len, fp_);
}

const uint8_t* FileStream::view(size_t /*len*/) { return nullptr; }

#endif  // POSIX

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
