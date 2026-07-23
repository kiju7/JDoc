// OLE Compound Document (MS-CFB) reader implementation.

#include "ole_reader.h"
#include "common/string_utils.h"
#include "common/binary_utils.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace jdoc {

// Special FAT values.
static constexpr uint32_t ENDOFCHAIN = 0xFFFFFFFE;
static constexpr uint32_t FREESECT   = 0xFFFFFFFF;
static constexpr uint32_t NOSTREAM   = 0xFFFFFFFF;

// Expected magic signature.
static const uint8_t kOleMagic[8] = {
    0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1
};

// ---------- OleReader --------------------------------------------------------

OleReader::OleReader(const std::string& path) {
    fp_ = std::fopen(path.c_str(), "rb");
    if (!fp_) return;
#ifdef _WIN32
    if (_fseeki64(fp_, 0, SEEK_END) != 0) return;
    const auto end = _ftelli64(fp_);
    if (end < 0 || _fseeki64(fp_, 0, SEEK_SET) != 0) return;
#else
    if (fseeko(fp_, 0, SEEK_END) != 0) return;
    const auto end = ftello(fp_);
    if (end < 0 || fseeko(fp_, 0, SEEK_SET) != 0) return;
#endif
    source_size_ = static_cast<uint64_t>(end);
    init_from_source();
}

OleReader::OleReader(const uint8_t* data, size_t size)
    : mem_data_(data), mem_size_(size), source_size_(size) {
    if (!data || size < 512) return;
    init_from_source();
}

void OleReader::init_from_source() {
    try {
        parse_header_and_fat();
        parse_directories();
        parse_mini_fat();
        valid_ = true;
    } catch (...) {
        valid_ = false;
    }
}

OleReader::~OleReader() {
    if (fp_) std::fclose(fp_);
}

bool OleReader::is_open() const {
    return (fp_ != nullptr || mem_data_ != nullptr) && valid_;
}

// ---------- header -----------------------------------------------------------

