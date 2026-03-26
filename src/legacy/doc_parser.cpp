// .doc (Word Binary) parser implementation.
// Supports Word 6/95 and Word 97-2003 formats.

#include "doc_parser.h"
#include "common/string_utils.h"
#include "common/binary_utils.h"
#include "common/file_utils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace jdoc {

// ── Smart quote and special character mapping ───────────────────

// Map CP1252 "smart" characters (0x80-0x9F) to readable Unicode.
// 0x91/0x92 → left/right single quote
// 0x93/0x94 → left/right double quote
// 0x96 → en dash, 0x97 → em dash, 0x85 → ellipsis
static uint32_t map_smart_char(uint8_t ch) {
    switch (ch) {
    case 0x85: return 0x2026;  // …
    case 0x91: return 0x2018;  // '
    case 0x92: return 0x2019;  // '
    case 0x93: return 0x201C;  // "
    case 0x94: return 0x201D;  // "
    case 0x96: return 0x2013;  // –
    case 0x97: return 0x2014;  // —
    case 0xA0: return 0x0020;  // non-breaking space → space
    default:   return 0;       // not a smart char
    }
}

// ── DocParser ───────────────────────────────────────────────────

DocParser::DocParser(OleReader& ole) : ole_(ole) {}

// ── Character processing (field markers, control chars) ─────────

bool DocParser::process_char(uint32_t ch, std::string& result) {
    // Field begin marker: start tracking nesting.
    if (ch == 0x13) {
        field_depth_++;
        field_show_result_ = false;
        return false;
    }
    // Field separator: switch to showing result text.
    if (ch == 0x14) {
        field_show_result_ = true;
        return false;
    }
    // Field end marker: close nesting.
    if (ch == 0x15) {
        if (field_depth_ > 0) field_depth_--;
        field_show_result_ = false;
        return false;
    }

    // Inside a field's instruction part (before separator): skip.
    if (field_depth_ > 0 && !field_show_result_) return false;

    // Line/paragraph breaks.
    if (ch == 0x0D || ch == 0x0A) {
        result.push_back('\n');
        return true;
    }
    // Cell mark (0x07): tab between cells, newline at row end.
    // Two consecutive 0x07 = last cell mark + row end mark → emit \t then \n.
    if (ch == 0x07) {
        if (!result.empty() && result.back() == '\t') {
            result.back() = '\n';  // Replace previous cell tab with row-end newline
        } else {
            result.push_back('\t');
        }
        return true;
    }
    // Tab.
    if (ch == 0x09) {
        result.push_back('\t');
        return true;
    }
    // Page break / section break → newline.
    if (ch == 0x0C) {
        result.push_back('\n');
        return true;
    }
    // Filter remaining control characters.
    if (ch < 0x20 && ch != '\t' && ch != '\n') return false;

    // Smart quotes and special CP1252 characters (in 0x80-0x9F range).
    if (ch >= 0x80 && ch <= 0x9F) {
        uint32_t mapped = map_smart_char(static_cast<uint8_t>(ch));
        if (mapped) {
            util::append_utf8(result, mapped);
            return true;
        }
    }

    // Append as Unicode codepoint.
    util::append_utf8(result, ch);
    return true;
}

// ── Main text extraction dispatcher ─────────────────────────────

