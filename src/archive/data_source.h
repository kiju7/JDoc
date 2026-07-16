#pragma once
// data_source.h - Random-access byte source abstraction
// Lets container readers (ZipReader, ...) operate on a file or an
// in-memory buffer (nested archive members) with a single code path.
// License: MIT

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace jdoc {

class DataSource {
public:
    virtual ~DataSource() = default;

    // Read up to len bytes at absolute offset. Returns bytes read (0 on EOF/error).
    virtual size_t read_at(uint64_t offset, void* buf, size_t len) const = 0;

    virtual uint64_t size() const = 0;
};

class FileSource final : public DataSource {
public:
    explicit FileSource(const std::string& path) {
        fp_ = fopen(path.c_str(), "rb");
        if (!fp_) return;
        fseek(fp_, 0, SEEK_END);
        long sz = ftell(fp_);
        if (sz < 0) {
            fclose(fp_);
            fp_ = nullptr;
            return;
        }
        size_ = static_cast<uint64_t>(sz);
    }

    ~FileSource() override {
        if (fp_) fclose(fp_);
    }

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    bool is_open() const { return fp_ != nullptr; }

    size_t read_at(uint64_t offset, void* buf, size_t len) const override {
        if (!fp_ || offset >= size_) return 0;
        if (fseek(fp_, static_cast<long>(offset), SEEK_SET) != 0) return 0;
        return fread(buf, 1, len, fp_);
    }

    uint64_t size() const override { return size_; }

private:
    FILE* fp_ = nullptr;
    uint64_t size_ = 0;
};

// Non-owning view: the caller must keep the buffer alive for the
// lifetime of the MemorySource.
class MemorySource final : public DataSource {
public:
    MemorySource(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    size_t read_at(uint64_t offset, void* buf, size_t len) const override {
        if (!data_ || offset >= size_) return 0;
        size_t avail = static_cast<size_t>(size_ - offset);
        size_t n = len < avail ? len : avail;
        memcpy(buf, data_ + offset, n);
        return n;
    }

    uint64_t size() const override { return size_; }

private:
    const uint8_t* data_ = nullptr;
    uint64_t size_ = 0;
};

} // namespace jdoc
