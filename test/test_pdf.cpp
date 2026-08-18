// test_pdf.cpp — Test PDF to Markdown conversion using PDFium backend
#include "jdoc/pdf.h"

#include <iostream>
#include <fstream>
#include <cassert>
#include <chrono>

int main(int argc, char* argv[]) {
    const char* test_pdf = (argc > 1) ? argv[1] : "test/fixtures/pdf/sample.pdf";

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
        opts.tables = false;
        std::string md = jdoc::pdf_to_markdown(test_pdf, opts);
        std::cout << "    No tables: " << md.size() << " bytes\n";
        assert(!md.empty());
    } catch (const std::exception& e) {
        std::cerr << "    FAIL: " << e.what() << "\n";
        return 1;
    }

    // Test 5: Raster drawing primitives composite instead of fragmenting.
    // The fixture models a print pipeline that emits text as image strips.
    // Those XObjects are one page drawing, so both Markdown and the chunk API
    // must expose exactly one PNG rather than the constituent strips.
    std::cout << "[5] Testing line-raster page compositing...\n";
    {
        const char* fixture = "test/fixtures/pdf/line_raster.pdf";
        std::ifstream f(fixture);
        if (!f.good()) {
            std::cout << "    SKIP: fixture not found\n";
        } else {
            f.close();
            try {
                auto chunks = jdoc::pdf_to_markdown_chunks(fixture);
                assert(chunks.size() == 1);
                assert(chunks[0].images.size() == 1);
                assert(chunks[0].images[0].format == "png");

                const std::string& md = chunks[0].text;
                size_t refs = 0;
                for (size_t i = md.find("!["); i != std::string::npos;
                     i = md.find("![", i + 2))
                    refs++;
                std::cout << "    Chunk images: " << chunks[0].images.size()
                          << ", image refs: " << refs << " (expected 1 each)\n";
                assert(refs == 1);
            } catch (const std::exception& e) {
                std::cerr << "    FAIL: " << e.what() << "\n";
                return 1;
            }
        }
    }

    // Test 6: Fragment clustering. Abutting placements (tiles, strips —
    // horizontal or vertical, above or below min_image_size, on pages with
    // or without text) composite into one region image; scattered assets
    // stay individual; repeated stamps deduplicate; an invisible-text (Tr 3)
    // OCR layer does not stop a shredded scan from compositing.
    std::cout << "[6] Testing fragment clustering fixtures...\n";
    {
        struct Case {
            const char* fixture;
            size_t want_images;
            const char* want_text; // must appear in the markdown, or nullptr
        };
        const Case cases[] = {
            {"test/fixtures/pdf/tile_grid.pdf", 1, nullptr},
            {"test/fixtures/pdf/vstrip.pdf", 1, nullptr},
            {"test/fixtures/pdf/strip_text.pdf", 1, "The quick brown fox"},
            {"test/fixtures/pdf/tiny_frags.pdf", 1, "Heading line"},
            {"test/fixtures/pdf/photo_stencil.pdf", 2, "Body text line"},
            {"test/fixtures/pdf/dedup_logo.pdf", 1, "Some report text"},
            {"test/fixtures/pdf/tr3_scan.pdf", 1, "Invisible ocr line"},
        };
        for (auto& c : cases) {
            std::ifstream f(c.fixture);
            if (!f.good()) {
                std::cout << "    SKIP: " << c.fixture << "\n";
                continue;
            }
            f.close();
            try {
                auto chunks = jdoc::pdf_to_markdown_chunks(c.fixture);
                assert(chunks.size() == 1);
                size_t refs = 0;
                const std::string& md = chunks[0].text;
                for (size_t i = md.find("!["); i != std::string::npos;
                     i = md.find("![", i + 2))
                    refs++;
                std::cout << "    " << c.fixture << ": "
                          << chunks[0].images.size() << " images, " << refs
                          << " refs (expected " << c.want_images << ")\n";
                assert(chunks[0].images.size() == c.want_images);
                assert(refs == c.want_images);
                assert(!c.want_text ||
                       md.find(c.want_text) != std::string::npos);
            } catch (const std::exception& e) {
                std::cerr << "    FAIL: " << c.fixture << ": " << e.what()
                          << "\n";
                return 1;
            }
        }
    }

    // Test 7: Region composites keep reading order. strip_text's strip block
    // sits below its six text lines, so the image ref must follow the text —
    // the old behavior pinned composites to the top of the page.
    std::cout << "[7] Testing region composite reading order...\n";
    {
        const char* fixture = "test/fixtures/pdf/strip_text.pdf";
        std::ifstream f(fixture);
        if (!f.good()) {
            std::cout << "    SKIP: fixture not found\n";
        } else {
            f.close();
            std::string md = jdoc::pdf_to_markdown(fixture);
            size_t last_text = md.rfind("quick brown fox");
            size_t ref = md.find("![");
            assert(last_text != std::string::npos);
            assert(ref != std::string::npos);
            std::cout << "    text@" << last_text << " ref@" << ref << "\n";
            assert(ref > last_text);
        }
    }

    // Test 8: Inline images (BI/ID/EI). The payload must never leak into the
    // operator loop: inline_corrupt's samples spell "(LEAKED) Tj", which
    // would show up as text if the lexer read them as tokens.
    std::cout << "[8] Testing inline images...\n";
    {
        const char* basic = "test/fixtures/pdf/inline_basic.pdf";
        std::ifstream f(basic);
        if (!f.good()) {
            std::cout << "    SKIP: fixture not found\n";
        } else {
            f.close();
            auto chunks = jdoc::pdf_to_markdown_chunks(basic);
            assert(chunks.size() == 1);
            assert(chunks[0].images.size() == 1);

            std::string md =
                jdoc::pdf_to_markdown("test/fixtures/pdf/inline_corrupt.pdf");
            assert(md.find("MARKER_TEXT_LINE_A") != std::string::npos);
            assert(md.find("MARKER_TEXT_LINE_C") != std::string::npos);
            assert(md.find("LEAKED") == std::string::npos);

            auto ahx = jdoc::pdf_to_markdown_chunks(
                "test/fixtures/pdf/inline_ahx.pdf");
            assert(ahx.size() == 1 && ahx[0].images.size() == 1);
            std::cout << "    inline basic/corrupt/ahx OK\n";
        }
    }

    // Test 9: RunLengthDecode images decode; an unknown filter surfaces as
    // degraded_images plus a markdown comment instead of vanishing.
    std::cout << "[9] Testing RunLength and decode diagnostics...\n";
    {
        std::ifstream f("test/fixtures/pdf/rl_image.pdf");
        if (!f.good()) {
            std::cout << "    SKIP: fixture not found\n";
        } else {
            f.close();
            auto rl = jdoc::pdf_to_markdown_chunks(
                "test/fixtures/pdf/rl_image.pdf");
            assert(rl.size() == 1 && rl[0].images.size() == 1);

            auto bad = jdoc::pdf_to_markdown_chunks(
                "test/fixtures/pdf/bad_filter.pdf");
            assert(bad.size() == 1);
            assert(bad[0].images.empty());
            assert(bad[0].degraded_images == 1);
            assert(bad[0].text.find("failed to decode") != std::string::npos);
            std::cout << "    rl_image 1 image, bad_filter degraded=1 OK\n";
        }
    }

    // Test 10: /Resources inherited from an ancestor Pages node two levels up
    // used to yield zero images from both extraction and compositing.
    std::cout << "[10] Testing inherited /Resources...\n";
    {
        std::ifstream f("test/fixtures/pdf/inherited_res.pdf");
        if (!f.good()) {
            std::cout << "    SKIP: fixture not found\n";
        } else {
            f.close();
            auto chunks = jdoc::pdf_to_markdown_chunks(
                "test/fixtures/pdf/inherited_res.pdf");
            assert(chunks.size() == 1);
            assert(chunks[0].images.size() == 1);
            std::cout << "    1 image extracted OK\n";
        }
    }

    // Test 11: JBIG2 symbol dictionary + text region through the PDF path
    // (dictionary in /JBIG2Globals). These used to decode to nothing.
    std::cout << "[11] Testing JBIG2 symbol text extraction...\n";
    {
        std::ifstream f("test/fixtures/pdf/jbig2_symtext.pdf");
        if (!f.good()) {
            std::cout << "    SKIP: fixture not found\n";
        } else {
            f.close();
            auto chunks = jdoc::pdf_to_markdown_chunks(
                "test/fixtures/pdf/jbig2_symtext.pdf");
            assert(chunks.size() == 1);
            assert(chunks[0].images.size() == 1);
            assert(chunks[0].degraded_images == 0);
            assert(chunks[0].images[0].width == 400);
            assert(chunks[0].images[0].height == 120);
            std::cout << "    400x120 symbol-text image extracted OK\n";
        }
    }

    std::cout << "\n=== All tests passed ===\n";
    return 0;
}
