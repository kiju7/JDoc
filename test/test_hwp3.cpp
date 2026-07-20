// test_hwp3.cpp - Tests for HWP 3.x legacy binary text extraction
// Builds synthetic fixtures from the HWP 3.0 format spec (signature +
// document info + font/style tables + paragraph stream) and verifies
// johab hangul conversion, table cell extraction, and crash safety.
// License: MIT

#include "jdoc/jdoc.h"
#include "convert_internal.h"
#include "legacy/hwp3_parser.h"

#include <zlib.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// ── Test helpers ────────────────────────────────────────────

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    std::cerr << "  Testing: " << #name << "... "; \
    try {

#define TEST_END \
        tests_passed++; \
        std::cerr << "OK\n"; \
    } catch (const std::exception& e) { \
        tests_failed++; \
        std::cerr << "FAILED: " << e.what() << "\n"; \
    }

#define ASSERT(cond) \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);

// ── Fixture builder ─────────────────────────────────────────

namespace {

using Bytes = std::vector<uint8_t>;

void put_u16(Bytes& b, uint16_t v) {
    b.push_back(uint8_t(v & 0xFF));
    b.push_back(uint8_t(v >> 8));
}

void put_u32(Bytes& b, uint32_t v) {
    put_u16(b, uint16_t(v & 0xFFFF));
    put_u16(b, uint16_t(v >> 16));
}

void put_zeros(Bytes& b, size_t n) { b.insert(b.end(), n, 0); }

// Encode a Unicode hangul syllable (U+AC00..U+D7A3) as an HWP 3.x johab
// word: MSB set, 5-bit cho/jung/jong indexes.
uint16_t johab(char32_t cp) {
    uint32_t s = cp - 0xAC00;
    uint32_t cho = s / (21 * 28);
    uint32_t jung = (s / 28) % 21;
    uint32_t jong = s % 28;
    // Johab jungseong codes skip 2 slots at the start of each 8-block.
    static const uint8_t kJungCode[21] = {3,  4,  5,  6,  7,  10, 11,
                                          12, 13, 14, 15, 18, 19, 20,
                                          21, 22, 23, 26, 27, 28, 29};
    uint16_t jong_code = uint16_t(jong == 0 ? 1 : jong <= 16 ? jong + 1
                                                             : jong + 2);
    return uint16_t(0x8000 | ((cho + 2) << 10) | (kJungCode[jung] << 5) |
                    jong_code);
}

// Append UTF-8 text as HWP character words (ASCII + hangul syllables only).
void put_text(Bytes& b, const std::string& utf8) {
    for (size_t i = 0; i < utf8.size();) {
        uint8_t c = uint8_t(utf8[i]);
        if (c < 0x80) {
            put_u16(b, c);
            i += 1;
        } else {  // decode a 3-byte UTF-8 hangul syllable
            char32_t cp = char32_t(c & 0x0F) << 12 |
                          char32_t(utf8[i + 1] & 0x3F) << 6 |
                          char32_t(utf8[i + 2] & 0x3F);
            put_u16(b, johab(cp));
            i += 3;
        }
    }
}

// Count HWP words the text occupies (ASCII = 1, 3-byte UTF-8 = 1).
uint16_t text_words(const std::string& utf8) {
    uint16_t n = 0;
    for (size_t i = 0; i < utf8.size(); i += (uint8_t(utf8[i]) < 0x80 ? 1 : 3))
        n++;
    return n;
}

// Paragraph header + representative char shape (+ para shape) + line info.
void put_para_header(Bytes& b, uint16_t nch, uint16_t nline = 1) {
    b.push_back(0);          // reuse_shape: paragraph carries its own shape
    put_u16(b, nch);
    put_u16(b, nline);
    b.push_back(0);          // contain_cshape
    b.push_back(0);          // etc flag
    put_u32(b, 0);           // control mask
    b.push_back(0);          // style index
    put_zeros(b, 31);        // char shape (present even in null paragraphs)
    if (nch != 0) put_zeros(b, 187);       // para shape
    put_zeros(b, size_t(nline) * 14);      // line info
}

// One complete paragraph holding plain text.
void put_text_para(Bytes& b, const std::string& utf8) {
    put_para_header(b, uint16_t(text_words(utf8) + 1));
    put_text(b, utf8);
    put_u16(b, 13);          // end-of-paragraph mark
}

// List terminator: null paragraph (header + char shape, no lines).
void put_null_para(Bytes& b) { put_para_header(b, 0, 0); }

// A 2x2 table paragraph (text box control, type 0).
void put_table_para(Bytes& b, const std::string cells[4]) {
    put_para_header(b, 4 + 1);   // text box charges 4 words + end mark
    put_u16(b, 10);              // CH_TEXT_BOX
    put_u32(b, 0);               // reserved
    put_u16(b, 10);              // dummy (= control code)
    Bytes info(84, 0);
    // type (offset 78) = 0: table; cell count (offset 80) = 4
    info[80] = 4;
    b.insert(b.end(), info.begin(), info.end());
    static const uint16_t xs[4] = {0, 4000, 0, 4000};
    static const uint16_t ys[4] = {0, 0, 2000, 2000};
    for (int c = 0; c < 4; c++) {  // 27-byte cell descriptors
        put_u16(b, uint16_t(c));   // serial
        put_u16(b, 0);             // color
        put_u16(b, xs[c]);
        put_u16(b, ys[c]);
        put_u16(b, 4000);          // width
        put_u16(b, 2000);          // height
        put_zeros(b, 27 - 12);
    }
    for (int c = 0; c < 4; c++) {  // one paragraph list per cell
        put_text_para(b, cells[c]);
        put_null_para(b);
    }
    put_null_para(b);              // empty caption list
    put_u16(b, 13);                // end of the hosting paragraph
}

// Assemble a full document around the given body (fonts/styles/paragraphs).
Bytes build_hwp3_file(const Bytes& body, bool compressed) {
    Bytes f;
    static const uint8_t kSig[30] = {'H', 'W', 'P', ' ', 'D',  'o',  'c',
                                     'u', 'm', 'e', 'n', 't',  ' ',  'F',
                                     'i', 'l', 'e', ' ', 'V',  '3',  '.',
                                     '0', '0', ' ', 0x1A, 0x01, 0x02, 0x03,
                                     0x04, 0x05};
    f.insert(f.end(), kSig, kSig + 30);
    Bytes docinfo(128, 0);
    docinfo[124] = compressed ? 1 : 0;
    f.insert(f.end(), docinfo.begin(), docinfo.end());
    put_zeros(f, 1008);          // summary
    // no info block (length 0 in docinfo)

    if (!compressed) {
        f.insert(f.end(), body.begin(), body.end());
        return f;
    }

    // Raw-deflate the body (no zlib/gzip header).
    Bytes comp(compressBound(uLong(body.size())) + 16);
    z_stream strm = {};
    deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                 Z_DEFAULT_STRATEGY);
    strm.next_in = const_cast<uint8_t*>(body.data());
    strm.avail_in = uInt(body.size());
    strm.next_out = comp.data();
    strm.avail_out = uInt(comp.size());
    int ret = deflate(&strm, Z_FINISH);
    assert(ret == Z_STREAM_END);
    (void)ret;
    comp.resize(strm.total_out);
    deflateEnd(&strm);
    f.insert(f.end(), comp.begin(), comp.end());
    put_u32(f, 0);               // trailer: crc32 (unchecked)
    put_u32(f, uint32_t(body.size()));
    return f;
}

