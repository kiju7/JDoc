// test_odf.cpp - Tests for ODF (odt/ods/odp) to Markdown conversion
// License: MIT

#include "jdoc/office.h"
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

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

namespace {

void put_u16(std::string& s, uint16_t v) {
    s.push_back(char(v & 0xFF));
    s.push_back(char((v >> 8) & 0xFF));
}
void put_u32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s.push_back(char((v >> (8 * i)) & 0xFF));
}
uint32_t crc32_of(const std::string& d) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : d) {
        crc ^= c;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
    }
    return ~crc;
}

// Minimal STORE-only zip.
std::string make_zip(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::string out;
    struct Cd { std::string name; uint32_t crc, size, offset; };
    std::vector<Cd> cd;
    for (const auto& [name, data] : entries) {
        uint32_t crc = crc32_of(data), off = uint32_t(out.size());
        put_u32(out, 0x04034b50); put_u16(out, 20); put_u16(out, 0x0800);
        put_u16(out, 0); put_u32(out, 0); put_u32(out, crc);
        put_u32(out, uint32_t(data.size())); put_u32(out, uint32_t(data.size()));
        put_u16(out, uint16_t(name.size())); put_u16(out, 0);
        out += name; out += data;
        cd.push_back({name, crc, uint32_t(data.size()), off});
    }
    uint32_t cd_off = uint32_t(out.size());
    for (const auto& c : cd) {
        put_u32(out, 0x02014b50); put_u16(out, 20); put_u16(out, 20);
        put_u16(out, 0x0800); put_u16(out, 0); put_u32(out, 0); put_u32(out, c.crc);
        put_u32(out, c.size); put_u32(out, c.size);
        put_u16(out, uint16_t(c.name.size()));
        put_u16(out, 0); put_u16(out, 0); put_u16(out, 0); put_u16(out, 0);
        put_u32(out, 0); put_u32(out, c.offset); out += c.name;
    }
    uint32_t cd_size = uint32_t(out.size()) - cd_off;
    put_u32(out, 0x06054b50); put_u16(out, 0); put_u16(out, 0);
    put_u16(out, uint16_t(cd.size())); put_u16(out, uint16_t(cd.size()));
    put_u32(out, cd_size); put_u32(out, cd_off); put_u16(out, 0);
    return out;
}

const char* MANIFEST =
    "<?xml version=\"1.0\"?><manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:"
    "opendocument:xmlns:manifest:1.0\"><manifest:file-entry manifest:full-path=\"/\""
    " manifest:media-type=\"x\"/></manifest:manifest>";

// Wrap a content.xml body in an ODF package. `mimetype` sets the kind.
std::string make_odf(const std::string& mimetype, const std::string& content,
                     const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    std::string doc =
        "<?xml version=\"1.0\"?><office:document-content "
        "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
        "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
        "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" "
        "xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
        "xmlns:presentation=\"urn:oasis:names:tc:opendocument:xmlns:presentation:1.0\" "
        "xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
        "<office:body>" + content + "</office:body></office:document-content>";
    std::vector<std::pair<std::string, std::string>> entries = {
        {"mimetype", mimetype},
        {"META-INF/manifest.xml", MANIFEST},
        {"content.xml", doc},
    };
    for (const auto& e : extra) entries.push_back(e);
    return make_zip(entries);
}

std::string conv(const std::string& pkg, const std::string& hint) {
    return jdoc::office_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(pkg.data()), pkg.size(), hint);
}

} // namespace

