// zip_reader.cpp - Lightweight ZIP reader using raw zlib inflate
// License: MIT

#include "zip_reader.h"
#include "archive/data_source.h"
#include "common/binary_utils.h"
#include "common/string_utils.h"
#include <zlib.h>
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace jdoc {

// ── ZIP format constants ────────────────────────────────

static constexpr uint32_t EOCD_SIGNATURE = 0x06054b50;
static constexpr uint32_t CD_SIGNATURE   = 0x02014b50;
static constexpr uint32_t LFH_SIGNATURE  = 0x04034b50;

// General purpose flag bit 11: filename is UTF-8
static constexpr uint16_t FLAG_UTF8_NAME = 0x0800;

// Cap for read_entry(): the central directory's uncompressed_size is not
// trusted, so streaming stops here even if the header claims less.
static constexpr uint64_t READ_ENTRY_CAP = 1ull << 30;  // 1 GiB

// ── ZipReader implementation ────────────────────────────

ZipReader::ZipReader(const std::string& path) {
    auto src = std::make_unique<FileSource>(path);
    if (!src->is_open()) return;
    src_ = std::move(src);
    open_ = parse_central_directory();
    if (!open_) src_.reset();
}

ZipReader::ZipReader(const uint8_t* data, size_t size) {
    src_ = std::make_unique<MemorySource>(data, size);
    open_ = parse_central_directory();
    if (!open_) src_.reset();
}

ZipReader::~ZipReader() = default;

bool ZipReader::parse_central_directory() {
    // Find EOCD by scanning backwards from end of file
    uint64_t file_size = src_->size();
    if (file_size < 22) return false;

    // EOCD is at least 22 bytes, max comment is 65535
    uint64_t search_start = file_size > 65557 ? file_size - 65557 : 0;
    size_t search_len = static_cast<size_t>(file_size - search_start);

    std::vector<unsigned char> buf(search_len);
    if (src_->read_at(search_start, buf.data(), search_len) != search_len)
        return false;

    // Find EOCD signature backwards
    int64_t eocd_offset = -1;
    for (int64_t i = static_cast<int64_t>(search_len) - 22; i >= 0; --i) {
        if (util::read_u32_le(&buf[i]) == EOCD_SIGNATURE) {
            eocd_offset = static_cast<int64_t>(search_start) + i;
            break;
        }
    }
    if (eocd_offset < 0) return false;

    // Parse EOCD
    unsigned char eocd[22];
    if (src_->read_at(static_cast<uint64_t>(eocd_offset), eocd, 22) != 22) return false;

    uint16_t num_entries = util::read_u16_le(&eocd[10]);
    uint32_t cd_offset   = util::read_u32_le(&eocd[16]);

    // Parse Central Directory
    uint64_t pos = cd_offset;
    entries_.reserve(num_entries);

    for (int i = 0; i < num_entries; ++i) {
        unsigned char hdr[46];
        if (src_->read_at(pos, hdr, 46) != 46) return false;
        if (util::read_u32_le(hdr) != CD_SIGNATURE) return false;
        pos += 46;

        Entry e;
        e.flags             = util::read_u16_le(&hdr[8]);
        e.method            = util::read_u16_le(&hdr[10]);
        e.compressed_size   = util::read_u32_le(&hdr[20]);
        e.uncompressed_size = util::read_u32_le(&hdr[24]);
        uint16_t name_len   = util::read_u16_le(&hdr[28]);
        uint16_t extra_len  = util::read_u16_le(&hdr[30]);
        uint16_t comment_len = util::read_u16_le(&hdr[32]);
        e.local_header_offset = util::read_u32_le(&hdr[42]);

        // Read filename
        std::vector<char> name_buf(name_len);
        if (name_len > 0 &&
            src_->read_at(pos, name_buf.data(), name_len) != name_len)
            return false;
        e.name.assign(name_buf.data(), name_len);
        pos += name_len;

        // Legacy Korean zips (ALZip, Windows explorer) store names in CP949.
        if (!(e.flags & FLAG_UTF8_NAME))
            e.name = util::legacy_name_to_utf8(e.name);

        // Skip extra + comment
        pos += static_cast<uint64_t>(extra_len) + comment_len;

        entries_.push_back(std::move(e));
    }

    return true;
}

bool ZipReader::find_data_offset(const Entry& entry, uint64_t& data_offset) const {
    unsigned char lfh[30];
    if (src_->read_at(entry.local_header_offset, lfh, 30) != 30) return false;
    if (util::read_u32_le(lfh) != LFH_SIGNATURE) return false;

    uint16_t name_len  = util::read_u16_le(&lfh[26]);
    uint16_t extra_len = util::read_u16_le(&lfh[28]);
    data_offset = entry.local_header_offset + 30 + name_len + extra_len;
    return true;
}

const uint8_t* ZipReader::stored_view(const Entry& entry) const {
    if (!open_ || entry.method != 0 ||
        entry.compressed_size != entry.uncompressed_size)
        return nullptr;
    uint64_t pos = 0;
    if (!find_data_offset(entry, pos)) return nullptr;
    return src_->view_at(pos, entry.uncompressed_size);
}

bool ZipReader::read_entry_streamed(const Entry& entry, const WriteFn& sink,
                                    std::string* err) const {
    auto fail = [&](const char* reason) {
        if (err) *err = reason;
        return false;
    };
    if (!open_) return fail("archive not open");

    uint64_t pos = 0;
    if (!find_data_offset(entry, pos))
        return fail("corrupt local file header");

    static constexpr size_t BUF_SIZE = 65536;

    if (entry.method == 0) {
        // STORE: pass through in chunks
        std::vector<char> buf(BUF_SIZE);
        uint64_t remaining = entry.uncompressed_size;
        while (remaining > 0) {
            size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, BUF_SIZE));
            if (src_->read_at(pos, buf.data(), chunk) != chunk)
                return fail("truncated stored data");
            if (!sink(buf.data(), chunk))
                return fail("aborted by sink");
            pos += chunk;
            remaining -= chunk;
        }
        return true;
    }

    if (entry.method == 8) {
        // DEFLATE: streaming decompression in 64KB chunks
        std::vector<unsigned char> in_buf(BUF_SIZE);
        std::vector<unsigned char> out_buf(BUF_SIZE);

        z_stream zs = {};
        // -MAX_WBITS for raw deflate (no gzip/zlib header)
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
            return fail("inflateInit failed");

        uint64_t remaining = entry.compressed_size;
        int ret = Z_OK;
        while (remaining > 0 && ret != Z_STREAM_END) {
            size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, BUF_SIZE));
            if (src_->read_at(pos, in_buf.data(), chunk) != chunk) {
                inflateEnd(&zs);
                return fail("truncated compressed data");
            }
            pos += chunk;
            remaining -= chunk;

            zs.next_in = in_buf.data();
            zs.avail_in = static_cast<uInt>(chunk);

            do {
                zs.next_out = out_buf.data();
                zs.avail_out = static_cast<uInt>(BUF_SIZE);
                ret = inflate(&zs, Z_NO_FLUSH);
                if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                    inflateEnd(&zs);
                    return fail("corrupt deflate stream");
                }
                size_t have = BUF_SIZE - zs.avail_out;
                if (have > 0 && !sink(reinterpret_cast<char*>(out_buf.data()), have)) {
                    inflateEnd(&zs);
                    return fail("aborted by sink");
                }
            } while (zs.avail_out == 0);
        }

        inflateEnd(&zs);
        if (ret != Z_STREAM_END)
            return fail("incomplete deflate stream");
        return true;
    }

    return fail("unsupported compression method");
}

