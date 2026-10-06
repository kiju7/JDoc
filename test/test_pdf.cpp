// test_pdf.cpp — Test PDF to Markdown conversion using PDFium backend
#include "jdoc/pdf.h"
#include "pdf/pdf_extract.h"

#include <iostream>
#include <fstream>
#include <chrono>
#include <string>
#include <vector>
#include <zlib.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ \
                      << ": " #condition "\n"; \
            return 1; \
        } \
    } while (false)


// Fraction of a grayscale PNG's pixels that are dark. Enough of a decoder to
// judge polarity: the fixtures below are 8-bit grayscale, no interlace.
static double dark_fraction(const std::vector<char>& png) {
    auto be32 = [&](size_t i) {
        return (uint32_t(uint8_t(png[i])) << 24) | (uint32_t(uint8_t(png[i+1])) << 16) |
               (uint32_t(uint8_t(png[i+2])) << 8) | uint32_t(uint8_t(png[i+3]));
    };
    if (png.size() < 8) return -1;
    uint32_t w = 0, h = 0; int depth = 0, ctype = -1;
    std::string idat;
    for (size_t p = 8; p + 8 <= png.size();) {
        uint32_t len = be32(p);
        std::string type(png.begin() + p + 4, png.begin() + p + 8);
        if (type == "IHDR") {
            w = be32(p + 8); h = be32(p + 12);
            depth = uint8_t(png[p + 16]); ctype = uint8_t(png[p + 17]);
        } else if (type == "IDAT") {
            idat.append(png.begin() + p + 8, png.begin() + p + 8 + len);
        }
        p += 12 + len;
    }
    if (depth != 8 || ctype != 0 || w == 0 || h == 0) return -1;

    std::vector<unsigned char> raw(size_t(h) * (size_t(w) + 1) * 4);
    uLongf out_len = uLongf(raw.size());
    if (uncompress(raw.data(), &out_len,
                   reinterpret_cast<const Bytef*>(idat.data()),
                   uLong(idat.size())) != Z_OK)
        return -1;

    std::vector<unsigned char> prev(w, 0), cur(w, 0);
    size_t dark = 0, i = 0;
    for (uint32_t y = 0; y < h && i < out_len; y++) {
        unsigned char f = raw[i++];
        for (uint32_t x = 0; x < w && i < out_len; x++, i++) {
            int a = x ? cur[x - 1] : 0, b = prev[x], c = x ? prev[x - 1] : 0;
            int v = raw[i];
            if (f == 1) v += a;
            else if (f == 2) v += b;
            else if (f == 3) v += (a + b) / 2;
            else if (f == 4) {
                int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b),
                    pc = std::abs(pp - c);
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            }
            cur[x] = static_cast<unsigned char>(v);
            if (cur[x] < 128) dark++;
        }
        prev = cur;
    }
    return double(dark) / (double(w) * h);
}

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
        CHECK(!md.empty());
    } catch (const std::exception& e) {
        std::cerr << "    FAIL: " << e.what() << "\n";
        return 1;
    }

    // Test 2: Chunk mode
    std::cout << "[2] Testing chunk mode...\n";
    try {
        auto chunks = jdoc::pdf_to_markdown_chunks(test_pdf);
        std::cout << "    Pages: " << chunks.size() << "\n";
        CHECK(!chunks.empty());
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
        CHECK(!md.empty());
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
        CHECK(!md.empty());
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
                CHECK(chunks.size() == 1);
                CHECK(chunks[0].images.size() == 1);
                CHECK(chunks[0].images[0].format == "png");

                const std::string& md = chunks[0].text;
                size_t refs = 0;
                for (size_t i = md.find("!["); i != std::string::npos;
                     i = md.find("![", i + 2))
                    refs++;
                std::cout << "    Chunk images: " << chunks[0].images.size()
                          << ", image refs: " << refs << " (expected 1 each)\n";
                CHECK(refs == 1);
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
                CHECK(chunks.size() == 1);
                size_t refs = 0;
                const std::string& md = chunks[0].text;
                for (size_t i = md.find("!["); i != std::string::npos;
                     i = md.find("![", i + 2))
                    refs++;
                std::cout << "    " << c.fixture << ": "
                          << chunks[0].images.size() << " images, " << refs
                          << " refs (expected " << c.want_images << ")\n";
                CHECK(chunks[0].images.size() == c.want_images);
                CHECK(refs == c.want_images);
                CHECK(!c.want_text ||
                      md.find(c.want_text) != std::string::npos);
            } catch (const std::exception& e) {
                std::cerr << "    FAIL: " << c.fixture << ": " << e.what()
                          << "\n";
                return 1;
            }
        }
    }

    // Test 7: A fragment page composites whole without losing its text —
    // the strips fold into one page image while every text line stays in
    // the markdown.
    std::cout << "[7] Testing fragment page keeps text...\n";
    {
        const char* fixture = "test/fixtures/pdf/strip_text.pdf";
        std::ifstream f(fixture);
        if (!f.good()) {
            std::cout << "    SKIP: fixture not found\n";
        } else {
            f.close();
            std::string md = jdoc::pdf_to_markdown(fixture);
            size_t refs = 0;
            for (size_t i = md.find("!["); i != std::string::npos;
                 i = md.find("![", i + 2))
                refs++;
            std::cout << "    refs=" << refs << "\n";
            CHECK(refs == 1);
            for (int i = 0; i < 6; i++)
                CHECK(md.find("The quick brown fox " + std::to_string(i)) !=
                      std::string::npos);
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
            CHECK(chunks.size() == 1);
            CHECK(chunks[0].images.size() == 1);

            std::string md =
                jdoc::pdf_to_markdown("test/fixtures/pdf/inline_corrupt.pdf");
            CHECK(md.find("MARKER_TEXT_LINE_A") != std::string::npos);
            CHECK(md.find("MARKER_TEXT_LINE_C") != std::string::npos);
            CHECK(md.find("LEAKED") == std::string::npos);

            auto ahx = jdoc::pdf_to_markdown_chunks(
                "test/fixtures/pdf/inline_ahx.pdf");
            CHECK(ahx.size() == 1 && ahx[0].images.size() == 1);
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
            CHECK(rl.size() == 1 && rl[0].images.size() == 1);

            auto bad = jdoc::pdf_to_markdown_chunks(
                "test/fixtures/pdf/bad_filter.pdf");
            CHECK(bad.size() == 1);
            CHECK(bad[0].images.empty());
            CHECK(bad[0].degraded_images == 1);
            CHECK(bad[0].text.find("failed to decode") != std::string::npos);
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
            CHECK(chunks.size() == 1);
            CHECK(chunks[0].images.size() == 1);
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
            CHECK(chunks.size() == 1);
            CHECK(chunks[0].images.size() == 1);
            CHECK(chunks[0].degraded_images == 0);
            CHECK(chunks[0].images[0].width == 400);
            CHECK(chunks[0].images[0].height == 120);
            std::cout << "    400x120 symbol-text image extracted OK\n";
        }
    }

    // Test 12: Vector figure regions. A booktabs table draws nothing but
    // ruling, so it belongs in the markdown table and must not also come out
    // as a raster; chart panels standing side by side merge their axis and
    // legend labels into page-wide lines, which must not read as body text
    // swallowed by the panel.
    std::cout << "[12] Testing vector figure region gating...\n";
    {
        struct Case {
            const char* fixture;
            size_t want_images;
            const char* want_text;
        };
        const Case cases[] = {
            {"test/fixtures/pdf/booktabs_rules.pdf", 0, "ResNet-152"},
            {"test/fixtures/pdf/panel_charts.pdf", 2, nullptr},
            // A title decoration band (grey fill + edge hairlines) must not
            // become a figure: it collapses to a handful of distinct vertex
            // positions while being far wider than tall.
            {"test/fixtures/pdf/decoration_band.pdf", 0, "Body text"},
            // A figure can be tiled out of rasters each placed as small
            // as a bullet — an attention-map grid, a sheet of glyph
            // samples. Nine of them on one page is a figure and all nine
            // must survive the inline-icon gate...
            {"test/fixtures/pdf/tiled_figure.pdf", 9, "Body text"},
            // ...while a handful of the same marks is decoration, and
            // still is.
            {"test/fixtures/pdf/icon_row.pdf", 0, "Body text"},
        };
        for (auto& c : cases) {
            std::ifstream f(c.fixture);
            if (!f.good()) {
                std::cout << "    SKIP: " << c.fixture << "\n";
                continue;
            }
            f.close();
            auto chunks = jdoc::pdf_to_markdown_chunks(c.fixture);
            CHECK(chunks.size() == 1);
            const std::string& md = chunks[0].text;
            size_t refs = 0;
            for (size_t i = md.find("!["); i != std::string::npos;
                 i = md.find("![", i + 2))
                refs++;
            std::cout << "    " << c.fixture << ": " << refs
                      << " refs (expected " << c.want_images << ")\n";
            CHECK(refs == c.want_images);
            CHECK(chunks[0].images.size() == c.want_images);
            CHECK(!c.want_text || md.find(c.want_text) != std::string::npos);
        }
    }

    // ── CCITT polarity, and a page the file itself replaced ──
    std::cout << "\n[13] Testing CCITT polarity and incremental updates...\n";
    {
        // decode_ccitt hands back the ITU-T convention (1 = black) whatever
        // BlackIs1 said — it takes the flag and ignores it. Applying the flag a
        // second time here turned every scan with the default flag inside out,
        // which is every scan: a page of paper came out 80% black.
        std::ifstream f1("test/fixtures/pdf/ccitt_scan.pdf");
        if (!f1.good()) {
            std::cout << "    SKIP: ccitt_scan.pdf\n";
        } else {
            f1.close();
            jdoc::ConvertOptions o;
            o.images = true;
            o.min_image_size = 0;
            auto chunks = jdoc::pdf_to_markdown_chunks(
                "test/fixtures/pdf/ccitt_scan.pdf", o);
            CHECK(chunks.size() == 1);
            CHECK(chunks[0].images.size() == 1);
            double dark = dark_fraction(chunks[0].images[0].data);
            std::cout << "    ccitt_scan.pdf: dark fraction " << dark
                      << " (paper ~0.20, inverted ~0.80)\n";
            CHECK(dark >= 0.0 && dark < 0.35);
        }

        // An incremental update rewrites objects into a new container while the
        // superseded one stays in the file. Expanding a container cached every
        // object in it, so whichever was reached first won — and the older one
        // is reached first, being numbered lower. The page came back with the
        // previous revision's content stream and without the /Rotate the update
        // had added.
        std::ifstream f2("test/fixtures/pdf/incremental_update.pdf");
        if (!f2.good()) {
            std::cout << "    SKIP: incremental_update.pdf\n";
        } else {
            f2.close();
            auto chunks = jdoc::pdf_to_markdown_chunks(
                "test/fixtures/pdf/incremental_update.pdf");
            CHECK(chunks.size() == 1);
            const std::string& md = chunks[0].text;
            std::cout << "    incremental_update.pdf: "
                      << (md.find("CURRENT REVISION") != std::string::npos
                              ? "current revision" : "STALE revision")
                      << ", page " << chunks[0].page_width << "x"
                      << chunks[0].page_height << "\n";
            CHECK(md.find("CURRENT REVISION") != std::string::npos);
            CHECK(md.find("STALE REVISION") == std::string::npos);
            // /Rotate 90 belongs to the replacement page object, so a landscape
            // page proves the newer object was the one that was read.
            CHECK(chunks[0].page_width > chunks[0].page_height);
        }
    }

    // Test 14: a page whose /Resources has no /Font but draws all of its text
    // through a Form XObject that carries its own /Font. With image extraction
    // disabled the page-level "no fonts, no images" shortcut used to skip the
    // page entirely (134 blank pages on an imposition-produced report).
    std::cout << "[14] Testing text behind a Form XObject with images off...\n";
    {
        std::ifstream f("test/fixtures/pdf/form_only_fonts.pdf");
        if (!f.good()) {
            std::cout << "    SKIP: fixture not found\n";
        } else {
            f.close();
            jdoc::ConvertOptions off;
            off.images = false;
            auto md_off = jdoc::pdf_to_markdown(
                "test/fixtures/pdf/form_only_fonts.pdf", off);
            CHECK(md_off.find("Form only text line") != std::string::npos);
            CHECK(md_off.find("Second line inside the form") != std::string::npos);
            auto md_on = jdoc::pdf_to_markdown(
                "test/fixtures/pdf/form_only_fonts.pdf");
            CHECK(md_on.find("Form only text line") != std::string::npos);
            std::cout << "    form-only text present with images off and on OK\n";
        }
    }

    // Closed cells with a one-line stub and several wrapped prose columns
    // retain their drawn row bands, including a last line in just one cell.
    // The same sparse rules around groups of numeric records still split.
    std::cout << "[38] Testing closed grids with wrapped cell text...\n";
    {
        using namespace jdoc::pdf_detail;
        for (int mode = 0; mode < 5; ++mode) {
            const bool numeric = mode == 1, spaced_records = mode == 2, repeated_stub = mode == 3;
            PageCharCache cache;
            auto put = [&](double x, double y, const std::string& text) {
                for (unsigned char u : text) {
                    cache.chars.push_back({x, y, x, x + 5, y + 8, y - 2,
                                           10, unsigned(u), 0, false});
                    x += 5;
                }
            };
            put(55, 650, "Group");
            for (int c = 0; c < 3; ++c) put(130 + 110 * c, 650, "Heading");
            for (int band = 0; band < 2; ++band) {
                put(55, 600 - 90 * band, band ? "Beta" : "Alpha");
                for (int row = 0; row < 4; ++row)
                    for (int c = 0; c < 3; ++c)
                        put(130 + 110 * c, 621 - 90 * band - row * (spaced_records ? 22 : 14),
                            numeric ? "123456789123456789" : "Wrapped sample text");
                if (repeated_stub)
                    for (int row = 0; row < 4; ++row)
                        put(55, 621 - 90 * band - row * 14, "Item");
                if (!numeric && !spaced_records && !repeated_stub)
                    put(350, band ? 460 : 560, "Final continuation");
            }
            for (size_t i = 0; i < cache.chars.size(); ++i) cache.y_sorted.push_back(i);
            std::stable_sort(cache.y_sorted.begin(), cache.y_sorted.end(),
                [&](size_t a, size_t b) { return cache.chars[a].y < cache.chars[b].y; });
            std::vector<double> levels = {445, 545, 635, 665};
            std::vector<PdfLineSegment> h, v;
            for (double y : levels) h.push_back({40, float(y), 440, float(y)});
            for (float x : {40.f, 110.f, 220.f, 330.f, 440.f}) v.push_back({x, 445, x, 665});
            auto table = build_table(levels, h, v, cache, mode != 4);
            if (numeric || spaced_records || repeated_stub || mode == 4) {
                CHECK(table.rows.size() > 3);
            } else {
                CHECK(table.rows.size() == 3);
                CHECK(table.rows[1][0] == "Alpha");
                CHECK(table.rows[2][0] == "Beta");
                CHECK(table.rows[1][3].find("Final continuation") != std::string::npos);
                CHECK(table.rows[2][3].find("Final continuation") != std::string::npos);
            }
        }
    }



    // The detector must keep a verified wrapped grid past its paragraph-size
    // filter, so the text fallback cannot replace four drawn columns by three.
    {
        using namespace jdoc::pdf_detail;
        PageCharCache cache;
        auto put = [&](double x, double y, const std::string& text) {
            for (unsigned char u : text) {
                cache.chars.push_back({x, y, x, x + 5, y + 8, y - 2,
                                       10, unsigned(u), 0, false});
                x += 5;
            }
        };
        for (int c = 0; c < 4; ++c) put(c ? 130 + (c - 1) * 110 : 55, 707, "Header");
        for (int b = 0; b < 2; ++b) {
            put(55, 550 - b * 300, "Stub");
            for (int r = 0; r < 20; ++r)
                for (int c = 0; c < 3; ++c)
                    put(130 + c * 110, 685 - b * 300 - r * 14, "Wrapped sample text");
        }
        for (size_t i = 0; i < cache.chars.size(); ++i) cache.y_sorted.push_back(i);
        std::stable_sort(cache.y_sorted.begin(), cache.y_sorted.end(),
            [&](size_t a, size_t b) { return cache.chars[a].y < cache.chars[b].y; });
        std::vector<PdfLineSegment> rules;
        for (float y : {100.f, 400.f, 700.f, 720.f}) rules.push_back({40, y, 440, y});
        for (float x : {40.f, 110.f, 220.f, 330.f, 440.f}) rules.push_back({x, 100, x, 720});
        auto tables = detect_tables(rules, cache, 600, 800);
        CHECK(tables.size() == 1);
        CHECK(tables[0].rows.size() == 3);
        CHECK(tables[0].rows[1].size() == 4);
        CHECK(tables[0].rows[1][1].size() > 300);
        CHECK(tables[0].rows[1][0] == "Stub");
    }

    // Compact ruled headers need body evidence for an omitted stub; a frame
    // without a header separator or mismatched body alignment is insufficient.
    std::cout << "[39] Testing compact tables with blank header stubs...\n";
    {
        using namespace jdoc::pdf_detail;
        auto put = [](PageCharCache& cache, double x, double y, const std::string& text) {
            for (unsigned char u : text) {
                cache.chars.push_back({x, y, x, x + 5, y + 8, y - 2,
                                       10, unsigned(u), 0, false});
                x += 5;
            }
        };
        PageCharCache cache;
        put(cache, 110, 650, "Average estimate");
        put(cache, 220, 650, "Maximum value");
        put(cache, 50, 630, "Alpha"); put(cache, 150, 630, "-0.51"); put(cache, 250, 630, "1.52");
        put(cache, 50, 610, "Beta"); put(cache, 150, 610, "0.008"); put(cache, 250, 610, "0.031");
        for (size_t i = 0; i < cache.chars.size(); ++i) cache.y_sorted.push_back(i);
        std::stable_sort(cache.y_sorted.begin(), cache.y_sorted.end(),
            [&](size_t a, size_t b) { return cache.chars[a].y < cache.chars[b].y; });
        std::vector<PdfLineSegment> rules = {
            {40, 660, 300, 660}, {40, 640, 300, 640}, {40, 600, 300, 600}};
        auto tables = detect_text_tables(cache, {}, 600, 800, 0, &rules);
        CHECK(tables.size() == 1);
        CHECK(tables[0].rows.size() == 3);
        CHECK(tables[0].rows[0].size() == 3);
        CHECK(tables[0].rows[0][0].empty());
        CHECK(tables[0].rows[0][1] == "Average estimate");
        CHECK(tables[0].rows[1][0] == "Alpha");
        CHECK(tables[0].rows[2][2] == "0.031");
        CHECK(detect_text_tables(cache, {}, 600, 800).empty());
        rules.erase(rules.begin() + 1);
        CHECK(detect_text_tables(cache, {}, 600, 800, 0, &rules).empty());
    }


    // A blank stub under a filled one is a two-line cell only when actual
    // rules enclose exactly those two physical rows, with vertical separators.
    std::cout << "[40] Testing two-line numeric cells inside ruled bands...\n";
    {
        using namespace jdoc::pdf_detail;
        PageCharCache cache;
        auto put = [&](double x, double y, const std::string& text) {
            for (unsigned char u : text) {
                cache.chars.push_back({x, y, x, x + 4, y + 8, y - 2,
                                       10, unsigned(u), 0, false});
                x += 4;
            }
        };
        for (int c = 0; c < 4; ++c) put(55 + c * 100, 390, "Header");
        put(55, 370, "Size");
        for (int c = 1; c < 4; ++c) {
            put(55 + c * 100, 370, "3M pix");
            put(55 + c * 100, 359, "1440x1920");
        }
        for (int r = 0; r < 3; ++r) {
            put(55, 340 - r * 11, "Item" + std::to_string(r));
            for (int c = 1; c < 4; ++c) put(55 + c * 100, 340 - r * 11, "12345");
        }
        for (size_t i = 0; i < cache.chars.size(); ++i) cache.y_sorted.push_back(i);
        std::stable_sort(cache.y_sorted.begin(), cache.y_sorted.end(),
            [&](size_t a, size_t b) { return cache.chars[a].y < cache.chars[b].y; });
        std::vector<PdfLineSegment> rules;
        for (float y : {310.f, 350.f, 380.f, 400.f}) rules.push_back({40, y, 440, y});
        // Inferred columns fall at the whitespace midpoints near these rules.
        for (float x : {116.f, 224.f, 324.f}) {
            rules.push_back({x, 350, x, 365});
            rules.push_back({x, 365, x, 380});
        }
        auto tables = detect_text_tables(cache, {}, 600, 800, 0, &rules);
        CHECK(tables.size() == 1);
        CHECK(tables[0].rows.size() == 5);
        CHECK(tables[0].rows[1][0] == "Size");
        CHECK(tables[0].rows[1][1] == "3M pix 1440x1920");
        CHECK(tables[0].rows[2][0] == "Item0");
        CHECK(tables[0].rows[4][0] == "Item2");
        auto no_rules = detect_text_tables(cache, {}, 600, 800);
        CHECK(no_rules.size() == 1);
        CHECK(no_rules[0].rows.size() == 6);
        auto separated = rules;
        separated.push_back({40, 365, 440, 365});
        auto independent = detect_text_tables(cache, {}, 600, 800, 0, &separated);
        CHECK(independent.size() == 1);
        CHECK(independent[0].rows.size() == 6);
    }
    std::cout << "\n=== All tests passed ===\n";
    return 0;
}
