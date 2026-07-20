// hwp3_parser.cpp - HWP 3.x (Hangul Word Processor legacy binary) parser
// File layout: 30-byte signature, 128-byte document info, 1008-byte summary,
// optional info block, then the body (raw-deflate-compressed when the doc
// info compression flag is set): 7 font-name lists, style table, paragraph
// list. Paragraph text is a stream of 16-bit words: values < 32 are inline
// control codes with code-specific payloads, everything else is a 2-byte
// johab/KS character code.
// Reference: HWP 3.0 file format spec (Hancom); framing cross-checked
// against the LibreOffice hwpfilter implementation and real documents.
// License: MIT

#include "legacy/hwp3_parser.h"
#include "common/binary_utils.h"
#include "common/file_utils.h"
#include "common/string_utils.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace jdoc {

namespace {

constexpr size_t kSignatureLen = 30;
constexpr size_t kDocInfoLen = 128;
constexpr size_t kSummaryLen = 1008;   // 9 fields x 112 bytes, 2-byte chars
constexpr size_t kFontNameLen = 40;
constexpr size_t kCharShapeLen = 31;
constexpr size_t kParaShapeLen = 187;
constexpr size_t kLineInfoLen = 14;
constexpr size_t kStyleNameLen = 20;
constexpr size_t kCellLen = 27;
constexpr int kMaxParaListDepth = 16;
constexpr size_t kMaxBodySize = 256u << 20;  // inflate guard (256 MiB)

[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string("Corrupt HWP 3.x file: ") + what);
}

// ── Bounds-checked little-endian reader ─────────────────────

struct Reader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;

    void need(size_t n) const {
        if (n > size - pos) fail("unexpected end of stream");
    }
    bool eof() const { return pos >= size; }
    uint8_t u8() {
        need(1);
        return data[pos++];
    }
    uint16_t u16() {
        need(2);
        uint16_t v = util::read_u16_le(data + pos);
        pos += 2;
        return v;
    }
    uint32_t u32() {
        need(4);
        uint32_t v = util::read_u32_le(data + pos);
        pos += 4;
        return v;
    }
    const uint8_t* block(size_t n) {
        need(n);
        const uint8_t* p = data + pos;
        pos += n;
        return p;
    }
    void skip(size_t n) {
        need(n);
        pos += n;
    }
};

// ── Character code conversion ───────────────────────────────
// HWP 3.x stores text as 16-bit codes: ASCII below 0x80, johab-composed
// hangul with the MSB set (5-bit cho/jung/jong indexes), and KS X 1001
// symbol/hanja codes in dedicated ranges.

// Johab 5-bit jungseong code -> vowel index (0-20), -1 = invalid/fill.
constexpr int8_t kJungIndex[32] = {
    -1, -1, -1,  0,  1,  2,  3,  4, -1, -1,  5,  6,  7,  8,  9, 10,
    -1, -1, 11, 12, 13, 14, 15, 16, -1, -1, 17, 18, 19, 20, -1, -1,
};

// Johab 5-bit jongseong code -> final-consonant index (0-27), -1 = invalid.
constexpr int8_t kJongIndex[32] = {
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, -1, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, -1, -1,
};

// Compatibility jamo (U+3131..) for isolated cho/jung with fill parts.
constexpr uint16_t kChoJamo[19] = {
    0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143,
    0x3145, 0x3146, 0x3147, 0x3148, 0x3149, 0x314A, 0x314B, 0x314C, 0x314D,
    0x314E,
};
constexpr uint16_t kJungJamo[21] = {
    0x314F, 0x3150, 0x3151, 0x3152, 0x3153, 0x3154, 0x3155, 0x3156, 0x3157,
    0x3158, 0x3159, 0x315A, 0x315B, 0x315C, 0x315D, 0x315E, 0x315F, 0x3160,
    0x3161, 0x3162, 0x3163,
};

// Decode a johab hangul word (MSB set) to Unicode.
uint32_t johab_to_unicode(uint16_t ch) {
    int cho = (ch >> 10) & 0x1F;
    int jung = (ch >> 5) & 0x1F;
    int jong = ch & 0x1F;

    int c = cho - 2;                              // 2..20 -> 0..18
    int j = kJungIndex[jung];
    int t = kJongIndex[jong];

    if (c >= 0 && c <= 18 && j >= 0 && t >= 0)
        return 0xAC00 + (uint32_t(c) * 21 + j) * 28 + t;

    // Isolated jamo (fill parts): map what is present to compatibility jamo.
    if (c >= 0 && c <= 18 && j < 0 && jong <= 1)
        return kChoJamo[c];
    if ((cho <= 1) && j >= 0 && jong <= 1)
        return kJungJamo[j];
    return 0xFFFD;
}

