#pragma once
// Lightweight ZIP reader using zlib inflate (no minizip dependency)
// Supports STORE (method 0) and DEFLATE (method 8) — sufficient for OOXML

#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>

namespace jdoc {

class ZipReader {
public:
    struct Entry {
        std::string name;
        uint32_t compressed_size = 0;
        uint32_t uncompressed_size = 0;
        uint16_t method = 0;            // 0=STORE, 8=DEFLATE
        uint32_t local_header_offset = 0;
    };

    explicit ZipReader(const std::string& path);
    ~ZipReader();

    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    bool is_open() const { return fp_ != nullptr; }
    const std::vector<Entry>& entries() const { return entries_; }

    // Find entries whose name starts with prefix
    std::vector<const Entry*> entries_with_prefix(const std::string& prefix) const;

    // Read entire entry into memory
    std::vector<char> read_entry(const std::string& name) const;
    std::vector<char> read_entry(const Entry& entry) const;

    // Stream entry directly to file (memory-efficient for large images)
    bool extract_entry_to_file(const Entry& entry, const std::string& output_path) const;

    // Check if entry exists
    bool has_entry(const std::string& name) const;

private:
    FILE* fp_ = nullptr;
    std::vector<Entry> entries_;

    bool parse_central_directory();
    bool read_local_data(const Entry& entry, std::vector<char>& out) const;
};

} // namespace jdoc
