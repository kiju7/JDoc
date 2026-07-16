// alz_reader.cpp - Sequential ALZ reader
// License: MIT

#include "archive/alz_reader.h"
#include "common/string_utils.h"

#include <zlib.h>
#include <algorithm>
#include <cstring>

namespace jdoc {
namespace {

constexpr uint32_t kAlzMagic      = 0x015A4C41;  // "ALZ\x01"
constexpr uint32_t kLocalMagic    = 0x015A4C42;  // "BLZ\x01" local file header
constexpr uint32_t kEndMagic      = 0x025A4C43;  // "CLZ\x02" end of archive
constexpr uint32_t kCentralMagic  = 0x015A4C43;  // "CLZ\x01" central-directory-style record

constexpr uint16_t kFlagEncrypted = 0x0001;
constexpr size_t   kEncHeaderSize = 12;          // precedes data of encrypted members
constexpr size_t   kMaxNameLen    = 4096;

uint32_t get_u32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t get_u16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint64_t get_var_size(const unsigned char* p, int width) {
    uint64_t v = 0;
    for (int i = 0; i < width; i++)
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

} // anonymous namespace

AlzReader::AlzReader(InputStream& src) : src_(src) {
    unsigned char hdr[8];  // magic(4) + version(2) + header id(2)
    if (!read_full(hdr, sizeof(hdr))) return;
    open_ = get_u32(hdr) == kAlzMagic;
}

bool AlzReader::read_full(void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    while (len > 0) {
        size_t n = src_.read(p, len);
        if (n == 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

bool AlzReader::next(Member& out) {
    if (data_pending_ && !skip_data()) return false;

    unsigned char magic[4];
    if (!read_full(magic, 4)) return false;
    uint32_t sig = get_u32(magic);
    if (sig != kLocalMagic) return false;  // kEndMagic/kCentralMagic/garbage: stop
    (void)kEndMagic; (void)kCentralMagic;

    unsigned char fixed[9];  // name_len(2) attr(1) time(4) bit_flags(2)
    if (!read_full(fixed, sizeof(fixed))) return false;
    uint16_t name_len = get_u16(fixed);
    uint16_t flags = get_u16(fixed + 7);
    if (name_len == 0 || name_len > kMaxNameLen) return false;

    cur_ = Member{};
    cur_.encrypted = (flags & kFlagEncrypted) != 0;

    // The size/method fields exist only when the descriptor's high nibble
    // (size-field width) is nonzero — flag-only descriptors (e.g. just the
    // encrypted bit) carry no data fields.
    int width = (flags & 0xF0) >> 4;
    if (width != 0) {
        if (width != 1 && width != 2 && width != 4 && width != 8) return false;
        // method(2) crc(4) csize(width) usize(width)
        unsigned char info[6 + 16];
        if (!read_full(info, 6 + 2 * static_cast<size_t>(width))) return false;
        cur_.method = static_cast<uint8_t>(get_u16(info));
        cur_.crc32 = get_u32(info + 2);
        cur_.compressed_size = get_var_size(info + 6, width);
        cur_.uncompressed_size = get_var_size(info + 6 + width, width);
        cur_.has_data_fields = true;
    }

    std::string name(name_len, '\0');
    if (!read_full(&name[0], name_len)) return false;
    // ALZip writes Windows separators; CP949 names predate the UTF-8 era.
    std::replace(name.begin(), name.end(), '\\', '/');
    cur_.name = util::is_valid_utf8(name) ? name
                                          : util::cp949_string_to_utf8(name);

    data_pending_ = true;
    out = cur_;
    return true;
}

bool AlzReader::skip_data() {
    if (!data_pending_) return true;
    data_pending_ = false;
    uint64_t len = cur_.compressed_size +
                   (cur_.encrypted ? kEncHeaderSize : 0);
    return len == 0 || discard_stream(src_, len);
}

bool AlzReader::read_data(const CodecSink& sink, std::string* err) {
    if (!data_pending_) {
        if (err) *err = "no member data pending";
        return false;
    }
    data_pending_ = false;

    if (cur_.encrypted) {
        discard_stream(src_, cur_.compressed_size + kEncHeaderSize);
        if (err) *err = "encrypted member unsupported";
        return false;
    }

    // Verify the header CRC while streaming so corruption that still
    // inflates cleanly is not passed to a parser.
    uLong crc = crc32(0, nullptr, 0);
    uint64_t produced = 0;
    auto checked_sink = [&](const char* p, size_t n) {
        crc = crc32(crc, reinterpret_cast<const Bytef*>(p),
                    static_cast<uInt>(n));
        produced += n;
        return sink(p, n);
    };

    bool ok;
    switch (cur_.method) {
        case 0:  ok = copy_stored_stream(src_, cur_.compressed_size, checked_sink, err); break;
        case 1:  ok = bzip2_stream(src_, cur_.compressed_size, checked_sink, err); break;
        case 2:  ok = inflate_raw_stream(src_, cur_.compressed_size, checked_sink, err); break;
        default:
            discard_stream(src_, cur_.compressed_size);
            if (err) *err = "unknown alz compression method";
            return false;
    }
    if (!ok) return false;

    if (produced != cur_.uncompressed_size ||
        static_cast<uint32_t>(crc) != cur_.crc32) {
        if (err) *err = "member crc mismatch";
        return false;
    }
    return true;
}

} // namespace jdoc
