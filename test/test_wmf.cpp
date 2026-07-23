// test_wmf.cpp - Tests for WMF metafile text extraction (jdoc::wmf_extract_text).
// License: MIT

#include "common/wmf_text.h"
#include "jdoc/detect.h"
#include "jdoc/jdoc.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    std::cerr << "  Testing: " << name << "... "; \
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

static void put_u16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF);
}
static void put_u32v(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF);
    b.push_back((v >> 16) & 0xFF); b.push_back((v >> 24) & 0xFF);
}

// Standard 18-byte WMF header (mtType=1, mtHeaderSize=9 words).
static std::vector<uint8_t> wmf_header() {
    std::vector<uint8_t> h;
    put_u16(h, 1);   // mtType
    put_u16(h, 9);   // mtHeaderSize (words)
    put_u16(h, 0x0300); // mtVersion
    put_u32v(h, 0);  // mtSize
    put_u16(h, 0);   // mtNoObjects
    put_u32v(h, 0);  // mtMaxRecord
    put_u16(h, 0);   // mtNoParameters
    return h;         // 18 bytes
}

// META_EXTTEXTOUT record (func 0x0A32) with ANSI string, no rect.
static std::vector<uint8_t> exttextout(int16_t x, int16_t y,
                                       const std::string& s) {
    std::vector<uint8_t> parm;
    put_u16(parm, (uint16_t)y);
    put_u16(parm, (uint16_t)x);
    put_u16(parm, (uint16_t)s.size());   // Count
    put_u16(parm, 0);                    // fwOpts (no rect)
    for (char c : s) parm.push_back((uint8_t)c);
    if (parm.size() & 1) parm.push_back(0);   // WORD-pad

    size_t rec_bytes = 6 + parm.size();
    std::vector<uint8_t> r;
    put_u32v(r, (uint32_t)(rec_bytes / 2));   // rdSize (words)
    put_u16(r, 0x0A32);                       // rdFunction
    r.insert(r.end(), parm.begin(), parm.end());
    return r;
}

// META_TEXTOUT record (func 0x0521).
static std::vector<uint8_t> textout(int16_t x, int16_t y, const std::string& s) {
    std::vector<uint8_t> parm;
    put_u16(parm, (uint16_t)s.size());   // Count
    for (char c : s) parm.push_back((uint8_t)c);
    if (parm.size() & 1) parm.push_back(0);
    put_u16(parm, (uint16_t)y);          // YStart
    put_u16(parm, (uint16_t)x);          // XStart

    size_t rec_bytes = 6 + parm.size();
    std::vector<uint8_t> r;
    put_u32v(r, (uint32_t)(rec_bytes / 2));
    put_u16(r, 0x0521);
    r.insert(r.end(), parm.begin(), parm.end());
    return r;
}

static std::vector<uint8_t> eof_record() {
    std::vector<uint8_t> r;
    put_u32v(r, 3);   // rdSize = 3 words
    put_u16(r, 0x0000);
    return r;
}

static void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

void test_exttextout() {
    std::cerr << "META_EXTTEXTOUT:\n";
    TEST("ascii text")
        auto w = wmf_header();
        append(w, exttextout(0, 0, "Hello WMF"));
        append(w, eof_record());
        ASSERT(jdoc::wmf_extract_text(w.data(), w.size()) == "Hello WMF");
    TEST_END
}

void test_textout() {
    std::cerr << "META_TEXTOUT:\n";
    TEST("ascii text")
        auto w = wmf_header();
        append(w, textout(0, 0, "Plain"));
        append(w, eof_record());
        ASSERT(jdoc::wmf_extract_text(w.data(), w.size()) == "Plain");
    TEST_END
}

void test_order_and_placeable() {
    std::cerr << "Ordering + placeable header:\n";

    TEST("reading order by y then x")
        auto w = wmf_header();
        append(w, exttextout(0, 300, "second"));   // y=300
        append(w, exttextout(0, 100, "first"));    // y=100 → first
        append(w, eof_record());
        ASSERT(jdoc::wmf_extract_text(w.data(), w.size()) == "first\nsecond");
    TEST_END

    TEST("aldus placeable header is skipped")
        std::vector<uint8_t> w;
        // 22-byte placeable header: magic D7 CD C6 9A + 18 bytes.
        put_u32v(w, 0x9AC6CDD7);
        while (w.size() < 22) w.push_back(0);
        append(w, wmf_header());
        append(w, exttextout(0, 0, "placeable"));
        append(w, eof_record());
        ASSERT(jdoc::wmf_extract_text(w.data(), w.size()) == "placeable");
    TEST_END
}

void test_rejects() {
    std::cerr << "Rejects / empty:\n";
    TEST("not a wmf")
        std::vector<uint8_t> junk = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
        ASSERT(jdoc::wmf_extract_text(junk.data(), junk.size()).empty());
    TEST_END
    TEST("header only, no text")
        auto w = wmf_header();
        append(w, eof_record());
        ASSERT(jdoc::wmf_extract_text(w.data(), w.size()).empty());
    TEST_END
    TEST("null")
        ASSERT(jdoc::wmf_extract_text(nullptr, 0).empty());
    TEST_END
}

