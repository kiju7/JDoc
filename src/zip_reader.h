#pragma once
// Lightweight ZIP reader using zlib inflate (no minizip dependency)
// Supports STORE (method 0) and DEFLATE (method 8) — sufficient for OOXML
// Backed by DataSource, so the archive can live on disk or in memory
// (nested archive members are parsed without extraction).

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jdoc {

class DataSource;

class ZipReader {
public:
    struct Entry {
        std::string name;               // UTF-8 (CP949 legacy names converted)
        uint32_t compressed_size = 0;
        uint32_t uncompressed_size = 0;
        uint16_t method = 0;            // 0=STORE, 8=DEFLATE
        uint16_t flags = 0;             // general purpose bit flags
        uint32_t local_header_offset = 0;
    };

    explicit ZipReader(const std::string& path);
    ZipReader(const uint8_t* data, size_t size);  // in-memory archive (non-owning)
    ~ZipReader();

    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    bool is_open() const { return open_; }
    const std::vector<Entry>& entries() const { return entries_; }

    // Find entries whose name starts with prefix
    std::vector<const Entry*> entries_with_prefix(const std::string& prefix) const;

    // Streaming read: decompressed data is delivered to sink in chunks.
    // Sink returns false to abort (e.g. size-limit enforcement during inflate).
    // Returns false on corruption, unsupported method, or sink abort;
    // err (if non-null) receives a reason.
    using WriteFn = std::function<bool(const char* data, size_t len)>;
    bool read_entry_streamed(const Entry& entry, const WriteFn& sink,
                             std::string* err = nullptr) const;

    // Read entire entry into memory (built on read_entry_streamed; output is
    // capped so a lying central directory cannot trigger unbounded allocation)
    std::vector<char> read_entry(const std::string& name) const;
    std::vector<char> read_entry(const Entry& entry) const;

    // Stream entry directly to file (memory-efficient for large images)
    bool extract_entry_to_file(const Entry& entry, const std::string& output_path) const;

    // Check if entry exists
    bool has_entry(const std::string& name) const;

private:
    std::unique_ptr<DataSource> src_;
    bool open_ = false;
    std::vector<Entry> entries_;

    bool parse_central_directory();
    // Locate the start of an entry's data (after the local file header).
    bool find_data_offset(const Entry& entry, uint64_t& data_offset) const;
};

} // namespace jdoc