// Standard body: font tables (1 dummy font per language), 1 style.
void put_tables(Bytes& b) {
    for (int i = 0; i < 7; i++) {
        put_u16(b, 1);
        Bytes name(40, 0);
        memcpy(name.data(), "System", 6);
        b.insert(b.end(), name.begin(), name.end());
    }
    put_u16(b, 1);               // style count
    put_zeros(b, 20 + 31 + 187); // style name + char shape + para shape
}

Bytes simple_doc(bool compressed) {
    Bytes body;
    put_tables(body);
    put_text_para(body, "Hello HWP3");
    put_text_para(body, "대한민국 한글 문서");
    put_null_para(body);
    return build_hwp3_file(body, compressed);
}

std::string convert_mem(const Bytes& f) {
    return jdoc::convert(f.data(), f.size(), "test.hwp");
}

} // namespace

// ── Tests ───────────────────────────────────────────────────

static void test_detection() {
    std::cerr << "Format detection:\n";

    TEST(signature_detected_as_hwp)
        auto doc = simple_doc(false);
        ASSERT(jdoc::detect_format_mem(doc.data(), doc.size(), "x.bin") ==
               jdoc::FileFormat::HWP);
    TEST_END

    TEST(signature_helper)
        auto doc = simple_doc(false);
        ASSERT(jdoc::is_hwp3_signature(doc.data(), doc.size()));
        ASSERT(!jdoc::is_hwp3_signature(doc.data(), 4));
        const uint8_t ole[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
        ASSERT(!jdoc::is_hwp3_signature(ole, 8));
    TEST_END
}

static void test_text_extraction() {
    std::cerr << "\nText extraction:\n";

    TEST(ascii_and_johab_hangul)
        auto md = convert_mem(simple_doc(false));
        ASSERT(md.find("Hello HWP3") != std::string::npos);
        ASSERT(md.find("대한민국 한글 문서") != std::string::npos);
    TEST_END

    TEST(compressed_body)
        auto md = convert_mem(simple_doc(true));
        ASSERT(md.find("Hello HWP3") != std::string::npos);
        ASSERT(md.find("대한민국 한글 문서") != std::string::npos);
    TEST_END

    TEST(paragraph_separation)
        auto md = convert_mem(simple_doc(false));
        ASSERT(md.find("Hello HWP3\n\n대한민국") != std::string::npos);
    TEST_END

    TEST(hanja_and_symbol_codes)
        Bytes body;
        put_tables(body);
        put_para_header(body, 3);
        put_u16(body, 0x517C);       // hanja 韓 (KS 0xF9DB packed at 0x4000)
        put_u16(body, 0x3438);       // symbol ※ (KS 0xA1D8 packed at 0x3400)
        put_u16(body, 13);
        put_null_para(body);
        auto md = convert_mem(build_hwp3_file(body, false));
        ASSERT(md.find("韓※") != std::string::npos);
    TEST_END

    TEST(johab_boundary_syllables)
        Bytes body;
        put_tables(body);
        put_text_para(body, "가힣");   // first/last modern syllables
        put_null_para(body);
        auto md = convert_mem(build_hwp3_file(body, false));
        ASSERT(md.find("가힣") != std::string::npos);
    TEST_END

    TEST(tab_and_spaces)
        Bytes body;
        put_tables(body);
        put_para_header(body, 4 + 2 + 1);
        put_text(body, "a");
        put_u16(body, 9);            // tab: width, leader, dummy
        put_u16(body, 0);
        put_u16(body, 0);
        put_u16(body, 9);
        put_text(body, "b");
        put_u16(body, 13);
        put_null_para(body);
        auto md = convert_mem(build_hwp3_file(body, false));
        ASSERT(md.find("a\tb") != std::string::npos);
    TEST_END
}

static void test_tables() {
    std::cerr << "\nTables:\n";

    TEST(table_cells_extracted)
        Bytes body;
        put_tables(body);
        const std::string cells[4] = {"이름", "나이", "홍길동", "30"};
        put_table_para(body, cells);
        put_null_para(body);
        auto md = convert_mem(build_hwp3_file(body, false));
        ASSERT(md.find("| 이름 | 나이 |") != std::string::npos);
        ASSERT(md.find("| --- | --- |") != std::string::npos);
        ASSERT(md.find("| 홍길동 | 30 |") != std::string::npos);
    TEST_END

    TEST(text_after_table)
        Bytes body;
        put_tables(body);
        const std::string cells[4] = {"a", "b", "c", "d"};
        put_table_para(body, cells);
        put_text_para(body, "after");
        put_null_para(body);
        auto md = convert_mem(build_hwp3_file(body, false));
        ASSERT(md.find("| a | b |") != std::string::npos);
        ASSERT(md.find("after") != std::string::npos);
    TEST_END
}

static void test_robustness() {
    std::cerr << "\nRobustness:\n";

    TEST(truncated_header_throws)
        auto doc = simple_doc(false);
        doc.resize(100);
        bool threw = false;
        try { convert_mem(doc); } catch (const std::exception&) { threw = true; }
        ASSERT(threw);
    TEST_END

    TEST(truncated_body_throws)
        auto doc = simple_doc(false);
        doc.resize(doc.size() - 40);
        bool threw = false;
        try { convert_mem(doc); } catch (const std::exception&) { threw = true; }
        ASSERT(threw);
    TEST_END

    TEST(corrupt_control_framing_throws)
        Bytes body;
        put_tables(body);
        put_para_header(body, 3);
        put_u16(body, 9);            // tab control with a bad dummy word
        put_u16(body, 0);
        put_u16(body, 0);
        put_u16(body, 0xBEEF);       // dummy != 9 -> framing error
        put_u16(body, 13);
        auto doc = build_hwp3_file(body, false);
        bool threw = false;
        try { convert_mem(doc); } catch (const std::exception&) { threw = true; }
        ASSERT(threw);
    TEST_END

    TEST(oversized_font_count_throws)
        Bytes body;
        put_u16(body, 0xFFFF);       // 65535 fonts x 40 bytes > remaining
        auto doc = build_hwp3_file(body, false);
        bool threw = false;
        try { convert_mem(doc); } catch (const std::exception&) { threw = true; }
        ASSERT(threw);
    TEST_END

    TEST(encrypted_doc_throws)
        auto doc = simple_doc(false);
        doc[30 + 96] = 1;            // docinfo encrypted flag
        bool threw = false;
        try {
            convert_mem(doc);
        } catch (const std::exception& e) {
            threw = std::string(e.what()).find("password") != std::string::npos;
        }
        ASSERT(threw);
    TEST_END

    TEST(garbage_compressed_body_throws)
        Bytes body;
        put_tables(body);
        auto doc = build_hwp3_file(body, false);
        doc[30 + 124] = 1;           // claim compression over stored bytes
        bool threw = false;
        try { convert_mem(doc); } catch (const std::exception&) { threw = true; }
        ASSERT(threw);
    TEST_END
}

int main() {
    std::cerr << "=== HWP 3.x Tests ===\n\n";

    test_detection();
    test_text_extraction();
    test_tables();
    test_robustness();

    std::cerr << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===\n";
    return tests_failed > 0 ? 1 : 0;
}
