// egg_reader.cpp - Sequential EGG reader
// License: MIT

#include "archive/egg_reader.h"
#include "common/string_utils.h"

#include <zlib.h>
#include <algorithm>
#include <cstring>

namespace jdoc {
namespace {

constexpr uint32_t kEggMagic       = 0x41474745;  // "EGGA"
constexpr uint32_t kEofArc         = 0x08E28222;  // ends header sections and the archive
constexpr uint32_t kFileMagic      = 0x0A8590E3;
constexpr uint32_t kFilenameMagic  = 0x0A8591AC;
constexpr uint32_t kBlockMagic     = 0x02B50C13;
constexpr uint32_t kEncryptMagic   = 0x08D1470F;
constexpr uint32_t kSolidMagic     = 0x24E5A060;
constexpr uint32_t kSplitMagic     = 0x24F5A262;
// Skipped generically via their flag+size body: comment 0x04C63672,
// dummy 0x07463307, win file info 0x2C86950B, posix file info 0x1EE922E5.

constexpr uint8_t kNameFlagEncrypt  = 0x04;
constexpr uint8_t kNameFlagAreaCode = 0x08;
constexpr size_t  kMaxNameLen = 4096;

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

    // Archive-level extra fields until EOFARC.
    for (;;) {
        unsigned char magic[4];
        if (!read_full(magic, 4)) return;
        uint32_t sig = get_u32(magic);
        if (sig == kEofArc) break;
        if (sig == kSolidMagic) solid_ = true;
        if (sig == kSplitMagic) split_ = true;
        if (!skip_extra_field(1 << 16)) return;
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

// Extension records share a "bit_flag(1) size(2) data(size)" body.
bool EggReader::skip_extra_field(uint64_t max_len) {
    unsigned char head[3];
    if (!read_full(head, 3)) return false;
    uint16_t size = get_u16(head + 1);
    if (size > max_len) return false;
    return size == 0 || discard_stream(src_, size);
}

bool EggReader::next(Member& out) {
    if (data_pending_ && !skip_data()) return false;

    unsigned char magic[4];
    if (!read_full(magic, 4)) return false;
    if (get_u32(magic) != kFileMagic) return false;  // EOFARC or garbage: stop

    unsigned char fh[12];  // file_id(4) file_length(8)
    if (!read_full(fh, sizeof(fh))) return false;

    cur_ = Member{};
    cur_.uncompressed_size = get_u64(fh + 4);

    // Header section: extension records until EOFARC. Some writers start
    // the first block without an end marker; the stream cannot rewind, so
    // remember that the block magic was already consumed.
    for (;;) {
        if (!read_full(magic, 4)) return false;
        uint32_t sig = get_u32(magic);
        if (sig == kEofArc) break;
        if (sig == kBlockMagic) {
            block_magic_consumed_ = true;
            break;
        }

        if (sig == kFilenameMagic) {
            unsigned char nh[3];
            if (!read_full(nh, 3)) return false;
            uint8_t flags = nh[0];
            uint16_t name_len = get_u16(nh + 1);
            if (name_len == 0 || name_len > kMaxNameLen) return false;
            if (flags & kNameFlagEncrypt) return false;  // encrypted names: stop
            if (flags & kNameFlagAreaCode) {
                unsigned char locale[2];
                if (!read_full(locale, 2)) return false;
            }
            std::string name(name_len, '\0');
            if (!read_full(&name[0], name_len)) return false;
            std::replace(name.begin(), name.end(), '\\', '/');
            cur_.name = util::sanitize_utf8(name);
        } else if (sig == kEncryptMagic) {
            cur_.encrypted = true;
            if (!skip_extra_field(1 << 16)) return false;
        } else {
            // win/posix info, comment, dummy, unknown — generic skip
            if (!skip_extra_field(1 << 16)) return false;
        }
    }

    data_pending_ = cur_.uncompressed_size > 0;
    out = cur_;
    return true;
}

bool EggReader::skip_data() {
    if (!data_pending_) return true;
    data_pending_ = false;
    return consume_blocks(nullptr, nullptr);
}

bool EggReader::read_data(const CodecSink& sink, std::string* err) {
    if (!data_pending_) {
        if (err) *err = "no member data pending";
        return false;
    }
    data_pending_ = false;

    if (cur_.encrypted) {
        consume_blocks(nullptr, nullptr);
        if (err) *err = "encrypted member unsupported";
        return false;
    }
    return consume_blocks(&sink, err);
}

bool EggReader::consume_blocks(const CodecSink* sink, std::string* err) {
    uint64_t remaining = cur_.uncompressed_size;

    while (remaining > 0) {
        if (block_magic_consumed_) {
            block_magic_consumed_ = false;
        } else {
            unsigned char magic[4];
            if (!read_full(magic, 4)) {
                if (err) *err = "truncated member data";
                return false;
            }
            if (get_u32(magic) != kBlockMagic) {
                if (err) *err = "corrupt egg block header";
                return false;
            }
        }
        // method(2) uncompressed(4) compressed(4) crc(4) EOFARC(4)
        unsigned char bh[18];
        if (!read_full(bh, sizeof(bh))) {
            if (err) *err = "truncated member data";
            return false;
        }
        uint8_t method = bh[0];
        uint64_t block_usize = get_u32(bh + 2);
        uint64_t block_csize = get_u32(bh + 6);
        uint32_t block_crc = get_u32(bh + 10);
        if (get_u32(bh + 14) != kEofArc) {
            if (err) *err = "corrupt egg block header";
            return false;
        }
        if (block_usize > remaining) {
            if (err) *err = "corrupt egg block (size overrun)";
            return false;
        }

        if (!sink) {
            if (!discard_stream(src_, block_csize)) {
                if (err) *err = "truncated member data";
                return false;
            }
            remaining -= block_usize;
            continue;
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

        if (produced != block_usize ||
            static_cast<uint32_t>(crc) != block_crc) {
            if (err) *err = "member crc mismatch";
            return false;
        }
        remaining -= block_usize;
    }
    return true;
}

} // namespace jdoc