// META_STRETCHDIB (0x0F43): 22 fixed bytes then a contiguous DIB.
static std::vector<uint8_t> stretchdib(const std::vector<uint8_t>& dib) {
    std::vector<uint8_t> parm(22, 0);
    parm.insert(parm.end(), dib.begin(), dib.end());
    if (parm.size() & 1) parm.push_back(0);
    size_t rec_bytes = 6 + parm.size();
    std::vector<uint8_t> r;
    put_u32v(r, (uint32_t)(rec_bytes / 2));
    put_u16(r, 0x0F43);
    r.insert(r.end(), parm.begin(), parm.end());
    return r;
}

void test_bitmap_extraction() {
    std::cerr << "Embedded bitmap extraction:\n";

    TEST("stretchdib -> valid BMP with correct dimensions")
        std::vector<uint8_t> dib(40, 0);           // BITMAPINFOHEADER
        auto set32 = [&](size_t o, uint32_t v) {
            dib[o]=v&0xFF; dib[o+1]=(v>>8)&0xFF; dib[o+2]=(v>>16)&0xFF; dib[o+3]=(v>>24)&0xFF;
        };
        set32(0, 40);       // biSize
        set32(4, 9);        // biWidth
        set32(8, 6);        // biHeight
        dib[12] = 1;        // biPlanes
        dib[14] = 24;       // biBitCount
        for (int k = 0; k < 12; ++k) dib.push_back(0xCD);  // pixel bytes

        auto w = wmf_header();
        append(w, stretchdib(dib));
        append(w, eof_record());

        auto bmps = jdoc::wmf_extract_bitmaps(w.data(), w.size());
        ASSERT(bmps.size() == 1);
        auto& b = bmps[0];
        ASSERT(b[0] == 'B' && b[1] == 'M');
        int bw = b[18] | b[19]<<8 | b[20]<<16 | b[21]<<24;
        int bh = b[22] | b[23]<<8 | b[24]<<16 | b[25]<<24;
        ASSERT(bw == 9 && bh == 6);
    TEST_END

    TEST("no dib records -> empty")
        auto w = wmf_header();
        append(w, exttextout(0, 0, "text only"));
        append(w, eof_record());
        ASSERT(jdoc::wmf_extract_bitmaps(w.data(), w.size()).empty());
    TEST_END
}

void test_end_to_end() {
    std::cerr << "End-to-end (detect + convert on placeable WMF):\n";

    // Placeable WMF (magic-detectable) carrying text.
    std::vector<uint8_t> w;
    put_u32v(w, 0x9AC6CDD7);
    while (w.size() < 22) w.push_back(0);
    append(w, wmf_header());
    append(w, exttextout(0, 0, "WMF text"));
    append(w, eof_record());

    TEST("detect classifies WMF as convertible")
        auto info = jdoc::detect(w.data(), w.size(), "");
        ASSERT(info.format == "WMF");
        ASSERT(info.convertible);
        ASSERT(info.extension == ".wmf");
    TEST_END

    TEST("convert extracts the metafile text")
        ASSERT(jdoc::convert(w.data(), w.size(), "d.wmf") == "WMF text");
    TEST_END
}

// Ad-hoc: `test_wmf <file.wmf>` prints text + bitmap extraction for a real WMF.
static int inspect_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    std::cerr << path << ": " << d.size() << " bytes\n";
    auto text = jdoc::wmf_extract_text(d.data(), d.size());
    std::cerr << "  text: " << text.size() << " bytes"
              << (text.empty() ? " (none)" : (": " + text.substr(0, 200))) << "\n";
    auto bmps = jdoc::wmf_extract_bitmaps(d.data(), d.size());
    std::cerr << "  bitmaps: " << bmps.size() << "\n";
    for (size_t i = 0; i < bmps.size(); ++i) {
        auto& b = bmps[i];
        int w = 0, h = 0;
        auto u = [&](size_t k) { return (unsigned char)b[k]; };
        if (b.size() >= 26) {
            w = u(18) | u(19)<<8 | u(20)<<16 | u(21)<<24;
            h = u(22) | u(23)<<8 | u(24)<<16 | u(25)<<24;
        }
        std::cerr << "    bmp[" << i << "]: " << b.size() << " bytes, "
                  << w << "x" << h << "\n";
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1) return inspect_file(argv[1]);
    std::cerr << "\n=== jdoc::wmf_extract_text tests ===\n\n";
    test_exttextout();
    test_textout();
    test_order_and_placeable();
    test_rejects();
    test_bitmap_extraction();
    test_end_to_end();
    std::cerr << "\n" << tests_passed << " passed, " << tests_failed
              << " failed\n";
    return tests_failed == 0 ? 0 : 1;
}