std::string DocParser::extract_text() {
    std::vector<char> word_doc = ole_.read_stream("WordDocument");
    if (word_doc.size() < 0x20) return "";

    // Read FIB header: wIdent (magic) and nFib (version).
    uint16_t w_ident = util::read_u16_le(word_doc.data());
    uint16_t n_fib = util::read_u16_le(word_doc.data() + 2);

    // Validate magic: 0xA5EC for Word, 0xA5DC for glossary docs.
    if (w_ident != 0xA5EC && w_ident != 0xA5DC) return "";

    // Check encryption (bit 0 of flags at offset 0x0A, bit 8 = fEncrypted).
    if (word_doc.size() >= 0x0C) {
        uint16_t flags = util::read_u16_le(word_doc.data() + 0x0A);
        bool encrypted = (flags & 0x0100) != 0;
        if (encrypted) {
            throw std::runtime_error("Encrypted DOC files are not supported");
        }
    }

    // Read language ID for DBCS detection (offset 0x06).
    if (word_doc.size() >= 0x08) {
        lid_ = util::read_u16_le(word_doc.data() + 0x06);
    }

    // Determine Word version from nFib.
    if (n_fib >= 0x00C1) {        // Word 97+ (nFib = 0x00C1 for Word 97)
        version_ = WordVersion::WORD8_PLUS;
    } else if (n_fib >= 0x0067) { // Word 7 (Word 95)
        version_ = WordVersion::WORD7;
    } else if (n_fib >= 0x0065) { // Word 6
        version_ = WordVersion::WORD6;
    } else {
        return ""; // Too old or unrecognized
    }

    // Reset field state for extraction.
    field_depth_ = 0;
    field_show_result_ = false;

    // Select which table stream to use.
    uint16_t flags = util::read_u16_le(word_doc.data() + 0x0A);
    bool use_1table = (flags >> 9) & 1;
    std::string table_name = use_1table ? "1Table" : "0Table";
    std::vector<char> table_stream = ole_.read_stream(table_name);

    if (version_ == WordVersion::WORD6 || version_ == WordVersion::WORD7) {
        return extract_text_word6(word_doc, table_stream);
    }
    return extract_text_word8(word_doc, table_stream);
}

// ── Word 6/95 text extraction ───────────────────────────────────

std::string DocParser::extract_text_word6(const std::vector<char>& word_doc,
                                           const std::vector<char>& table_stream) {
    // Word 6/95 FIB: fcClx at offset 0x01A2 is the same as Word 97 in
    // many cases, but if that fails, try the Word 6 layout where the
    // CLX offset is at different positions in the FIB.

    // For Word 6/95, the document text is often stored contiguously
    // starting at FIB offset ccpText. Extract directly from WordDocument.

    if (word_doc.size() < 0x60) return "";

    // ccpText: character count of main text (offset 0x4C for Word 6/95)
    uint32_t ccp_text = 0;
    if (word_doc.size() >= 0x50) {
        ccp_text = util::read_u32_le(word_doc.data() + 0x4C);
    }
    if (ccp_text == 0 || ccp_text > 0x1000000) return "";

    // fcMin: file offset where text begins (offset 0x18)
    uint32_t fc_min = 0;
    if (word_doc.size() >= 0x1C) {
        fc_min = util::read_u32_le(word_doc.data() + 0x18);
    }
    if (fc_min == 0) fc_min = 0x600; // typical default

    // Word 6/95 text is single-byte (CP1252 or DBCS depending on language).
    std::string result;
    result.reserve(ccp_text);

    // Detect if this is a DBCS document.
    bool is_korean = (lid_ == 0x0412);
    bool is_japanese = (lid_ == 0x0411);

    size_t end = std::min(static_cast<size_t>(fc_min + ccp_text), word_doc.size());
    size_t pos = fc_min;

    while (pos < end) {
        uint8_t ch = static_cast<uint8_t>(word_doc[pos]);

        // DBCS: check for lead byte.
        if (is_korean && util::is_cp949_lead(ch) && pos + 1 < end) {
            uint8_t trail = static_cast<uint8_t>(word_doc[pos + 1]);
            result += util::cp949_to_utf8(ch, trail);
            pos += 2;
            continue;
        }
        if (is_japanese && util::is_cp932_lead(ch) && pos + 1 < end) {
            uint8_t trail = static_cast<uint8_t>(word_doc[pos + 1]);
            result += util::cp932_to_utf8(ch, trail);
            pos += 2;
            continue;
        }

        // Single-byte: process through common handler.
        process_char(ch, result);
        pos++;
    }

    // If that yielded nothing, try the piece table approach (same as Word 8).
    if (result.empty() && !table_stream.empty()) {
        return extract_text_word8(word_doc, table_stream);
    }

    return result;
}

