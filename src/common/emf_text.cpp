// EMF text extraction — see emf_text.h.
//
// Walks the record stream ([uint32 type][uint32 size][body…]) from EMR_HEADER
// (validated by the " EMF" signature) to EMR_EOF, decoding text-out records.
//
// The one subtlety worth stating: the offString / nChars fields inside a
// text-out record are byte offsets measured from the *start of the whole
// record* (including its 8-byte common header). We keep a pointer to the record
// start (`rec`) and index the string as `rec + offString`, so no ad-hoc ∓8
// juggling leaks into the field reads.
// License: MIT

#include "common/emf_text.h"
#include "common/wmf_text.h"
#include "common/string_utils.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace jdoc {
namespace {

// Record type IDs (MS-EMF §2.1.1). Only the ones we act on are named.
constexpr uint32_t EMR_HEADER       = 0x00000001;
constexpr uint32_t EMR_EOF          = 0x0000000E;
constexpr uint32_t EMR_EXTTEXTOUTA  = 0x00000053;
constexpr uint32_t EMR_EXTTEXTOUTW  = 0x00000054;
constexpr uint32_t EMR_POLYTEXTOUTA = 0x00000060;
constexpr uint32_t EMR_POLYTEXTOUTW = 0x00000061;
constexpr uint32_t EMR_SMALLTEXTOUT = 0x0000006C;

// " EMF" little-endian, at file offset 40 in the EMR_HEADER record.
constexpr uint32_t ENHMETA_SIGNATURE = 0x464D4520;

// ExtTextOutOptions bits we read (MS-EMF §2.1.11).
constexpr uint32_t ETO_NO_RECT     = 0x00000100;
constexpr uint32_t ETO_SMALL_CHARS = 0x00000200;

constexpr size_t RECORD_HEADER = 8;   // type(4) + size(4)

// Guards against a pathological / hostile metafile.
constexpr uint32_t MAX_RECORDS = 1u << 20;
constexpr size_t   MAX_OUTPUT  = 64u << 20;

inline uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
           uint32_t(p[3]) << 24;
}
inline int32_t rd_i32(const uint8_t* p) { return int32_t(rd_u32(p)); }

// One decoded text run, tagged with its reference point for reading-order sort.
struct Run {
    int32_t x = 0;
    int32_t y = 0;
    std::string text;
};

// Decode a text-out record whose common 8-byte header starts at `rec`
// (rec_size bytes, already bounds-checked against the buffer). Appends at most
// one Run. `field_base` is the byte offset (from rec) of the EMRTEXT/field block
// this record variant uses.
void decode_text_record(const uint8_t* rec, size_t rec_size, uint32_t type,
                        std::vector<Run>& out) {
    int32_t x = 0, y = 0;
    uint32_t chars = 0;
    size_t str_off = 0;   // byte offset from `rec` to the string
    bool ansi = false;

    auto in_bounds = [&](size_t off, size_t len) {
        return off <= rec_size && len <= rec_size - off;
    };

    switch (type) {
        case EMR_EXTTEXTOUTW:
        case EMR_EXTTEXTOUTA: {
            // rec+8: RECTL bounds, iGraphicsMode, exScale, eyScale (28 bytes),
            // then EMRTEXT { reference(x,y), Chars, offString, … } at rec+36.
            if (rec_size < 52) return;
            ansi  = (type == EMR_EXTTEXTOUTA);
            x     = rd_i32(rec + 36);
            y     = rd_i32(rec + 40);
            chars = rd_u32(rec + 44);
            str_off = rd_u32(rec + 48);   // offString, from record start
            break;
        }
        case EMR_POLYTEXTOUTW:
        case EMR_POLYTEXTOUTA: {
            // rec+8: bounds, iGraphicsMode, exScale, eyScale, cStrings (32),
            // then the first EMRTEXT at rec+40. We surface the first string.
            // Best-effort: for POLYTEXTOUT the string's offString is measured
            // from the EMRTEXT (rec+40), not the record start — the bounds
            // check below rejects a misread rather than reading out of range.
            if (rec_size < 60) return;
            ansi  = (type == EMR_POLYTEXTOUTA);
            x     = rd_i32(rec + 40);
            y     = rd_i32(rec + 44);
            chars = rd_u32(rec + 48);
            str_off = size_t(40) + rd_u32(rec + 52);
            break;
        }
        case EMR_SMALLTEXTOUT: {
            // rec+8: x, y, cChars, fuOptions, iGraphicsMode, exScale, eyScale;
            // an optional 16-byte Bounds rect precedes the string unless
            // ETO_NO_RECT is set.
            if (rec_size < 36) return;
            x     = rd_i32(rec + 8);
            y     = rd_i32(rec + 12);
            chars = rd_u32(rec + 16);
            uint32_t opts = rd_u32(rec + 20);
            ansi  = (opts & ETO_SMALL_CHARS) != 0;
            str_off = 36;
            if ((opts & ETO_NO_RECT) == 0) str_off += 16;
            break;
        }
        default:
            return;
    }

    if (chars == 0 || chars > (1u << 24)) return;   // sane upper bound
    size_t char_size = ansi ? 1 : 2;
    size_t str_bytes = size_t(chars) * char_size;
    if (!in_bounds(str_off, str_bytes)) return;

    const char* s = reinterpret_cast<const char*>(rec + str_off);
    std::string text = ansi
        ? util::plain_text_to_utf8(std::string(s, str_bytes))  // UTF-8 else CP949
        : util::utf16le_to_utf8(s, str_bytes);
    if (!text.empty())
        out.push_back({x, y, std::move(text)});
}

