// String conversion utilities implementation
// License: MIT

#include "common/string_utils.h"

#include <cctype>

namespace jdoc { namespace util {

const uint16_t kCp1252Table[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, // 80-87
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F, // 88-8F
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 90-97
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178, // 98-9F
};

std::string utf16le_to_utf8(const char* data, size_t byte_len) {
    std::string out;
    out.reserve(byte_len);
    for (size_t i = 0; i + 1 < byte_len; i += 2) {
        uint16_t code = static_cast<uint8_t>(data[i])
                      | (static_cast<uint16_t>(static_cast<uint8_t>(data[i + 1])) << 8);
        if (code >= 0xD800 && code <= 0xDBFF && i + 3 < byte_len) {
            uint16_t low = static_cast<uint8_t>(data[i + 2])
                         | (static_cast<uint16_t>(static_cast<uint8_t>(data[i + 3])) << 8);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                uint32_t cp = 0x10000
                    + ((static_cast<uint32_t>(code - 0xD800) << 10) | (low - 0xDC00));
                append_utf8(out, cp);
                i += 2;
                continue;
            }
        }
        append_utf8(out, code);
    }
    return out;
}

void append_cp1252(std::string& out, uint8_t ch) {
    if (ch >= 0x80 && ch <= 0x9F) {
        append_utf8(out, kCp1252Table[ch - 0x80]);
    } else {
        append_utf8(out, ch);
    }
}

// ── CP949 (Korean) → Unicode ──────────────────────────────
//
// CP949 (= EUC-KR superset, Windows-949) maps double-byte sequences to
// Unicode. We use the KS X 1001 core mapping for the lead byte range
// 0xB0-0xC8 (Hangul syllables block) which covers the vast majority of
// Korean text. For other ranges we do a direct computation using the
// Johab/Wansung ordering that maps to the Unicode Hangul Syllables block
// U+AC00..U+D7A3. This avoids shipping a 170KB lookup table.

// KS X 1001 mapping tables generated from Python codecs cp949
#include "cp949_tables.inc"

static uint32_t cp949_pair_to_unicode(uint8_t lead, uint8_t trail) {
    // KS X 1001 Hangul syllables: lead 0xB0-0xC8, trail 0xA1-0xFE
    if (lead >= 0xB0 && lead <= 0xC8 && trail >= 0xA1 && trail <= 0xFE) {
        int idx = (lead - 0xB0) * 94 + (trail - 0xA1);
        if (idx < 2350) return KSX1001_HANGUL[idx];
    }

    // KS X 1001 symbols/Latin: lead 0xA1-0xAF, trail 0xA1-0xFE
    if (lead >= 0xA1 && lead <= 0xAF && trail >= 0xA1 && trail <= 0xFE) {
        int idx = (lead - 0xA1) * 94 + (trail - 0xA1);
        if (idx < 1410) return KSX1001_SYMBOLS[idx];
    }

    // KS X 1001 Hanja: lead 0xCA-0xFD, trail 0xA1-0xFE
    if (lead >= 0xCA && lead <= 0xFD && trail >= 0xA1 && trail <= 0xFE) {
        int idx = (lead - 0xCA) * 94 + (trail - 0xA1);
        if (idx < 4888) return KSX1001_HANJA[idx];
    }

    // UHC (Unified Hangul Code) extension: lead 0x81-0xC6
    // Maps to all 11172 Hangul syllables U+AC00..U+D7A3
    if (lead >= 0x81 && lead <= 0xC6) {
        int row = lead - 0x81;
        int col = -1;
        if (trail >= 0x41 && trail <= 0x5A)      col = trail - 0x41;
        else if (trail >= 0x61 && trail <= 0x7A)  col = 26 + (trail - 0x61);
        else if (trail >= 0x81 && trail <= 0xFE)  col = 52 + (trail - 0x81);
        if (col >= 0) {
            int idx = row * 178 + col;
            if (idx < 11172) return 0xAC00 + idx;
        }
    }

    return 0xFFFD;
}

void append_cp949(std::string& out, uint8_t lead, uint8_t trail) {
    append_utf8(out, cp949_pair_to_unicode(lead, trail));
}

// ── CP932 (Japanese / Shift-JIS) → Unicode ────────────────
//
// CP932 maps double-byte sequences to Unicode via the JIS X 0208 standard.
// We compute the JIS row/cell from the Shift-JIS encoding, then map to
// Unicode using the standard JIS-to-Unicode relationship.

static uint32_t cp932_pair_to_unicode(uint8_t lead, uint8_t trail) {
    // Convert Shift-JIS to JIS X 0208 row/cell
    int row, cell;

    if (trail >= 0x40 && trail <= 0xFC && trail != 0x7F) {
        int adjusted_trail = (trail >= 0x80) ? trail - 1 : trail;
        if (lead >= 0x81 && lead <= 0x9F) {
            row = (lead - 0x81) * 2 + 1;
        } else if (lead >= 0xE0 && lead <= 0xFC) {
            row = (lead - 0xE0) * 2 + 63;
        } else {
            return 0xFFFD;
        }
        if (adjusted_trail >= 0x9F) {
            row += 1;
            cell = adjusted_trail - 0x9F + 1;
        } else {
            cell = adjusted_trail - 0x40 + 1;
        }
    } else {
        return 0xFFFD;
    }

    // JIS row 1-8: symbols, Latin, Greek, Cyrillic, box drawing
    // JIS row 16-84: kanji (level 1 and 2)
    // We handle the most common mappings:

    // Row 1: symbols (selected common ones)
    if (row == 1) {
        if (cell == 1) return 0x3000;  // ideographic space
        if (cell == 2) return 0x3001;  // ideographic comma
        if (cell == 3) return 0x3002;  // ideographic period
        if (cell == 4) return 0xFF0C;  // fullwidth comma
        if (cell == 5) return 0xFF0E;  // fullwidth period
        if (cell == 6) return 0x30FB;  // katakana middle dot
        if (cell == 7) return 0xFF1A;  // fullwidth colon
        if (cell == 8) return 0xFF1B;  // fullwidth semicolon
    }

    // Row 4: Hiragana (0x3041-0x3093)
    if (row == 4 && cell >= 1 && cell <= 83) {
        return 0x3041 + cell - 1;
    }

    // Row 5: Katakana (0x30A1-0x30F6)
    if (row == 5 && cell >= 1 && cell <= 86) {
        return 0x30A1 + cell - 1;
    }

    // Row 3: full-width ASCII (0xFF01-0xFF5E)
    if (row == 3 && cell >= 16 && cell <= 94) {
        return 0xFF01 + cell - 16;
    }

    // Rows 16-84: CJK Unified Ideographs
    // The JIS-to-Unicode mapping for kanji is complex and non-linear.
    // For a production system, a lookup table (about 12KB) is needed.
    // We handle the most frequent kanji range (JIS level 1: rows 16-47).
    // This is a simplified mapping; for complete accuracy, use a full table.
    if (row >= 16 && row <= 84) {
        // Approximate: map to CJK Unified Ideographs starting at U+4E00
        int kanji_idx = (row - 16) * 94 + (cell - 1);
        if (kanji_idx >= 0 && kanji_idx < 6355) {
            // This is an approximation; real mapping is non-sequential
            return 0x4E00 + kanji_idx;
        }
    }

    return 0xFFFD;
}

void append_cp932(std::string& out, uint8_t lead, uint8_t trail) {
    append_utf8(out, cp932_pair_to_unicode(lead, trail));
}

// ── UTF-8 Validation ──────────────────────────────────────

uint32_t decode_utf8(const char* data, size_t len, size_t& pos) {
    if (pos >= len) return 0xFFFD;

    uint8_t b0 = static_cast<uint8_t>(data[pos]);

    // ASCII fast path
    if (b0 < 0x80) {
        pos++;
        return b0;
    }

    uint32_t cp;
    int expected;
    uint32_t min_cp; // minimum valid codepoint (overlong detection)

    if ((b0 & 0xE0) == 0xC0) {
        cp = b0 & 0x1F;
        expected = 1;
        min_cp = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        cp = b0 & 0x0F;
        expected = 2;
        min_cp = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        cp = b0 & 0x07;
        expected = 3;
        min_cp = 0x10000;
    } else {
        // Invalid lead byte (0x80-0xBF or 0xF8+)
        pos++;
        return 0xFFFD;
    }

    if (pos + 1 + expected > len) {
        pos++;
        return 0xFFFD;
    }

    for (int i = 0; i < expected; i++) {
        uint8_t cont = static_cast<uint8_t>(data[pos + 1 + i]);
        if ((cont & 0xC0) != 0x80) {
            pos++;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cont & 0x3F);
    }

    pos += 1 + expected;

    // Reject overlong sequences
    if (cp < min_cp) return 0xFFFD;

    // Reject surrogates and out-of-range codepoints
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0xFFFD;
    if (cp > 0x10FFFF) return 0xFFFD;

    return cp;
}

std::string sanitize_utf8(const char* data, size_t len) {
    std::string out;
    out.reserve(len);
    size_t pos = 0;
    while (pos < len) {
        uint32_t cp = decode_utf8(data, len, pos);
        append_utf8(out, cp);
    }
    return out;
}

bool is_valid_utf8(const char* data, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        if (static_cast<unsigned char>(data[pos]) < 0x80) { pos++; continue; }
        size_t before = pos;
        if (decode_utf8(data, len, pos) == 0xFFFD) return false;
        if (pos == before) return false;
    }
    return true;
}