// Append a KS X 1001 (EUC-KR) row/cell pair via the shared CP949 tables.
void append_euc(std::string& out, int lead, int trail) {
    out += util::cp949_to_utf8(uint8_t(lead), uint8_t(trail));
}

// Convert one 16-bit HWP character code to UTF-8, appended to out.
// Non-hangul 2-byte codes pack a KS X 1001 row/cell in code-area-specific
// ways; they are routed through the shared CP949 tables.
void append_hwp_char(std::string& out, uint16_t ch) {
    if (ch < 0x80) {
        if (ch >= 0x20) out.push_back(static_cast<char>(ch));
        return;                                    // drop stray controls
    }
    if (ch == 0x81 || ch == 0x82) {                // curly double quotes
        out.push_back('"');
        return;
    }
    if (ch == 0x83 || ch == 0x84) {                // curly single quotes
        out.push_back('\'');
        return;
    }
    if (ch & 0x8000) {                             // johab hangul
        util::append_utf8(out, johab_to_unicode(ch));
        return;
    }
    if (ch & 0x4000) {                             // hanja area
        uint16_t idx = ch - 0x4000;
        if (idx < 52 * 94)
            append_euc(out, idx / 94 + 0xCA, idx % 94 + 0xA1);
        else
            util::append_utf8(out, 0xFFFD);
        return;
    }
    if ((ch >> 8) == 0x1F) {                       // kana area -> KS AA/AB
        int low = ch & 0xFF;
        append_euc(out, low < 0x60 ? 0xAA : 0xAB, low % 0x60 + 0xA0);
        return;
    }
    if (ch >= 0x3013 && ch < 0x3013 + 77) {        // line-drawing glyphs
        static const uint8_t kDir[11] = {10, 11, 9, 14, 15, 13, 6, 7, 5, 3, 12};
        uint8_t dir = kDir[(ch - 0x3013) % 11];
        out.push_back(dir == 3 ? '-' : dir == 12 ? '|' : '+');
        return;
    }
    if (ch >= 0x2F00 && ch < 0x2F70 && (ch & 0x0F) < 9) {  // bullet area
        uint16_t base = ch < 0x2F10 ? 0xA1E0 : ch < 0x2F20 ? 0xA1DB
                      : ch < 0x2F30 ? 0xA1DE : ch < 0x2F40 ? 0xA1E2
                      : ch < 0x2F50 ? 0xA1E4 : ch < 0x2F60 ? 0xA2B7 : 0xA2B9;
        if ((ch & 0x0F) >= 6) base++;
        append_euc(out, base >> 8, base & 0xFF);
        return;
    }
    if (ch >= 0x3400 && ch < 0x37C0) {             // symbol area
        uint16_t t = ch - 0x3400;
        int row = t / 0x60 + 0xA1;
        if (row == 0xAA) row = 0xAC;               // kana rows live at 0x1Fxx
        append_euc(out, row, t % 0x60 + 0xA0);
        return;
    }
    if (ch >= 0x37C0 && ch < 0x37C6) {             // vendor logo glyphs
        static const uint16_t kLogo[6] = {0xD55C, 0xAE00, 0xACFC,
                                          0xCEF4, 0xD4E8, 0xD130};
        util::append_utf8(out, kLogo[ch - 0x37C0]);
        return;
    }
    util::append_utf8(out, 0xFFFD);
}

// ── Inline control codes ────────────────────────────────────

enum Hwp3Ctrl : uint16_t {
    CTRL_FIELD = 5,          // click-here field
    CTRL_BOOKMARK = 6,
    CTRL_DATE_FORMAT = 7,
    CTRL_DATE_CODE = 8,
    CTRL_TAB = 9,
    CTRL_TEXT_BOX = 10,      // text box / table / equation container
    CTRL_PICTURE = 11,
    CTRL_END_PARA = 13,
    CTRL_LINE = 14,          // horizontal line object
    CTRL_HIDDEN = 15,
    CTRL_HEADER_FOOTER = 16,
    CTRL_FOOTNOTE = 17,
    CTRL_AUTO_NUM = 18,
    CTRL_NEW_NUM = 19,
    CTRL_SHOW_PAGE_NUM = 20,
    CTRL_PAGE_NUM_CTRL = 21,
    CTRL_MAIL_MERGE = 22,
    CTRL_COMPOSE = 23,       // overlaid characters
    CTRL_HYPHEN = 24,
    CTRL_TOC_MARK = 25,
    CTRL_INDEX_MARK = 26,
    CTRL_OUTLINE = 28,
    CTRL_KEEP_SPACE = 30,
    CTRL_FIXED_SPACE = 31,
};

