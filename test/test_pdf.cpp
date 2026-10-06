// test_pdf.cpp — Test PDF to Markdown conversion using PDFium backend
#include "jdoc/pdf.h"
#include "pdf/pdf_base14.h"
#include "pdf/pdf_core.h"
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
    // over — nor is a paragraph's short last line right above the figure,
    // on a two-column page or a single-column one. Strips span 435x120 pt; taking in the text above would make the
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
        // One column, a paragraph of a single full line and a short last
        // line right above the figure, and too few body-size lines on the
        // page to measure a column: the short line still ends the
        // paragraph. The composite is the strips alone (435x120 pt; the
        // line taken in made it about 3.2:1) and the image follows the
        // whole paragraph instead of splitting it.
        const char* single = "test/fixtures/pdf/strip_figure_single_column_short_tail.pdf";
        std::ifstream sf(single);
        if (!sf.good()) {
            std::cout << "    SKIP: " << single << "\n";
        } else {
            sf.close();
            auto chunks = jdoc::pdf_to_markdown_chunks(single);
            CHECK(chunks.size() == 1);
            CHECK(chunks[0].images.size() == 1);
            if (chunks.size() == 1 && chunks[0].images.size() == 1) {
                const auto& img = chunks[0].images[0];
                double aspect = img.height > 0 ? double(img.width) / img.height : 0;
                std::cout << "    " << single << ": " << img.width << "x" << img.height
                          << " (expected aspect >= 3.4)\n";
                CHECK(aspect >= 3.4);
                const std::string& t = chunks[0].text;
                size_t tail = t.find("results, shown in Figure 3.");
                size_t ref = t.find("![");
                size_t cap = t.find("Figure 3. Analysis flow");
                CHECK(tail != std::string::npos && ref != std::string::npos &&
                      cap != std::string::npos);
                CHECK(tail < ref && ref < cap);
                CHECK(t.find("summary of\nresults, shown") != std::string::npos);
            }
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

    // Test 24: CID fonts with no /ToUnicode and no program, shown through
    // Identity-H (make_cid_collection_fixtures.py). Their CIDs read through
    // the Adobe collection /CIDSystemInfo names: Korea1 3296 1204 2479 is
    // 한국어, GB1 4559 3795 is 中文. Taken as Unicode they were Kannada and
    // Arabic letters.
    std::cout << "[24] Testing CID fonts read through their Adobe collection...\n";
    {
        const char* fx = "test/fixtures/pdf/cid_collection.pdf";
        std::ifstream f(fx);
        if (!f.good()) {
            std::cout << "    SKIP: " << fx << "\n";
        } else {
            f.close();
            std::string md = jdoc::pdf_to_markdown(fx);
            CHECK(md.find("\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4") != std::string::npos);  // 한국어
            CHECK(md.find("\xE4\xB8\xAD\xE6\x96\x87") != std::string::npos);              // 中文
            std::cout << "    Korea1 and GB1 CIDs read as text OK\n";
        }
    }

    // Test 25: an LZWDecode content stream (make_lzw_fixture.py). With the
    // default EarlyChange 1 the codes widen one table entry early; widening
    // one late misread every code after the first few hundred bytes and the
    // page lost all but its first lines.
    std::cout << "[25] Testing an LZW-compressed content stream...\n";
    {
        const char* fx = "test/fixtures/pdf/lzw_content.pdf";
        std::ifstream f(fx);
        if (!f.good()) {
            std::cout << "    SKIP: " << fx << "\n";
        } else {
            f.close();
            std::string md = jdoc::pdf_to_markdown(fx);
            CHECK(md.find("Line 000 of a long LZW compressed page") != std::string::npos);
            CHECK(md.find("Line 219 of a long LZW compressed page") != std::string::npos);
            CHECK(md.find("END OF LZW TEXT") != std::string::npos);
            std::cout << "    all lines decoded OK\n";
        }
    }

    // Test 26: table rows whose word spaces are no-break spaces (U+00A0).
    // The cells normalise them to spaces; the prose lines must too, or the
    // capture check that drops a table's lines from the prose flow misses
    // them and the shaded bold header (and a data row with "12 300") is
    // printed a second time after the table.
    std::cout << "[26] Testing table rows spelled with no-break spaces...\n";
    {
        const char* fx = "test/fixtures/pdf/nbsp_table.pdf";
        std::ifstream f(fx);
        if (!f.good()) {
            std::cout << "    SKIP: " << fx << "\n";
        } else {
            f.close();
            std::string md = jdoc::pdf_to_markdown(fx);
            auto count = [&](const std::string& needle) {
                size_t n = 0;
                for (size_t p = md.find(needle); p != std::string::npos;
                     p = md.find(needle, p + 1))
                    n++;
                return n;
            };
            CHECK(count("| **Park**") == 1);
            CHECK(count("Inside temp") == 1);
            CHECK(count("Road temp") == 1);
            CHECK(count("North Park") == 1);
            CHECK(count("12 300") == 1);
            CHECK(md.find("\xC2\xA0") == std::string::npos);
            size_t tbl = md.find("| Corner");
            size_t tail = md.find("The smallest park");
            CHECK(tbl != std::string::npos && tail != std::string::npos && tbl < tail);
        }
    }

    // [27] Markdown table padding counts display columns, not UTF-8 bytes:
    // a Hangul syllable is 3 bytes but 2 columns, so byte-based padding left
    // Korean cells short and the pipes out of line with Latin rows. Wide
    // (Hangul, kana, CJK, full-width), combining (U+0301, conjoining Jamo
    // vowels and finals) and narrow cells must all end on the same columns,
    // the separator dashes included, and bold markers count as text.
    std::cout << "[27] Testing table padding by display width...\n";
    {
        jdoc::pdf_detail::TableData t{};
        t.rows = {{"공원", "면적 ha", "비고"},
                  {"중앙공원", "42", "カナ漢字"},
                  {"Riverside", "7", "ＡＢ e\xCC\x81"},
                  {"\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8", "", "x"}};
        t.cell_bold = {{1, 1, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        const std::string md = jdoc::pdf_detail::format_table(t);
        const std::string want =
            "| **공원**  | **면적 ha** | 비고     |\n"
            "| --------- | ----------- | -------- |\n"
            "| 중앙공원  | 42          | カナ漢字 |\n"
            "| Riverside | 7           | ＡＢ e\xCC\x81   |\n"
            "| \xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8        |             | x        |\n";
        if (md != want) std::cerr << "got:\n" << md << "want:\n" << want;
        CHECK(md == want);
    }

    // [28] Banded tables: rows separated only by background shading, with
    // the first or last row left white. The run of painted rows must extend
    // to the white rows that continue its column alignment (table 1: last
    // body row; table 2: header above and last row below the stripes), and
    // a fill written `re h f` stays a shading rect, not four fake rules.
    // A header-only tint (table 3) must not absorb a body row of a tighter
    // pitch. With tables off, a row's widely spaced numbers keep their spaces.
    std::cout << "[28] Testing banded tables with unshaded edge rows...\n";
    {
        const char* fx = "test/fixtures/pdf/shaded_band_table.pdf";
        std::ifstream f(fx);
        if (!f.good()) {
            std::cout << "    SKIP: " << fx << "\n";
        } else {
            f.close();
            std::string md = jdoc::pdf_to_markdown(fx);
            auto count = [&](const std::string& hay, const std::string& needle) {
                size_t n = 0;
                for (size_t p = hay.find(needle); p != std::string::npos;
                     p = hay.find(needle, p + 1))
                    n++;
                return n;
            };
            // A right-aligned header wider than its numbers keeps its
            // first letters: the column boundary sits in the blank stretch.
            CHECK(count(md, "| **Parks** ") == 1);
            CHECK(count(md, "| **Mean area ha** ") == 1);
            CHECK(count(md, "| Large ") == 1);
            CHECK(count(md, "| Small ") == 1);
            CHECK(count(md, "| **Hour**") == 1);
            CHECK(count(md, "| 10 pm ") == 1);
            CHECK(md.find("Small3") == std::string::npos);
            CHECK(md.find("10 pm25") == std::string::npos);
            CHECK(md.find("\nSmall") == std::string::npos);
            CHECK(md.find("| **Table") == std::string::npos);
            CHECK(md.find("| Larger") == std::string::npos);
            CHECK(md.find("| The gap") == std::string::npos);
            size_t small = md.find("| Small ");
            size_t prose = md.find("Larger parks were cooler");
            size_t cap2 = md.find("Table 2. Mean temperature");
            size_t hour = md.find("| **Hour**");
            size_t last = md.find("| 10 pm ");
            size_t tail = md.find("The gap between the park");
            CHECK(small != std::string::npos && prose != std::string::npos &&
                  small < prose && prose < cap2 && cap2 < hour &&
                  hour < last && last < tail && tail != std::string::npos);

            // Table 3: a header-only tint over a tighter body is no band;
            // its first body row stays with the rest of the body.
            CHECK(count(md, "| Austria ") == 1);
            CHECK(count(md, "| Germany ") == 1);
            CHECK(count(md, "| Spain ") == 1);
            CHECK(md.find("Germany Spain") == std::string::npos);

            jdoc::ConvertOptions no_tables;
            no_tables.tables = false;
            std::string plain = jdoc::pdf_to_markdown(fx, no_tables);
            CHECK(plain.find("Small 3 2.8 0.5") != std::string::npos);
            CHECK(plain.find("10 pm 25.2 25.9 0.7") != std::string::npos);
        }
    }

    // Test 29: glyphs drawn again over themselves (make_overprint_fixture.py):
    // a run struck twice 0.3pt apart to fake bold and a headline stacked
    // eight times read once, while doubled letters a whole advance apart
    // ("ll", "ss", "III", "77") all stay.
    std::cout << "[29] Testing overprinted glyphs...\n";
    {
        const char* fx = "test/fixtures/pdf/overprint.pdf";
        std::ifstream f(fx);
        if (!f.good()) {
            std::cout << "    SKIP: " << fx << "\n";
        } else {
            f.close();
            std::string md = jdoc::pdf_to_markdown(fx);
            CHECK(md.find("Bold Title") != std::string::npos);
            CHECK(md.find("BBoo") == std::string::npos);
            CHECK(md.find("Shadow") != std::string::npos);
            CHECK(md.find("SSha") == std::string::npos);
            CHECK(md.find("Hallucination Association III 77") != std::string::npos);
            std::cout << "    copies dropped, doubled letters kept OK\n";
        }
    }

    // [30] Column detection on pages with few lines
    // (make_short_page_fixtures.py). A page of ten rows is too short for the
    // gutter histogram alone; it reads as two columns when three rows set
    // text side by side, flush left in two columns of comparable width.
    // Then the left column (heading, four lines) comes before the right one
    // instead of the two being joined line by line, the centred title stays
    // across the page, and the page number at the right margin does not
    // break every line of the right column into its own paragraph. A
    // centred borderless table, a form of short labels beside long values,
    // rows of figures whose cell gaps are as wide as the gap on the page
    // centre (under sideways column heads), and a one-column page set on
    // the same short page keep their rows.
    std::cout << "[30] Testing column detection on short pages...\n";
    {
        const std::string dir = "test/fixtures/pdf/";
        std::ifstream f(dir + "short_page_columns.pdf");
        if (!f.good()) {
            std::cout << "    SKIP: short_page_*.pdf\n";
        } else {
            f.close();
            auto in_order = [](const std::string& s,
                               std::initializer_list<const char*> parts) {
                size_t at = 0;
                for (const char* p : parts) {
                    size_t k = s.find(p, at);
                    if (k == std::string::npos) {
                        std::cerr << "    out of order or missing: " << p << "\n";
                        return false;
                    }
                    at = k + 1;
                }
                return true;
            };
            jdoc::ConvertOptions text_opts;
            text_opts.format = jdoc::OutputFormat::PLAINTEXT;

            std::string md = jdoc::pdf_to_markdown(dir + "short_page_columns.pdf");
            CHECK(in_order(md, {"## Green Space and Summer Heat",
                                "## 1. Introduction",
                                "Green space in a city is known to lower the air\n"
                                "temperature on a summer afternoon, but how\n",
                                "the size of the park\naround it.\n",
                                "## 2. Measurement",
                                "The readings were taken last summer in\n"
                                "twelve city parks, twice a day, with the\n"
                                "same calibrated thermometer at every\n"}));
            CHECK(md.find("lower the air The readings") == std::string::npos);
            std::string txt = jdoc::pdf_to_markdown(dir + "short_page_columns.pdf",
                                                    text_opts);
            CHECK(in_order(txt, {"Green Space and Summer Heat", "1. Introduction",
                                 "around it.", "2. Measurement",
                                 "same calibrated thermometer at every"}));

            for (bool tables : {true, false}) {
                jdoc::ConvertOptions o;
                o.tables = tables;
                std::string t = jdoc::pdf_to_markdown(dir + "short_page_table.pdf", o);
                CHECK(in_order(t, {"Central", "31.2 degrees", "Riverside Garden",
                                   "33.6", "North Hill", "32.4 in the shade",
                                   "Corner lot", "34.0"}));
                std::string fm = jdoc::pdf_to_markdown(dir + "short_page_form.pdf", o);
                CHECK(in_order(fm, {"Applicant", "Kim Minsu", "Date of submission",
                                    "5 October 2026", "Title of the study",
                                    "Urban green space", "Supervisor",
                                    "Professor Lee Jiwon"}));
                std::string sg = jdoc::pdf_to_markdown(dir + "short_page_single.pdf", o);
                CHECK(in_order(sg, {"2026 45", "## 3. Results",
                                    "but how much it\nlowers it varies",
                                    "- Larger parks were cooler."}));
                std::string nb = jdoc::pdf_to_markdown(dir + "short_page_numbers.pdf", o);
                CHECK(in_order(nb, {"2026 45", "Table 9", "Caltech101", "Mean",
                                    "Model-H (large)", "Model-L (small)"}));
            }
            std::cout << "    short two-column page split; table, form and "
                         "one-column page kept OK\n";
        }
    }

    // [31] Two columns in part of a page (make_column_band_fixtures.py). A
    // narrow side column (keywords, correspondence) beside a wide abstract
    // has its gutter away from the page's column boundary, which the body
    // below sets at the centre. The side column's lines read as one block
    // before the abstract instead of each being glued to the abstract line
    // beside it, and the body still reads left column first. A borderless
    // glossary of short terms beside long definitions keeps each term with
    // its definition.
    std::cout << "[31] Testing two columns in part of a page...\n";
    {
        const std::string dir = "test/fixtures/pdf/";
        std::ifstream f(dir + "column_band_sidebar.pdf");
        if (!f.good()) {
            std::cout << "    SKIP: column_band_*.pdf\n";
        } else {
            f.close();
            auto in_order = [](const std::string& s,
                               std::initializer_list<const char*> parts) {
                size_t at = 0;
                for (const char* p : parts) {
                    size_t k = s.find(p, at);
                    if (k == std::string::npos) {
                        std::cerr << "    out of order or missing: " << p << "\n";
                        return false;
                    }
                    at = k + 1;
                }
                return true;
            };
            std::string md = jdoc::pdf_to_markdown(dir + "column_band_sidebar.pdf");
            CHECK(in_order(md, {"## Abstract", "Keywords", "urban heat island; city parks",
                                "Correspondence", "Department of Geography, City\n"
                                "University, 12 Riverside Road,\nRiverside 12345, Country.",
                                "E-mail: parks.study@example.org",
                                "Parks lower the air temperature of the streets around them",
                                "## 1. Introduction", "Cities are warmer",
                                "Earlier studies measured single parks"}));
            CHECK(md.find("Keywords Parks") == std::string::npos);
            CHECK(md.find("City the most") == std::string::npos);

            for (bool tables : {true, false}) {
                jdoc::ConvertOptions o;
                o.tables = tables;
                std::string g = jdoc::pdf_to_markdown(dir + "column_band_terms.pdf", o);
                const std::pair<const char*, const char*> rows[] = {
                    {"Albedo", "The share of sunlight"}, {"Canopy", "The layer of leaves"},
                    {"Heat island", "A city area"}, {"Sky view", "The fraction of the sky"}};
                for (const auto& [term, text] : rows) {
                    size_t at = g.find(term);
                    size_t eol = at == std::string::npos ? at : g.find('\n', at);
                    size_t def = at == std::string::npos ? at : g.find(text, at);
                    bool together = def != std::string::npos && def < eol;
                    if (!together)
                        std::cerr << "    term split from its definition: " << term << "\n";
                    CHECK(together);
                }
            }
            std::cout << "    side column read before the abstract; glossary rows "
                         "kept OK\n";
        }
    }

    // [32] Ruled tables of two columns (make_two_col_grid_fixtures.py). A
    // grid of terms beside their descriptions ends a word in one cell and
    // starts one in the next on every row, as any table of words does; it
    // stays a table. A paragraph a vertical rule cuts between two words,
    // with only a word space across the rule, is text, not a table.
    std::cout << "[32] Testing ruled tables of two columns...\n";
    {
        const std::string dir = "test/fixtures/pdf/";
        std::ifstream f(dir + "grid_two_col_terms.pdf");
        if (!f.good()) {
            std::cout << "    SKIP: grid_*.pdf\n";
        } else {
            f.close();
            std::string md = jdoc::pdf_to_markdown(dir + "grid_two_col_terms.pdf");
            CHECK(md.find("| heroin") != std::string::npos);
            CHECK(md.find("| anxiety, euphoria") != std::string::npos);
            CHECK(md.find("| dopamine") != std::string::npos);
            std::string prose = jdoc::pdf_to_markdown(dir + "grid_split_prose.pdf");
            CHECK(prose.find('|') == std::string::npos);
            CHECK(prose.find("Parks lower the air temperature of the streets around them") !=
                  std::string::npos);
            std::cout << "    term grid kept as a table; prose cut by a rule kept as text OK\n";
        }
    }

    // [33] Layout blocks of two columns (make_two_col_grid_fixtures.py). A
    // borderless table of codes beside counts that no detector takes keeps
    // its rows aligned in a fenced layout block; years joined to their
    // labels by dot leaders keep each year on its label's line, as a table
    // (a narrow label column beside its values) or in a layout block; a
    // contents list (titles beside rising page numbers) stays a list, its
    // shape notwithstanding.
    std::cout << "[33] Testing layout blocks of two columns...\n";
    {
        const std::string dir = "test/fixtures/pdf/";
        std::ifstream f(dir + "layout_two_col_counts.pdf");
        if (!f.good()) {
            std::cout << "    SKIP: layout_*.pdf\n";
        } else {
            f.close();
            std::string md = jdoc::pdf_to_markdown(dir + "layout_two_col_counts.pdf");
            size_t fence = md.find("```text");
            CHECK(fence != std::string::npos);
            CHECK(md.find("01000110", fence) != std::string::npos);
            CHECK(md.find("480", fence) != std::string::npos);
            std::string yr = jdoc::pdf_to_markdown(dir + "layout_leader_years.pdf");
            {
                size_t p = yr.find("Korea");
                CHECK(p != std::string::npos);
                size_t ls = yr.rfind('\n', p), le = yr.find('\n', p);
                std::string line = yr.substr(ls == std::string::npos ? 0 : ls,
                                             le == std::string::npos ? std::string::npos : le - ls);
                CHECK(line.find("2004 to 2009") != std::string::npos);
                CHECK(yr.find("```text") != std::string::npos || line.find('|') != std::string::npos);
            }
            std::string toc = jdoc::pdf_to_markdown(dir + "layout_contents.pdf");
            CHECK(toc.find("```") == std::string::npos);
            CHECK(toc.find('|') == std::string::npos);
            std::cout << "    counts kept in a layout block; contents kept as a list OK\n";
        }
    }

    // [34] Ligatures and spacing accents (make_ligature_accent_fixtures.py).
    // Glyphs named "fl" and "fi" read as their letters; a tilde drawn back
    // over an "n" from the same font reads as "ñ"; a circumflex from another
    // font over a variable (a hat in an equation) stays as drawn.
    std::cout << "[34] Testing ligatures and spacing accents...\n";
    {
        const std::string path = "test/fixtures/pdf/ligature_accent.pdf";
        std::ifstream f(path);
        if (!f.good()) {
            std::cout << "    SKIP: ligature_accent.pdf\n";
        } else {
            f.close();
            std::string md = jdoc::pdf_to_markdown(path);
            CHECK(md.find("effluents and fields") != std::string::npos);
            CHECK(md.find("Mu\xC3\xB1oz") != std::string::npos);
            CHECK(md.find("\xEF\xAC") == std::string::npos);      // no U+FB0x left
            CHECK(md.find("Y\xCB\x86") != std::string::npos);     // "Yˆ" kept
            CHECK(jdoc::pdf_detail::compose_spacing_accent('e', 0x02C6) == 0xEA);
            std::cout << "    ligatures spelt out, accent joined, equation hat kept OK\n";
        }
    }

    // [35] A table of two columns of words is not two columns of text
    // (make_glossary_fixtures.py). A glossary's terms beside their
    // definitions: the gap between them dips like a page gutter, but the
    // terms are a label column, under ten ems wide beside one far wider.
    // A notation table whose definitions run long is a table all the same:
    // their lines end where their words end, ragged, as prose never does.
    // Labels joined by dot leaders to values in words keep each value on
    // its label's line, as a table or in a layout block: the leaders pair
    // the rows, figures or not.
    // A dialogue beside its replies, both columns prose-wide, ruled off
    // row from row: the rules box it as a table for the column vote, and
    // the text table detector takes a band whose rows rules part as a
    // table whatever its cells read like. Each row keeps its two cells on
    // one line, and the dialogue comes out as a table.
    std::cout << "[35] Testing two-column tables of words against the column split...\n";
    {
        const std::string dir = "test/fixtures/pdf/";
        std::ifstream f(dir + "column_glossary.pdf");
        if (!f.good()) {
            std::cout << "    SKIP: column_glossary.pdf\n";
        } else {
            f.close();
            auto same_line = [](const std::string& md, const std::string& a, const std::string& b) {
                size_t p = md.find(a);
                if (p == std::string::npos) return false;
                size_t s = md.rfind('\n', p), e = md.find('\n', p);
                std::string line = md.substr(s == std::string::npos ? 0 : s, e == std::string::npos ? std::string::npos : e - s);
                return line.find(b) != std::string::npos;
            };
            std::string gl = jdoc::pdf_to_markdown(dir + "column_glossary.pdf");
            CHECK(same_line(gl, "ASC", "Accounting Standards Codification"));
            CHECK(same_line(gl, "NYMEX", "New York Mercantile Exchange"));
            CHECK(same_line(gl, "WTI", "West Texas Intermediate"));
            // Long ragged definitions make a table, not prose.
            std::string nt = jdoc::pdf_to_markdown(dir + "glossary_long.pdf");
            CHECK(nt.find('|') != std::string::npos);
            CHECK(same_line(nt, "| S ", "set of all possible messages"));
            // Labels joined by dot leaders to values in words keep each
            // value on its label's line: a table (a narrow label column
            // beside ragged values) or, failing that, a layout block.
            std::string lw = jdoc::pdf_to_markdown(dir + "leader_words.pdf");
            CHECK(same_line(lw, "Buildings and related improvements", "5 to 40 years"));
            CHECK(same_line(lw, "Machinery and equipment", "1 to 15 years"));
            CHECK(lw.find("```text") != std::string::npos || same_line(lw, "| Machinery", "1 to 15 years"));
            std::string rr = jdoc::pdf_to_markdown(dir + "ruled_rows.pdf");
            CHECK(same_line(rr, "Input: hear it ?", "Choice: first reply"));
            CHECK(same_line(rr, "Fifth: the man is a man of faith", "Seventh: it is my duty"));
            // Rules parting the rows make it a table, prose-like cells or not.
            CHECK(rr.find('|') != std::string::npos);
            std::cout << "    glossary and ruled dialogue rows kept whole OK\n";
        }
    }

    // [37] The shape of a two-column band of words (make_glossary_fixtures.py).
    // Products beside the terms of their revenue recognition: a narrow
    // column of terms under a bold header beside descriptions of several
    // lines each, no rule anywhere. The wrapped lines keep the band whole
    // and the shape says table where the cells' lengths say prose. Two
    // tables parted by one line of prose running edge to edge stay two
    // tables, the prose line between them. A table's bold group labels
    // spanning most of its width do not part it: the rows below keep the
    // column gap of the rows above.
    std::cout << "[37] Testing the shape of two-column bands of words...\n";
    {
        const std::string dir = "test/fixtures/pdf/";
        std::ifstream f(dir + "hanging_terms.pdf");
        if (!f.good()) {
            std::cout << "    SKIP: hanging_terms.pdf\n";
        } else {
            f.close();
            auto line_of = [](const std::string& md, const std::string& a) {
                size_t p = md.find(a);
                if (p == std::string::npos) return std::string();
                size_t s = md.rfind('\n', p), e = md.find('\n', p);
                return md.substr(s == std::string::npos ? 0 : s, e == std::string::npos ? std::string::npos : e - s);
            };
            auto same_line = [&](const std::string& md, const std::string& a, const std::string& b) {
                return line_of(md, a).find(b) != std::string::npos;
            };
            auto separators = [](const std::string& md) {
                size_t n = 0, p = 0;
                while ((p = md.find("\n| ---", p)) != std::string::npos) { n++; p += 6; }
                return n;
            };
            std::string ht = jdoc::pdf_to_markdown(dir + "hanging_terms.pdf");
            CHECK(same_line(ht, "| Instruments", "For instruments that include installation"));
            CHECK(same_line(ht, "| Cloud services", "Cloud services, which allow customers"));
            CHECK(same_line(ht, "| Extended warranty", "straight-line basis"));
            std::string ta = jdoc::pdf_to_markdown(dir + "tables_apart.pdf");
            CHECK(same_line(ta, "| Expected dividend yield", "$-"));
            CHECK(same_line(ta, "Weighted average fair value", "$26.15"));
            CHECK(same_line(ta, "Balance at December 31, 2006", "4,872"));
            CHECK(line_of(ta, "The following table summarizes").find('|') == std::string::npos);
            CHECK(separators(ta) == 2);
            std::string gl = jdoc::pdf_to_markdown(dir + "group_labels.pdf");
            CHECK(same_line(gl, "| No. of Cameras in the ACS", "| 2 "));
            CHECK(same_line(gl, "| Translations of cameras", "2 x 30 x 100"));
            CHECK(same_line(gl, "| Rotation noise", "0 to 2.4"));
            CHECK(separators(gl) == 1);
            std::cout << "    hanging terms a table; two tables kept apart; group labels kept inside OK\n";
        }
    }

    // [36] A chart's labels are not a table (make_chart_fixtures.py). Tick
    // labels, years and a legend align across two chart panels as neatly as
    // cells do; the bars and lines drawn around them (drawing_regions) say
    // they are a figure's, so no table is made of them, and every label
    // stays in the text.
    std::cout << "[36] Testing chart labels against the table detectors...\n";
    {
        const std::string path = "test/fixtures/pdf/chart_labels.pdf";
        std::ifstream f(path);
        if (!f.good()) {
            std::cout << "    SKIP: chart_labels.pdf\n";
        } else {
            f.close();
            std::string md = jdoc::pdf_to_markdown(path);
            CHECK(md.find('|') == std::string::npos);
            CHECK(md.find("```") == std::string::npos);
            for (const char* label : {"2019", "2022", "Advanced", "Emerging", "60", "-4", "Output level"})
                CHECK(md.find(label) != std::string::npos);
            CHECK(md.find("Output grew in every region") != std::string::npos);
            std::cout << "    chart labels kept as text, no table OK\n";
        }
    }


    // Sparse grids can contain one-cell continuation fragments after text
    // segmentation. A rule through that cell makes a separate row; a partial
    // rule in a different column does not. Construct page geometry directly
    // so these cases run without optional external fixtures or a PDF writer.
    std::cout << "[38] Testing continuation rows against cell rules...\n";
    {
        using namespace jdoc::pdf_detail;
        for (int mode = 0; mode < 8; ++mode) {
            PageCharCache cache;
            for (int r = 0; r < 10; ++r) {
                for (int c = 0; c < 3; ++c) {
                    if ((r == 2 || (mode == 4 && r == 5)) && c == 0) continue;
                    double x = 65 + c * 100, y = 400 - r * 20;
                    cache.chars.push_back({x, y, x, x + 5, y + 8, y - 2,
                                           10, unsigned('A' + r), 0, false});
                }
            }
            for (size_t i = 0; i < cache.chars.size(); ++i)
                cache.y_sorted.push_back(i);
            std::stable_sort(cache.y_sorted.begin(), cache.y_sorted.end(),
                [&](size_t a, size_t b) { return cache.chars[a].y < cache.chars[b].y; });
            std::vector<double> levels = {210, 410};
            std::vector<PdfLineSegment> horizontal = {
                {50, 210, 350, 210}, {50, 410, 350, 410}};
            if (mode > 0 && mode < 4) {
                levels.insert(levels.begin() + 1, 370);
                horizontal.push_back({mode == 2 ? 150.f : 50.f, 370,
                                      mode == 3 ? 150.f : 350.f, 370});
            }
            // The third physical line has a blank stub and one wide cell.
            std::vector<PdfLineSegment> vertical = {
                {50, 210, 50, 410}, {150, 210, 150, 410},
                {250, 210, 250, 349}, {250, 371, 250, 410},
                {350, 210, 350, 410}};
            if (mode == 4) {
                // An earlier merge must not shift the later boundary evidence.
                levels.insert(levels.begin() + 1, 310);
                horizontal.push_back({50, 310, 350, 310});
                vertical[2].y1 = 289;
                vertical.push_back({250, 311, 250, 349});
            } else if (mode == 5) {
                // PDF producers may emit a rule as adjoining cell segments.
                levels.insert(levels.begin() + 1, 370);
                horizontal.push_back({150, 370, 250, 370});
                horizontal.push_back({250, 370, 350, 370});
            }
            if (mode == 6) {
                // Staggered short strokes do not form one horizontal border.
                levels.insert(levels.begin() + 1, 367);
                levels.insert(levels.begin() + 2, 373);
                horizontal.push_back({150, 367, 250, 367});
                horizontal.push_back({250, 373, 350, 373});
            }
            if (mode == 7) {
                // Slightly misaligned segments still form one rule level.
                levels.insert(levels.begin() + 1, 370);
                horizontal.push_back({150, 368, 250, 368});
                horizontal.push_back({250, 371, 350, 371});
            }
            auto table = build_table(levels, horizontal, vertical, cache);
            bool separates = mode == 1 || mode == 2 || mode == 5 || mode == 7;
            CHECK(table.rows.size() == (separates ? 10u : 9u));
            if (separates) {
                CHECK(table.rows[1][1] == "B");
                CHECK(table.rows[2][1] == "C C");
            } else {
                CHECK(table.rows[1][1] == "B C C");
                if (mode == 4) {
                    CHECK(table.rows[3][1] == "E");
                    CHECK(table.rows[4][1] == "F F");
                }
            }
        }
    }


    // A header may omit the stub and center its labels above two data rows.
    // Its sparse stub must not disappear before the table is accepted.
    std::cout << "[39] Testing sparse stubs under ruled headers...\n";
    {
        using namespace jdoc::pdf_detail;
        auto put = [](PageCharCache& cache, double x, double y, const std::string& text) {
            for (unsigned char u : text) {
                cache.chars.push_back({x, y, x, x + 5, y + 8, y - 2,
                                       10, unsigned(u), 0, false});
                x += 5;
            }
        };
        auto index = [](PageCharCache& cache) {
            for (size_t i = 0; i < cache.chars.size(); ++i) cache.y_sorted.push_back(i);
            std::stable_sort(cache.y_sorted.begin(), cache.y_sorted.end(),
                [&](size_t a, size_t b) { return cache.chars[a].y < cache.chars[b].y; });
        };
        PageCharCache cache;
        put(cache, 110, 650, "Average estimate");
        put(cache, 220, 650, "Maximum value");
        put(cache, 50, 630, "Alpha"); put(cache, 150, 630, "-0.51"); put(cache, 250, 630, "1.52");
        put(cache, 50, 610, "Beta"); put(cache, 150, 610, "0.008"); put(cache, 250, 610, "0.031");
        index(cache);
        std::vector<PdfLineSegment> rules = {
            {40, 660, 300, 660}, {40, 640, 300, 640}, {40, 600, 300, 600}};
        auto tables = detect_text_tables(cache, {}, 600, 800, 0, {}, &rules);
        CHECK(tables.size() == 1);
        CHECK(tables[0].rows.size() == 3);
        CHECK(tables[0].rows[0].size() == 3);
        CHECK(tables[0].rows[1][0] == "Alpha");
        CHECK(tables[0].rows[1][1] == "-0.51");
        CHECK(tables[0].rows[2][2] == "0.031");
        CHECK(detect_text_tables(cache, {}, 600, 800).empty());
        auto frame_only = rules;
        frame_only.erase(frame_only.begin() + 1);
        CHECK(detect_text_tables(cache, {}, 600, 800, 0, {}, &frame_only).empty());
        std::vector<std::array<double, 4>> drawing = {{{40, 600, 300, 660}}};
        CHECK(detect_text_tables(cache, {}, 600, 800, 0, drawing, &rules).empty());

        // Actual rules must not turn two framed prose columns into a table.
        PageCharCache prose;
        for (int r = 0; r < 8; ++r) {
            put(prose, 40, 650 - r * 20, "Words form a line of prose.");
            put(prose, 300, 650 - r * 20, "Other words form more prose.");
        }
        index(prose);
        std::vector<PdfLineSegment> prose_rules = {
            {30, 660, 550, 660}, {30, 640, 550, 640}, {30, 500, 550, 500}};
        CHECK(detect_text_tables(prose, {}, 600, 800, 0, {}, &prose_rules).empty());
    }

    // Closed cells with a one-line stub and several wrapped prose columns
    // retain their drawn row bands, including a last line in just one cell.
    // The same sparse rules around groups of numeric records still split.
    std::cout << "[40] Testing closed grids with wrapped cell text...\n";
    {
        using namespace jdoc::pdf_detail;
        for (int mode = 0; mode < 4; ++mode) {
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
            auto table = build_table(levels, h, v, cache);
            if (numeric || spaced_records || repeated_stub) {
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
    std::cout << "\n=== All tests passed ===\n";
    return 0;
}