// ── Word 97+ text extraction ────────────────────────────────────

std::string DocParser::extract_text_word8(const std::vector<char>& word_doc,
                                           const std::vector<char>& table_stream) {
    if (table_stream.empty()) return "";
    if (word_doc.size() < 0x60) return "";

    // Locate fcClx/lcbClx — the Clx (piece table) in the table stream.
    uint32_t fc_clx = 0, lcb_clx = 0;

    // Validate a candidate fcClx/lcbClx pair.
    auto valid_clx = [&](uint32_t fc, uint32_t lcb) -> bool {
        if (fc == 0 || lcb == 0) return false;
        if (static_cast<size_t>(fc) + lcb > table_stream.size()) return false;
        size_t base = fc;
        size_t p = 0;
        while (p < lcb && base + p < table_stream.size() &&
               static_cast<uint8_t>(table_stream[base + p]) == 0x01) {
            if (p + 3 > lcb || base + p + 3 > table_stream.size()) return false;
            uint16_t prc_len = util::read_u16_le(table_stream.data() + base + p + 1);
            p += 3 + prc_len;
        }
        return p < lcb && base + p < table_stream.size() &&
               static_cast<uint8_t>(table_stream[base + p]) == 0x02;
    };

    // Method 1: absolute FIB offset 0x01A2 (MS-DOC §2.5.6, Word 97+).
    if (0x01A2 + 8 <= word_doc.size()) {
        uint32_t fc  = util::read_u32_le(word_doc.data() + 0x01A2);
        uint32_t lcb = util::read_u32_le(word_doc.data() + 0x01A2 + 4);
        if (valid_clx(fc, lcb)) { fc_clx = fc; lcb_clx = lcb; }
    }

    // Method 2: navigate FIB variable-length sections.
    if (fc_clx == 0 && word_doc.size() >= 0x22) {
        uint16_t csw = util::read_u16_le(word_doc.data() + 0x20);
        size_t pos = 0x22 + csw * 2;
        if (pos + 2 <= word_doc.size()) {
            uint16_t cslw = util::read_u16_le(word_doc.data() + pos);
            pos += 2 + cslw * 4;
        }
        if (pos + 2 <= word_doc.size()) {
            uint16_t cb_rg = util::read_u16_le(word_doc.data() + pos);
            pos += 2;
            for (int idx : {65, 66}) {
                size_t off = pos + idx * 8;
                if (off + 8 > word_doc.size() || idx >= cb_rg) continue;
                uint32_t fc  = util::read_u32_le(word_doc.data() + off);
                uint32_t lcb = util::read_u32_le(word_doc.data() + off + 4);
                if (valid_clx(fc, lcb)) { fc_clx = fc; lcb_clx = lcb; break; }
            }
        }
    }

    if (fc_clx == 0 || lcb_clx == 0) return "";

    // Parse Clx structure.
    const char* clx = table_stream.data() + fc_clx;
    size_t clx_end = lcb_clx;
    size_t clx_pos = 0;

    // Skip Prc entries (type 0x01).
    while (clx_pos < clx_end && static_cast<uint8_t>(clx[clx_pos]) == 0x01) {
        if (clx_pos + 3 > clx_end) return "";
        uint16_t prc_len = util::read_u16_le(clx + clx_pos + 1);
        clx_pos += 3 + prc_len;
    }

    // Must be Pcdt (type 0x02).
    if (clx_pos >= clx_end || static_cast<uint8_t>(clx[clx_pos]) != 0x02) return "";
    clx_pos++;

    if (clx_pos + 4 > clx_end) return "";
    uint32_t lcb = util::read_u32_le(clx + clx_pos);
    clx_pos += 4;

    // PlcPcd: (n+1) CPs + n PCDs.
    if (lcb < 4) return "";
    uint32_t n = (lcb - 4) / 12;
    if (n == 0 || n > 0x31FFF) return ""; // safety limit

    const char* plcpcd = clx + clx_pos;
    size_t plcpcd_size = (n + 1) * 4 + n * 8;
    if (clx_pos + plcpcd_size > clx_end) return "";

    // Read CPs.
    std::vector<uint32_t> cps(n + 1);
    for (uint32_t i = 0; i <= n; ++i) {
        cps[i] = util::read_u32_le(plcpcd + i * 4);
    }

    const char* pcds = plcpcd + (n + 1) * 4;
    std::string result;

    bool is_korean = (lid_ == 0x0412);
    bool is_japanese = (lid_ == 0x0411);

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t char_count = cps[i + 1] - cps[i];
        if (char_count == 0 || char_count > 0x1000000) continue;

        // PCD: 2 bytes (unused) + 4 bytes (fc) + 2 bytes (prm).
        uint32_t fc_raw = util::read_u32_le(pcds + i * 8 + 2);
        bool compressed = (fc_raw & 0x40000000) != 0;
        uint32_t byte_offset = fc_raw & 0x3FFFFFFF;

        if (compressed) {
            // CP1252 or DBCS, 1 byte per CP position.
            byte_offset /= 2;
            if (static_cast<size_t>(byte_offset) + char_count > word_doc.size()) continue;

            size_t pos = byte_offset;
            size_t end = byte_offset + char_count;

            while (pos < end) {
                uint8_t ch = static_cast<uint8_t>(word_doc[pos]);

                // DBCS lead byte check.
                if (is_korean && util::is_cp949_lead(ch) && pos + 1 < end) {
                    uint8_t trail = static_cast<uint8_t>(word_doc[pos + 1]);
                    result += util::cp949_to_utf8(ch, trail);
                    pos += 2;
                    continue;
                }
                if (is_japanese && util::is_cp932_lead(ch) && pos + 1 < end) {
                    uint8_t trail = static_cast<uint8_t>(word_doc[pos + 1]);
                    result += util::cp932_to_utf8(ch, trail);
                    pos += 2;
                    continue;
                }

                process_char(ch, result);
                pos++;
            }
        } else {
            // UTF-16LE, 2 bytes per character.
            size_t byte_len = static_cast<size_t>(char_count) * 2;
            if (static_cast<size_t>(byte_offset) + byte_len > word_doc.size()) continue;
            const char* src = word_doc.data() + byte_offset;

            for (uint32_t j = 0; j < char_count; ++j) {
                uint16_t ch = util::read_u16_le(src + j * 2);

                // Handle surrogate pairs.
                if (ch >= 0xD800 && ch <= 0xDBFF && j + 1 < char_count) {
                    uint16_t low = util::read_u16_le(src + (j + 1) * 2);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        uint32_t cp = 0x10000
                            + ((static_cast<uint32_t>(ch - 0xD800) << 10)
                               | (low - 0xDC00));
                        process_char(cp, result);
                        j++;
                        continue;
                    }
                }

                process_char(ch, result);
            }
        }
    }

    return result;
}