// Words each control code occupies in the paragraph character stream
// (charged against the paragraph character count, code word included).
constexpr uint8_t kCtrlWords[32] = {
    1, 4, 4, 4, 4, 4, 4, 42,   //  0.. 7 (7 = date format)
    48, 4, 4, 4, 4, 1, 4, 4,   //  8..15 (8 = date code, 13 = end para)
    4, 4, 4, 4, 4, 4, 12, 5,   // 16..23 (22 = mail merge, 23 = compose)
    3, 3, 123, 4, 32, 4, 2, 2, // 24..31 (26 = index mark, 28 = outline)
};

// Control codes whose payload is a generic length-prefixed skip block
// (reserved codes, column definition, cross reference).
inline bool is_skip_block(uint16_t w) {
    return w <= 4 || w == 12 || w == 27 || w == 29;
}

// ── Table grid assembly ─────────────────────────────────────

struct CellText {
    int x = 0;               // column position (HWP units)
    int y = 0;               // row position
    std::string text;
};

// Render collected cells as a markdown table, grouping rows by y and
// ordering columns by x.
std::string render_table(std::vector<CellText>& cells) {
    if (cells.empty()) return "";

    std::stable_sort(cells.begin(), cells.end(),
                     [](const CellText& a, const CellText& b) {
                         if (a.y != b.y) return a.y < b.y;
                         return a.x < b.x;
                     });

    std::vector<std::vector<std::string>> rows;
    int last_y = cells.front().y - 1;
    for (auto& c : cells) {
        if (rows.empty() || c.y != last_y) {
            rows.emplace_back();
            last_y = c.y;
        }
        std::string content = util::escape_cell(util::trim(c.text));
        rows.back().push_back(std::move(content));
    }

    size_t cols = 0;
    for (auto& r : rows) cols = std::max(cols, r.size());
    if (cols == 0) return "";

    std::string md;
    for (size_t r = 0; r < rows.size(); r++) {
        md += "|";
        for (size_t c = 0; c < cols; c++) {
            md += " ";
            if (c < rows[r].size()) md += rows[r][c];
            md += " |";
        }
        md += "\n";
        if (r == 0) {
            md += "|";
            for (size_t c = 0; c < cols; c++) md += " --- |";
            md += "\n";
        }
    }
    md += "\n";
    return md;
}

// ── Document parser ─────────────────────────────────────────

class Hwp3Doc {
public:
    Hwp3Doc(const uint8_t* data, size_t size) : file_{data, size, 0} {}

    // Parse the document; the extracted markdown is available via text().
    void parse() {
        parse_header();
        skip_font_and_styles();
        parse_para_list(0);
        flush_paragraph();
    }

    std::string& text() { return out_; }

private:
    Reader file_;                    // whole file
    std::vector<uint8_t> inflated_;  // backing store for compressed bodies
    Reader body_;                    // body stream (fonts/styles/paragraphs)
    std::string out_;                // markdown output
    std::string para_;               // current paragraph accumulator

    // ── Header / body setup ─────────────────────────────

    void parse_header() {
        const uint8_t* sig = file_.block(kSignatureLen);
        if (memcmp(sig, "HWP Document File V3.0", 22) != 0) {
            // V2.x and other variants use a different in-body layout.
            fail("unsupported HWP legacy version (only V3.0x is supported)");
        }

        const uint8_t* info = file_.block(kDocInfoLen);
        if (util::read_u16_le(info + 0x60) != 0)
            fail("document is password-protected");

        uint8_t compressed = info[0x7C];
        uint16_t info_block_len = util::read_u16_le(info + 0x7E);

        file_.skip(kSummaryLen);
        file_.skip(info_block_len);

        if (compressed) {
            inflate_body(file_.data + file_.pos, file_.size - file_.pos);
            body_ = Reader{inflated_.data(), inflated_.size(), 0};
        } else {
            body_ = Reader{file_.data + file_.pos, file_.size - file_.pos, 0};
        }
    }

