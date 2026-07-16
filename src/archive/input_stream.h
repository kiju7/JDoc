#pragma once
// input_stream.h - Sequential (pull-based) byte streams
// Used for formats that are consumed front-to-back: gzip members and
// tar containers. Composing TarReader(GzInflateStream(FileStream))
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

} // namespace jdoc