void OleReader::parse_header_and_fat() {
    uint8_t hdr[512];
    if (mem_data_) {
        if (mem_size_ < 512)
            throw std::runtime_error("Failed to read OLE header");
        std::memcpy(hdr, mem_data_, 512);
    } else {
        if (std::fread(hdr, 1, 512, fp_) != 512)
            throw std::runtime_error("Failed to read OLE header");
    }

    // Validate magic.
    if (std::memcmp(hdr, kOleMagic, 8) != 0)
        throw std::runtime_error("Not an OLE compound document");

    major_version_ = util::read_u16_le(hdr + 0x1A);
    if (major_version_ != 3 && major_version_ != 4)
        throw std::runtime_error("Unsupported OLE major version");
    uint16_t byte_order = util::read_u16_le(hdr + 0x1C);
    if (byte_order != 0xFFFE)
        throw std::runtime_error("Unexpected byte order");

    uint16_t sector_pow = util::read_u16_le(hdr + 0x1E);
    uint16_t mini_pow = util::read_u16_le(hdr + 0x20);
    const uint16_t expected_sector_pow = major_version_ == 3 ? 9 : 12;
    if (sector_pow != expected_sector_pow || mini_pow != 6)
        throw std::runtime_error("Invalid OLE sector size");
    sector_size_ = uint32_t{1} << sector_pow;
    mini_sector_size_ = uint32_t{1} << mini_pow;
    if (source_size_ < sector_size_ || source_size_ % sector_size_ != 0)
        throw std::runtime_error("Truncated OLE sector data");
    sector_count_ = source_size_ / sector_size_ - 1;

    uint32_t total_sat_sectors = util::read_u32_le(hdr + 0x2C);
    first_dir_sector_ = util::read_u32_le(hdr + 0x30);
    mini_cutoff_ = util::read_u32_le(hdr + 0x38);

    first_mini_fat_sector_ = util::read_u32_le(hdr + 0x3C);
    num_mini_fat_sectors_  = util::read_u32_le(hdr + 0x40);
    uint32_t first_difat_sector    = util::read_u32_le(hdr + 0x44);
    uint32_t num_difat_sectors     = util::read_u32_le(hdr + 0x48);
    if (mini_cutoff_ != 4096)
        throw std::runtime_error("Invalid OLE mini-stream cutoff");
    if (total_sat_sectors > sector_count_ ||
        num_difat_sectors > sector_count_)
        throw std::runtime_error("OLE allocation table exceeds file size");

    // Build the complete list of FAT sector indices from DIFAT.
    // First 109 entries are in the header at offset 0x4C.
    std::vector<uint32_t> fat_sectors;
    fat_sectors.reserve(total_sat_sectors);

    for (int i = 0; i < 109 && fat_sectors.size() < total_sat_sectors; ++i) {
        uint32_t s = util::read_u32_le(hdr + 0x4C + i * 4);
        if (s == FREESECT) continue;
        if (s == ENDOFCHAIN || s >= sector_count_)
            throw std::runtime_error("Invalid OLE FAT sector");
        fat_sectors.push_back(s);
    }

    // Follow DIFAT chain for additional entries.
    uint32_t difat_sec = first_difat_sector;
    uint32_t entries_per_difat = sector_size_ / 4 - 1; // last uint32 is next DIFAT sector
    std::vector<bool> seen_difat(static_cast<size_t>(sector_count_), false);
    for (uint32_t d = 0; d < num_difat_sectors && difat_sec != ENDOFCHAIN && difat_sec != FREESECT; ++d) {
        if (difat_sec >= sector_count_ || seen_difat[difat_sec])
            throw std::runtime_error("Invalid OLE DIFAT chain");
        seen_difat[difat_sec] = true;
        std::vector<uint8_t> buf(sector_size_);
        read_sector(difat_sec, buf.data());
        for (uint32_t j = 0; j < entries_per_difat && fat_sectors.size() < total_sat_sectors; ++j) {
            uint32_t s = util::read_u32_le(buf.data() + j * 4);
            if (s == FREESECT) continue;
            if (s == ENDOFCHAIN || s >= sector_count_)
                throw std::runtime_error("Invalid OLE FAT sector");
            fat_sectors.push_back(s);
        }
        difat_sec = util::read_u32_le(buf.data() + entries_per_difat * 4);
    }
    if (fat_sectors.size() != total_sat_sectors)
        throw std::runtime_error("Incomplete OLE FAT");

    uint32_t entries_per_sector = sector_size_ / 4;
    fat_.resize(fat_sectors.size() * entries_per_sector);
    std::vector<uint8_t> sbuf(sector_size_);
    for (size_t i = 0; i < fat_sectors.size(); ++i) {
        read_sector(fat_sectors[i], sbuf.data());
        for (uint32_t j = 0; j < entries_per_sector; ++j) {
            fat_[i * entries_per_sector + j] = util::read_u32_le(sbuf.data() + j * 4);
        }
    }

}

// ---------- read_sector ------------------------------------------------------

void OleReader::read_sector(uint32_t sector, void* buf) const {
    if (!buf || sector >= sector_count_)
        throw std::runtime_error("OLE sector is outside the source");
    uint64_t offset = (static_cast<uint64_t>(sector) + 1) * sector_size_;

    if (mem_data_) {
        std::memcpy(buf, mem_data_ + offset, sector_size_);
        return;
    }

#ifdef _WIN32
    if (_fseeki64(fp_, static_cast<__int64>(offset), SEEK_SET) != 0)
        throw std::runtime_error("Failed to seek OLE sector");
#else
    if (fseeko(fp_, static_cast<off_t>(offset), SEEK_SET) != 0)
        throw std::runtime_error("Failed to seek OLE sector");
#endif
    if (std::fread(buf, 1, sector_size_, fp_) != sector_size_)
        throw std::runtime_error("Failed to read OLE sector");
}

// ---------- parse_directories ------------------------------------------------

