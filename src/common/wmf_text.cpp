// WMF text extraction — see wmf_text.h.
//
// WMF layout: an optional 22-byte Aldus placeable header, then an 18-byte
// standard header, then records. Each record is [uint32 rdSize (in 16-bit
// WORDs)][uint16 rdFunction][rdParm…]; record byte length = rdSize * 2, and
// rdParm occupies the bytes after the 6-byte record header.
// License: MIT

#include "common/wmf_text.h"
#include "common/string_utils.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace jdoc {
namespace {

// Aldus placeable metafile magic (little-endian D7 CD C6 9A).
constexpr uint32_t PLACEABLE_MAGIC = 0x9AC6CDD7;
constexpr size_t   PLACEABLE_SIZE  = 22;
constexpr size_t   WMF_HEADER_SIZE = 18;

constexpr uint16_t META_EOF        = 0x0000;
constexpr uint16_t META_TEXTOUT    = 0x0521;
constexpr uint16_t META_EXTTEXTOUT = 0x0A32;

// ExtTextOut option bits that put a clipping/opaque rectangle before the string.
constexpr uint16_t ETO_OPAQUE  = 0x0002;
constexpr uint16_t ETO_CLIPPED = 0x0004;

constexpr uint32_t MAX_RECORDS = 1u << 20;
constexpr size_t   MAX_OUTPUT  = 64u << 20;

inline uint16_t rd_u16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline int16_t  rd_i16(const uint8_t* p) { return int16_t(rd_u16(p)); }
inline uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
           uint32_t(p[3]) << 24;
}

struct Run { int32_t x = 0; int32_t y = 0; std::string text; };

// Decode an ANSI string of `count` bytes at parm+off, if in range.
void take_text(const uint8_t* parm, size_t parm_len, size_t off, size_t count,
               int32_t x, int32_t y, std::vector<Run>& out) {
    if (count == 0 || off > parm_len || count > parm_len - off) return;
    std::string t = util::plain_text_to_utf8(
        std::string(reinterpret_cast<const char*>(parm + off), count));
    if (!t.empty()) out.push_back({x, y, std::move(t)});
}

// DIB-carrying WMF records → byte offset of the DIB within rdParm.
struct DibRecord { uint16_t func; size_t dib_off; };
constexpr DibRecord DIB_RECORDS[] = {
    {0x0940, 16},   // META_DIBBITBLT      (ROP, ySrc,xSrc, h,w, yDst,xDst)
    {0x0B41, 20},   // META_DIBSTRETCHBLT  (ROP, sh,sw, ySrc,xSrc, dh,dw, yDst,xDst)
    {0x0F43, 22},   // META_STRETCHDIB     (ROP, usage, sh,sw, ySrc,xSrc, dh,dw, yDst,xDst)
    {0x0D33, 18},   // META_SETDIBTODEV    (usage, scans, start, yDib,xDib, h,w, yDst,xDst)
};

const DibRecord* dib_record(uint16_t func) {
    for (const auto& dr : DIB_RECORDS)
        if (dr.func == func) return &dr;
    return nullptr;
}

// Reassemble a contiguous DIB (BITMAPINFOHEADER + palette + pixels) into a BMP
// file. Returns empty if the DIB header is not plausible. char buffer so the
// result moves straight into ImageData::data.
std::vector<char> dib_to_bmp(const uint8_t* dib, size_t len) {
    if (len < 20) return {};
    uint32_t bi_size = rd_u32(dib);
    // Accept BITMAPCOREHEADER (12) and the BITMAPINFOHEADER family (>=40).
    if (bi_size != 12 && (bi_size < 40 || bi_size > 200)) return {};
    if (bi_size > len) return {};

    uint16_t bit_count;
    uint32_t clr_used = 0;
    int32_t w, h;
    if (bi_size == 12) {                      // BITMAPCOREHEADER
        w = int16_t(rd_u16(dib + 4));
        h = int16_t(rd_u16(dib + 6));
        bit_count = rd_u16(dib + 10);
    } else {                                  // BITMAPINFOHEADER+
        w = int32_t(rd_u32(dib + 4));
        h = int32_t(rd_u32(dib + 8));
        bit_count = rd_u16(dib + 14);
        if (bi_size >= 36) clr_used = rd_u32(dib + 32);
    }
    if (w <= 0 || h == 0 || w > 40000 || h < -40000 || h > 40000) return {};

    size_t entry = (bi_size == 12) ? 3 : 4;   // RGBTRIPLE vs RGBQUAD
    size_t palette = clr_used ? clr_used
                   : (bit_count <= 8 ? (size_t(1) << bit_count) : 0);
    size_t off_bits = 14 + bi_size + palette * entry;
    if (off_bits > 14 + len) off_bits = 14 + bi_size;   // fall back on overshoot

    uint32_t file_size = uint32_t(14 + len);
    std::vector<char> out(file_size);
    auto* o = reinterpret_cast<uint8_t*>(out.data());
    o[0] = 'B'; o[1] = 'M';
    o[2] = file_size & 0xFF; o[3] = (file_size >> 8) & 0xFF;
    o[4] = (file_size >> 16) & 0xFF; o[5] = (file_size >> 24) & 0xFF;
    o[10] = off_bits & 0xFF; o[11] = (off_bits >> 8) & 0xFF;
    o[12] = (off_bits >> 16) & 0xFF; o[13] = (off_bits >> 24) & 0xFF;
    std::memcpy(out.data() + 14, dib, len);
    return out;
}

} // namespace

