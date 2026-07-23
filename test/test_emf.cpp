// test_emf.cpp - Tests for EMF metafile text extraction (jdoc::emf_extract_text).
// Builds synthetic EMF record streams and checks the recovered text.
// License: MIT

#include "common/emf_text.h"
#include "jdoc/detect.h"
#include "jdoc/jdoc.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
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

// EMR_STRETCHDIBITS (0x51) record carrying a DIB (header + pixels).
static std::vector<uint8_t> stretchdibits(const std::vector<uint8_t>& bmi,
                                          const std::vector<uint8_t>& bits) {
    const size_t off_bmi = 80;
    size_t off_bits = off_bmi + bmi.size();
    size_t total = off_bits + bits.size();
    total = (total + 3) & ~size_t(3);
    std::vector<uint8_t> r(total, 0);
    put_u32(r, 0, 0x51);
    put_u32(r, 4, (uint32_t)total);
    put_u32(r, 48, (uint32_t)off_bmi);         // offBmiSrc
    put_u32(r, 52, (uint32_t)bmi.size());      // cbBmiSrc
    put_u32(r, 56, (uint32_t)off_bits);        // offBitsSrc
    put_u32(r, 60, (uint32_t)bits.size());     // cbBitsSrc
    std::memcpy(r.data() + off_bmi, bmi.data(), bmi.size());
    std::memcpy(r.data() + off_bits, bits.data(), bits.size());
    return r;
}

void test_bitmap_extraction() {
    std::cerr << "Embedded bitmap extraction:\n";

    TEST("stretchdibits -> valid BMP with correct dimensions")
        std::vector<uint8_t> bmi(40, 0);           // BITMAPINFOHEADER
        put_u32(bmi, 0, 40);                        // biSize
        put_u32(bmi, 4, 7);                         // biWidth
        put_u32(bmi, 8, 5);                         // biHeight
        bmi[12] = 1;                                // biPlanes
        bmi[14] = 24;                               // biBitCount
        std::vector<uint8_t> bits(12, 0xAB);        // some pixel bytes

        std::vector<uint8_t> emf = emf_header();
        append(emf, stretchdibits(bmi, bits));
        append(emf, emf_eof());

        auto bmps = jdoc::emf_extract_bitmaps(emf.data(), emf.size());
        ASSERT(bmps.size() == 1);
        auto& b = bmps[0];
        ASSERT(b.size() == 14 + bmi.size() + bits.size());
        ASSERT(b[0] == 'B' && b[1] == 'M');
        int w = b[18] | b[19]<<8 | b[20]<<16 | b[21]<<24;   // width in the DIB
        int h = b[22] | b[23]<<8 | b[24]<<16 | b[25]<<24;
        ASSERT(w == 7 && h == 5);
    TEST_END

    TEST("no bitmap records -> empty")
        std::vector<uint8_t> emf = emf_header();
        append(emf, emf_eof());
        ASSERT(jdoc::emf_extract_bitmaps(emf.data(), emf.size()).empty());
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

// Ad-hoc: `test_emf <file.emf>` prints text + bitmap extraction for a real EMF.
static int inspect_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    std::cerr << path << ": " << d.size() << " bytes\n";
    auto text = jdoc::emf_extract_text(d.data(), d.size());
    std::cerr << "  text: " << text.size() << " bytes"
              << (text.empty() ? " (none)" : (": " + text.substr(0, 200))) << "\n";
    auto bmps = jdoc::emf_extract_bitmaps(d.data(), d.size());
    std::cerr << "  bitmaps: " << bmps.size() << "\n";
    for (size_t i = 0; i < bmps.size(); ++i) {
        auto& b = bmps[i];
        int w = 0, h = 0, bpp = 0;
        auto u = [&](size_t k) { return (unsigned char)b[k]; };
        if (b.size() >= 30) {
            w = u(18) | u(19)<<8 | u(20)<<16 | u(21)<<24;
            h = u(22) | u(23)<<8 | u(24)<<16 | u(25)<<24;
            bpp = u(28) | u(29)<<8;
        }
        std::cerr << "    bmp[" << i << "]: " << b.size() << " bytes, hdr="
                  << (b.size() > 2 && b[0]=='B' && b[1]=='M' ? "BM" : "??")
                  << ", " << w << "x" << h << " " << bpp << "bpp\n";
    }
    return 0;
}

