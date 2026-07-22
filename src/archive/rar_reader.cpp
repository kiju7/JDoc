// rar_reader.cpp - Sequential RAR 4.x / 5.x reader (headers + store members)
// Clean-room from public format documentation (RARLAB technotes; libarchive's
// BSD-licensed readers as cross-check). Contains no RAR compression code.
// License: MIT

#include "archive/rar_reader.h"
#include "common/string_utils.h"
#include "common/binary_utils.h"

#include <zlib.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace jdoc {
namespace {

constexpr size_t kMaxNameLen = 4096;
constexpr uint64_t kMaxHeaderSize = 1 << 20;  // rar5 header incl. name + extra

// ── RAR 4.x block layout ────────────────────────────────
// Block: HEAD_CRC(2) HEAD_TYPE(1) HEAD_FLAGS(2) HEAD_SIZE(2) [type fields].
// If HEAD_FLAGS bit 15 is set, a 4-byte ADD_SIZE of trailing data follows
// the base fields (for file headers that field is PACK_SIZE).
constexpr uint8_t kV4Main    = 0x73;
constexpr uint8_t kV4File    = 0x74;
constexpr uint8_t kV4NewSub  = 0x7A;  // service data (CMT, RR, ...): file layout
constexpr uint8_t kV4End     = 0x7B;

constexpr uint16_t kV4MainPassword  = 0x0080;  // headers encrypted
constexpr uint16_t kV4FileSplitPrev = 0x0001;
constexpr uint16_t kV4FileSplitNext = 0x0002;
constexpr uint16_t kV4FileEncrypted = 0x0004;
constexpr uint16_t kV4FileLarge     = 0x0100;  // 64-bit sizes present
constexpr uint16_t kV4FileUnicode   = 0x0200;  // name carries encoded UTF-16
constexpr uint16_t kV4FileDirMask   = 0x00E0;  // all set = directory
constexpr uint16_t kV4LongBlock     = 0x8000;
constexpr uint8_t  kV4MethodStore   = 0x30;

// ── RAR 5.x block layout ────────────────────────────────
// Block: CRC32(4) size(vint) { type(vint) flags(vint) [extra_size(vint)]
// [data_size(vint)] type fields [extra area] } data...
// where size counts everything inside the braces.
constexpr uint64_t kV5Main    = 1;
constexpr uint64_t kV5File    = 2;
constexpr uint64_t kV5Service = 3;
constexpr uint64_t kV5Crypt   = 4;   // archive encryption: headers unreadable
constexpr uint64_t kV5End     = 5;

constexpr uint64_t kV5FlagExtra     = 0x0001;
constexpr uint64_t kV5FlagData      = 0x0002;
constexpr uint64_t kV5FlagSplitPrev = 0x0008;
constexpr uint64_t kV5FlagSplitNext = 0x0010;

constexpr uint64_t kV5FileDirectory   = 0x0001;
constexpr uint64_t kV5FileHasMtime    = 0x0002;
constexpr uint64_t kV5FileHasCrc      = 0x0004;
constexpr uint64_t kV5FileSizeUnknown = 0x0008;
constexpr uint64_t kV5ExtraCrypt      = 0x01;  // extra record: file encryption

bool get_vint(const unsigned char*& p, const unsigned char* end, uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 70; shift += 7) {
        if (p >= end || shift >= 64) return false;
        unsigned char b = *p++;
        v |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

// RAR4 unicode filename: raw = ANSI name, NUL, then a compact encoding of
// the UTF-16 name (first byte = default high byte; then 2-bit opcodes:
// 0 = literal low byte, 1 = low byte + default high, 2 = full 16-bit LE,
// 3 = run copied from the raw bytes, optionally with a correction).
// Documented in libarchive (read_unicode_filename) and the rarfile docs.
std::string decode_v4_unicode_name(const unsigned char* raw, size_t raw_len,
                                   size_t nul) {
    const unsigned char* enc = raw + nul + 1;
    size_t enc_len = raw_len - nul - 1;
    if (enc_len == 0) return std::string();

    std::string u16;  // UTF-16LE bytes
    auto push = [&u16](uint16_t cu) {
        u16.push_back(static_cast<char>(cu & 0xFF));
        u16.push_back(static_cast<char>(cu >> 8));
    };

    size_t ep = 0, dp = 0;
    uint8_t high = enc[ep++];
    uint8_t flags = 0;
    int flag_bits = 0;
    while (ep < enc_len && dp < kMaxNameLen) {
        if (flag_bits == 0) {
            flags = enc[ep++];
            flag_bits = 8;
            if (ep >= enc_len) break;
        }
        switch (flags >> 6) {
            case 0:
                push(enc[ep++]);
                dp++;
                break;
            case 1:
                push(static_cast<uint16_t>(enc[ep++] | (high << 8)));
                dp++;
                break;
            case 2:
                if (ep + 2 > enc_len) return std::string();
                push(util::read_u16_le(enc + ep));
                ep += 2;
                dp++;
                break;
            case 3: {
                unsigned len = enc[ep++];
                if (len & 0x80) {
                    if (ep >= enc_len) return std::string();
                    uint8_t corr = enc[ep++];
                    for (len = (len & 0x7F) + 2; len > 0 && dp < raw_len;
                         len--, dp++)
                        push(static_cast<uint16_t>(
                            ((raw[dp] + corr) & 0xFF) | (high << 8)));
                } else {
                    for (len += 2; len > 0 && dp < raw_len; len--, dp++)
                        push(raw[dp]);
                }
                break;
            }
        }
        flags = static_cast<uint8_t>(flags << 2);
        flag_bits -= 2;
    }
    return util::utf16le_to_utf8(u16.data(), u16.size());
}

std::string v4_name_to_utf8(std::string raw, bool unicode) {
    size_t nul = raw.find('\0');
    if (unicode && nul != std::string::npos) {
        std::string decoded = decode_v4_unicode_name(
            reinterpret_cast<const unsigned char*>(raw.data()), raw.size(), nul);
        if (!decoded.empty()) return decoded;
    }
    if (nul != std::string::npos) raw.resize(nul);
    return util::legacy_name_to_utf8(raw);
}

} // anonymous namespace

RarReader::RarReader(InputStream& src) : src_(src) {
    static const unsigned char kSig[6] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07};
    unsigned char sig[7];
    if (!read_full(sig, sizeof(sig)) || memcmp(sig, kSig, 6) != 0) return;

