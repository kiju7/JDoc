// test_pdf.cpp — Test PDF to Markdown conversion using PDFium backend
#include "jdoc/pdf.h"
#include "pdf/pdf_base14.h"
#include "pdf/pdf_core.h"

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
// [fx0, fx1) limits the count to that horizontal band of the image.
static double dark_fraction(const std::vector<char>& png, double fx0 = 0.0,
                            double fx1 = 1.0) {
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
    const uint32_t bx0 = uint32_t(fx0 * w), bx1 = uint32_t(fx1 * w);
    if (bx1 <= bx0) return -1;
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
            if (cur[x] < 128 && x >= bx0 && x < bx1) dark++;
        }
        prev = cur;
    }
    return double(dark) / (double(bx1 - bx0) * h);
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

    // Test 15: standard 14 fonts with no /Widths and no font program (PDF
    // 32000-1 9.6.2.2). Every glyph used to get one fallback width, so word
    // boxes overran the next word: words placed one by one with no space
    // glyph ran together and interleaved. The metrics are compiled in, so
    // this holds on any OS regardless of installed fonts.
    std::cout << "[15] Testing standard 14 fonts without /Widths...\n";
    {
        using namespace jdoc::pdf_detail;
        // Name resolution: aliases, style suffixes, subset tags.
        auto face = [](const char* n) {
            const Base14Font* f = find_base14_font(n);
            return std::string(f ? f->name : "");
        };
        CHECK(face("Helvetica") == "Helvetica");
        CHECK(face("ABCDEF+Helvetica-BoldOblique") == "Helvetica-BoldOblique");
        CHECK(face("Arial") == "Helvetica");
        CHECK(face("ArialMT") == "Helvetica");
        CHECK(face("Arial,Bold") == "Helvetica-Bold");
        CHECK(face("Arial-BoldItalicMT") == "Helvetica-BoldOblique");
        CHECK(face("Times New Roman,Italic") == "Times-Italic");
        CHECK(face("TimesNewRomanPSMT") == "Times-Roman");
        CHECK(face("TimesNewRomanPS-BoldMT") == "Times-Bold");
        CHECK(face("TimesNewRoman,BoldItalic") == "Times-BoldItalic");
        CHECK(face("CourierNewPSMT") == "Courier");
        CHECK(face("CourierNew,Bold") == "Courier-Bold");
        CHECK(face("Courier-Oblique") == "Courier-Oblique");
        CHECK(face("Symbol") == "Symbol");
        CHECK(face("ZapfDingbats") == "ZapfDingbats");
        // Different metrics: not a standard face.
        CHECK(face("ArialNarrow").empty());
        CHECK(face("Arial-Black").empty());
        CHECK(face("Helvetica-Light").empty());
        CHECK(face("ArialUnicodeMS").empty());
        CHECK(face("Calibri").empty());

        const uint8_t placeholder = 0;
        PdfDoc doc(&placeholder, 1);
        auto font_dict = [](const char* subtype, const char* base) {
            auto d = PdfObj::make_dict();
            d.dict.push_back({"Type", PdfObj::make_name("Font")});
            d.dict.push_back({"Subtype", PdfObj::make_name(subtype)});
            d.dict.push_back({"BaseFont", PdfObj::make_name(base)});
            return d;
        };
        // AFM widths through the default StandardEncoding (quoteright at 0x27).
        auto helv = load_font(doc, font_dict("Type1", "Helvetica"));
        CHECK(helv.get_width('i') == 222);
        CHECK(helv.get_width('W') == 944);
        CHECK(helv.get_width(' ') == 278);
        CHECK(helv.get_width(0x27) == 222);
        CHECK(helv.decode_char(0x27) == 0x2019);
        // WinAnsi: accented letters; /Differences by glyph name.
        auto tb = font_dict("Type1", "Times-Bold");
        auto enc = PdfObj::make_dict();
        enc.dict.push_back({"BaseEncoding", PdfObj::make_name("WinAnsiEncoding")});
        auto diffs = PdfObj::make_arr();
        diffs.arr.push_back(PdfObj::make_int(1));
        diffs.arr.push_back(PdfObj::make_name("fi"));
        enc.dict.push_back({"Differences", diffs});
        tb.dict.push_back({"Encoding", enc});
        auto times_bold = load_font(doc, tb);
        CHECK(times_bold.get_width(0xE9) == 444);  // eacute
        CHECK(times_bold.get_width(1) == 556);     // fi
        CHECK(times_bold.get_width('a') == 500);
        // An alias picks up its standard face's metrics.
        auto arial = font_dict("TrueType", "Arial,Bold");
        arial.dict.push_back({"Encoding", PdfObj::make_name("WinAnsiEncoding")});
        CHECK(load_font(doc, arial).get_width('i') == 278);
        // Symbol reads through its own built-in encoding.
        auto sym = load_font(doc, font_dict("Type1", "Symbol"));
        CHECK(sym.decode_char('a') == 0x03B1);
        CHECK(sym.get_width('a') == 631);
        // Widths given by the PDF always win.
        auto given = font_dict("Type1", "Helvetica");
        given.dict.push_back({"FirstChar", PdfObj::make_int(105)});
        auto warr = PdfObj::make_arr();
        warr.arr.push_back(PdfObj::make_int(500));
        given.dict.push_back({"Widths", warr});
        auto helv_given = load_font(doc, given);
        CHECK(helv_given.get_width('i') == 500);
        CHECK(helv_given.get_width('W') == 0);  // outside /Widths: not filled in
        // Not a standard face: nothing invented.
        CHECK(load_font(doc, font_dict("TrueType", "Calibri")).get_width('i') == 0);

        // End to end: words placed one by one at their real positions.
        std::ifstream f("test/fixtures/pdf/base14_nowidths.pdf");
        if (!f.good()) {
            std::cout << "    SKIP: fixture not found\n";
        } else {
            f.close();
            jdoc::ConvertOptions text_opts;
            text_opts.format = jdoc::OutputFormat::PLAINTEXT;
            for (const auto& out :
                 {jdoc::pdf_to_markdown("test/fixtures/pdf/base14_nowidths.pdf"),
                  jdoc::pdf_to_markdown("test/fixtures/pdf/base14_nowidths.pdf", text_opts)}) {
                CHECK(out.find("illicit little lilies fill it, will Bill?") != std::string::npos);
                CHECK(out.find("Minimal wiggly lily fills") != std::string::npos);
                CHECK(out.find("fill it till I lift") != std::string::npos);
                CHECK(out.find("Caf\xC3\xA9 menu is closed") != std::string::npos);
                CHECK(out.find("\xCE\xB1 \xCE\xB2 \xCE\xB3") != std::string::npos);  // α β γ
                CHECK(out.find("\xE2\x9C\x94") != std::string::npos);                // ✔
                CHECK(out.find("Total 1234.12") != std::string::npos);
            }
            std::cout << "    base14 metrics, aliases and word spacing OK\n";
        }
    }

    // Test 16: rotated text is laid out in its own reading direction in
    // plain text. A table turned 90/180/270 degrees used to come out glued
    // ("ItemQ1Q2Q3"): word gaps and lines were measured on the page axes,
    // and gaps over 8 em counted as no gap at all. Sideways prose beside
    // upright text keeps its spaces.
    std::cout << "[16] Testing rotated text in plain text and markdown...\n";
    {
        const char* fixture = "test/fixtures/pdf/rotated_text.pdf";
        std::ifstream f(fixture);
        if (!f.good()) {
            std::cout << "    SKIP: fixture not found\n";
        } else {
            f.close();
            jdoc::ConvertOptions text_opts;
            text_opts.format = jdoc::OutputFormat::PLAINTEXT;
            auto pages = jdoc::pdf_to_markdown_chunks(fixture, text_opts);
            CHECK(pages.size() == 4);
            // Cells of one row stay on one line, in reading order, with a
            // column gap between them, whichever way the page is turned.
            auto row_ok = [](const std::string& t, const std::vector<std::string>& cells) {
                size_t line_start = t.find(cells[0]);
                if (line_start == std::string::npos) return false;
                size_t line_end = t.find('\n', line_start);
                std::string line = t.substr(line_start, line_end - line_start);
                size_t pos = 0;
                for (size_t k = 0; k < cells.size(); k++) {
                    size_t at = line.find(cells[k], pos);
                    if (at == std::string::npos) return false;
                    if (k > 0 && at < pos + 2) return false;  // a column gap
                    pos = at + cells[k].size();
                }
                return true;
            };
            for (int p = 0; p < 3; p++) {
                const std::string& t = pages[p].text;
                CHECK(t.find("ItemQ1") == std::string::npos);
                CHECK(t.find("Row0111") == std::string::npos);
                CHECK(row_ok(t, {"Item", "Q1", "Q2", "Q3"}));
                CHECK(row_ok(t, {"Row01", "11", "21", "31"}));
                CHECK(row_ok(t, {"Row05", "15", "25", "35"}));
                CHECK(t.find("Item") < t.find("Row01"));
                CHECK(t.find("Row01") < t.find("Row05"));
            }
            CHECK(pages[3].text.find("Upright body text stays on the page grid.") !=
                  std::string::npos);
            // ...and a rotated ligature keeps both of its letters.
            CHECK(pages[3].text.find("Sideways caption with five words offset") !=
                  std::string::npos);

            // Markdown: a 180-degree run reads first line first, and the
            // lines of a 90/270-degree run (which share one page-space
            // midpoint) are not welded into one line.
            auto md = jdoc::pdf_to_markdown_chunks(fixture);
            CHECK(md.size() == 4);
            for (int p = 0; p < 3; p++) {
                const std::string& t = md[p].text;
                CHECK(t.find("Q3 Row01") == std::string::npos);
                CHECK(t.find("Row01") != std::string::npos);
                CHECK(t.find("Item") < t.find("Row01"));
                // Each row once: the layout block replaces its lines.
                CHECK(t.find("Row03") == t.rfind("Row03"));
            }
            CHECK(md[3].text.find("Sideways caption with five words offset") !=
                  std::string::npos);
            std::cout << "    90/180/270 tables and a sideways caption OK\n";
        }
    }

    // Test 17: word spaces between runs. Two runs far apart on one baseline
    // (a footer and its page number) used to glue ("2024" + "99"): gaps
    // over 8 em were dropped. A written space set off the baseline and
    // kerned below a word space still separates; a break space kerned out
    // entirely does not; Tc-spread, kerned-tight and glyph-per-Td words stay
    // whole; a page number beside a running head is not a section heading.
    std::cout << "[17] Testing word spaces between runs...\n";
    {
        const char* fixture = "test/fixtures/pdf/far_gap.pdf";
        std::ifstream f(fixture);
        if (!f.good()) {
            std::cout << "    SKIP: fixture not found\n";
        } else {
            f.close();
            jdoc::ConvertOptions text_opts;
            text_opts.format = jdoc::OutputFormat::PLAINTEXT;
            for (int mode = 0; mode < 2; mode++) {
                auto pages = mode == 0 ? jdoc::pdf_to_markdown_chunks(fixture)
                                       : jdoc::pdf_to_markdown_chunks(fixture, text_opts);
                CHECK(pages.size() == 2);
                const std::string& t = pages[0].text;
                CHECK(t.find("202499") == std::string::npos);
                CHECK(t.find("Report 2024") != std::string::npos);
                CHECK(t.find("99") != std::string::npos);
                CHECK(t.find("SPREAD") != std::string::npos);
                CHECK(t.find("Kerned") != std::string::npos);
                CHECK(t.find("Glyphs") != std::string::npos);
                CHECK(t.find("Maria Hazel") != std::string::npos);
                const std::string& t2 = pages[1].text;
                CHECK(t2.find("4Annual") == std::string::npos);
                if (mode == 0) {
                    CHECK(t2.find("## 4") == std::string::npos);
                    CHECK(t2.find("## 1 Introduction") != std::string::npos);
                } else {
                    // The grid follows geometry: no space where none shows.
                    CHECK(t.find("www.example.org") != std::string::npos);
                }
            }
            std::cout << "    far runs, written spaces and whole words OK\n";
        }
    }

    // Test 18: table layouts (fixtures from make_table_fixtures.py).
    std::cout << "[18] Testing table layouts...\n";
    {
        using Rows = std::vector<std::vector<std::string>>;
        auto tables_of = [](const char* path) {
            std::ifstream f(path);
            if (!f.good()) {
                std::cerr << "    missing fixture " << path << "\n";
                return std::vector<Rows>{{{"<missing>"}}};
            }
            auto chunks = jdoc::pdf_to_markdown_chunks(path);
            return chunks.empty() ? std::vector<Rows>{} : chunks[0].tables;
        };
        auto find_table = [](const std::vector<Rows>& ts, const std::string& first) {
            for (auto& t : ts)
                if (!t.empty() && !t[0].empty() && t[0][0] == first) return &t;
            return static_cast<const Rows*>(nullptr);
        };

        // A label-column table in one page column whose rule heights match
        // a grid in the other page column, with full-width section rows that
        // carry no column rule: one table of six rows, not two rejected
        // halves, and the grid beside it stays its own table.
        {
            auto ts = tables_of("test/fixtures/pdf/tables_side_by_side.pdf");
            CHECK(ts.size() == 2);
            const Rows* t = find_table(ts, "Section one header");
            CHECK(t && t->size() == 6);
            CHECK((*t)[3][0] == "Section two header");
            CHECK(((*t)[5] == std::vector<std::string>{"Labels", "('O', 'O', 'B')"}));
            const Rows* g = find_table(ts, "Set");
            CHECK(g && g->size() == 4);
            std::cout << "    side-by-side page columns OK\n";
        }
        // Three grids stacked under their captions beside a framed figure:
        // three tables, no caption inside a table.
        {
            auto ts = tables_of("test/fixtures/pdf/tables_stacked_frame.pdf");
            CHECK(ts.size() == 3);
            for (auto& t : ts) {
                CHECK(t.size() == 4);
                CHECK((t[0] == std::vector<std::string>{"sample", "result", "sample", "result"}));
                for (auto& row : t)
                    for (auto& c : row) CHECK(c.rfind("Table ", 0) != 0);
            }
            std::cout << "    stacked grids beside a frame OK\n";
        }
        // Two closed two-row grids 34pt apart are two tables (a two-column
        // key/value box included); a box with an empty cell in each row is
        // none.
        {
            auto ts = tables_of("test/fixtures/pdf/tables_small_boxes.pdf");
            CHECK(ts.size() == 2);
            const Rows* kv = find_table(ts, "Platform");
            const Rows* res = find_table(ts, "Model");
            CHECK(kv && (*kv == Rows{{"Platform", "Linux"}, {"Framework", "PyTorch 2.1"}}));
            CHECK(res && (*res == Rows{{"Model", "Acc", "F1"}, {"Ours", "91.2", "88.7"}}));
            std::cout << "    small closed grids OK\n";
        }
        // A wide label column gets no phantom empty column.
        {
            auto ts = tables_of("test/fixtures/pdf/tables_wide_label.pdf");
            CHECK(ts.size() == 1);
            CHECK((ts[0][0] == std::vector<std::string>{"Item", "2017", "2018", "2019", "2020"}));
            CHECK(ts[0].size() == 4);
            std::cout << "    wide label column OK\n";
        }
        // A table caption between two borderless tables separates them.
        {
            auto ts = tables_of("test/fixtures/pdf/tables_caption_between.pdf");
            CHECK(ts.size() == 2);
            const Rows* a = find_table(ts, "Item");
            const Rows* b = find_table(ts, "Code");
            CHECK(a && a->size() == 4);
            CHECK(b && b->size() == 4);
            CHECK(((*b)[0] == std::vector<std::string>{"Code", "Min", "Max", "Avg"}));
            std::cout << "    caption between tables OK\n";
        }
        // Sparse columns keep their own boundaries: a last column filled in
        // four of ten rows and an inner one filled in three.
        {
            auto ts = tables_of("test/fixtures/pdf/tables_sparse_columns.pdf");
            CHECK(ts.size() == 1);
            const Rows& t = ts[0];
            CHECK(t.size() == 11);
            CHECK((t[0] == std::vector<std::string>{"Region", "2019", "2020", "2021", "2022", "2023", "2024"}));
            CHECK((t[2] == std::vector<std::string>{"Busan", "", "43.1", "0.7", "71.6", "", "89.3"}));
            CHECK((t[5] == std::vector<std::string>{"Daejeon", "55.3", "18.8", "85.2", "", "71.6", "92.7"}));
            CHECK((t[9] == std::vector<std::string>{"Suwon", "97.2", "53.7", "23.4", "94.3", "45.7", ""}));
            std::cout << "    sparse columns OK\n";
        }
    }

    // Test 19: text drawn over a single raster. A chart saved as one JPEG
    // with its title, values and axis labels typed over it must come out
    // composited (PNG, labels drawn), while the original JPEG passes through
    // when nothing is drawn over it (or only under it), when the raster is a
    // page-sized background, when body text runs across it, and when the
    // only text on it is an invisible (Tr 3) OCR layer. The text stays in the
    // markdown in every case.
    std::cout << "[19] Testing text drawn over a single raster...\n";
    {
        struct Case {
            const char* fixture;
            const char* want_format;
            const char* want_text;
        };
        const Case cases[] = {
            {"test/fixtures/pdf/overlay_chart.pdf", "png", "Quarterly"},
            {"test/fixtures/pdf/overlay_none.pdf", "jpeg", "Figure"},
            {"test/fixtures/pdf/overlay_page_background.pdf", "jpeg", "Annual"},
            {"test/fixtures/pdf/overlay_backdrop.pdf", "jpeg", "Body"},
            {"test/fixtures/pdf/overlay_ocr_figure.pdf", "jpeg", "Invisible"},
            {"test/fixtures/pdf/overlay_hidden_text.pdf", "jpeg", "Hidden"},
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
            std::cout << "    " << c.fixture << ": "
                      << chunks[0].images.size() << " images, "
                      << (chunks[0].images.empty() ? "-" : chunks[0].images[0].format)
                      << " (expected 1, " << c.want_format << ")\n";
            CHECK(chunks[0].images.size() == 1);
            CHECK(chunks[0].images[0].format == c.want_format);
            CHECK(chunks[0].text.find(c.want_text) != std::string::npos);
        }
        // The composite is the picture's region (its labels included), not
        // the page: wider than tall like the placement, far from page-sized.
        auto chunks = jdoc::pdf_to_markdown_chunks("test/fixtures/pdf/overlay_chart.pdf");
        if (chunks.size() == 1 && chunks[0].images.size() == 1) {
            const auto& img = chunks[0].images[0];
            CHECK(img.width > img.height);
        }
    }

    // Test 20: chart ticks and dot leaders (make_chart_toc_fixtures.py).
    // Tick labels on both value axes of a chart, mirrored (30 | 30) or on
    // two scales (20 | 2,500), are not tables; a statement with dot leaders
    // and a year-stub table beside them still are. Contents entries joined
    // to their page numbers by leaders are a list, not a table, each title
    // whole however its word gaps line up with the rows around it.
    std::cout << "[20] Testing chart ticks and dot-leader entries...\n";
    {
        using Rows = std::vector<std::vector<std::string>>;
        auto tables_of = [](const char* path) {
            std::ifstream f(path);
            if (!f.good()) {
                std::cerr << "    missing fixture " << path << "\n";
                return std::vector<Rows>{{{"<missing>"}}};
            }
            auto chunks = jdoc::pdf_to_markdown_chunks(path);
            return chunks.empty() ? std::vector<Rows>{} : chunks[0].tables;
        };
        {
            auto ts = tables_of("test/fixtures/pdf/chart_ticks.pdf");
            CHECK(ts.size() == 2);
            for (auto& t : ts)
                for (auto& row : t)
                    for (auto& c : row) CHECK(c != "30" && c != "2,500" && c != "20");
            CHECK(ts[0].size() == 7);
            CHECK((ts[0][0] == std::vector<std::string>{"Revenues", "1,250.0", "1,100.5"}));
            CHECK((ts[0][3] == std::vector<std::string>{"Operating expenses:", "", ""}));
            CHECK(ts[1].size() == 7);
            CHECK((ts[1][1] == std::vector<std::string>{"1965", "49.5", "10.0"}));
            std::cout << "    axis ticks are not tables; statements are OK\n";
        }
        {
            // A contents list is a list, not a table: its entries stay
            // lines of text, each title whole up to its page number.
            auto ts = tables_of("test/fixtures/pdf/leader_toc.pdf");
            CHECK(ts.empty());
            std::string md = jdoc::pdf_to_markdown("test/fixtures/pdf/leader_toc.pdf");
            CHECK(md.find("4.4.1 \xEC\x9E\x85\xEB\xA0\xA5 \xEC\xA1\xB0\xEA\xB1\xB4 "
                          "\xEB\xB3\x80\xEC\x88\x98\xEC\x9D\x98 "
                          "\xEA\xB8\xB0\xEC\x97\xAC\xEB\x8F\x84 \xEB\xB6\x84\xEC\x84\x9D")
                  != std::string::npos);          // 입력 조건 변수의 기여도 분석
            // The English entries' word spaces are no-break spaces: look
            // for the words on one line.
            size_t pp = md.find("Pressure");
            CHECK(pp != std::string::npos && md.find("ahead", pp) < md.find('\n', pp));
            CHECK(md.find('|') == std::string::npos);
            std::cout << "    contents entries stay whole list lines OK\n";
        }
    }

    // Test 21: a figure drawn as thin raster strips on a two-column page,
    // a paragraph ending just above it in each column and a short axis title
    // between them (make_chart_toc_fixtures.py). The composite is the figure
    // and its title: 455pt wide, about 160pt tall. Taking in the paragraphs
    // (each column's lines are under 0.4 of the page wide) made it some
    // 240pt tall.
    std::cout << "[21] Testing a fragmented figure under two-column text...\n";
    {
        const char* fx = "test/fixtures/pdf/fragment_two_column.pdf";
        std::ifstream f(fx);
        if (!f.good()) {
            std::cerr << "    missing fixture " << fx << "\n";
            return 1;
        }
        auto chunks = jdoc::pdf_to_markdown_chunks(fx);
        CHECK(chunks.size() == 1);
        CHECK(chunks[0].images.size() == 1);
        const auto& img = chunks[0].images[0];
        CHECK(img.format == "png");
        std::cout << "    composite " << img.width << "x" << img.height << "\n";
        CHECK(img.width > 0 && img.height * 100 < img.width * 42);
        CHECK(img.height * 100 > img.width * 30);   // the strips and their title
    }

    // Test 22: a figure stored as raster strips under two columns of body
    // text. Its region composite is the figure alone: column lines are
    // narrower than half the page yet are body text, not labels to grow
    // over — nor is a paragraph's short last line right above the figure.
    // Strips span 435x120 pt; taking in the text above would make the
    // image barely twice as wide as tall.
    std::cout << "[22] Testing figure strips under multi-column body text...\n";
    {
        const char* fixtures[] = {
            "test/fixtures/pdf/strip_figure_columns.pdf",
            "test/fixtures/pdf/strip_figure_columns_short_tail.pdf",
        };
        for (const char* fx : fixtures) {
            std::ifstream f(fx);
            if (!f.good()) {
                std::cout << "    SKIP: " << fx << "\n";
                continue;
            }
            f.close();
            auto chunks = jdoc::pdf_to_markdown_chunks(fx);
            CHECK(chunks.size() == 1);
            CHECK(chunks[0].images.size() == 1);
            const auto& img = chunks[0].images[0];
            double aspect = img.height > 0 ? double(img.width) / img.height : 0;
            std::cout << "    " << fx << ": " << img.width << "x" << img.height
                      << " (expected aspect >= 3)\n";
            CHECK(aspect >= 3.0);
            CHECK(chunks[0].text.find("sentences") != std::string::npos);
            CHECK(chunks[0].text.find("evaluation") != std::string::npos);
        }
    }

    // Test 23: a glyph of a CID-keyed CFF font whose charset is not the
    // identity, drawn over a raster. Glyph 1 is a bar on the left half of
    // the em, glyph 2 one on the right; every fixture's ToUnicode says the
    // shown code is "A" (glyph 1). Shown by CID (the spec) the charset
    // selects it; shown by glyph index (codes the charset maps elsewhere,
    // or does not hold) the font's cmap or the charset's coverage says so.
    // Either way the composite is dark on the left and light on the right.
    std::cout << "[23] Testing CID-keyed CFF glyph selection...\n";
    {
        const char* fixtures[] = {
            "test/fixtures/pdf/cid_cff_otf_gids.pdf",
            "test/fixtures/pdf/cid_cff_otf_cids.pdf",
            "test/fixtures/pdf/cid_cff_bare_gids.pdf",
            "test/fixtures/pdf/cid_cff_bare_cids.pdf",
        };
        for (const char* fx : fixtures) {
            std::ifstream f(fx);
            if (!f.good()) {
                std::cout << "    SKIP: " << fx << "\n";
                continue;
            }
            f.close();
            auto chunks = jdoc::pdf_to_markdown_chunks(fx);
            CHECK(chunks.size() == 1);
            CHECK(chunks[0].images.size() == 1);
            const auto& img = chunks[0].images[0];
            CHECK(img.format == "png");
            double left = dark_fraction(img.data, 0.0, 0.5);
            double right = dark_fraction(img.data, 0.5, 1.0);
            std::cout << "    " << fx << ": dark left " << left << ", right " << right
                      << " (expected left bar)\n";
            CHECK(left > 0.2);
            CHECK(right < 0.05);
        }
    }

    std::cout << "\n=== All tests passed ===\n";
    return 0;
}
