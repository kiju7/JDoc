// test_hwpx.cpp - Tests for HWPX text that lives outside the plain run stream
// Builds synthetic HWPX packages (mimetype + Contents/section0.xml in a stored
// zip) and verifies that text held in drawing objects (<hp:rect>/<hp:container>
// with an <hp:drawText> body), in headers and footers, in table captions, and
// in tables nested inside a table cell is extracted — each exactly once.
// License: MIT

#include "jdoc/hwpx.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <zlib.h>

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

static size_t count_occurrences(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        n++;
    return n;
}

// ── Fixture builder ─────────────────────────────────────────

namespace {

static void put_u16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}
static void put_u32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

// Minimal store-only zip writer — enough for what ZipReader needs to open a
// HWPX package.
std::string make_zip(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::string out;
    struct CdInfo { std::string name; uint32_t crc, size, offset; };
    std::vector<CdInfo> cd;

    for (const auto& [name, data] : entries) {
        uint32_t crc = static_cast<uint32_t>(
            crc32(0, reinterpret_cast<const Bytef*>(data.data()),
                  static_cast<uInt>(data.size())));
        uint32_t offset = static_cast<uint32_t>(out.size());

        put_u32(out, 0x04034b50);
        put_u16(out, 20);
        put_u16(out, 0x0800);  // UTF-8 names
        put_u16(out, 0);       // stored
        put_u32(out, 0);       // time/date
        put_u32(out, crc);
        put_u32(out, static_cast<uint32_t>(data.size()));
        put_u32(out, static_cast<uint32_t>(data.size()));
        put_u16(out, static_cast<uint16_t>(name.size()));
        put_u16(out, 0);
        out += name;
        out += data;

        cd.push_back({name, crc, static_cast<uint32_t>(data.size()), offset});
    }

    uint32_t cd_offset = static_cast<uint32_t>(out.size());
    for (const auto& c : cd) {
        put_u32(out, 0x02014b50);
        put_u16(out, 20);
        put_u16(out, 20);
        put_u16(out, 0x0800);
        put_u16(out, 0);
        put_u32(out, 0);
        put_u32(out, c.crc);
        put_u32(out, c.size);
        put_u32(out, c.size);
        put_u16(out, static_cast<uint16_t>(c.name.size()));
        put_u16(out, 0);  // extra
        put_u16(out, 0);  // comment
        put_u16(out, 0);  // disk
        put_u16(out, 0);  // internal attrs
        put_u32(out, 0);  // external attrs
        put_u32(out, c.offset);
        out += c.name;
    }
    uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_offset;

    put_u32(out, 0x06054b50);
    put_u16(out, 0);
    put_u16(out, 0);
    put_u16(out, static_cast<uint16_t>(cd.size()));
    put_u16(out, static_cast<uint16_t>(cd.size()));
    put_u32(out, cd_size);
    put_u32(out, cd_offset);
    put_u16(out, 0);  // comment len
    return out;
}

// Wrap section body markup in a <hs:sec> root and package it as a HWPX file.
std::string make_hwpx(const std::string& section_body,
                      const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    std::string section =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<hs:sec xmlns:hs=\"http://www.hancom.co.kr/hwpml/2011/section\""
        " xmlns:hp=\"http://www.hancom.co.kr/hwpml/2011/paragraph\">" +
        section_body + "</hs:sec>";
    std::vector<std::pair<std::string, std::string>> entries = {
        {"mimetype", "application/hwp+zip"}, {"Contents/section0.xml", section}};
    for (const auto& e : extra) entries.push_back(e);
    return make_zip(entries);
}

std::string convert(const std::string& hwpx) {
    return jdoc::hwpx_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(hwpx.data()), hwpx.size());
}

// A paragraph list holding a single text run, as found under <hp:subList>.
std::string sub_para(const std::string& text) {
    return "<hp:p><hp:run><hp:t>" + text + "</hp:t></hp:run></hp:p>";
}

}  // namespace

// ── Drawing object tests ────────────────────────────────────

