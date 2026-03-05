// zip_reader.cpp - Lightweight ZIP reader using raw zlib inflate
// License: MIT

#include "zip_reader.h"
#include "common/binary_utils.h"
#include <zlib.h>
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace jdoc {

// ── ZIP format constants ────────────────────────────────

static constexpr uint32_t EOCD_SIGNATURE = 0x06054b50;
static constexpr uint32_t CD_SIGNATURE   = 0x02014b50;
static constexpr uint32_t LFH_SIGNATURE  = 0x04034b50;

// ── ZipReader implementation ────────────────────────────

ZipReader::ZipReader(const std::string& path) {
    fp_ = fopen(path.c_str(), "rb");
    if (!fp_) return;
    if (!parse_central_directory()) {
        fclose(fp_);
        fp_ = nullptr;
    }
}

ZipReader::~ZipReader() {
    if (fp_) fclose(fp_);
}

bool ZipReader::parse_central_directory() {
    // Find EOCD by scanning backwards from end of file
    fseek(fp_, 0, SEEK_END);
    long file_size = ftell(fp_);
    if (file_size < 22) return false;

    // EOCD is at least 22 bytes, max comment is 65535
    long search_start = std::max(0L, file_size - 65557L);
    long search_len = file_size - search_start;

    std::vector<unsigned char> buf(search_len);
    fseek(fp_, search_start, SEEK_SET);
    if (fread(buf.data(), 1, search_len, fp_) != (size_t)search_len)
        return false;

    // Find EOCD signature backwards
    long eocd_offset = -1;
    for (long i = search_len - 22; i >= 0; --i) {
        if (util::read_u32_le(&buf[i]) == EOCD_SIGNATURE) {
            eocd_offset = search_start + i;
            break;
        }
    }
    if (eocd_offset < 0) return false;

    // Parse EOCD
    fseek(fp_, eocd_offset, SEEK_SET);
    unsigned char eocd[22];
    if (fread(eocd, 1, 22, fp_) != 22) return false;

    uint16_t num_entries = util::read_u16_le(&eocd[10]);
    uint32_t cd_offset   = util::read_u32_le(&eocd[16]);

    // Parse Central Directory
    fseek(fp_, cd_offset, SEEK_SET);
    entries_.reserve(num_entries);

    for (int i = 0; i < num_entries; ++i) {
        unsigned char hdr[46];
        if (fread(hdr, 1, 46, fp_) != 46) return false;
        if (util::read_u32_le(hdr) != CD_SIGNATURE) return false;

        Entry e;
        e.method           = util::read_u16_le(&hdr[10]);
        e.compressed_size   = util::read_u32_le(&hdr[20]);
        e.uncompressed_size = util::read_u32_le(&hdr[24]);
        uint16_t name_len   = util::read_u16_le(&hdr[28]);
        uint16_t extra_len  = util::read_u16_le(&hdr[30]);
        uint16_t comment_len = util::read_u16_le(&hdr[32]);
        e.local_header_offset = util::read_u32_le(&hdr[42]);

        // Read filename
        std::vector<char> name_buf(name_len);
        if (fread(name_buf.data(), 1, name_len, fp_) != name_len) return false;
        e.name.assign(name_buf.data(), name_len);

        // Skip extra + comment
        fseek(fp_, extra_len + comment_len, SEEK_CUR);

        entries_.push_back(std::move(e));
    }

    return true;
}

bool ZipReader::read_local_data(const Entry& entry, std::vector<char>& out) const {
    // Read local file header to find data offset
    fseek(fp_, entry.local_header_offset, SEEK_SET);
    unsigned char lfh[30];
    if (fread(lfh, 1, 30, fp_) != 30) return false;
    if (util::read_u32_le(lfh) != LFH_SIGNATURE) return false;

    uint16_t name_len  = util::read_u16_le(&lfh[26]);
    uint16_t extra_len = util::read_u16_le(&lfh[28]);
    fseek(fp_, name_len + extra_len, SEEK_CUR);

    // Read compressed data
    std::vector<char> compressed(entry.compressed_size);
    if (entry.compressed_size > 0) {
        if (fread(compressed.data(), 1, entry.compressed_size, fp_) != entry.compressed_size)
            return false;
    }

    if (entry.method == 0) {
        // STORE
        out = std::move(compressed);
        return true;
    } else if (entry.method == 8) {
        // DEFLATE
        out.resize(entry.uncompressed_size);

        z_stream zs = {};
        zs.next_in  = reinterpret_cast<unsigned char*>(compressed.data());
        zs.avail_in = entry.compressed_size;
        zs.next_out = reinterpret_cast<unsigned char*>(out.data());
        zs.avail_out = entry.uncompressed_size;

        // -MAX_WBITS for raw deflate (no gzip/zlib header)
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
        int ret = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);

        return (ret == Z_STREAM_END);
    }

    return false; // unsupported method
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
    read_local_data(entry, out);
    return out;
}