// `test_emf bench <file.emf>` times the old two-pass+copy path vs the new
// single-pass+move path for extracting text + images into ImageData-like bytes.
static int bench_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    const int N = 200;
    using clk = std::chrono::steady_clock;

    // OLD: parse for text, parse again for bitmaps, then copy each BMP into a
    // separate char buffer (as the pre-optimization code did).
    auto t0 = clk::now();
    volatile size_t sink = 0;
    for (int it = 0; it < N; ++it) {
        std::string text = jdoc::emf_extract_text(d.data(), d.size());
        auto bmps = jdoc::emf_extract_bitmaps(d.data(), d.size());
        for (auto& b : bmps) {
            std::vector<char> copy;
            copy.assign(b.begin(), b.end());   // the extra copy into ImageData
            sink += copy.size() + (copy.empty() ? 0 : (unsigned char)copy.back());
        }
        sink += text.size();
    }
    double old_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count() / N;

    // NEW: one pass, move the BMP buffer into place (no copy).
    auto t1 = clk::now();
    for (int it = 0; it < N; ++it) {
        auto c = jdoc::emf_extract(d.data(), d.size(), true, true);
        for (auto& b : c.images) {
            std::vector<char> moved = std::move(b);   // move into ImageData
            sink += moved.size() + (moved.empty() ? 0 : (unsigned char)moved.back());
        }
        sink += c.text.size();
    }
    double new_ms = std::chrono::duration<double, std::milli>(clk::now() - t1).count() / N;

    std::cerr << path << " (" << d.size() << " bytes)\n";
    std::cerr << "  old (2 parses + copy): " << old_ms << " ms/call\n";
    std::cerr << "  new (1 pass + move):   " << new_ms << " ms/call\n";
    std::cerr << "  speedup: " << (old_ms / new_ms) << "x  ("
              << (old_ms - new_ms) << " ms saved)\n";
    (void)sink;
    return 0;
}

void test_concurrency() {
    std::cerr << "Thread safety (8 threads x 400 iters on shared buffers):\n";

    TEST("concurrent emf_extract on shared const buffers is race-free & stable")
        // Build a text EMF and an image EMF once; all threads read them.
        auto s = utf16le({'S','a','f','e', 0xD55C, 0xAE00});
        std::vector<uint8_t> text_emf = emf_header();
        append(text_emf, exttextout(0x54, 0, 0, 6, s));
        append(text_emf, emf_eof());

        std::vector<uint8_t> bmi(40, 0);
        auto set32 = [&](std::vector<uint8_t>& v, size_t o, uint32_t x) {
            v[o]=x&0xFF; v[o+1]=(x>>8)&0xFF; v[o+2]=(x>>16)&0xFF; v[o+3]=(x>>24)&0xFF;
        };
        set32(bmi, 0, 40); set32(bmi, 4, 12); set32(bmi, 8, 8);
        bmi[12] = 1; bmi[14] = 24;
        std::vector<uint8_t> bits(24, 0x77);
        std::vector<uint8_t> img_emf = emf_header();
        append(img_emf, stretchdibits(bmi, bits));
        append(img_emf, emf_eof());

        // Single-threaded reference outputs.
        const std::string ref_text = jdoc::emf_extract_text(text_emf.data(), text_emf.size());
        const size_t ref_bmps = jdoc::emf_extract_bitmaps(img_emf.data(), img_emf.size()).size();
        ASSERT(ref_text == "Safe한글");
        ASSERT(ref_bmps == 1);

        std::atomic<int> mismatches{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&]() {
                for (int i = 0; i < 400; ++i) {
                    // combined single-pass on the text buffer
                    auto c = jdoc::emf_extract(text_emf.data(), text_emf.size(), true, true);
                    if (c.text != ref_text) mismatches++;
                    // bitmap extraction on the image buffer
                    auto b = jdoc::emf_extract_bitmaps(img_emf.data(), img_emf.size());
                    if (b.size() != ref_bmps) mismatches++;
                }
            });
        }
        for (auto& th : threads) th.join();
        ASSERT(mismatches.load() == 0);
    TEST_END
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "bench") return bench_file(argv[2]);
    if (argc > 1) return inspect_file(argv[1]);
    std::cerr << "\n=== jdoc::emf_extract_text tests ===\n\n";
    test_exttextout_w();
    test_exttextout_a();
    test_reading_order();
    test_rejects_non_emf();
    test_bitmap_extraction();
    test_end_to_end();
    test_concurrency();
    std::cerr << "\n" << tests_passed << " passed, " << tests_failed
              << " failed\n";
    return tests_failed == 0 ? 0 : 1;
}