static void test_shape_text_extracted() {
    std::cerr << "Drawing objects:\n";

    TEST(rect_draw_text_extracted)
        auto md = convert(make_hwpx(
            "<hp:p><hp:run><hp:t>본문 문단</hp:t></hp:run></hp:p>"
            "<hp:p><hp:run><hp:rect><hp:drawText><hp:subList>" +
            sub_para("글상자 안 텍스트") +
            "</hp:subList></hp:drawText></hp:rect></hp:run></hp:p>"));
        ASSERT(count_occurrences(md, "본문 문단") == 1);
        ASSERT(count_occurrences(md, "글상자 안 텍스트") == 1);
    TEST_END

    TEST(container_recurses_into_grouped_shapes)
        // <hp:container> groups shapes and has no body of its own; both nested
        // text boxes must be reached, and neither may be emitted twice.
        auto md = convert(make_hwpx(
            "<hp:p><hp:run><hp:container>"
            "<hp:rect><hp:drawText><hp:subList>" + sub_para("첫째 도형") +
            "</hp:subList></hp:drawText></hp:rect>"
            "<hp:ellipse><hp:drawText><hp:subList>" + sub_para("둘째 도형") +
            "</hp:subList></hp:drawText></hp:ellipse>"
            "</hp:container></hp:run></hp:p>"));
        ASSERT(count_occurrences(md, "첫째 도형") == 1);
        ASSERT(count_occurrences(md, "둘째 도형") == 1);
    TEST_END

    TEST(multi_paragraph_shape_body)
        auto md = convert(make_hwpx(
            "<hp:p><hp:run><hp:rect><hp:drawText><hp:subList>" +
            sub_para("첫 줄") + sub_para("둘째 줄") +
            "</hp:subList></hp:drawText></hp:rect></hp:run></hp:p>"));
        ASSERT(count_occurrences(md, "첫 줄") == 1);
        ASSERT(count_occurrences(md, "둘째 줄") == 1);
    TEST_END

    TEST(shape_inside_table_cell)
        auto md = convert(make_hwpx(
            "<hp:p><hp:run><hp:tbl rowCnt=\"1\" colCnt=\"1\"><hp:tr><hp:tc>"
            "<hp:subList><hp:p><hp:run>"
            "<hp:t>셀 본문</hp:t>"
            "<hp:rect><hp:drawText><hp:subList>" + sub_para("셀 안 글상자") +
            "</hp:subList></hp:drawText></hp:rect>"
            "</hp:run></hp:p></hp:subList>"
            "</hp:tc></hp:tr></hp:tbl></hp:run></hp:p>"));
        ASSERT(count_occurrences(md, "셀 본문") == 1);
        ASSERT(count_occurrences(md, "셀 안 글상자") == 1);
    TEST_END
}

// ── Table tests ─────────────────────────────────────────────

static void test_tables() {
    std::cerr << "\nTables:\n";

    TEST(nested_table_text_kept_once)
        // A table nested in a cell is inlined into that cell rather than
        // rendered as its own grid, which would break the enclosing table.
        auto md = convert(make_hwpx(
            "<hp:p><hp:run><hp:tbl rowCnt=\"1\" colCnt=\"1\"><hp:tr><hp:tc>"
            "<hp:subList><hp:p><hp:run>"
            "<hp:t>바깥 셀</hp:t>"
            "<hp:tbl rowCnt=\"1\" colCnt=\"1\"><hp:tr><hp:tc><hp:subList>" +
            sub_para("안쪽 셀") +
            "</hp:subList></hp:tc></hp:tr></hp:tbl>"
            "</hp:run></hp:p></hp:subList>"
            "</hp:tc></hp:tr></hp:tbl></hp:run></hp:p>"));
        ASSERT(count_occurrences(md, "바깥 셀") == 1);
        ASSERT(count_occurrences(md, "안쪽 셀") == 1);
    TEST_END

    TEST(caption_precedes_table)
        auto md = convert(make_hwpx(
            "<hp:p><hp:run><hp:tbl rowCnt=\"1\" colCnt=\"1\">"
            "<hp:caption><hp:subList>" + sub_para("표 1 캡션") +
            "</hp:subList></hp:caption>"
            "<hp:tr><hp:tc><hp:subList>" + sub_para("셀 내용") +
            "</hp:subList></hp:tc></hp:tr>"
            "</hp:tbl></hp:run></hp:p>"));
        ASSERT(count_occurrences(md, "표 1 캡션") == 1);
        ASSERT(count_occurrences(md, "셀 내용") == 1);
        ASSERT(md.find("표 1 캡션") < md.find("셀 내용"));
    TEST_END

    TEST(plain_cell_text_not_duplicated)
        auto md = convert(make_hwpx(
            "<hp:p><hp:run><hp:tbl rowCnt=\"1\" colCnt=\"2\"><hp:tr>"
            "<hp:tc><hp:subList>" + sub_para("좌측 셀") + "</hp:subList></hp:tc>"
            "<hp:tc><hp:subList>" + sub_para("우측 셀") + "</hp:subList></hp:tc>"
            "</hp:tr></hp:tbl></hp:run></hp:p>"));
        ASSERT(count_occurrences(md, "좌측 셀") == 1);
        ASSERT(count_occurrences(md, "우측 셀") == 1);
    TEST_END
}

// ── Header / footer tests ───────────────────────────────────