// Byte offsets (from the record start, incl. the 8-byte header) of the DIB
// locator fields inside each bitmap record, plus the minimum record size that
// makes the record well-formed. Mirrors MS-EMF §2.3.1 record layouts.
struct BitmapFields {
    uint32_t type;
    size_t off_bmi, cb_bmi, off_bits, cb_bits, min_size;
};
constexpr BitmapFields BITMAP_TABLE[] = {
    {0x00000051, 48, 52, 56, 60, 80},   // EMR_STRETCHDIBITS
    {0x00000050, 48, 52, 56, 60, 76},   // EMR_SETDIBITSTODEVICE
    {0x0000004C, 84, 88, 92, 96, 100},  // EMR_BITBLT
    {0x0000004D, 84, 88, 92, 96, 108},  // EMR_STRETCHBLT
    {0x0000004E, 84, 88, 92, 96, 128},  // EMR_MASKBLT
    {0x0000004F, 96, 100, 104, 108, 140}, // EMR_PLGBLT
    {0x00000072, 84, 88, 92, 96, 104},  // EMR_ALPHABLEND
    {0x00000074, 84, 88, 92, 96, 108},  // EMR_TRANSPARENTBLT
};

const BitmapFields* bitmap_fields(uint32_t type) {
    for (const auto& f : BITMAP_TABLE)
        if (f.type == type) return &f;
    return nullptr;
}

// Reassemble a DIB (header + pixels) into a BMP file: a 14-byte
// BITMAPFILEHEADER, the BITMAPINFOHEADER(+palette), then the pixels. The single
// memcpy of each half into the output buffer is the only copy of the pixel
// data — the source EMF buffer is read in place otherwise. char buffer so the
// result moves straight into ImageData::data.
std::vector<char> dib_to_bmp(const uint8_t* bmi, size_t cb_bmi,
                             const uint8_t* bits, size_t cb_bits) {
    uint32_t off_bits = 14 + uint32_t(cb_bmi);
    uint32_t file_size = off_bits + uint32_t(cb_bits);
    std::vector<char> out(file_size);
    auto* o = reinterpret_cast<uint8_t*>(out.data());
    o[0] = 'B'; o[1] = 'M';
    o[2] = file_size & 0xFF; o[3] = (file_size >> 8) & 0xFF;
    o[4] = (file_size >> 16) & 0xFF; o[5] = (file_size >> 24) & 0xFF;
    // reserved1/reserved2 stay zero
    o[10] = off_bits & 0xFF; o[11] = (off_bits >> 8) & 0xFF;
    o[12] = (off_bits >> 16) & 0xFF; o[13] = (off_bits >> 24) & 0xFF;
    std::memcpy(out.data() + 14, bmi, cb_bmi);
    std::memcpy(out.data() + off_bits, bits, cb_bits);
    return out;
}