std::vector<const ZipReader::Entry*> ZipReader::entries_with_prefix(const std::string& prefix) const {
    std::vector<const Entry*> result;
    for (auto& e : entries_) {
        if (e.name.size() >= prefix.size() &&
            e.name.compare(0, prefix.size(), prefix) == 0 &&
            e.name.size() > prefix.size()) {  // skip directory entries
            result.push_back(&e);
        }
    }
    return result;
}

std::vector<char> ZipReader::read_entry(const std::string& name) const {
    for (auto& e : entries_) {
        if (e.name == name) {
            return read_entry(e);
        }
    }
    return {};
}

std::vector<char> ZipReader::read_entry(const Entry& entry) const {
    std::vector<char> out;
    out.reserve(std::min<uint64_t>(entry.uncompressed_size, READ_ENTRY_CAP));
    bool ok = read_entry_streamed(entry, [&](const char* data, size_t len) {
        if (out.size() + len > READ_ENTRY_CAP) return false;
        out.insert(out.end(), data, data + len);
        return true;
    });
    if (!ok) out.clear();
    return out;
}

bool ZipReader::extract_entry_to_file(const Entry& entry, const std::string& output_path) const {
    FILE* out = fopen(output_path.c_str(), "wb");
    if (!out) return false;

    bool ok = read_entry_streamed(entry, [&](const char* data, size_t len) {
        return fwrite(data, 1, len, out) == len;
    });

    fclose(out);
    return ok;
}

bool ZipReader::has_entry(const std::string& name) const {
    for (auto& e : entries_) {
        if (e.name == name) return true;
    }
    return false;
}

} // namespace jdoc