MetafileContent wmf_extract(const uint8_t* data, size_t size,
                            bool want_text, bool want_images) {
    MetafileContent content;
    if (!data || size < 4) return content;

    size_t pos = 0;
    // Skip the Aldus placeable header if present.
    if (rd_u32(data) == PLACEABLE_MAGIC) {
        if (size < PLACEABLE_SIZE + WMF_HEADER_SIZE) return content;
        pos = PLACEABLE_SIZE;
    }
    // Standard header: validate mtType (1 or 2) and mtHeaderSize (9 words).
    if (pos + WMF_HEADER_SIZE > size) return content;
    uint16_t mtType = rd_u16(data + pos);
    if ((mtType != 1 && mtType != 2) || rd_u16(data + pos + 2) != 9) return content;
    pos += WMF_HEADER_SIZE;

    std::vector<Run> runs;
    uint32_t seen = 0;
    while (pos + 6 <= size && seen < MAX_RECORDS) {
        uint32_t rec_words = rd_u32(data + pos);
        uint16_t func = rd_u16(data + pos + 4);
        size_t rec_bytes = size_t(rec_words) * 2;
        if (rec_bytes < 6 || pos + rec_bytes > size) break;
        if (func == META_EOF) break;

        const uint8_t* parm = data + pos + 6;
        size_t parm_len = rec_bytes - 6;

        if (want_text && func == META_TEXTOUT) {
            // rdParm: Count(2), String(Count, WORD-padded), YStart(2), XStart(2).
            if (parm_len >= 2) {
                size_t count = rd_u16(parm);
                size_t padded = (count + 1) & ~size_t(1);
                int32_t x = 0, y = 0;
                if (2 + padded + 4 <= parm_len) {
                    y = rd_i16(parm + 2 + padded);
                    x = rd_i16(parm + 2 + padded + 2);
                }
                take_text(parm, parm_len, 2, count, x, y, runs);
            }
        } else if (want_text && func == META_EXTTEXTOUT) {
            // rdParm: Y(2), X(2), Count(2), fwOpts(2), [Rect(8) if opaque/clipped],
            // String(Count), then optional Dx array.
            if (parm_len >= 8) {
                int32_t y = rd_i16(parm);
                int32_t x = rd_i16(parm + 2);
                size_t count = rd_u16(parm + 4);
                uint16_t opts = rd_u16(parm + 6);
                size_t str_off = 8;
                if (opts & (ETO_OPAQUE | ETO_CLIPPED)) str_off += 8;
                take_text(parm, parm_len, str_off, count, x, y, runs);
            }
        } else if (want_images) {
            if (const DibRecord* dr = dib_record(func)) {
                if (dr->dib_off + 20 <= parm_len) {   // room for a DIB header
                    auto bmp = dib_to_bmp(parm + dr->dib_off, parm_len - dr->dib_off);
                    if (!bmp.empty()) content.images.push_back(std::move(bmp));
                }
            }
        }
        pos += rec_bytes;
        ++seen;
    }

    if (want_text && !runs.empty()) {
        std::stable_sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
            if (a.y != b.y) return a.y < b.y;
            return a.x < b.x;
        });
        int32_t prev_y = runs.front().y;
        bool first = true;
        for (auto& r : runs) {
            if (!first && r.y != prev_y) content.text += '\n';
            content.text += r.text;
            prev_y = r.y;
            first = false;
            if (content.text.size() > MAX_OUTPUT) break;
        }
    }
    return content;
}

std::string wmf_extract_text(const uint8_t* data, size_t size) {
    return wmf_extract(data, size, /*want_text=*/true, /*want_images=*/false).text;
}

std::vector<std::vector<char>> wmf_extract_bitmaps(const uint8_t* data,
                                                   size_t size) {
    return wmf_extract(data, size, /*want_text=*/false, /*want_images=*/true).images;
}

} // namespace jdoc