// Pull the DIB out of a bitmap record and append its BMP to `out`.
void take_bitmap(const uint8_t* rec, uint32_t rsize, const BitmapFields& f,
                 std::vector<std::vector<char>>& out) {
    if (rsize < f.min_size) return;
    uint32_t off_bmi = rd_u32(rec + f.off_bmi);
    uint32_t cb_bmi = rd_u32(rec + f.cb_bmi);
    uint32_t off_bits = rd_u32(rec + f.off_bits);
    uint32_t cb_bits = rd_u32(rec + f.cb_bits);
    auto ok = [&](uint32_t o, uint32_t n) {   // range o..o+n inside the record
        return o <= rsize && n <= rsize - o;
    };
    if (cb_bmi >= 12 && cb_bits > 0 && ok(off_bmi, cb_bmi) && ok(off_bits, cb_bits))
        out.push_back(dib_to_bmp(rec + off_bmi, cb_bmi, rec + off_bits, cb_bits));
}

} // namespace

MetafileContent emf_extract(const uint8_t* data, size_t size,
                            bool want_text, bool want_images) {
    MetafileContent content;
    if (!data || size < RECORD_HEADER) return content;

    // First record must be EMR_HEADER with the " EMF" signature at offset 40.
    if (rd_u32(data) != EMR_HEADER || rd_u32(data + 4) < 44 ||
        rd_u32(data + 40) != ENHMETA_SIGNATURE)
        return content;

    std::vector<Run> runs;   // text runs, sorted into reading order below
    size_t off = 0;
    uint32_t seen = 0;
    while (off + RECORD_HEADER <= size && seen < MAX_RECORDS) {
        uint32_t type = rd_u32(data + off);
        uint32_t rsize = rd_u32(data + off + 4);
        // Size must cover the header and stay within the buffer, and be
        // 4-byte aligned per spec — a bad size means a corrupt stream.
        if (rsize < RECORD_HEADER || (rsize & 3) != 0 || off + rsize > size)
            break;
        if (type == EMR_EOF) break;

        if (want_text) {
            switch (type) {
                case EMR_EXTTEXTOUTW: case EMR_EXTTEXTOUTA:
                case EMR_POLYTEXTOUTW: case EMR_POLYTEXTOUTA:
                case EMR_SMALLTEXTOUT:
                    decode_text_record(data + off, rsize, type, runs);
                    break;
                default: break;
            }
        }
        if (want_images) {
            if (const BitmapFields* f = bitmap_fields(type))
                take_bitmap(data + off, rsize, *f, content.images);
        }
        off += rsize;
        ++seen;
    }

    if (want_text && !runs.empty()) {
        // Reading order: top-to-bottom, then left-to-right. stable_sort keeps
        // the record order among runs that share a reference point.
        std::stable_sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
            if (a.y != b.y) return a.y < b.y;
            return a.x < b.x;
        });
        int32_t prev_y = runs.front().y;
        bool first = true;
        for (auto& r : runs) {
            if (!first && r.y != prev_y) content.text += '\n';  // new baseline
            content.text += r.text;
            prev_y = r.y;
            first = false;
            if (content.text.size() > MAX_OUTPUT) break;
        }
    }
    return content;
}

std::string emf_extract_text(const uint8_t* data, size_t size) {
    return emf_extract(data, size, /*want_text=*/true, /*want_images=*/false).text;
}

std::vector<std::vector<char>> emf_extract_bitmaps(const uint8_t* data,
                                                   size_t size) {
    return emf_extract(data, size, /*want_text=*/false, /*want_images=*/true).images;
}

std::string metafile_extract_text(const char* format, const uint8_t* data,
                                  size_t size) {
    if (!format) return {};
    if (std::strcmp(format, "emf") == 0) return emf_extract_text(data, size);
    if (std::strcmp(format, "wmf") == 0) return wmf_extract_text(data, size);
    return {};
}

} // namespace jdoc