    // Inflate the raw-deflate compressed body.
    void inflate_body(const uint8_t* data, size_t size) {
        z_stream strm = {};
        if (inflateInit2(&strm, -MAX_WBITS) != Z_OK)
            fail("inflate init failed");

        inflated_.resize(std::min(std::max<size_t>(size * 4, 4096),
                                  kMaxBodySize));
        strm.next_in = const_cast<uint8_t*>(data);
        strm.avail_in = static_cast<uInt>(size);

        int ret = Z_OK;
        while (ret != Z_STREAM_END) {
            if (strm.total_out >= inflated_.size()) {
                if (inflated_.size() >= kMaxBodySize) {
                    inflateEnd(&strm);
                    fail("compressed body too large");
                }
                inflated_.resize(std::min(inflated_.size() * 2, kMaxBodySize));
            }
            strm.next_out = inflated_.data() + strm.total_out;
            strm.avail_out = static_cast<uInt>(inflated_.size() - strm.total_out);
            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_BUF_ERROR ||
                (ret == Z_OK && strm.avail_out != 0 && strm.avail_in == 0)) {
                // Truncated stream: keep what was decoded so far.
                break;
            }
            if (ret != Z_OK && ret != Z_STREAM_END) {
                inflateEnd(&strm);
                fail("compressed body is corrupt");
            }
        }
        inflated_.resize(strm.total_out);
        inflateEnd(&strm);
    }

    // ── Font / style tables ─────────────────────────────

    void skip_font_and_styles() {
        for (int i = 0; i < 7; i++) {
            uint16_t n = body_.u16();
            body_.skip(size_t(n) * kFontNameLen);
        }
        uint16_t styles = body_.u16();
        body_.skip(size_t(styles) * (kStyleNameLen + kCharShapeLen +
                                     kParaShapeLen));
    }

    // ── Paragraphs ──────────────────────────────────────

    // Append finished paragraph text to the output.
    void flush_paragraph() {
        std::string t = util::trim(para_);
        para_.clear();
        if (t.empty()) return;
        out_ += t;
        out_ += "\n\n";
    }

    // Parse one paragraph list (document body, table cell, caption,
    // header/footer, footnote...). A paragraph with zero characters
    // terminates the list. Returns when the list ends or the stream is
    // exhausted (top-level body has no terminator in some writers).
    void parse_para_list(int depth) {
        if (depth > kMaxParaListDepth) fail("paragraph nesting too deep");

        while (!body_.eof()) {
            if (!parse_paragraph(depth)) return;   // list terminator
            if (depth == 0) flush_paragraph();
        }
    }

    // Parse a single paragraph. Returns false on the list-end marker.
    bool parse_paragraph(int depth) {
        uint8_t reuse_shape = body_.u8();
        uint16_t nch = body_.u16();
        uint16_t nline = body_.u16();
        uint8_t contain_cshape = body_.u8();
        body_.u8();                                // etc flag
        body_.u32();                               // control mask
        body_.u8();                                // style index

        if (nch == 0) {
            // Null paragraph (list terminator). It still carries its char
            // shape and line info; tolerate truncation at end of stream.
            size_t tail = kCharShapeLen + size_t(nline) * kLineInfoLen;
            body_.skip(std::min(tail, body_.size - body_.pos));
            return false;
        }

        body_.skip(kCharShapeLen);                 // paragraph char shape
        if (!reuse_shape) body_.skip(kParaShapeLen);
        body_.skip(size_t(nline) * kLineInfoLen);  // line segments

        if (contain_cshape) {
            for (uint16_t i = 0; i < nch; i++) {
                if (body_.u8() == 0)               // 0 = explicit shape
                    body_.skip(kCharShapeLen);
            }
        }

        // Text word stream: nch words including control payloads.
        uint32_t i = 0;
        while (i < nch) {
            uint16_t w = body_.u16();
            if (w > 31) {
                append_hwp_char(para_, w);
                i++;
                continue;
            }
            i += kCtrlWords[w];
            if (w == CTRL_END_PARA) break;
            handle_control(w, depth);
        }
        return true;
    }

    // Consume one inline control code's stream payload; emit text for the
    // controls that carry it.
    void handle_control(uint16_t w, int depth) {
        if (is_skip_block(w)) {
            uint32_t len = body_.u32();
            expect_dummy(w);
            body_.skip(len);
            return;
        }

        switch (w) {
            case CTRL_FIELD:
                body_.u32();                       // block size (informative)
                body_.u16();                       // dummy (not validated)
                parse_field();
                break;
            case CTRL_BOOKMARK: {
                uint32_t len = body_.u32();
                expect_dummy(w);
                body_.skip(len);                   // marker name + type
                break;
            }
            case CTRL_DATE_FORMAT:
                body_.skip(80);                    // format template
                expect_dummy(w);
                break;
            case CTRL_DATE_CODE: {
                body_.skip(80);                    // format template
                const uint8_t* d = body_.block(12); // y/m/d h:m + weekday
                expect_dummy(w);
                emit_date(d);
                break;
            }
            case CTRL_TAB:
                body_.skip(4);                     // width + leader
                expect_dummy(w);
                para_ += "\t";
                break;
            case CTRL_TEXT_BOX:
                body_.skip(4);                     // reserved
                expect_dummy(w);
                parse_text_box(depth);
                break;
            case CTRL_PICTURE:
                body_.skip(4);                     // reserved
                expect_dummy(w);
                parse_picture(depth);
                break;
            case CTRL_LINE:
                body_.skip(4);                     // reserved
                expect_dummy(w);
                body_.skip(84);                    // line object info
                break;
            case CTRL_HIDDEN:
                body_.skip(4);
                expect_dummy(w);
                body_.skip(8);
                parse_sub_list(depth);             // hidden body text
                break;
            case CTRL_HEADER_FOOTER: {
                body_.skip(4);
                expect_dummy(w);
                body_.skip(8);
                body_.u8();                        // type (header/footer)
                body_.u8();                        // odd/even
                parse_sub_list(depth);
                break;
            }
            case CTRL_FOOTNOTE:
                body_.skip(4);
                expect_dummy(w);
                body_.skip(8 + 6);                 // info, number, type, width
                parse_sub_list(depth);
                break;
            case CTRL_AUTO_NUM:
            case CTRL_NEW_NUM:
            case CTRL_SHOW_PAGE_NUM:
            case CTRL_PAGE_NUM_CTRL:
                body_.skip(4);
                expect_dummy(w);
                break;
            case CTRL_MAIL_MERGE:
                body_.skip(20);
                expect_dummy(w);
                break;
            case CTRL_COMPOSE: {
                const uint8_t* d = body_.block(6); // up to 3 overlaid chars
                expect_dummy(w);
                for (int k = 0; k < 3; k++) {
                    uint16_t ch = util::read_u16_le(d + 2 * k);
                    if (ch) append_hwp_char(para_, ch);
                }
                break;
            }
            case CTRL_HYPHEN:
                body_.skip(2);                     // width
                expect_dummy(w);
                break;
            case CTRL_TOC_MARK:
                body_.skip(2);
                expect_dummy(w);
                break;
            case CTRL_INDEX_MARK:
                body_.skip(240);                   // two 60-word keywords
                body_.skip(2);
                expect_dummy(w);
                break;
            case CTRL_OUTLINE:
                body_.skip(60);
                expect_dummy(w);
                break;
            case CTRL_KEEP_SPACE:
            case CTRL_FIXED_SPACE:
                expect_dummy(w);
                para_ += " ";
                break;
            default:
                break;                             // unreachable (all mapped)
        }
    }

    // Every control payload ends (or starts) with a word repeating the
    // control code; mismatch means the stream is misframed.
    void expect_dummy(uint16_t w) {
        if (body_.u16() != w) fail("control code framing mismatch");
    }

    // Parse a sub paragraph list (header/footer/footnote/hidden bodies),
    // separating its text from the surrounding paragraph.
    void parse_sub_list(int depth) {
        std::string saved;
        saved.swap(para_);
        parse_para_list_nested(depth + 1);
        std::string sub = util::trim(para_);
        para_.swap(saved);
        if (!sub.empty()) {
            out_ += sub;
            out_ += "\n\n";
        }
    }

    // Nested list: terminates at the zero-char paragraph.
    void parse_para_list_nested(int depth) {
        if (depth > kMaxParaListDepth) fail("paragraph nesting too deep");
        while (!body_.eof()) {
            if (!parse_paragraph(depth)) return;
            para_ += "\n";
        }
    }

    // Collect one nested paragraph list into a string.
    std::string collect_sub_list(int depth) {
        std::string saved;
        saved.swap(para_);
        parse_para_list_nested(depth + 1);
        std::string sub = para_;
        para_.swap(saved);
        return sub;
    }

    // ── Field (click-here) ──────────────────────────────

    void parse_field() {
        // Field block: type(2) reserved(4) location(2) reserved(22), four
        // dword lengths, three 2-byte-char strings, then binary data.
        body_.skip(2 + 4 + 2 + 22);
        uint32_t len1 = body_.u32();
        uint32_t len2 = body_.u32();
        uint32_t len3 = body_.u32();
        uint32_t binlen = body_.u32();

        body_.skip(len1);                          // field command
        body_.skip(len2);                          // help text
        // Display/result string (2-byte chars).
        uint32_t n = len3 / 2;
        for (uint32_t k = 0; k < n; k++) append_hwp_char(para_, body_.u16());
        body_.skip(len3 & 1);
        body_.skip(binlen);                        // attached binary data
    }

    // ── Text box / table ────────────────────────────────

    void parse_text_box(int depth) {
        // 84-byte box info: type at +78, cell count at +80.
        const uint8_t* info = body_.block(84);
        uint16_t type = util::read_u16_le(info + 78);   // 0 = table
        uint16_t ncell = util::read_u16_le(info + 80);

        if (ncell == 0) fail("text box without cells");

        std::vector<CellText> cells(ncell);
        for (uint16_t c = 0; c < ncell; c++) {
            const uint8_t* cd = body_.block(kCellLen);
            cells[c].x = util::read_u16_le(cd + 4);
            cells[c].y = util::read_u16_le(cd + 6);
        }
        for (uint16_t c = 0; c < ncell; c++)
            cells[c].text = collect_sub_list(depth);

        if (type == 0 && ncell > 1) {
            flush_paragraph();
            out_ += render_table(cells);
        } else {
            // Text box / equation / single-cell table: emit inline.
            for (auto& cell : cells) {
                std::string t = util::trim(cell.text);
                if (t.empty()) continue;
                if (!para_.empty()) para_ += " ";
                para_ += t;
            }
        }

        emit_caption(depth);                       // caption list always follows
    }

    // ── Picture ─────────────────────────────────────────

    void parse_picture(int depth) {
        // 348-byte picture info; the trailing-data length is its first dword.
        const uint8_t* info = body_.block(348);
        uint32_t follow = util::read_u32_le(info + 0);
        body_.skip(follow);                        // drawing/hyperlink data
        emit_caption(depth);
    }

    // Read the always-present caption paragraph list; emit non-empty text.
    void emit_caption(int depth) {
        std::string cap = util::trim(collect_sub_list(depth));
        if (!cap.empty()) {
            out_ += cap;
            out_ += "\n\n";
        }
    }

    // ── Date code ───────────────────────────────────────

    void emit_date(const uint8_t* d) {
        int y = util::read_u16_le(d + 0);          // year, month, weekday,
        int mo = util::read_u16_le(d + 2);         // day, hour, minute
        int day = util::read_u16_le(d + 6);
        if (y < 1900 || y > 2200 || mo < 1 || mo > 12 || day < 1 || day > 31)
            return;
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, mo, day);
        para_ += buf;
    }
};

} // namespace

