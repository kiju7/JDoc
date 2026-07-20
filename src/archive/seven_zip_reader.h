#pragma once
// 7z reader backed by the vendored LZMA SDK (decoder-only subset).
// Mirrors the ZipReader API: entries() enumeration + streamed member reads.
// Backed by DataSource, so the archive can live on disk or in memory
// (nested archive members are parsed without extraction).
//
// 7z groups members into solid blocks (folders); the SDK decodes a whole
// folder into one buffer. That buffer is cached between reads so consecutive
// members of the same folder decode only once. Peak memory equals the
// largest decoded folder — callers should reject oversized folders before
// reading (folder_unpack_size is exposed per entry for that check).
// License: MIT

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jdoc {

class DataSource;

class SevenZipReader {
public:
    struct Entry {
        std::string name;                // UTF-8, '/'-separated
        uint64_t uncompressed_size = 0;
        uint32_t file_index = 0;         // index into the 7z file table
        uint32_t folder_index = 0;       // solid block index; kNoFolder if empty file
        uint64_t folder_unpack_size = 0; // decoded size of the containing solid block
    };
    static constexpr uint32_t kNoFolder = 0xFFFFFFFFu;

    explicit SevenZipReader(const std::string& path);
    SevenZipReader(const uint8_t* data, size_t size);  // in-memory archive (non-owning)
    ~SevenZipReader();

    SevenZipReader(const SevenZipReader&) = delete;
    SevenZipReader& operator=(const SevenZipReader&) = delete;

    bool is_open() const { return open_; }
    const std::vector<Entry>& entries() const { return entries_; }

    // Streaming read: decoded data is delivered to sink in chunks.
    // Sink returns false to abort (e.g. size-limit enforcement).
    // Returns false on corruption, unsupported coder, or sink abort;
    // err (if non-null) receives a reason.
    using WriteFn = std::function<bool(const char* data, size_t len)>;
    bool read_entry_streamed(const Entry& entry, const WriteFn& sink,
                             std::string* err = nullptr) const;

    // Zero-copy read: decode (or reuse) the member's solid block and return
    // a view of the member's bytes inside the cached block buffer — the
    // member is never copied out. The view stays valid until release_cache()
    // or the next read of a different folder.
    bool read_entry_view(const Entry& entry, const uint8_t** data,
                         size_t* size, std::string* err = nullptr) const;

    // Drop the cached solid-block buffer (frees peak memory early).
    void release_cache() const;

private:
    struct Impl;  // hides LZMA SDK types from this header
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<DataSource> src_;
    bool open_ = false;
    std::vector<Entry> entries_;

    void open_archive();
};

} // namespace jdoc