void OleReader::parse_directories() {
    // Read all sectors in the directory chain.
    std::vector<char> dir_data;
    uint32_t sec = first_dir_sector_;
    std::vector<bool> seen(fat_.size(), false);
    while (sec != ENDOFCHAIN && sec != FREESECT && sec < fat_.size()) {
        if (seen[sec])
            throw std::runtime_error("Circular OLE directory chain");
        seen[sec] = true;
        size_t old_size = dir_data.size();
        dir_data.resize(old_size + sector_size_);
        read_sector(sec, dir_data.data() + old_size);
        sec = fat_[sec];
    }

    // Each directory entry is 128 bytes.
    size_t num_entries = dir_data.size() / 128;
    dirs_.resize(num_entries);

    for (size_t i = 0; i < num_entries; ++i) {
        const uint8_t* e = reinterpret_cast<const uint8_t*>(dir_data.data()) + i * 128;
        uint16_t name_size = util::read_u16_le(e + 0x40); // bytes including null terminator
        if (name_size > 64 || (name_size & 1) != 0)
            throw std::runtime_error("Invalid OLE directory name length");
        size_t name_bytes = (name_size > 2) ? (name_size - 2) : 0; // strip null terminator (2 bytes for UTF-16)
        dirs_[i].name = util::utf16le_to_utf8(reinterpret_cast<const char*>(e), name_bytes);
        dirs_[i].type = e[0x42];
        auto directory_id = [](uint32_t value) {
            return value == NOSTREAM ||
                   value > static_cast<uint32_t>(std::numeric_limits<int>::max())
                ? -1 : static_cast<int>(value);
        };
        dirs_[i].left_id  = directory_id(util::read_u32_le(e + 0x44));
        dirs_[i].right_id = directory_id(util::read_u32_le(e + 0x48));
        dirs_[i].child_id = directory_id(util::read_u32_le(e + 0x4C));
        dirs_[i].start_sector = util::read_u32_le(e + 0x74);

        if (major_version_ == 4) {
            dirs_[i].size = util::read_u64_le(e + 0x78);
        } else {
            dirs_[i].size = util::read_u32_le(e + 0x78);
        }
    }

    // Build mini-stream from root entry (entry 0).
    if (!dirs_.empty() && dirs_[0].type == 5 && dirs_[0].size > 0) {
        mini_stream_ = read_chain(dirs_[0].start_sector, dirs_[0].size);
    }
}

// ---------- parse_mini_fat ---------------------------------------------------

void OleReader::parse_mini_fat() {
    if (first_mini_fat_sector_ == ENDOFCHAIN ||
        num_mini_fat_sectors_ == 0)
        return;

    uint32_t entries_per_sector = sector_size_ / 4;
    uint32_t sec = first_mini_fat_sector_;
    std::vector<uint8_t> sbuf(sector_size_);

    uint32_t visited = 0;
    std::vector<bool> seen(fat_.size(), false);
    while (sec != ENDOFCHAIN && sec != FREESECT && sec < fat_.size()) {
        if (++visited > num_mini_fat_sectors_ || seen[sec])
            throw std::runtime_error("Invalid OLE mini-FAT chain");
        seen[sec] = true;
        read_sector(sec, sbuf.data());
        for (uint32_t j = 0; j < entries_per_sector; ++j) {
            mini_fat_.push_back(util::read_u32_le(sbuf.data() + j * 4));
        }
        sec = fat_[sec];
    }
    if (visited != num_mini_fat_sectors_)
        throw std::runtime_error("Incomplete OLE mini-FAT chain");
}

// ---------- read_chain -------------------------------------------------------

std::vector<char> OleReader::read_chain(uint32_t start, uint64_t size) const {
    if (size > source_size_ || size > std::numeric_limits<size_t>::max())
        throw std::runtime_error("OLE stream size exceeds source size");
    std::vector<char> result(static_cast<size_t>(size));

    uint32_t sec = start;
    uint32_t fast = start;
    uint64_t written = 0;
    auto advance = [this](uint32_t sector) {
        return sector < fat_.size() ? fat_[sector] : ENDOFCHAIN;
    };
    auto check_cycle = [&] {
        sec = advance(sec);
        fast = advance(advance(fast));
        if (sec < fat_.size() && sec == fast)
            throw std::runtime_error("Circular OLE stream chain");
    };

    if (mem_data_) {
        // Memory-based: direct copy from mapped buffer, no intermediate buffer
        while (sec != ENDOFCHAIN && sec != FREESECT && sec < fat_.size() && written < size) {
            uint64_t offset = (static_cast<uint64_t>(sec) + 1) * sector_size_;
            uint64_t to_copy = std::min(static_cast<uint64_t>(sector_size_), size - written);
            if (offset > source_size_ || to_copy > source_size_ - offset)
                throw std::runtime_error("Truncated OLE stream chain");
            std::memcpy(result.data() + written, mem_data_ + offset, static_cast<size_t>(to_copy));
            written += to_copy;
            check_cycle();
        }
    } else {
        // File-based: sector-at-a-time
        std::vector<char> sbuf(sector_size_);
        while (sec != ENDOFCHAIN && sec != FREESECT && sec < fat_.size() && written < size) {
            read_sector(sec, sbuf.data());
            uint64_t to_copy = std::min(static_cast<uint64_t>(sector_size_), size - written);
            std::memcpy(result.data() + written, sbuf.data(), static_cast<size_t>(to_copy));
            written += to_copy;
            check_cycle();
        }
    }

    if (written != size)
        throw std::runtime_error("Truncated OLE stream chain");
    return result;
}