// ── Text to Markdown ────────────────────────────────────────────

std::string DocParser::text_to_markdown(const std::string& raw_text) {
    std::vector<std::string> lines;
    std::istringstream iss(raw_text);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0'))
            line.pop_back();
        lines.push_back(line);
    }

    // Helper: check if a line contains tab-separated cells (table row)
    auto count_tabs = [](const std::string& s) {
        int n = 0;
        for (char c : s) if (c == '\t') n++;
        return n;
    };

    // Helper: convert tab-separated line to markdown table row
    auto tabs_to_row = [](const std::string& s) {
        std::string row = "|";
        std::string cell;
        for (char c : s) {
            if (c == '\t') {
                for (auto& ch : cell) { if (ch == '|') ch = '/'; if (ch == '\n') ch = ' '; }
                row += " " + cell + " |";
                cell.clear();
            } else {
                cell += c;
            }
        }
        for (auto& ch : cell) { if (ch == '|') ch = '/'; if (ch == '\n') ch = ' '; }
        row += " " + cell + " |";
        return row;
    };

    std::string md;
    md.reserve(raw_text.size() * 2);
    int consecutive_empty = 0;
    bool found_title = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& ln = lines[i];

        // Limit consecutive empty lines to 1
        if (ln.empty()) {
            consecutive_empty++;
            if (consecutive_empty <= 1) md.push_back('\n');
            continue;
        }
        consecutive_empty = 0;

        // Table detection: 1+ tab in consecutive lines
        int tabs = count_tabs(ln);
        if (tabs >= 1) {
            // Collect all consecutive tab-separated rows
            size_t tbl_start = i;
            int max_cols = tabs + 1;
            while (i < lines.size() && count_tabs(lines[i]) >= 1) {
                max_cols = std::max(max_cols, count_tabs(lines[i]) + 1);
                i++;
            }
            // Emit markdown table
            for (size_t r = tbl_start; r < i; r++) {
                md += tabs_to_row(lines[r]) + "\n";
                if (r == tbl_start) {
                    md += "|";
                    for (int c = 0; c < max_cols; c++) md += " --- |";
                    md += "\n";
                }
            }
            md += "\n";
            i--; // will be incremented by for loop
            continue;
        }

        // Bullet detection
        bool is_bullet = false;
        std::string content = ln;
        char first = ln[0];
        if (first == '-' || first == '*') {
            if (ln.size() > 1 && (ln[1] == ' ' || ln[1] == '\t')) {
                is_bullet = true;
                content = ln.substr(2);
            }
        } else if (ln.size() >= 3 &&
                   static_cast<uint8_t>(ln[0]) == 0xE2 &&
                   static_cast<uint8_t>(ln[1]) == 0x80 &&
                   static_cast<uint8_t>(ln[2]) == 0xA2) {
            is_bullet = true;
            content = ln.substr(3);
            while (!content.empty() && content[0] == ' ') content = content.substr(1);
        }
        if (is_bullet) {
            md += "- " + content + "\n";
            continue;
        }

        // Ordered list
        {
            size_t j = 0;
            while (j < ln.size() && ln[j] >= '0' && ln[j] <= '9') ++j;
            if (j > 0 && j < ln.size() && ln[j] == '.') {
                std::string prefix = ln.substr(0, j + 1);
                std::string rest = (j + 1 < ln.size()) ? ln.substr(j + 1) : "";
                while (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
                md += prefix + " " + rest + "\n";
                continue;
            }
        }

        // First non-empty content line → title (H1)
        if (!found_title && ln.size() < 120) {
            md += "# " + ln + "\n\n";
            found_title = true;
            continue;
        }

        // Heading heuristic: short line followed by longer text
        bool is_heading = false;
        if (ln.size() < 80) {
            for (size_t k = i + 1; k < lines.size() && k <= i + 3; ++k) {
                if (lines[k].empty()) continue;
                if (lines[k].size() > ln.size()) is_heading = true;
                break;
            }
        }

        if (is_heading) {
            md += "## " + ln + "\n\n";
        } else {
            md += ln + "\n";
        }
    }

    return md;
}

