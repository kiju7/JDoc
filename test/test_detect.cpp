// test_detect.cpp - Tests for the public format detection API (jdoc::detect).
// License: MIT

#include "jdoc/detect.h"

#include <cassert>
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

// Detect a synthetic in-memory buffer.
static jdoc::FormatInfo detect_bytes(const std::vector<unsigned char>& b,
                                     const std::string& hint = "") {
    return jdoc::detect(b.data(), b.size(), hint);
}

static void expect(const jdoc::FormatInfo& info, const std::string& fmt,
                   jdoc::FormatCategory cat, bool convertible) {
    if (info.format != fmt)
        throw std::runtime_error("format=" + info.format + " expected " + fmt);
    if (info.category != cat)
        throw std::runtime_error("category mismatch for " + fmt + " (" +
                                 jdoc::format_category_name(info.category) + ")");
    if (info.convertible != convertible)
        throw std::runtime_error("convertible mismatch for " + fmt);
}

// "BM the quick brown fox…" — starts with BM but is plain text.
static std::vector<unsigned char> text_start_bm() {
    std::string s = "BM the quick brown fox jumps over the lazy dog\n";
    return std::vector<unsigned char>(s.begin(), s.end());
}

void test_image_magic() {
    std::cerr << "Image signatures (detect-only, convertible=false):\n";

    TEST("png")
        std::vector<unsigned char> png = {0x89,'P','N','G','\r','\n',0x1a,'\n',0,0,0,0};
        expect(detect_bytes(png), "PNG", jdoc::FormatCategory::Image, false);
    TEST_END

    TEST("jpeg")
        std::vector<unsigned char> jpg = {0xFF,0xD8,0xFF,0xE0,0,0,0,0};
        expect(detect_bytes(jpg), "JPEG", jdoc::FormatCategory::Image, false);
    TEST_END

    TEST("gif")
        std::vector<unsigned char> gif = {'G','I','F','8','9','a',0,0};
        expect(detect_bytes(gif), "GIF", jdoc::FormatCategory::Image, false);
    TEST_END

    TEST("bmp")
        std::vector<unsigned char> bmp = {'B','M',0x36,0,0,0, 0,0,0,0, 0x36};
        expect(detect_bytes(bmp), "BMP", jdoc::FormatCategory::Image, false);
    TEST_END

    TEST("tiff_le")
        std::vector<unsigned char> t = {'I','I',0x2a,0x00,0,0,0,0};
        expect(detect_bytes(t), "TIFF", jdoc::FormatCategory::Image, false);
    TEST_END

    TEST("webp")
        std::vector<unsigned char> w = {'R','I','F','F',0x10,0,0,0,'W','E','B','P'};
        expect(detect_bytes(w), "WEBP", jdoc::FormatCategory::Image, false);
    TEST_END

    TEST("bmp_guard_rejects_text")
        // "BM" text without the zero reserved field must NOT be seen as BMP.
        std::vector<unsigned char> b(text_start_bm());
        auto info = detect_bytes(b, "note.txt");
        ASSERT(info.format != "BMP");
    TEST_END
}

void test_document_magic() {
    std::cerr << "Document / text signatures:\n";

    TEST("pdf")
        std::string s = "%PDF-1.7\n1 0 obj\n";
        std::vector<unsigned char> b(s.begin(), s.end());
        expect(detect_bytes(b), "PDF", jdoc::FormatCategory::Document, true);
    TEST_END

    TEST("text")
        std::string s = "hello world, this is plain text.\n";
        std::vector<unsigned char> b(s.begin(), s.end());
        expect(detect_bytes(b, "readme.txt"), "TXT", jdoc::FormatCategory::Text, true);
    TEST_END

    TEST("rtf")
        std::string s = "{\\rtf1\\ansi test}";
        std::vector<unsigned char> b(s.begin(), s.end());
        auto info = detect_bytes(b, "a.rtf");
        // RTF magic maps to the OFFICE bucket, refined to RTF by name hint.
        ASSERT(info.format == "RTF" || info.format == "OFFICE");
        ASSERT(info.convertible);
    TEST_END

    TEST("unknown")
        std::vector<unsigned char> b = {0x00, 0x01, 0x02, 0x03, 0x00};
        expect(detect_bytes(b), "UNKNOWN", jdoc::FormatCategory::Unknown, false);
    TEST_END
}

void test_real_files(const std::string& root) {
    std::cerr << "Real fixture files:\n";

    TEST("pdf_file")
        auto info = jdoc::detect(root + "/test/fixtures/pdf/sample.pdf");
        expect(info, "PDF", jdoc::FormatCategory::Document, true);
        ASSERT(info.extension == ".pdf");
        ASSERT(info.mime == "application/pdf");
    TEST_END

    TEST("7z_file")
        auto info = jdoc::detect(root + "/test/fixtures/7z/store.7z");
        expect(info, "7Z", jdoc::FormatCategory::Archive, true);
    TEST_END

    TEST("rar_file")
        auto info = jdoc::detect(root + "/test/fixtures/rar/rar5_store.rar");
        expect(info, "RAR", jdoc::FormatCategory::Archive, true);
    TEST_END

    TEST("zip_container")
        // zip64 fixture is a plain zip container of files (not an office pkg).
        auto info = jdoc::detect(root + "/test/fixtures/zip64/small_zip64.zip");
        ASSERT(info.category == jdoc::FormatCategory::Archive);
        ASSERT(info.convertible);
    TEST_END

    TEST("nonexistent_unknown_ext")
        // Unreadable file, no recognizable extension → UNKNOWN.
        auto info = jdoc::detect(root + "/no/such/file.xyzzy");
        expect(info, "UNKNOWN", jdoc::FormatCategory::Unknown, false);
    TEST_END

    TEST("extension_fallback")
        // When bytes decide nothing (here: unreadable), the extension is the
        // fallback — consistent with convert()'s detection. A missing ".pdf"
        // still classifies as PDF by name.
        auto info = jdoc::detect(root + "/no/such/file.pdf");
        expect(info, "PDF", jdoc::FormatCategory::Document, true);
    TEST_END
}

int main(int argc, char** argv) {
    // Fixtures resolve relative to the repo root (working dir set by ctest).
    std::string root = (argc > 1) ? argv[1] : ".";

    std::cerr << "\n=== jdoc::detect tests ===\n\n";
    test_image_magic();
    test_document_magic();
    test_real_files(root);

    std::cerr << "\n" << tests_passed << " passed, " << tests_failed
              << " failed\n";
    return tests_failed == 0 ? 0 : 1;
}