bool ZipReader::extract_entry_to_file(const Entry& entry, const std::string& output_path) const {
    // For streaming: read local file header, then decompress in chunks
    fseek(fp_, entry.local_header_offset, SEEK_SET);
    unsigned char lfh[30];
    if (fread(lfh, 1, 30, fp_) != 30) return false;
    if (util::read_u32_le(lfh) != LFH_SIGNATURE) return false;

    uint16_t name_len  = util::read_u16_le(&lfh[26]);
    uint16_t extra_len = util::read_u16_le(&lfh[28]);
    fseek(fp_, name_len + extra_len, SEEK_CUR);

    FILE* out = fopen(output_path.c_str(), "wb");
    if (!out) return false;

    if (entry.method == 0) {
        // STORE: stream directly
        static constexpr size_t BUF_SIZE = 65536;
        std::vector<char> buf(BUF_SIZE);
        size_t remaining = entry.uncompressed_size;
        while (remaining > 0) {
            size_t chunk = std::min(remaining, BUF_SIZE);
            if (fread(buf.data(), 1, chunk, fp_) != chunk) {
                fclose(out);
                return false;
            }
            fwrite(buf.data(), 1, chunk, out);
            remaining -= chunk;
        }
    } else if (entry.method == 8) {
        // DEFLATE: streaming decompression in 64KB chunks
        static constexpr size_t BUF_SIZE = 65536;
        std::vector<unsigned char> in_buf(BUF_SIZE);
        std::vector<unsigned char> out_buf(BUF_SIZE);

        z_stream zs = {};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
            fclose(out);
            return false;
        }

        size_t remaining = entry.compressed_size;
        int ret = Z_OK;
        while (remaining > 0 && ret != Z_STREAM_END) {
            size_t chunk = std::min(remaining, BUF_SIZE);
            if (fread(in_buf.data(), 1, chunk, fp_) != chunk) {
                inflateEnd(&zs);
                fclose(out);
                return false;
            }
            remaining -= chunk;

            zs.next_in = in_buf.data();
            zs.avail_in = (uInt)chunk;

            do {
                zs.next_out = out_buf.data();
                zs.avail_out = (uInt)BUF_SIZE;
                ret = inflate(&zs, Z_NO_FLUSH);
                if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                    inflateEnd(&zs);
                    fclose(out);
                    return false;
                }
                size_t have = BUF_SIZE - zs.avail_out;
                fwrite(out_buf.data(), 1, have, out);
            } while (zs.avail_out == 0);
        }

        inflateEnd(&zs);
    } else {
        fclose(out);
        return false;
    }

    fclose(out);
    return true;
}

bool ZipReader::has_entry(const std::string& name) const {
    for (auto& e : entries_) {
        if (e.name == name) return true;
    }
    return false;
}

} // namespace jdoc
