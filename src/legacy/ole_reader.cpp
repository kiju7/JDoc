// OLE Compound Document (MS-CFB) reader implementation.

#include "ole_reader.h"
#include "common/string_utils.h"
#include "common/binary_utils.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace jdoc {

// Special FAT values.
static constexpr uint32_t ENDOFCHAIN  = 0xFFFFFFFE;
static constexpr uint32_t FREESECT    = 0xFFFFFFFF;
static constexpr uint32_t FATSECT     = 0xFFFFFFFD;
static constexpr uint32_t DIFSECT     = 0xFFFFFFFC;
static constexpr uint32_t NOSTREAM    = 0xFFFFFFFF;

// Expected magic signature.
static const uint8_t kOleMagic[8] = {
    0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1
};

// ---------- OleReader --------------------------------------------------------

OleReader::OleReader(const std::string& path) {
    fp_ = std::fopen(path.c_str(), "rb");
    if (!fp_) return;
    init_from_source();
}

OleReader::OleReader(const uint8_t* data, size_t size)
    : mem_data_(data), mem_size_(size) {
    if (!data || size < 512) return;
    init_from_source();
}

void OleReader::init_from_source() {
    try {
        parse_header();
        parse_fat();
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

void OleReader::parse_header() {
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
    uint16_t byte_order = util::read_u16_le(hdr + 0x1C);
    if (byte_order != 0xFFFE)
        throw std::runtime_error("Unexpected byte order");

    uint16_t sector_pow = util::read_u16_le(hdr + 0x1E);
    sector_size_ = 1u << sector_pow;

    uint16_t mini_pow = util::read_u16_le(hdr + 0x20);
    mini_sector_size_ = 1u << mini_pow;

    uint32_t total_sat_sectors = util::read_u32_le(hdr + 0x2C);
    first_dir_sector_ = util::read_u32_le(hdr + 0x30);
    mini_cutoff_ = util::read_u32_le(hdr + 0x38);

    uint32_t first_mini_fat_sector = util::read_u32_le(hdr + 0x3C);
    uint32_t num_mini_fat_sectors  = util::read_u32_le(hdr + 0x40);
    uint32_t first_difat_sector    = util::read_u32_le(hdr + 0x44);
    uint32_t num_difat_sectors     = util::read_u32_le(hdr + 0x48);

    // Build the complete list of FAT sector indices from DIFAT.
    // First 109 entries are in the header at offset 0x4C.
    std::vector<uint32_t> fat_sectors;
    fat_sectors.reserve(total_sat_sectors);

    for (int i = 0; i < 109 && fat_sectors.size() < total_sat_sectors; ++i) {
        uint32_t s = util::read_u32_le(hdr + 0x4C + i * 4);
        if (s == FREESECT || s == ENDOFCHAIN) break;
        fat_sectors.push_back(s);
    }

    // Follow DIFAT chain for additional entries.
    uint32_t difat_sec = first_difat_sector;
    uint32_t entries_per_difat = sector_size_ / 4 - 1; // last uint32 is next DIFAT sector
    for (uint32_t d = 0; d < num_difat_sectors && difat_sec != ENDOFCHAIN && difat_sec != FREESECT; ++d) {
        std::vector<uint8_t> buf(sector_size_);
        read_sector(difat_sec, buf.data());
        for (uint32_t j = 0; j < entries_per_difat && fat_sectors.size() < total_sat_sectors; ++j) {
            uint32_t s = util::read_u32_le(buf.data() + j * 4);
            if (s == FREESECT || s == ENDOFCHAIN) break;
            fat_sectors.push_back(s);
        }
        difat_sec = util::read_u32_le(buf.data() + entries_per_difat * 4);
    }

    // Store for use in parse_fat.
    // We temporarily stash fat_sectors; parse_fat will use them.
    // Actually, we can just build the FAT right here.
    uint32_t entries_per_sector = sector_size_ / 4;
    fat_.resize(fat_sectors.size() * entries_per_sector);
    std::vector<uint8_t> sbuf(sector_size_);
    for (size_t i = 0; i < fat_sectors.size(); ++i) {
        read_sector(fat_sectors[i], sbuf.data());
        for (uint32_t j = 0; j < entries_per_sector; ++j) {
            fat_[i * entries_per_sector + j] = util::read_u32_le(sbuf.data() + j * 4);
        }
    }

    // Store mini-FAT chain start for parse_mini_fat.
    // We'll read it after directories are parsed (need root entry for mini-stream).
    // Save these for later use.
    // Actually, let's just store them and read in parse_mini_fat.
    (void)first_mini_fat_sector;
    (void)num_mini_fat_sectors;

    // We need these values later - store in temporary members via a small trick:
    // parse_mini_fat is called after parse_directories, so we can store these in the
    // mini_fat_ vector temporarily. But that's ugly. Instead, just re-read them.
    // We'll cache them as class-level values. We add hidden storage via the header buffer
    // we already have. Let me just re-read the header when needed. Actually, the simplest
    // approach: store the values we need.

    // We don't have dedicated member vars for these, so let's save the raw header
    // and re-read these values in parse_mini_fat. Store in mini_fat_ temporarily.
    // That's too hacky. Let's just store a couple of extra values.
    // We will re-seek and re-read the relevant header fields in parse_mini_fat.
}

// ---------- read_sector ------------------------------------------------------

void OleReader::read_sector(uint32_t sector, void* buf) const {
    uint64_t offset = static_cast<uint64_t>(sector + 1) * sector_size_;

    if (mem_data_) {
        if (offset + sector_size_ <= mem_size_) {
            std::memcpy(buf, mem_data_ + offset, sector_size_);
        } else {
            std::memset(buf, 0, sector_size_);
        }
        return;
    }

#ifdef _WIN32
    _fseeki64(fp_, offset, SEEK_SET);
#else
    std::fseek(fp_, static_cast<long>(offset), SEEK_SET);
#endif
    std::fread(buf, 1, sector_size_, fp_);
}

// ---------- parse_fat (already done in parse_header) -------------------------

void OleReader::parse_fat() {
    // FAT was already built in parse_header.
}

// ---------- parse_directories ------------------------------------------------

void OleReader::parse_directories() {
    // Read all sectors in the directory chain.
    std::vector<char> dir_data;
    uint32_t sec = first_dir_sector_;
    while (sec != ENDOFCHAIN && sec != FREESECT && sec < fat_.size()) {
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
        size_t name_bytes = (name_size > 2) ? (name_size - 2) : 0; // strip null terminator (2 bytes for UTF-16)
        dirs_[i].name = util::utf16le_to_utf8(reinterpret_cast<const char*>(e), name_bytes);
        dirs_[i].type = e[0x42];
        dirs_[i].left_id  = static_cast<int>(util::read_u32_le(e + 0x44));
        dirs_[i].right_id = static_cast<int>(util::read_u32_le(e + 0x48));
        dirs_[i].child_id = static_cast<int>(util::read_u32_le(e + 0x4C));
        dirs_[i].start_sector = util::read_u32_le(e + 0x74);

        if (major_version_ == 4) {
            dirs_[i].size = util::read_u64_le(e + 0x78);
        } else {
            dirs_[i].size = util::read_u32_le(e + 0x78);
        }

        // Sanitize sentinel values.
        if (dirs_[i].left_id == static_cast<int>(NOSTREAM)) dirs_[i].left_id = -1;
        if (dirs_[i].right_id == static_cast<int>(NOSTREAM)) dirs_[i].right_id = -1;
        if (dirs_[i].child_id == static_cast<int>(NOSTREAM)) dirs_[i].child_id = -1;
    }

    // Build mini-stream from root entry (entry 0).
    if (!dirs_.empty() && dirs_[0].type == 5 && dirs_[0].size > 0) {
        mini_stream_ = read_chain(dirs_[0].start_sector, dirs_[0].size);
    }
}

// ---------- parse_mini_fat ---------------------------------------------------

void OleReader::parse_mini_fat() {
    // Re-read the header to get first mini-FAT sector and count.
    uint8_t hdr[512];
    if (mem_data_) {
        std::memcpy(hdr, mem_data_, 512);
    } else {
        std::fseek(fp_, 0, SEEK_SET);
        if (std::fread(hdr, 1, 512, fp_) != 512) return;
    }

    uint32_t first_mini_fat_sector = util::read_u32_le(hdr + 0x3C);
    uint32_t num_mini_fat_sectors  = util::read_u32_le(hdr + 0x40);

    if (first_mini_fat_sector == ENDOFCHAIN || num_mini_fat_sectors == 0) return;

    uint32_t entries_per_sector = sector_size_ / 4;
    uint32_t sec = first_mini_fat_sector;
    std::vector<uint8_t> sbuf(sector_size_);

    while (sec != ENDOFCHAIN && sec != FREESECT && sec < fat_.size()) {
        read_sector(sec, sbuf.data());
        for (uint32_t j = 0; j < entries_per_sector; ++j) {
            mini_fat_.push_back(util::read_u32_le(sbuf.data() + j * 4));
        }
        sec = fat_[sec];
    }
}

// ---------- read_chain -------------------------------------------------------

std::vector<char> OleReader::read_chain(uint32_t start, uint64_t size) const {
    std::vector<char> result(static_cast<size_t>(size));

    uint32_t sec = start;
    uint64_t written = 0;

    if (mem_data_) {
        // Memory-based: direct copy from mapped buffer, no intermediate buffer
        while (sec != ENDOFCHAIN && sec != FREESECT && sec < fat_.size() && written < size) {
            uint64_t offset = static_cast<uint64_t>(sec + 1) * sector_size_;
            uint64_t to_copy = std::min(static_cast<uint64_t>(sector_size_), size - written);
            if (offset + to_copy <= mem_size_) {
                std::memcpy(result.data() + written, mem_data_ + offset, static_cast<size_t>(to_copy));
            }
            written += to_copy;
            sec = fat_[sec];
        }
    } else {
        // File-based: sector-at-a-time
        std::vector<char> sbuf(sector_size_);
        while (sec != ENDOFCHAIN && sec != FREESECT && sec < fat_.size() && written < size) {
            read_sector(sec, sbuf.data());
            uint64_t to_copy = std::min(static_cast<uint64_t>(sector_size_), size - written);
            std::memcpy(result.data() + written, sbuf.data(), static_cast<size_t>(to_copy));
            written += to_copy;
            sec = fat_[sec];
        }
    }

    return result;
}

// ---------- read_mini_chain --------------------------------------------------

std::vector<char> OleReader::read_mini_chain(uint32_t start, uint64_t size) const {
    std::vector<char> result;
    result.reserve(static_cast<size_t>(size));

    uint32_t sec = start;
    uint64_t remaining = size;

    while (sec != ENDOFCHAIN && sec != FREESECT && sec < mini_fat_.size() && remaining > 0) {
        uint64_t offset = static_cast<uint64_t>(sec) * mini_sector_size_;
        uint64_t to_copy = std::min(static_cast<uint64_t>(mini_sector_size_), remaining);
        if (offset + to_copy <= mini_stream_.size()) {
            result.insert(result.end(),
                          mini_stream_.begin() + offset,
                          mini_stream_.begin() + offset + to_copy);
        } else {
            // Pad with zeros if mini-stream is too short.
            size_t avail = (offset < mini_stream_.size()) ? (mini_stream_.size() - static_cast<size_t>(offset)) : 0;
            if (avail > 0)
                result.insert(result.end(),
                              mini_stream_.begin() + offset,
                              mini_stream_.begin() + offset + avail);
            result.resize(result.size() + static_cast<size_t>(to_copy - avail), 0);
        }
        remaining -= to_copy;
        sec = mini_fat_[sec];
    }

    result.resize(static_cast<size_t>(size));
    return result;
}


// ---------- traverse_dir -----------------------------------------------------

void OleReader::traverse_dir(int id, std::vector<std::string>& names) const {
    if (id < 0 || id >= static_cast<int>(dirs_.size())) return;

    const DirEntry& e = dirs_[id];

    // In-order traversal of the red-black tree.
    traverse_dir(e.left_id, names);

    if (e.type == 2) { // stream
        names.push_back(e.name);
    } else if (e.type == 1 || e.type == 5) { // storage or root
        if (e.type == 1) names.push_back(e.name + "/");
        traverse_dir(e.child_id, names);
    }

    traverse_dir(e.right_id, names);
}

// ---------- find_entry -------------------------------------------------------

int OleReader::find_entry(const std::string& name) const {
    // Support "/" separated paths like "BinData/BIN0001.png"
    auto slash = name.find('/');
    if (slash != std::string::npos) {
        std::string parent = name.substr(0, slash);
        std::string child = name.substr(slash + 1);
        int storage_id = find_storage(parent);
        if (storage_id < 0) return -1;
        // Search among children of this storage
        int child_id = dirs_[storage_id].child_id;
        if (child_id < 0) return -1;
        // BFS/linear search in the subtree for the child stream
        for (size_t i = 0; i < dirs_.size(); ++i) {
            if (dirs_[i].name == child && dirs_[i].type == 2)
                return static_cast<int>(i);
        }
        return -1;
    }
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

    int child = dirs_[sid].child_id;
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

    const DirEntry& e = dirs_[idx];
    if (e.size == 0) return {};

    if (e.size < mini_cutoff_) {
        return read_mini_chain(e.start_sector, e.size);
    } else {
        return read_chain(e.start_sector, e.size);
    }
}

} // namespace jdoc
