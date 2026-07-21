#pragma once
// data_source.h - Random-access byte source abstraction
// Lets container readers (ZipReader, ...) operate on a file or an
// in-memory buffer (nested archive members) with a single code path.
// License: MIT

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define JDOC_HAVE_POSIX_IO 1
#endif

namespace jdoc {

class DataSource {
public:
    virtual ~DataSource() = default;

    // Read up to len bytes at absolute offset. Returns bytes read (0 on EOF/error).
    virtual size_t read_at(uint64_t offset, void* buf, size_t len) const = 0;

    // Zero-copy view of [offset, offset+len) when the bytes are already
    // resident in memory. Returns nullptr when unavailable (file-backed
    // source or out of bounds); callers must fall back to read_at. The
    // pointer is valid for the lifetime of the source's backing buffer.
    virtual const uint8_t* view_at(uint64_t offset, size_t len) const {
        (void)offset;
        (void)len;
        return nullptr;
    }

    virtual uint64_t size() const = 0;
};

class FileSource final : public DataSource {
public:
    explicit FileSource(const std::string& path) {
#ifdef JDOC_HAVE_POSIX_IO
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) return;
        struct stat st;
        if (::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode)) {
            ::close(fd_);
            fd_ = -1;
            return;
        }
        size_ = static_cast<uint64_t>(st.st_size);
#else
        fp_ = fopen(path.c_str(), "rb");
        if (!fp_) return;
        if (_fseeki64(fp_, 0, SEEK_END) != 0) { close_fp(); return; }
        long long sz = _ftelli64(fp_);
        if (sz < 0) { close_fp(); return; }
        size_ = static_cast<uint64_t>(sz);
#endif
    }

    ~FileSource() override {
#ifdef JDOC_HAVE_POSIX_IO
        if (fd_ >= 0) ::close(fd_);
#else
        close_fp();
#endif
    }

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

#ifdef JDOC_HAVE_POSIX_IO
    bool is_open() const { return fd_ >= 0; }

    // pread, not fseek+fread: it takes a 64-bit offset (the old fseek(long)
    // truncated past 2 GiB on 32-bit platforms, which zip64 members reach) and
    // leaves no file position behind, so this stays const and thread-safe.
    size_t read_at(uint64_t offset, void* buf, size_t len) const override {
        if (fd_ < 0 || offset >= size_) return 0;
        size_t avail = static_cast<size_t>(size_ - offset);
        if (len > avail) len = avail;
        size_t got = 0;
        while (got < len) {
            ssize_t n = ::pread(fd_, static_cast<char*>(buf) + got, len - got,
                                static_cast<off_t>(offset + got));
            if (n <= 0) break;  // EOF or error; report the short read
            got += static_cast<size_t>(n);
        }
        return got;
    }
#else
    bool is_open() const { return fp_ != nullptr; }

    size_t read_at(uint64_t offset, void* buf, size_t len) const override {
        if (!fp_ || offset >= size_) return 0;
        if (_fseeki64(fp_, static_cast<long long>(offset), SEEK_SET) != 0) return 0;
        return fread(buf, 1, len, fp_);
    }
#endif

    uint64_t size() const override { return size_; }

private:
#ifdef JDOC_HAVE_POSIX_IO
    int fd_ = -1;
#else
    void close_fp() {
        if (fp_) fclose(fp_);
        fp_ = nullptr;
    }

    FILE* fp_ = nullptr;
#endif
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

    const uint8_t* view_at(uint64_t offset, size_t len) const override {
        if (!data_ || offset > size_ || len > size_ - offset) return nullptr;
        return data_ + offset;
    }

    uint64_t size() const override { return size_; }

private:
    const uint8_t* data_ = nullptr;
    uint64_t size_ = 0;
};

} // namespace jdoc