// ── Image detection helpers ─────────────────────────────────────

std::string DocParser::detect_image_format(const char* data, size_t len) {
    if (len < 8) return "";
    const uint8_t* d = reinterpret_cast<const uint8_t*>(data);

    if (d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "jpeg";
    if (d[0] == 0x89 && d[1] == 0x50 && d[2] == 0x4E && d[3] == 0x47 &&
        d[4] == 0x0D && d[5] == 0x0A && d[6] == 0x1A && d[7] == 0x0A) return "png";
    if (d[0] == 0x47 && d[1] == 0x49 && d[2] == 0x46 && d[3] == 0x38) return "gif";
    if (d[0] == 0x42 && d[1] == 0x4D) return "bmp";
    if (d[0] == 0x49 && d[1] == 0x49 && d[2] == 0x2A && d[3] == 0x00) return "tiff";
    if (d[0] == 0x4D && d[1] == 0x4D && d[2] == 0x00 && d[3] == 0x2A) return "tiff";
    if (len >= 4 && d[0] == 0xD7 && d[1] == 0xCD && d[2] == 0xC6 && d[3] == 0x9A) return "wmf";
    if (len >= 44 && d[0] == 0x01 && d[1] == 0x00 && d[2] == 0x00 && d[3] == 0x00) {
        if (d[40] == ' ' && d[41] == 'E' && d[42] == 'M' && d[43] == 'F') return "emf";
    }

    return "";
}

size_t DocParser::find_jpeg_end(const char* data, size_t len) {
    for (size_t i = 2; i + 1 < len; ++i) {
        if (static_cast<uint8_t>(data[i]) == 0xFF &&
            static_cast<uint8_t>(data[i + 1]) == 0xD9) {
            return i + 2;
        }
    }
    return len;
}

size_t DocParser::find_png_end(const char* data, size_t len) {
    const uint8_t iend_sig[4] = {0x49, 0x45, 0x4E, 0x44};
    for (size_t i = 8; i + 7 < len; ++i) {
        if (std::memcmp(data + i + 4, iend_sig, 4) == 0) {
            return i + 12;
        }
    }
    return len;
}

// ── Image extraction ────────────────────────────────────────────

std::vector<ImageData> DocParser::extract_images() {
    std::vector<ImageData> images;

    // Try "Data" stream first, then fall back to "WordDocument".
    std::vector<char> stream;
    if (ole_.has_stream("Data")) {
        stream = ole_.read_stream("Data");
    }
    if (stream.empty()) {
        stream = ole_.read_stream("WordDocument");
    }
    if (stream.empty()) return images;

    int img_idx = 0;
    size_t pos = 0;
    while (pos + 8 < stream.size()) {
        std::string fmt = detect_image_format(stream.data() + pos, stream.size() - pos);
        if (fmt.empty()) {
            ++pos;
            continue;
        }

        size_t img_start = pos;
        size_t img_end = stream.size();

        if (fmt == "jpeg") {
            img_end = img_start + find_jpeg_end(stream.data() + img_start, stream.size() - img_start);
        } else if (fmt == "png") {
            img_end = img_start + find_png_end(stream.data() + img_start, stream.size() - img_start);
        } else {
            size_t max_size = std::min(stream.size() - img_start, size_t(10 * 1024 * 1024));
            img_end = img_start + max_size;
        }

        size_t img_size = img_end - img_start;
        if (img_size > 16) {
            ImageData img;
            img.page_number = 1;
            img.name = "image_" + std::to_string(++img_idx);
            img.format = fmt;
            img.data.assign(stream.begin() + img_start, stream.begin() + img_end);
            images.push_back(std::move(img));
        }

        pos = img_end;
    }

    // Parse OfficeArt records in the "Data" stream for BLIP images.
    // Handles both standalone BLIPs and BLIPs inline within FBSE records.
    if (ole_.has_stream("Data")) {
        const auto& data = ole_.read_stream("Data");

        auto extract_blip = [&](size_t blip_pos, size_t blip_end) {
            if (blip_pos + 8 > blip_end) return;
            uint16_t bvi = util::read_u16_le(data.data() + blip_pos);
            uint16_t btype = util::read_u16_le(data.data() + blip_pos + 2);
            uint32_t blen = util::read_u32_le(data.data() + blip_pos + 4);
            uint16_t binst = bvi >> 4;

            std::string fmt;
            if (btype == 0xF01D) fmt = "jpeg";
            else if (btype == 0xF01E) fmt = "png";
            else if (btype == 0xF01A) fmt = "emf";
            else if (btype == 0xF01B) fmt = "wmf";
            else if (btype == 0xF01F) fmt = "bmp";
            else if (btype == 0xF029) fmt = "tiff";
            if (fmt.empty()) return;

            // BLIP header: 8 (record) + 16 (UID) + 1 (tag) = 25 bytes
            // Two-UID variant: 8 + 32 + 1 = 41 bytes
            size_t skip = 25;
            if (binst == 0x46B || binst == 0x6E1 || binst == 0x6E3 || binst == 0x6E5)
                skip = 41;

            size_t img_start = blip_pos + skip;
            size_t img_end = std::min(blip_pos + 8 + blen, blip_end);
            if (img_start >= img_end) return;
            size_t img_size = img_end - img_start;

            // Deduplicate by first 64 bytes
            for (const auto& existing : images) {
                if (existing.data.size() == img_size) {
                    if (std::memcmp(existing.data.data(), data.data() + img_start,
                                    std::min(size_t(64), img_size)) == 0)
                        return;
                }
            }

            ImageData img;
            img.page_number = 1;
            img.name = "image_" + std::to_string(++img_idx);
            img.format = fmt;
            img.data.assign(data.begin() + img_start, data.begin() + img_start + img_size);
            images.push_back(std::move(img));
        };

        pos = 0;
        while (pos + 8 < data.size()) {
            uint16_t rvi = util::read_u16_le(data.data() + pos);
            uint16_t rtype = util::read_u16_le(data.data() + pos + 2);
            uint32_t rlen = util::read_u32_le(data.data() + pos + 4);

            if (rtype >= 0xF000 && rtype <= 0xF200 && pos + 8 + rlen <= data.size()) {
                if (rtype == 0xF000 || rtype == 0xF001) {
                    // Container — descend into children
                    pos += 8;
                    continue;
                }
                if (rtype == 0xF007 && rlen >= 36) {
                    // FBSE — may contain inline BLIP after 36-byte header
                    uint8_t cbName = data[pos + 8 + 33];
                    size_t blip_start = pos + 8 + 36 + cbName;
                    size_t fbse_end = pos + 8 + rlen;
                    if (blip_start + 8 < fbse_end) {
                        extract_blip(blip_start, fbse_end);
                    }
                } else if (rtype >= 0xF01A && rtype <= 0xF029) {
                    // Standalone BLIP
                    extract_blip(pos, pos + 8 + rlen);
                }
                pos += 8 + rlen;
            } else {
                ++pos;
            }
        }
    }

    return images;
}

// ── Public API ──────────────────────────────────────────────────

std::string DocParser::to_markdown(const ConvertOptions& opts) {
    std::string raw = extract_text();
    std::string md = text_to_markdown(raw);

    if (opts.extract_images) {
        auto images = extract_images();
        for (auto& img : images) {
            std::string filename = img.name + "." + img.format;
            if (!opts.image_output_dir.empty() && !img.data.empty()) {
                util::ensure_dir(opts.image_output_dir);
                std::string path = opts.image_output_dir + "/" + filename;
                std::ofstream ofs(path, std::ios::binary);
                if (ofs) ofs.write(img.data.data(), img.data.size());
            }
            md += "\n![" + filename + "](" + filename + ")\n";
        }
    }

    return md;
}

std::vector<PageChunk> DocParser::to_chunks(const ConvertOptions& opts) {
    std::vector<PageChunk> chunks;

    PageChunk chunk;
    chunk.page_number = 1;

    std::string raw = extract_text();
    chunk.text = text_to_markdown(raw);

    if (opts.extract_images) {
        chunk.images = extract_images();
    }

    chunk.page_width = 612.0;
    chunk.page_height = 792.0;
    chunk.body_font_size = 12.0;

    chunks.push_back(std::move(chunk));
    return chunks;
}

} // namespace jdoc
