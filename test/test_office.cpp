// test_office.cpp - Basic tests for Office document to Markdown conversion
// License: MIT

#include "jdoc/office.h"
#include "zip_reader.h"
#include "legacy/ole_reader.h"
#include <iostream>
#include <cassert>
#include <fstream>
#include <cstring>

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

// ── Tests ───────────────────────────────────────────────────

void test_format_detection() {
    std::cerr << "Format Detection:\n";

    TEST(unknown_format)
        auto fmt = jdoc::detect_office_format("/nonexistent/file.xyz");
        ASSERT(fmt == jdoc::DocFormat::UNKNOWN);
    TEST_END

    TEST(format_name)
        ASSERT(std::string(jdoc::format_name(jdoc::DocFormat::DOCX)) == "DOCX");
        ASSERT(std::string(jdoc::format_name(jdoc::DocFormat::XLSX)) == "XLSX");
        ASSERT(std::string(jdoc::format_name(jdoc::DocFormat::PPTX)) == "PPTX");
        ASSERT(std::string(jdoc::format_name(jdoc::DocFormat::DOC)) == "DOC");
        ASSERT(std::string(jdoc::format_name(jdoc::DocFormat::XLS)) == "XLS");
        ASSERT(std::string(jdoc::format_name(jdoc::DocFormat::PPT)) == "PPT");
        ASSERT(std::string(jdoc::format_name(jdoc::DocFormat::RTF)) == "RTF");
    TEST_END
}

void test_zip_reader() {
    std::cerr << "\nZipReader:\n";

    TEST(open_nonexistent)
        jdoc::ZipReader zip("/nonexistent/file.zip");
        ASSERT(!zip.is_open());
    TEST_END

    TEST(has_entry_empty)
        jdoc::ZipReader zip("/nonexistent/file.zip");
        ASSERT(!zip.has_entry("test.txt"));
    TEST_END
}

void test_ole_reader() {
    std::cerr << "\nOleReader:\n";

    TEST(open_nonexistent)
        jdoc::OleReader ole("/nonexistent/file.doc");
        ASSERT(!ole.is_open());
    TEST_END

    TEST(list_streams_empty)
        jdoc::OleReader ole("/nonexistent/file.doc");
        auto streams = ole.list_streams();
        ASSERT(streams.empty());
    TEST_END
}

void test_rtf_parser() {
    std::cerr << "\nRTF Parser:\n";

    const char* rtf_content = "{\\rtf1\\ansi\\deff0"
        "{\\fonttbl{\\f0 Times New Roman;}}"
        "\\pard Hello \\b World\\b0 !\\par"
        "This is a \\i test\\i0  document.\\par"
        "}";

    std::string rtf_path = "/tmp/test_jdoc_office.rtf";
    {
        std::ofstream f(rtf_path, std::ios::binary);
        f.write(rtf_content, strlen(rtf_content));
    }

    TEST(detect_rtf_format)
        auto fmt = jdoc::detect_office_format(rtf_path);
        ASSERT(fmt == jdoc::DocFormat::RTF);
    TEST_END

    TEST(rtf_to_markdown)
        auto md = jdoc::office_to_markdown(rtf_path);
        ASSERT(!md.empty());
        ASSERT(md.find("Hello") != std::string::npos);
        ASSERT(md.find("**World**") != std::string::npos || md.find("World") != std::string::npos);
    TEST_END

    TEST(rtf_to_chunks)
        auto chunks = jdoc::office_to_markdown_chunks(rtf_path);
        ASSERT(!chunks.empty());
        ASSERT(chunks[0].page_number >= 0);
        ASSERT(!chunks[0].text.empty());
    TEST_END

    std::remove(rtf_path.c_str());
}

// ── Main ────────────────────────────────────────────────────

int main() {
    std::cerr << "=== jdoc office tests ===\n\n";

    test_format_detection();
    test_zip_reader();
    test_ole_reader();
    test_rtf_parser();

    std::cerr << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===\n";

    return tests_failed > 0 ? 1 : 0;
}