    if (sig[6] == 0x01) {  // rar5: 8-byte signature ends with 0x00
        unsigned char last;
        if (!read_full(&last, 1) || last != 0x00) return;
        version_ = 5;
        open_ = true;  // main/crypt header handled lazily in next_v5
        return;
    }
    if (sig[6] != 0x00) return;

    // rar4: the main archive header must follow the marker.
    unsigned char hdr[7];
    if (!read_full(hdr, sizeof(hdr)) || hdr[2] != kV4Main) return;
    uint16_t flags = util::read_u16_le(hdr + 3);
    uint16_t head_size = util::read_u16_le(hdr + 5);
    if (head_size < 7) return;
    headers_encrypted_ = (flags & kV4MainPassword) != 0;
    if (!headers_encrypted_ && !discard_stream(src_, head_size - 7)) return;
    version_ = 4;
    open_ = true;
}

bool RarReader::read_full(void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    while (len > 0) {
        size_t n = src_.read(p, len);
        if (n == 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

bool RarReader::next(Member& out) {
    if (!open_ || headers_encrypted_) return false;
    if (data_pending_ && !skip_data()) return false;
    return version_ == 4 ? next_v4(out) : next_v5(out);
}

bool RarReader::next_v4(Member& out) {
    for (;;) {
        unsigned char hdr[7];
        if (!read_full(hdr, sizeof(hdr))) return false;  // EOF: archives may
        uint8_t type = hdr[2];                           // lack an end block
        uint16_t flags = util::read_u16_le(hdr + 3);
        uint16_t head_size = util::read_u16_le(hdr + 5);
        if (type == kV4End || head_size < 7) return false;

        if (type != kV4File && type != kV4NewSub) {
            // Generic block: skip the header and any trailing data.
            uint64_t consumed = 7, add = 0;
            if (flags & kV4LongBlock) {
                unsigned char a[4];
                if (head_size < 11 || !read_full(a, 4)) return false;
                add = util::read_u32_le(a);
                consumed = 11;
            }
            if (!discard_stream(src_, head_size - consumed + add)) return false;
            continue;
        }

        // File / service header: PACK_SIZE(4) UNP_SIZE(4) HOST_OS(1)
        // FILE_CRC(4) FTIME(4) UNP_VER(1) METHOD(1) NAME_SIZE(2) ATTR(4)
        unsigned char f[25];
        if (!read_full(f, sizeof(f))) return false;
        uint64_t pack = util::read_u32_le(f);
        uint64_t unp = util::read_u32_le(f + 4);
        uint32_t crc = util::read_u32_le(f + 9);
        uint8_t method = f[18];
        uint16_t name_size = util::read_u16_le(f + 19);
        uint64_t consumed = 7 + 25;

        if (flags & kV4FileLarge) {
            unsigned char hi[8];
            if (!read_full(hi, sizeof(hi))) return false;
            pack |= static_cast<uint64_t>(util::read_u32_le(hi)) << 32;
            unp |= static_cast<uint64_t>(util::read_u32_le(hi + 4)) << 32;
            consumed += 8;
        }

        if (name_size == 0 || name_size > kMaxNameLen ||
            head_size < consumed + name_size)
            return false;
        std::string raw(name_size, '\0');
        if (!read_full(&raw[0], name_size)) return false;
        consumed += name_size;
        // Remaining header fields (salt, extended time, ...): skip via size.
        if (!discard_stream(src_, head_size - consumed)) return false;

        data_size_ = pack;
        if (type == kV4NewSub) {  // service data (comment, recovery record)
            if (!discard_stream(src_, data_size_)) return false;
            data_size_ = 0;
            continue;
        }

        cur_ = Member{};
        std::replace(raw.begin(), raw.end(), '\\', '/');
        cur_.name = v4_name_to_utf8(std::move(raw),
                                    (flags & kV4FileUnicode) != 0);
        cur_.compressed_size = pack;
        cur_.uncompressed_size = unp;
        cur_.crc32 = crc;
        cur_.has_crc = true;
        cur_.stored = method == kV4MethodStore;
        cur_.encrypted = (flags & kV4FileEncrypted) != 0;
        cur_.split = (flags & (kV4FileSplitPrev | kV4FileSplitNext)) != 0;
        cur_.directory = (flags & kV4FileDirMask) == kV4FileDirMask;
        data_pending_ = true;
        out = cur_;
        return true;
    }
}

bool RarReader::next_v5(Member& out) {
    for (;;) {
        unsigned char crc_buf[4];
        if (!read_full(crc_buf, sizeof(crc_buf))) return false;  // EOF

        // Header size vint comes from the stream byte by byte; its bytes
        // are included in the header CRC.
        unsigned char size_bytes[10];
        int size_len = 0;
        uint64_t head_size = 0;
        for (int shift = 0;; shift += 7) {
            if (size_len >= 10 || shift >= 64 ||
                !read_full(&size_bytes[size_len], 1))
                return false;
            unsigned char b = size_bytes[size_len++];
            head_size |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) break;
        }
        if (head_size == 0 || head_size > kMaxHeaderSize) return false;

        std::vector<unsigned char> buf(head_size);
        if (!read_full(buf.data(), buf.size())) return false;
        uLong crc = crc32(0, nullptr, 0);
        crc = crc32(crc, size_bytes, static_cast<uInt>(size_len));
        crc = crc32(crc, buf.data(), static_cast<uInt>(buf.size()));
        if (static_cast<uint32_t>(crc) != util::read_u32_le(crc_buf)) return false;

        const unsigned char* p = buf.data();
        const unsigned char* end = p + buf.size();
        uint64_t type, flags, extra_size = 0;
        data_size_ = 0;
        if (!get_vint(p, end, type) || !get_vint(p, end, flags)) return false;
        if ((flags & kV5FlagExtra) && !get_vint(p, end, extra_size)) return false;
        if ((flags & kV5FlagData) && !get_vint(p, end, data_size_)) return false;
        if (extra_size > buf.size()) return false;

        if (type == kV5End) return false;
        if (type == kV5Crypt) {
            headers_encrypted_ = true;
            return false;
        }
        if (type != kV5File && type != kV5Service) {
            if (!discard_stream(src_, data_size_)) return false;  // main, ...
            continue;
        }

        uint64_t file_flags, unp, attr, comp_info, host, name_len;
        if (!get_vint(p, end, file_flags) || !get_vint(p, end, unp) ||
            !get_vint(p, end, attr))
            return false;
        if (file_flags & kV5FileHasMtime) {
            if (end - p < 4) return false;
            p += 4;
        }
        uint32_t crc32_field = 0;
        if (file_flags & kV5FileHasCrc) {
            if (end - p < 4) return false;
            crc32_field = util::read_u32_le(p);
            p += 4;
        }
        if (!get_vint(p, end, comp_info) || !get_vint(p, end, host) ||
            !get_vint(p, end, name_len))
            return false;
        if (name_len == 0 || name_len > kMaxNameLen ||
            name_len > static_cast<uint64_t>(end - p))
            return false;
        std::string name(reinterpret_cast<const char*>(p), name_len);

        // Extra area (last extra_size header bytes): the presence of a
        // file-encryption record marks the member encrypted.
        bool encrypted = false;
        if (extra_size > 0) {
            const unsigned char* ep = end - extra_size;
            while (ep < end) {
                uint64_t rec_size, rec_type;
                const unsigned char* rp = ep;
                if (!get_vint(rp, end, rec_size) ||
                    rec_size > static_cast<uint64_t>(end - rp))
                    break;
                const unsigned char* rec_end = rp + rec_size;
                if (get_vint(rp, rec_end, rec_type) &&
                    rec_type == kV5ExtraCrypt)
                    encrypted = true;
                ep = rec_end;
            }
        }

        if (type == kV5Service) {  // comment, quick-open cache, ...
            if (!discard_stream(src_, data_size_)) return false;
            data_size_ = 0;
            continue;
        }

        cur_ = Member{};
        std::replace(name.begin(), name.end(), '\\', '/');
        cur_.name = std::move(name);  // rar5 names are UTF-8 already
        cur_.compressed_size = data_size_;
        cur_.uncompressed_size = (file_flags & kV5FileSizeUnknown) ? 0 : unp;
        cur_.crc32 = crc32_field;
        cur_.has_crc = (file_flags & kV5FileHasCrc) != 0;
        // Compression info: bits 0-5 version, bits 7-9 method (0 = store).
        cur_.stored = ((comp_info >> 7) & 0x07) == 0;
        cur_.encrypted = encrypted;
        cur_.split = (flags & (kV5FlagSplitPrev | kV5FlagSplitNext)) != 0;
        cur_.directory = (file_flags & kV5FileDirectory) != 0;
        data_pending_ = true;
        out = cur_;
        return true;
    }
}

bool RarReader::skip_data() {
    if (!data_pending_) return true;
    data_pending_ = false;
    return data_size_ == 0 || discard_stream(src_, data_size_);
}

bool RarReader::read_data(const CodecSink& sink, std::string* err) {
    if (!data_pending_) {
        if (err) *err = "no member data pending";
        return false;
    }
    data_pending_ = false;

    if (cur_.encrypted) {
        discard_stream(src_, data_size_);
        if (err) *err = "encrypted member unsupported";
        return false;
    }
    if (cur_.split) {
        discard_stream(src_, data_size_);
        if (err) *err = "split rar member unsupported";
        return false;
    }
    if (!cur_.stored) {
        discard_stream(src_, data_size_);
        if (err) *err = "rar compressed member unsupported";
        return false;
    }

    uLong crc = crc32(0, nullptr, 0);
    uint64_t produced = 0;
    auto checked_sink = [&](const char* p, size_t n) {
        crc = crc32(crc, reinterpret_cast<const Bytef*>(p),
                    static_cast<uInt>(n));
        produced += n;
        return sink(p, n);
    };
    if (!copy_stored_stream(src_, data_size_, checked_sink, err)) return false;

    if ((cur_.uncompressed_size != 0 && produced != cur_.uncompressed_size) ||
        (cur_.has_crc && static_cast<uint32_t>(crc) != cur_.crc32)) {
        if (err) *err = "member crc mismatch";
        return false;
    }
    return true;
}

} // namespace jdoc
