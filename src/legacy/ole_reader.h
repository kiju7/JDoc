#pragma once
// OLE Compound Document (MS-CFB) reader
// Parses the binary container format used by .doc, .xls, .ppt files.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace jdoc {

class OleReader {
public:
    explicit OleReader(const std::string& path);
    ~OleReader();

    OleReader(const OleReader&) = delete;
    OleReader& operator=(const OleReader&) = delete;

    bool is_open() const;

    // List all stream names in the directory tree.
    std::vector<std::string> list_streams() const;

    // Read a stream's full contents by name. Returns empty if not found.
    std::vector<char> read_stream(const std::string& name) const;

    // Check if a stream with the given name exists.
    bool has_stream(const std::string& name) const;

private:
    struct DirEntry {
        std::string name;
        uint8_t type;       // 0=unknown, 1=storage, 2=stream, 5=root
        uint32_t start_sector;
        uint64_t size;
        int child_id;
        int left_id;
        int right_id;
    };

    FILE* fp_ = nullptr;
    bool valid_ = false;
    uint16_t major_version_ = 0;
    uint32_t sector_size_ = 512;
    uint32_t mini_sector_size_ = 64;
    uint32_t mini_cutoff_ = 4096;
    uint32_t first_dir_sector_ = 0;
    std::vector<uint32_t> fat_;
    std::vector<uint32_t> mini_fat_;
    std::vector<DirEntry> dirs_;
    std::vector<char> mini_stream_;

    // Read one full sector into buf (must be sector_size_ bytes).
    void read_sector(uint32_t sector, void* buf) const;

    // Follow a FAT chain and return concatenated data (truncated to size).
    std::vector<char> read_chain(uint32_t start, uint64_t size) const;

    // Follow a mini-FAT chain through the mini-stream.
    std::vector<char> read_mini_chain(uint32_t start, uint64_t size) const;

    void parse_header();
    void parse_fat();
    void parse_directories();
    void parse_mini_fat();

    // Recursively traverse the red-black tree of directory entries.
    void traverse_dir(int id, std::vector<std::string>& names) const;

    // Find directory entry index by name, or -1.
    int find_entry(const std::string& name) const;
};

} // namespace jdoc
