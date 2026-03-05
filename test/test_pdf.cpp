// test_pdf.cpp — Test PDF to Markdown conversion using PDFium backend
#include "jdoc/pdf.h"

#include <iostream>
#include <fstream>
#include <cassert>
#include <chrono>

int main(int argc, char* argv[]) {
    const char* test_pdf = (argc > 1) ? argv[1] : "test/sample.pdf";

    std::cout << "=== jdoc PDF Test ===\n\n";

    std::ifstream check(test_pdf);
    if (!check.good()) {
        std::cerr << "Test PDF not found: " << test_pdf << "\n";
        std::cerr << "Usage: test_pdf [path/to/test.pdf]\n";
        return 1;
    }
    check.close();

    // Test 1: Basic conversion
    std::cout << "[1] Converting to Markdown...\n";
    try {
        auto t0 = std::chrono::high_resolution_clock::now();
        std::string md = jdoc::pdf_to_markdown(test_pdf);
        auto t1 = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(t1 - t0).count();

        std::cout << "    Time: " << elapsed << "s\n";
        std::cout << "    Output length: " << md.size() << " bytes\n";
        assert(!md.empty());
    } catch (const std::exception& e) {
        std::cerr << "    FAIL: " << e.what() << "\n";
        return 1;
    }

    // Test 2: Chunk mode
    std::cout << "[2] Testing chunk mode...\n";
    try {
        auto chunks = jdoc::pdf_to_markdown_chunks(test_pdf);
        std::cout << "    Pages: " << chunks.size() << "\n";
        assert(!chunks.empty());
        for (auto& c : chunks) {
            std::cout << "    Page " << c.page_number
                      << ": " << c.text.size() << " bytes"
                      << ", " << c.tables.size() << " tables"
                      << ", body=" << c.body_font_size << "pt\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "    FAIL: " << e.what() << "\n";
        return 1;
    }

    // Test 3: Selective page
    std::cout << "[3] Testing selective page...\n";
    try {
        jdoc::ConvertOptions opts;
        opts.pages = {0};
        std::string md = jdoc::pdf_to_markdown(test_pdf, opts);
        std::cout << "    Page 0 only: " << md.size() << " bytes\n";
        assert(!md.empty());
    } catch (const std::exception& e) {
        std::cerr << "    FAIL: " << e.what() << "\n";
        return 1;
    }

    // Test 4: No tables mode
    std::cout << "[4] Testing no-tables mode...\n";
    try {
        jdoc::ConvertOptions opts;
        opts.extract_tables = false;
        std::string md = jdoc::pdf_to_markdown(test_pdf, opts);
        std::cout << "    No tables: " << md.size() << " bytes\n";
        assert(!md.empty());
    } catch (const std::exception& e) {
        std::cerr << "    FAIL: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\n=== All tests passed ===\n";
    return 0;
}
