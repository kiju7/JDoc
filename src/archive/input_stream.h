#pragma once
// input_stream.h - Sequential (pull-based) byte streams
// Used for formats that are consumed front-to-back: gzip/bzip2 members
// and tar containers. Composing TarReader(GzInflateStream(FileStream))
// walks a .tar.gz without ever holding the container in memory.
// License: MIT

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

typedef struct z_stream_s z_stream;

namespace jdoc {

class InputStream {
public:
    virtual ~InputStream() = default;

    // Read up to len bytes. Returns bytes read; 0 means EOF or error.
    virtual size_t read(void* buf, size_t len) = 0;

    // Zero-copy view of the next len bytes when the stream is memory-backed
    // and that many bytes remain. On success the stream advances past them;
    // nullptr means the caller must fall back to read(). The pointer is
    // valid for the lifetime of the stream's backing buffer.
    virtual const uint8_t* view(size_t len) {
        (void)len;
        return nullptr;
    }
};

class FileStream final : public InputStream {
public:
    explicit FileStream(const std::string& path);
    ~FileStream() override;

    FileStream(const FileStream&) = delete;
    FileStream& operator=(const FileStream&) = delete;

    bool is_open() const { return fp_ != nullptr; }
    size_t read(void* buf, size_t len) override;

private:
    FILE* fp_ = nullptr;
};

// Non-owning view over a memory buffer.
class MemoryStream final : public InputStream {
public:
    MemoryStream(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    size_t read(void* buf, size_t len) override;

    const uint8_t* view(size_t len) override {
        if (len > size_ - pos_) return nullptr;
        const uint8_t* p = data_ + pos_;
        pos_ += len;
        return p;
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
};

// Streaming gzip decompressor over another stream (src must outlive this).
// Tracks compressed-in / uncompressed-out byte counts so callers can run
// compression-ratio (bomb) checks while streaming.
class GzInflateStream final : public InputStream {
public:
    explicit GzInflateStream(InputStream& src);
    ~GzInflateStream() override;

    GzInflateStream(const GzInflateStream&) = delete;
    GzInflateStream& operator=(const GzInflateStream&) = delete;

    bool is_open() const { return zs_ != nullptr; }
    size_t read(void* buf, size_t len) override;

    bool error() const { return error_; }
    bool finished() const { return finished_; }
    uint64_t compressed_in() const { return compressed_in_; }
    uint64_t uncompressed_out() const { return uncompressed_out_; }

private:
    InputStream& src_;
    z_stream* zs_ = nullptr;
    std::vector<unsigned char> in_buf_;
    bool src_eof_ = false;
    bool finished_ = false;
    bool error_ = false;
    uint64_t compressed_in_ = 0;
    uint64_t uncompressed_out_ = 0;
};

// Streaming bzip2 decompressor over another stream (src must outlive this).
// Same contract as GzInflateStream. Always compiles; without JDOC_WITH_BZIP2
// is_open() stays false (bzlib.h never leaks into this header — bz_stream is
// an anonymous typedef, hence the void* handle).
class BzInflateStream final : public InputStream {
public:
    explicit BzInflateStream(InputStream& src);
    ~BzInflateStream() override;

    BzInflateStream(const BzInflateStream&) = delete;
    BzInflateStream& operator=(const BzInflateStream&) = delete;

    // True when the build includes libbz2 (JDOC_WITH_BZIP2).
    static bool supported();

    bool is_open() const { return bs_ != nullptr; }
    size_t read(void* buf, size_t len) override;

    bool error() const { return error_; }
    bool finished() const { return finished_; }
    uint64_t compressed_in() const { return compressed_in_; }
    uint64_t uncompressed_out() const { return uncompressed_out_; }

private:
    InputStream& src_;
    void* bs_ = nullptr;  // bz_stream*
    std::vector<char> in_buf_;
    bool src_eof_ = false;
    bool finished_ = false;
    bool error_ = false;
    uint64_t compressed_in_ = 0;
    uint64_t uncompressed_out_ = 0;
};

} // namespace jdoc