void test_odf() {
    std::cerr << "=== jdoc odf tests ===\n\n";

    TEST(odt_headings_and_paragraphs)
        std::string body =
            "<office:text>"
            "<text:h text:outline-level=\"1\">Main Title</text:h>"
            "<text:h text:outline-level=\"2\">Sub Head</text:h>"
            "<text:p>a paragraph</text:p>"
            "<text:p/>"                       // blank spacer, dropped
            "<text:p>second para</text:p>"
            "</office:text>";
        auto md = conv(make_odf("application/vnd.oasis.opendocument.text", body),
                       "d.odt");
        ASSERT(md.find("# Main Title") != std::string::npos);
        ASSERT(md.find("## Sub Head") != std::string::npos);
        ASSERT(md.find("a paragraph") != std::string::npos);
        ASSERT(md.find("second para") != std::string::npos);
        ASSERT(md.find("text:p") == std::string::npos);   // no raw XML leaked
        ASSERT(md.find("office:") == std::string::npos);
    TEST_END

    TEST(ods_markdown_table_with_repeat)
        std::string body =
            "<office:spreadsheet><table:table table:name=\"Data\">"
            "<table:table-row>"
            "<table:table-cell office:value-type=\"string\"><text:p>A</text:p></table:table-cell>"
            "<table:table-cell office:value-type=\"string\"><text:p>B</text:p></table:table-cell>"
            "</table:table-row>"
            "<table:table-row>"
            "<table:table-cell office:value-type=\"string\"><text:p>x</text:p></table:table-cell>"
            "<table:table-cell office:value-type=\"string\"><text:p>x</text:p></table:table-cell>"
            "</table:table-row>"
            "</table:table></office:spreadsheet>";
        auto md = conv(make_odf("application/vnd.oasis.opendocument.spreadsheet", body),
                       "d.ods");
        ASSERT(md.find("## Data") != std::string::npos);
        ASSERT(md.find("| --- | --- |") != std::string::npos);  // 2-column separator
        ASSERT(md.find("| A | B |") != std::string::npos);
        ASSERT(md.find("| x | x |") != std::string::npos);
    TEST_END

    TEST(ods_column_repeat_expands)
        std::string body =
            "<office:spreadsheet><table:table table:name=\"R\">"
            "<table:table-row>"
            "<table:table-cell office:value-type=\"string\"><text:p>v</text:p></table:table-cell>"
            "<table:table-cell office:value-type=\"string\" "
            "table:number-columns-repeated=\"3\"><text:p>r</text:p></table:table-cell>"
            "</table:table-row>"
            "</table:table></office:spreadsheet>";
        auto md = conv(make_odf("application/vnd.oasis.opendocument.spreadsheet", body),
                       "d.ods");
        ASSERT(md.find("| v | r | r | r |") != std::string::npos);
    TEST_END

    TEST(odp_slides_and_title)
        std::string body =
            "<office:presentation>"
            "<draw:page draw:name=\"p1\">"
            "<draw:frame presentation:class=\"title\"><draw:text-box>"
            "<text:p>Slide Title</text:p></draw:text-box></draw:frame>"
            "<draw:frame presentation:class=\"subtitle\"><draw:text-box>"
            "<text:p>slide one body</text:p></draw:text-box></draw:frame>"
            "</draw:page>"
            "<draw:page draw:name=\"p2\">"
            "<draw:frame><draw:text-box><text:p>slide two body</text:p>"
            "</draw:text-box></draw:frame>"
            "</draw:page>"
            "</office:presentation>";
        auto pkg = make_odf("application/vnd.oasis.opendocument.presentation", body);
        auto md = conv(pkg, "d.odp");
        ASSERT(md.find("# Slide Title") != std::string::npos);
        ASSERT(md.find("slide one body") != std::string::npos);
        ASSERT(md.find("slide two body") != std::string::npos);
        ASSERT(md.find("--- Page 2 ---") != std::string::npos);
        auto chunks = jdoc::office_to_markdown_chunks_mem(
            reinterpret_cast<const uint8_t*>(pkg.data()), pkg.size(), "d.odp", {});
        ASSERT(chunks.size() == 2);
    TEST_END

    TEST(odp_speaker_notes_extracted_not_leaked)
        std::string body =
            "<office:presentation><draw:page draw:name=\"p1\">"
            "<draw:frame presentation:class=\"subtitle\"><draw:text-box>"
            "<text:p>visible body</text:p></draw:text-box></draw:frame>"
            "<presentation:notes><draw:frame><draw:text-box>"
            "<text:p>hidden speaker note</text:p></draw:text-box></draw:frame>"
            "</presentation:notes>"
            "</draw:page></office:presentation>";
        auto md = conv(make_odf("application/vnd.oasis.opendocument.presentation", body),
                       "d.odp");
        ASSERT(md.find("visible body") != std::string::npos);
        ASSERT(md.find("> **Notes:** hidden speaker note") != std::string::npos);
        // The note text must appear once (in the Notes block), not leaked twice.
        size_t first = md.find("hidden speaker note");
        ASSERT(first != std::string::npos);
        ASSERT(md.find("hidden speaker note", first + 1) == std::string::npos);
    TEST_END

    TEST(detected_via_mimetype_without_extension)
        // No .od? extension — detection must fall back to the mimetype member.
        std::string body = "<office:text><text:p>from mimetype</text:p></office:text>";
        auto md = conv(make_odf("application/vnd.oasis.opendocument.text", body),
                       "unnamed.bin");
        ASSERT(md.find("from mimetype") != std::string::npos);
    TEST_END

    std::cerr << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===\n";
}

int main() {
    test_odf();
    return tests_failed > 0 ? 1 : 0;
}