static void test_header_footer() {
    std::cerr << "\nHeaders and footers:\n";

    TEST(header_and_footer_extracted)
        auto md = convert(make_hwpx(
            "<hp:p><hp:run>"
            "<hp:ctrl><hp:header><hp:subList>" + sub_para("머리말 문구") +
            "</hp:subList></hp:header></hp:ctrl>"
            "<hp:ctrl><hp:footer><hp:subList>" + sub_para("꼬리말 문구") +
            "</hp:subList></hp:footer></hp:ctrl>"
            "</hp:run></hp:p>"
            "<hp:p><hp:run><hp:t>본문입니다</hp:t></hp:run></hp:p>"));
        ASSERT(count_occurrences(md, "머리말 문구") == 1);
        ASSERT(count_occurrences(md, "꼬리말 문구") == 1);
        ASSERT(count_occurrences(md, "본문입니다") == 1);
    TEST_END

    TEST(shape_inside_header_extracted)
        auto md = convert(make_hwpx(
            "<hp:p><hp:run><hp:ctrl><hp:header><hp:subList>"
            "<hp:p><hp:run><hp:rect><hp:drawText><hp:subList>" +
            sub_para("머리말 도형") +
            "</hp:subList></hp:drawText></hp:rect></hp:run></hp:p>"
            "</hp:subList></hp:header></hp:ctrl></hp:run></hp:p>"));
        ASSERT(count_occurrences(md, "머리말 도형") == 1);
    TEST_END

    TEST(footnote_still_collected)
        // Header/footer handling must not displace the existing footnote path
        auto md = convert(make_hwpx(
            "<hp:p><hp:run><hp:t>참조 문단</hp:t>"
            "<hp:ctrl><hp:footNote number=\"1\"><hp:subList>" +
            sub_para("각주 본문") +
            "</hp:subList></hp:footNote></hp:ctrl>"
            "</hp:run></hp:p>"));
        ASSERT(count_occurrences(md, "참조 문단") == 1);
        ASSERT(count_occurrences(md, "각주 본문") == 1);
    TEST_END
}

// ── Charts and captions ─────────────────────────────────────

static void test_charts_and_captions() {
    std::cerr << "\nCharts and captions:\n";

    const std::string chart_xml =
        "<?xml version=\"1.0\"?><c:chartSpace xmlns:c=\"http://schemas.openxmlformats.org/"
        "drawingml/2006/chart\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/"
        "main\"><c:chart><c:title><c:tx><c:rich><a:p><a:r><a:t>분기 매출</a:t></a:r></a:p>"
        "</c:rich></c:tx></c:title><c:plotArea><c:barChart><c:ser><c:tx><c:v>서울</c:v></c:tx>"
        "<c:cat><c:strLit><c:pt idx=\"0\"><c:v>1분기</c:v></c:pt></c:strLit></c:cat>"
        "<c:val><c:numLit><c:pt idx=\"0\"><c:v>10</c:v></c:pt></c:numLit></c:val></c:ser>"
        "</c:barChart></c:plotArea></c:chart></c:chartSpace>";

    TEST(chart_in_switch_read_from_its_part)
        // Hancom writes a chart as <hp:switch>: an OOXML chart case and an
        // OLE fallback. The switch was not recognised and the chart vanished.
        auto md = convert(make_hwpx(
            "<hp:p><hp:run><hp:switch><hp:case hp:required-namespace=\"x\">"
            "<hp:chart chartIDRef=\"Chart/chart1.xml\"><hp:caption><hp:subList>" +
            sub_para("차트 캡션 홍길동") + "</hp:subList></hp:caption></hp:chart></hp:case>"
            "<hp:default><hp:ole binaryItemIDRef=\"none\"/></hp:default></hp:switch>"
            "</hp:run></hp:p>",
            {{"Chart/chart1.xml", chart_xml}}));
        ASSERT(md.find("분기 매출") != std::string::npos);
        ASSERT(md.find("서울") != std::string::npos);
        ASSERT(md.find("1분기") != std::string::npos);
        ASSERT(md.find("10") == std::string::npos);   // values stay out
        ASSERT(count_occurrences(md, "차트 캡션 홍길동") == 1);
    TEST_END

    TEST(picture_and_shape_captions_extracted)
        // Only table captions used to be read.
        auto md = convert(make_hwpx(
            "<hp:p><hp:run><hp:pic><hp:caption><hp:subList>" + sub_para("그림 캡션 박영수") +
            "</hp:subList></hp:caption></hp:pic></hp:run></hp:p>"
            "<hp:p><hp:run><hp:rect><hp:drawText><hp:subList>" + sub_para("도형 글") +
            "</hp:subList></hp:drawText><hp:caption><hp:subList>" + sub_para("도형 캡션 정다은") +
            "</hp:subList></hp:caption></hp:rect></hp:run></hp:p>"));
        ASSERT(count_occurrences(md, "그림 캡션 박영수") == 1);
        ASSERT(count_occurrences(md, "도형 글") == 1);
        ASSERT(count_occurrences(md, "도형 캡션 정다은") == 1);
    TEST_END
}

int main() {
    std::cerr << "\n=== HWPX out-of-run text tests ===\n";

    test_shape_text_extracted();
    test_tables();
    test_header_footer();
    test_charts_and_captions();

    std::cerr << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===\n";
    return tests_failed == 0 ? 0 : 1;
}
