#pragma once
// mapped_file.h - read-only whole-file mapping with a heap fallback.
//
// A parser that needs the whole file as one contiguous read-only buffer can
// map it instead of malloc'ing a buffer and read()'ing into it: the pages are
// file-backed and demand-loaded, so peak RSS drops by the file size and the
// read() copy disappears. When mapping is unavailable (a non-POSIX/Windows
// build, an mmap/MapViewOfFile failure, or a non-regular file) it transparently
// falls back to reading the file into a heap buffer, so callers always get a
// valid pointer or valid()==false.
//
// Read-only only: the mapping is PAGE_READONLY / PROT_READ. Callers must not
// write through data() (the PDF parser already treats its input as const —
// convert_bytes() hands it read-only Python buffers today).
// License: MIT
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace jdoc {

class MappedFile {
public:
    explicit MappedFile(const std::string& path) { open_(path); }
    ~MappedFile() { close_(); }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    bool valid() const { return data_ != nullptr; }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    std::vector<uint8_t> fallback_;
#if defined(_WIN32)
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
#elif defined(__unix__) || defined(__APPLE__)
    void* map_ = nullptr;
#endif

    // Last resort: pull the whole file into a heap buffer.
    void read_fallback_(const std::string& path) {
        FILE* fp = std::fopen(path.c_str(), "rb");
        if (!fp) return;
        if (std::fseek(fp, 0, SEEK_END) == 0) {
            long sz = std::ftell(fp);
            if (sz > 0) {
                std::fseek(fp, 0, SEEK_SET);
                fallback_.resize(static_cast<size_t>(sz));
                size_t got = std::fread(fallback_.data(), 1, fallback_.size(), fp);
                fallback_.resize(got);
                if (got) { data_ = fallback_.data(); size_ = got; }
            }
        }
        std::fclose(fp);
    }

#if defined(_WIN32)
    void open_(const std::string& path) {
        file_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER li;
        if (!GetFileSizeEx(file_, &li) || li.QuadPart <= 0 ||
            static_cast<unsigned long long>(li.QuadPart) >
                static_cast<unsigned long long>(SIZE_MAX)) {
            read_fallback_(path);
            return;
        }
        mapping_ = CreateFileMappingA(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_) { read_fallback_(path); return; }
        view_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
        if (!view_) { read_fallback_(path); return; }
        data_ = static_cast<const uint8_t*>(view_);
        size_ = static_cast<size_t>(li.QuadPart);
    }
    void close_() {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
#elif defined(__unix__) || defined(__APPLE__)
    void open_(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return;
        struct stat st;
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            ::close(fd);
            read_fallback_(path);
            return;
        }
        size_t sz = static_cast<size_t>(st.st_size);
        void* p = ::mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);  // the mapping holds its own reference to the file
        if (p == MAP_FAILED) { read_fallback_(path); return; }
        map_ = p;
        data_ = static_cast<const uint8_t*>(p);
        size_ = sz;
        // No MADV_WILLNEED: the point is to let only the pages the parser
        // actually touches become resident (xref + used objects), so peak RSS
        // stays at or below the old whole-file heap read. Eager readahead would
        // fault the entire file in and defeat that.
    }
    void close_() {
        if (map_) ::munmap(map_, size_);
    }
#else
    void open_(const std::string& path) { read_fallback_(path); }
    void close_() {}
#endif
};

} // namespace jdoc
