// egg_reader.cpp - Sequential EGG reader
// License: MIT

#include "archive/egg_reader.h"
#include "common/string_utils.h"

#include <zlib.h>
#include <algorithm>
#include <cstring>

namespace jdoc {
namespace {

// Signatures from the ESTsoft EGG spec v1.0, section 5.
constexpr uint32_t kEggMagic       = 0x41474745;  // "EGGA"
constexpr uint32_t kEofArc         = 0x08E28222;  // ends header sections and the archive
constexpr uint32_t kFileMagic      = 0x0A8590E3;
constexpr uint32_t kFilenameMagic  = 0x0A8591AC;
constexpr uint32_t kBlockMagic     = 0x02B50C13;
constexpr uint32_t kEncryptMagic   = 0x08D1470F;
constexpr uint32_t kSolidMagic     = 0x24E5A060;
constexpr uint32_t kSplitMagic     = 0x24F5A262;
constexpr uint32_t kGlobalEncrypt  = 0x08D144A8;
// Skipped generically: comment 0x04C63672, dummy 0x07463307,
// win file info 0x2C86950B, posix file info 0x1EE922E5, skip 0xFFFF0000.

// Extra-field / filename bit flags (spec: extra field & filename header).
constexpr uint8_t kFlagSize32       = 0x01;  // size field is 4 bytes, not 2
constexpr uint8_t kNameFlagEncrypt  = 0x04;
constexpr uint8_t kNameFlagAreaCode = 0x08;  // 2-byte locale before the name
constexpr uint8_t kNameFlagRelPath  = 0x10;  // 4-byte parent path id before the name

constexpr size_t kMaxNameLen = 4096;

uint32_t get_u32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t get_u16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint64_t get_u64(const unsigned char* p) {
    return static_cast<uint64_t>(get_u32(p)) |
           (static_cast<uint64_t>(get_u32(p + 4)) << 32);
}

} // anonymous namespace

EggReader::EggReader(InputStream& src) : src_(src) {
    unsigned char hdr[14];  // magic(4) version(2) header_id(4) reserved(4)
    if (!read_full(hdr, sizeof(hdr))) return;
    if (get_u32(hdr) != kEggMagic) return;

    // Extra Field 1: split / solid / global encryption, until EOFARC.
    for (;;) {
        unsigned char magic[4];
        if (!read_full(magic, 4)) return;
        uint32_t sig = get_u32(magic);
        if (sig == kEofArc) break;
        if (sig == kSolidMagic) solid_ = true;
        if (sig == kSplitMagic) split_ = true;
        if (sig == kGlobalEncrypt) global_encrypted_ = true;
        if (!skip_extra_field()) return;
    }
    open_ = true;
}

bool EggReader::read_full(void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    while (len > 0) {
        size_t n = src_.read(p, len);
        if (n == 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

// Generic extra field body: bit_flag(1) + size(2, or 4 when flag bit0 is
// set) + data(size). The spec sizes the field data with the size value.
bool EggReader::skip_extra_field() {
    unsigned char flag;
    if (!read_full(&flag, 1)) return false;
    uint32_t size;
    if (flag & kFlagSize32) {
        unsigned char sz[4];
        if (!read_full(sz, 4)) return false;
        size = get_u32(sz);
        if (size > (64u << 20)) return false;  // implausible field
    } else {
        unsigned char sz[2];
        if (!read_full(sz, 2)) return false;
        size = get_u16(sz);
    }
    return size == 0 || discard_stream(src_, size);
}

// Filename header body: flag(1) + size(2|4) + [locale(2) if area-code] +
// [parent path id(4) if relative-path] + name bytes. The size covers
// everything after itself, so the name length is derived by subtraction.
bool EggReader::parse_filename(Member& m) {
    unsigned char flag;
    if (!read_full(&flag, 1)) return false;
    uint32_t size;
    if (flag & kFlagSize32) {
        unsigned char sz[4];
        if (!read_full(sz, 4)) return false;
        size = get_u32(sz);
    } else {
        unsigned char sz[2];
        if (!read_full(sz, 2)) return false;
        size = get_u16(sz);
    }

    uint16_t locale = 0;
    if (flag & kNameFlagAreaCode) {
        unsigned char lb[2];
        if (size < 2 || !read_full(lb, 2)) return false;
        locale = get_u16(lb);
        size -= 2;
    }
    if (flag & kNameFlagRelPath) {
        unsigned char pid[4];
        if (size < 4 || !read_full(pid, 4)) return false;
        size -= 4;
    }
    if (size == 0 || size > kMaxNameLen) return false;

    std::string name(size, '\0');
    if (!read_full(&name[0], size)) return false;

    if (flag & kNameFlagEncrypt) {
        // Name bytes are encrypted; surface the member as encrypted with a
        // stable placeholder path instead of aborting the walk.
        m.encrypted = true;
        m.name = "(encrypted name)";
        return true;
    }

    std::replace(name.begin(), name.end(), '\\', '/');
    if (flag & kNameFlagAreaCode) {
        // Area-code names are in a legacy codepage; 949 (Korean) is the
        // common case. Fall back to UTF-8 validation for other locales.
        m.name = (locale == 949) ? util::cp949_string_to_utf8(name)
                                 : util::legacy_name_to_utf8(name);
    } else {
        m.name = util::legacy_name_to_utf8(name);
    }
    return true;
}

bool EggReader::next(Member& out) {
    if (data_pending_ && !skip_data()) return false;
    if (at_solid_data_) return false;

    // Find the next file header, tolerating stray extension records
    // (e.g. a global comment) between members.
    unsigned char magic[4];
    uint32_t sig;
    for (;;) {
        if (!read_full(magic, 4)) return false;
        sig = get_u32(magic);
        if (sig == kFileMagic) break;
        if (sig == kEofArc) return false;  // end of archive
        if (sig == kBlockMagic) {
            // Solid archives put the shared data after the last header.
            block_magic_consumed_ = true;
            at_solid_data_ = true;
            return false;
        }
        if (sig == kEggMagic || sig == kSolidMagic || sig == kSplitMagic)
            return false;  // unexpected primary record: lenient stop
        if (!skip_extra_field()) return false;  // comment/dummy/unknown
    }

    unsigned char fh[12];  // file_id(4) file_length(8)
    if (!read_full(fh, sizeof(fh))) return false;

    cur_ = Member{};
    cur_.uncompressed_size = get_u64(fh + 4);

    // Extra Field 2 until EOFARC. Some writers start the first block
    // without an end marker; the stream cannot rewind, so remember that
    // the block magic was already consumed.
    for (;;) {
        if (!read_full(magic, 4)) return false;
        sig = get_u32(magic);
        if (sig == kEofArc) break;
        if (sig == kBlockMagic) {
            block_magic_consumed_ = true;
            break;
        }

        if (sig == kFilenameMagic) {
            if (!parse_filename(cur_)) return false;
        } else if (sig == kEncryptMagic) {
            cur_.encrypted = true;
            if (!skip_extra_field()) return false;
        } else {
            // win/posix info, comment, dummy, unknown — generic skip
            if (!skip_extra_field()) return false;
        }
    }

    data_pending_ = !solid_ && cur_.uncompressed_size > 0;
    out = cur_;
    return true;
}

bool EggReader::skip_data() {
    if (!data_pending_) return true;
    data_pending_ = false;
    return consume_blocks(cur_.uncompressed_size, nullptr, nullptr);
}

bool EggReader::read_data(const CodecSink& sink, std::string* err) {
    if (!data_pending_) {
        if (err) *err = "no member data pending";
        return false;
    }
    data_pending_ = false;

    if (cur_.encrypted) {
        consume_blocks(cur_.uncompressed_size, nullptr, nullptr);
        if (err) *err = "encrypted member unsupported";
        return false;
    }
    return consume_blocks(cur_.uncompressed_size, &sink, err);
}

bool EggReader::read_solid_stream(const CodecSink& sink, std::string* err) {
    if (!at_solid_data_) {
        if (err) *err = "no solid data pending";
        return false;
    }
    at_solid_data_ = false;

    // Decode blocks until the archive end marker.
    for (;;) {
        if (block_magic_consumed_) {
            block_magic_consumed_ = false;
        } else {
            unsigned char magic[4];
            if (!read_full(magic, 4)) {
                if (err) *err = "truncated solid data";
                return false;
            }
            uint32_t sig = get_u32(magic);
            if (sig == kEofArc) return true;
            if (sig != kBlockMagic) {
                if (err) *err = "corrupt egg block header";
                return false;
            }
            block_magic_consumed_ = true;
        }
        uint64_t produced = 0;
        if (!decode_block(UINT64_MAX, produced, &sink, err)) return false;
    }
}

bool EggReader::consume_blocks(uint64_t remaining, const CodecSink* sink,
                               std::string* err) {
    while (remaining > 0) {
        if (!block_magic_consumed_) {
            unsigned char magic[4];
            if (!read_full(magic, 4)) {
                if (err) *err = "truncated member data";
                return false;
            }
            if (get_u32(magic) != kBlockMagic) {
                if (err) *err = "corrupt egg block header";
                return false;
            }
            block_magic_consumed_ = true;
        }
        uint64_t produced = 0;
        if (!decode_block(remaining, produced, sink, err)) return false;
        remaining -= produced;
    }
    return true;
}

bool EggReader::decode_block(uint64_t remaining, uint64_t& produced_out,
                             const CodecSink* sink, std::string* err) {
    block_magic_consumed_ = false;

    // Block header body: method(1) hint(1) uncompressed(4) compressed(4)
    // crc(4), then Extra Field 3 records until EOFARC, then the data.
    unsigned char bh[14];
    if (!read_full(bh, sizeof(bh))) {
        if (err) *err = "truncated member data";
        return false;
    }
    uint8_t method = bh[0];
    uint64_t block_usize = get_u32(bh + 2);
    uint64_t block_csize = get_u32(bh + 6);
    uint32_t block_crc = get_u32(bh + 10);

    for (;;) {
        unsigned char magic[4];
        if (!read_full(magic, 4)) {
            if (err) *err = "truncated member data";
            return false;
        }
        uint32_t sig = get_u32(magic);
        if (sig == kEofArc) break;
        if (!skip_extra_field()) {
            if (err) *err = "corrupt egg block header";
            return false;
        }
    }

    if (block_usize > remaining) {
        if (err) *err = "corrupt egg block (size overrun)";
        return false;
    }
    produced_out = block_usize;

    if (!sink) {
        if (!discard_stream(src_, block_csize)) {
            if (err) *err = "truncated member data";
            return false;
        }
        return true;
    }

    uLong crc = crc32(0, nullptr, 0);
    uint64_t produced = 0;
    auto checked_sink = [&](const char* p, size_t n) {
        crc = crc32(crc, reinterpret_cast<const Bytef*>(p),
                    static_cast<uInt>(n));
        produced += n;
        return (*sink)(p, n);
    };

    bool ok;
    switch (method) {
        case 0:  ok = copy_stored_stream(src_, block_csize, checked_sink, err); break;
        case 1:  ok = inflate_raw_stream(src_, block_csize, checked_sink, err); break;
        case 2:  ok = bzip2_stream(src_, block_csize, checked_sink, err); break;
        case 3:
            discard_stream(src_, block_csize);
            if (err) *err = "AZO-compressed member unsupported (proprietary codec)";
            return false;
        case 4:  ok = lzma_egg_stream(src_, block_csize, block_usize, checked_sink, err); break;
        default:
            discard_stream(src_, block_csize);
            if (err) *err = "unknown egg compression method";
            return false;
    }
    if (!ok) return false;

    if (produced != block_usize || static_cast<uint32_t>(crc) != block_crc) {
        if (err) *err = "member crc mismatch";
        return false;
    }
    return true;
}

} // namespace jdoc