// ---------- read_mini_chain --------------------------------------------------

std::vector<char> OleReader::read_mini_chain(uint32_t start, uint64_t size) const {
    if (size > mini_stream_.size() || size > std::numeric_limits<size_t>::max())
        throw std::runtime_error("OLE mini stream size exceeds root stream");
    std::vector<char> result(static_cast<size_t>(size));

    uint32_t sec = start;
    uint32_t fast = start;
    uint64_t written = 0;
    auto advance = [this](uint32_t sector) {
        return sector < mini_fat_.size()
            ? mini_fat_[sector] : ENDOFCHAIN;
    };
    while (sec != ENDOFCHAIN && sec != FREESECT &&
           sec < mini_fat_.size() && written < size) {
        uint64_t offset = static_cast<uint64_t>(sec) * mini_sector_size_;
        uint64_t to_copy = std::min(static_cast<uint64_t>(mini_sector_size_),
                                    size - written);
        if (offset > mini_stream_.size() ||
            to_copy > mini_stream_.size() - static_cast<size_t>(offset))
            throw std::runtime_error("Truncated OLE mini-stream chain");
        std::memcpy(result.data() + written, mini_stream_.data() + offset,
                    static_cast<size_t>(to_copy));
        written += to_copy;
        sec = advance(sec);
        fast = advance(advance(fast));
        if (sec < mini_fat_.size() && sec == fast)
            throw std::runtime_error("Circular OLE mini-stream chain");
    }

    if (written != size)
        throw std::runtime_error("Truncated OLE mini-stream chain");
    return result;
}


// ---------- traverse_dir -----------------------------------------------------

void OleReader::traverse_dir(int id, std::vector<std::string>& names) const {
    struct Work {
        int id;
        bool emit;
    };
    std::vector<Work> stack{{id, false}};
    std::vector<bool> visited(dirs_.size(), false);
    while (!stack.empty()) {
        Work work = stack.back();
        stack.pop_back();
        if (work.id < 0 || work.id >= static_cast<int>(dirs_.size()))
            continue;
        const size_t index = static_cast<size_t>(work.id);
        const DirEntry& entry = dirs_[index];
        if (work.emit) {
            if (entry.type == 2)
                names.push_back(entry.name);
            else if (entry.type == 1)
                names.push_back(entry.name + "/");
            continue;
        }
        if (visited[index]) continue;
        visited[index] = true;

        stack.push_back({entry.right_id, false});
        if (entry.type == 1 || entry.type == 5)
            stack.push_back({entry.child_id, false});
        stack.push_back({work.id, true});
        stack.push_back({entry.left_id, false});
    }
}

// ---------- find_entry -------------------------------------------------------

int OleReader::find_in_siblings(int sibling_root, const std::string& name) const {
    // Iterative walk of the left/right sibling tree; never follows child links,
    // so a deeper storage's like-named stream is not matched.
    std::vector<int> stack;
    std::vector<bool> visited(dirs_.size(), false);
    if (sibling_root >= 0) stack.push_back(sibling_root);
    while (!stack.empty()) {
        int id = stack.back();
        stack.pop_back();
        if (id < 0 || id >= static_cast<int>(dirs_.size())) continue;
        const size_t index = static_cast<size_t>(id);
        if (visited[index]) continue;
        visited[index] = true;
        const DirEntry& e = dirs_[index];
        if (e.name == name) return id;
        stack.push_back(e.left_id);
        stack.push_back(e.right_id);
    }
    return -1;
}

int OleReader::find_entry(const std::string& name) const {
    // Support "/" separated paths like "BinData/BIN0001.png".
    auto slash = name.find('/');
    if (slash != std::string::npos) {
        std::string parent = name.substr(0, slash);
        std::string child = name.substr(slash + 1);
        int storage_id = find_storage(parent);
        if (storage_id < 0) return -1;
        // Scope the child lookup to this storage's own subtree. CFB packages
        // (e.g. Outlook .msg with multiple recipient storages) reuse the same
        // leaf name across sibling storages, so a global scan would return the
        // wrong stream.
        int found = find_in_siblings(
            dirs_[static_cast<size_t>(storage_id)].child_id, child);
        return (found >= 0 &&
                dirs_[static_cast<size_t>(found)].type == 2) ? found : -1;
    }
    // Top-level stream: a global scan by name, tolerant of imperfect sibling
    // links, which some writers (and our test fixtures) produce.
    for (size_t i = 0; i < dirs_.size(); ++i) {
        if (dirs_[i].name == name && dirs_[i].type == 2)
            return static_cast<int>(i);
    }
    return -1;
}