// ── Public interface ────────────────────────────────────────

bool is_hwp3_signature(const uint8_t* data, size_t size) {
    static const char kPrefix[] = "HWP Document File V";
    return data && size >= sizeof(kPrefix) - 1 &&
           memcmp(data, kPrefix, sizeof(kPrefix) - 1) == 0;
}

Hwp3Parser::Hwp3Parser(const std::string& file_path) {
    std::ifstream f(file_path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open file: " + file_path);
    owned_.assign(std::istreambuf_iterator<char>(f),
                  std::istreambuf_iterator<char>());
    data_ = owned_.data();
    size_ = owned_.size();
}

Hwp3Parser::Hwp3Parser(const uint8_t* data, size_t size)
    : data_(data), size_(size) {}

std::string Hwp3Parser::to_markdown(const ConvertOptions& opts) {
    Hwp3Doc doc(data_, size_);
    doc.parse();
    std::string md = std::move(doc.text());
    while (!md.empty() && md.back() == '\n') md.pop_back();
    if (!md.empty()) md += "\n";
    if (opts.format == OutputFormat::PLAINTEXT)
        md = util::strip_markdown(md);
    return md;
}

std::vector<PageChunk> Hwp3Parser::to_chunks(const ConvertOptions& opts) {
    PageChunk chunk;
    chunk.page_number = 0;
    chunk.text = to_markdown(opts);
    return {chunk};
}

} // namespace jdoc