bool is_valid_utf8(const std::string& s) {
    return is_valid_utf8(s.data(), s.size());
}

std::string cp949_string_to_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (size_t i = 0; i < s.size(); ) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            i++;
        } else if (is_cp949_lead(c) && i + 1 < s.size()) {
            append_cp949(out, c, static_cast<uint8_t>(s[i + 1]));
            i += 2;
        } else {
            append_utf8(out, 0xFFFD);
            i++;
        }
    }
    return out;
}

// ── MIME / mail codecs ──────────────────────────────────────

std::string decode_base64(const std::string& in) {
    static const int8_t kInv = -1;
    auto val = [](unsigned char c) -> int8_t {
        if (c >= 'A' && c <= 'Z') return static_cast<int8_t>(c - 'A');
        if (c >= 'a' && c <= 'z') return static_cast<int8_t>(c - 'a' + 26);
        if (c >= '0' && c <= '9') return static_cast<int8_t>(c - '0' + 52);
        if (c == '+') return 62;
        if (c == '/') return 63;
        return kInv;
    };

    std::string out;
    out.reserve(in.size() * 3 / 4 + 3);
    uint32_t buf = 0;
    int bits = 0;
    for (unsigned char c : in) {
        if (c == '=') break;             // padding — end of data
        int8_t v = val(c);
        if (v < 0) continue;             // skip whitespace / stray bytes
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

std::string decode_quoted_printable(const std::string& in, bool q_encoding) {
    auto hex = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };

    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (q_encoding && c == '_') {
            out.push_back(' ');           // RFC 2047 'Q': '_' is space
        } else if (c == '=') {
            if (i + 2 < in.size()) {
                int hi = hex(static_cast<unsigned char>(in[i + 1]));
                int lo = hex(static_cast<unsigned char>(in[i + 2]));
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            // "=\r\n" or "=\n" soft line break: drop the '=' and the newline
            if (i + 1 < in.size() && in[i + 1] == '\r' &&
                i + 2 < in.size() && in[i + 2] == '\n') {
                i += 2;
            } else if (i + 1 < in.size() && in[i + 1] == '\n') {
                i += 1;
            }
            // else: stray '=', drop it
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string charset_to_utf8(const std::string& bytes, const std::string& charset) {
    // Normalize the label: lower-case, strip '-'/'_'/' '.
    std::string k;
    k.reserve(charset.size());
    for (char c : charset) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u == '-' || u == '_' || u == ' ') continue;
        k.push_back(static_cast<char>(std::tolower(u)));
    }

    if (k.empty())
        return is_valid_utf8(bytes) ? bytes : cp949_string_to_utf8(bytes);
    if (k == "utf8" || k == "usascii" || k == "ascii")
        return sanitize_utf8(bytes);
    if (k == "euckr" || k == "ksc56011987" || k == "ksc5601" || k == "cp949" ||
        k == "uhc" || k == "ms949" || k == "windows949" || k == "korean")
        return cp949_string_to_utf8(bytes);
    if (k == "iso88591" || k == "latin1" || k == "l1") {
        std::string out;
        out.reserve(bytes.size() * 2);
        for (unsigned char c : bytes) append_utf8(out, c);
        return out;
    }
    // Unknown charset: keep valid UTF-8, otherwise assume CP949 (Korean corpus).
    return is_valid_utf8(bytes) ? bytes : cp949_string_to_utf8(bytes);
}

}} // namespace jdoc::util