int OleReader::find_storage(const std::string& name) const {
    for (size_t i = 0; i < dirs_.size(); ++i) {
        if (dirs_[i].name == name && (dirs_[i].type == 1 || dirs_[i].type == 5))
            return static_cast<int>(i);
    }
    return -1;
}

// ---------- public API -------------------------------------------------------

std::vector<std::string> OleReader::list_streams() const {
    std::vector<std::string> names;
    if (!valid_ || dirs_.empty()) return names;
    traverse_dir(dirs_[0].child_id, names);
    return names;
}

bool OleReader::has_stream(const std::string& name) const {
    return find_entry(name) >= 0;
}

std::vector<std::string> OleReader::entries(const std::string& storage_name) const {
    std::vector<std::string> result;
    if (!valid_) return result;

    int sid = find_storage(storage_name);
    if (sid < 0) return result;

    int child = dirs_[static_cast<size_t>(sid)].child_id;
    if (child < 0) return result;

    // Traverse the red-black tree under this storage
    std::vector<std::string> all;
    traverse_dir(child, all);

    // Filter to just stream names (traverse_dir appends "/" for storages)
    for (auto& name : all) {
        if (!name.empty() && name.back() != '/')
            result.push_back(name);
    }
    return result;
}

std::vector<char> OleReader::read_stream(const std::string& name) const {
    int idx = find_entry(name);
    if (idx < 0) return {};

    const DirEntry& e = dirs_[static_cast<size_t>(idx)];
    if (e.size == 0) return {};

    if (e.size < mini_cutoff_) {
        return read_mini_chain(e.start_sector, e.size);
    } else {
        return read_chain(e.start_sector, e.size);
    }
}

uint64_t OleReader::stream_size(const std::string& name) const {
    int idx = find_entry(name);
    if (idx < 0) return 0;
    return dirs_[static_cast<size_t>(idx)].size;
}

size_t OleReader::write_stream_to_file(const std::string& name,
                                        const std::string& path) const {
    int idx = find_entry(name);
    if (idx < 0) return 0;

    const DirEntry& e = dirs_[static_cast<size_t>(idx)];
    if (e.size == 0) return 0;

    // Mini-stream: small enough to read into memory
    if (e.size < mini_cutoff_) {
        auto data = read_mini_chain(e.start_sector, e.size);
        FILE* out = std::fopen(path.c_str(), "wb");
        if (!out) return 0;
        size_t written = std::fwrite(data.data(), 1, data.size(), out);
        std::fclose(out);
        return written;
    }

    // Regular chain: stream sector-by-sector to file
    FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return 0;

    uint32_t sec = e.start_sector;
    uint64_t remaining = e.size;
    uint32_t max_sectors = static_cast<uint32_t>(e.size / sector_size_ + 2);
    uint32_t visited = 0;
    std::vector<char> sbuf(sector_size_);

    if (mem_data_) {
        while (sec != ENDOFCHAIN && sec != FREESECT && sec < fat_.size() && remaining > 0) {
            if (++visited > max_sectors) break;
            uint64_t offset = static_cast<uint64_t>(sec + 1) * sector_size_;
            uint64_t to_write = std::min(static_cast<uint64_t>(sector_size_), remaining);
            if (offset + to_write <= mem_size_) {
                std::fwrite(mem_data_ + offset, 1, static_cast<size_t>(to_write), out);
            }
            remaining -= to_write;
            sec = fat_[sec];
        }
    } else {
        while (sec != ENDOFCHAIN && sec != FREESECT && sec < fat_.size() && remaining > 0) {
            if (++visited > max_sectors) break;
            read_sector(sec, sbuf.data());
            uint64_t to_write = std::min(static_cast<uint64_t>(sector_size_), remaining);
            std::fwrite(sbuf.data(), 1, static_cast<size_t>(to_write), out);
            remaining -= to_write;
            sec = fat_[sec];
        }
    }

    std::fclose(out);
    return static_cast<size_t>(e.size - remaining);
}

} // namespace jdoc
