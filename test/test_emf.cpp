// test_emf.cpp - Tests for EMF metafile text extraction (jdoc::emf_extract_text).
// Builds synthetic EMF record streams and checks the recovered text.
// License: MIT

#include "common/emf_text.h"
#include "jdoc/detect.h"
#include "jdoc/jdoc.h"

#include <cassert>
#include <cstdint>
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

// ── EMF byte-buffer builder ─────────────────────────────────

static void put_u32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    if (b.size() < off + 4) b.resize(off + 4, 0);
    b[off] = v & 0xFF; b[off + 1] = (v >> 8) & 0xFF;
    b[off + 2] = (v >> 16) & 0xFF; b[off + 3] = (v >> 24) & 0xFF;
}

// EMR_HEADER (88 bytes) with the mandatory " EMF" signature at offset 40.
static std::vector<uint8_t> emf_header() {
    std::vector<uint8_t> h(88, 0);
    put_u32(h, 0, 0x00000001);   // iType = EMR_HEADER
    put_u32(h, 4, 88);           // nSize
    put_u32(h, 40, 0x464D4520);  // " EMF"
    return h;
}

// EMR_EOF (20 bytes): type + size + minimal body.
static std::vector<uint8_t> emf_eof() {
    std::vector<uint8_t> e(20, 0);
    put_u32(e, 0, 0x0000000E);   // iType = EMR_EOF
    put_u32(e, 4, 20);           // nSize
    return e;
}

// An EMR_EXTTEXTOUTW/A record placing `str_bytes` at record offset 76.
// type = 0x54 (W) or 0x53 (A); x/y = reference point; chars = char count.
static std::vector<uint8_t> exttextout(uint32_t type, int32_t x, int32_t y,
                                       uint32_t chars,
                                       const std::vector<uint8_t>& str_bytes) {
    const size_t str_off = 76;
    size_t total = str_off + str_bytes.size();
    total = (total + 3) & ~size_t(3);            // 4-byte align
    std::vector<uint8_t> r(total, 0);
    put_u32(r, 0, type);
    put_u32(r, 4, (uint32_t)total);
    put_u32(r, 36, (uint32_t)x);                 // EMRTEXT.reference.x
    put_u32(r, 40, (uint32_t)y);                 // EMRTEXT.reference.y
    put_u32(r, 44, chars);                       // EMRTEXT.Chars
    put_u32(r, 48, (uint32_t)str_off);           // EMRTEXT.offString (record-rel)
    std::memcpy(r.data() + str_off, str_bytes.data(), str_bytes.size());
    return r;
}

static std::vector<uint8_t> utf16le(const std::vector<uint16_t>& units) {
    std::vector<uint8_t> b;
    for (uint16_t u : units) { b.push_back(u & 0xFF); b.push_back(u >> 8); }
    return b;
}

static void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

// ── Tests ───────────────────────────────────────────────────

void test_exttextout_w() {
    std::cerr << "EXTTEXTOUTW (Unicode):\n";

    TEST("ascii + korean")
        // "Hello 한글" in UTF-16LE (8 code units).
        auto s = utf16le({'H','e','l','l','o',' ', 0xD55C, 0xAE00});
        std::vector<uint8_t> emf = emf_header();
        append(emf, exttextout(0x54, 100, 200, 8, s));
        append(emf, emf_eof());
        auto out = jdoc::emf_extract_text(emf.data(), emf.size());
        ASSERT(out == "Hello 한글");
    TEST_END
}

void test_exttextout_a() {
    std::cerr << "EXTTEXTOUTA (ANSI):\n";

    TEST("ascii bytes route through utf8/cp949 path")
        std::vector<uint8_t> s = {'T','e','s','t','1','2','3'};
        std::vector<uint8_t> emf = emf_header();
        append(emf, exttextout(0x53, 0, 0, 7, s));
        append(emf, emf_eof());
        auto out = jdoc::emf_extract_text(emf.data(), emf.size());
        ASSERT(out == "Test123");
    TEST_END
}

void test_reading_order() {
    std::cerr << "Reading order (sort by y then x):\n";

    TEST("lower y comes first, newline between lines")
        auto a = utf16le({'B','B'});   // placed at y=300
        auto b = utf16le({'A','A'});   // placed at y=100 → should come first
        std::vector<uint8_t> emf = emf_header();
        append(emf, exttextout(0x54, 0, 300, 2, a));
        append(emf, exttextout(0x54, 0, 100, 2, b));
        append(emf, emf_eof());
        auto out = jdoc::emf_extract_text(emf.data(), emf.size());
        ASSERT(out == "AA\nBB");
    TEST_END

    TEST("same y, sorted by x, no newline")
        auto a = utf16le({'2'});
        auto b = utf16le({'1'});
        std::vector<uint8_t> emf = emf_header();
        append(emf, exttextout(0x54, 500, 100, 1, a));   // x=500
        append(emf, exttextout(0x54, 10,  100, 1, b));   // x=10 → first
        append(emf, emf_eof());
        auto out = jdoc::emf_extract_text(emf.data(), emf.size());
        ASSERT(out == "12");
    TEST_END
}

void test_rejects_non_emf() {
    std::cerr << "Rejects / empty:\n";

    TEST("not an emf")
        std::vector<uint8_t> junk = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
        ASSERT(jdoc::emf_extract_text(junk.data(), junk.size()).empty());
    TEST_END

    TEST("valid header, no text records")
        std::vector<uint8_t> emf = emf_header();
        append(emf, emf_eof());
        ASSERT(jdoc::emf_extract_text(emf.data(), emf.size()).empty());
    TEST_END

    TEST("null / tiny")
        ASSERT(jdoc::emf_extract_text(nullptr, 0).empty());
        std::vector<uint8_t> tiny = {0x01, 0x00};
        ASSERT(jdoc::emf_extract_text(tiny.data(), tiny.size()).empty());
    TEST_END

    TEST("header claims size beyond buffer")
        auto h = emf_header();
        put_u32(h, 4, 100000);   // nSize > buffer
        ASSERT(jdoc::emf_extract_text(h.data(), h.size()).empty());
    TEST_END
}

void test_end_to_end() {
    std::cerr << "End-to-end (detect + convert on standalone EMF):\n";

    // A standalone EMF blob carrying "EMF텍스트".
    auto s = utf16le({'E','M','F', 0xD14D, 0xC2A4, 0xD2B8});   // EMF텍스트
    std::vector<uint8_t> emf = emf_header();
    append(emf, exttextout(0x54, 0, 0, 6, s));
    append(emf, emf_eof());

    TEST("detect classifies EMF as convertible")
        auto info = jdoc::detect(emf.data(), emf.size(), "diagram.emf");
        ASSERT(info.format == "EMF");
        ASSERT(info.category == jdoc::FormatCategory::Image);
        ASSERT(info.convertible);
        ASSERT(info.extension == ".emf");
    TEST_END

    TEST("convert extracts the metafile text")
        std::string out = jdoc::convert(emf.data(), emf.size(), "diagram.emf");
        ASSERT(out == "EMF텍스트");
    TEST_END
}

int main() {
    std::cerr << "\n=== jdoc::emf_extract_text tests ===\n\n";
    test_exttextout_w();
    test_exttextout_a();
    test_reading_order();
    test_rejects_non_emf();
    test_end_to_end();
    std::cerr << "\n" << tests_passed << " passed, " << tests_failed
              << " failed\n";
    return tests_failed == 0 ? 0 : 1;
}
