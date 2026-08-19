// test_office.cpp - Basic tests for Office document to Markdown conversion
// License: MIT

#include "jdoc/office.h"
#include "jdoc/archive.h"
#include "jdoc/detect.h"
#include "zip_reader.h"
#include "common/string_utils.h"
#include "legacy/ole_reader.h"
#include "ooxml/xlsb_parser.h"
#include <iostream>
#include <cassert>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>

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

#ifdef _WIN32
static void set_env(const char* name, const char* value) { _putenv_s(name, value); }
static void unset_env(const char* name) { _putenv_s(name, ""); }
#else
static void set_env(const char* name, const char* value) { setenv(name, value, 1); }
static void unset_env(const char* name) { unsetenv(name); }
#endif

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

    std::string rtf_path =
        (std::filesystem::temp_directory_path() / "test_jdoc_office.rtf").string();
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

// ── PPTX shape tree ─────────────────────────────────────────

// Minimal store-only zip writer, enough to synthesise a .pptx package.

static void put_u16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}
static void put_u32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

static uint32_t crc32_of(const std::string& d) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : d) {
        crc ^= c;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
    }
    return ~crc;
}

static std::string make_zip(
    const std::vector<std::pair<std::string, std::string>>& entries) {
    std::string out;
    struct CdInfo { std::string name; uint32_t crc, size, offset; };
    std::vector<CdInfo> cd;

    for (const auto& [name, data] : entries) {
        uint32_t crc = crc32_of(data);
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

static size_t count_occurrences(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        n++;
    return n;
}

// Wrap shape-tree markup in a one-slide presentation. `extra` adds further zip
// entries (e.g. a notes slide plus the relationship that points at it).
static std::string make_pptx(
    const std::string& sp_tree,
    const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    std::vector<std::pair<std::string, std::string>> entries = {
        {"[Content_Types].xml",
         "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/"
         "package/2006/content-types\"><Default Extension=\"xml\" ContentType=\""
         "application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>"
         "</Types>"},
        {"ppt/presentation.xml", "<?xml version=\"1.0\"?><p:presentation/>"},
        {"ppt/slides/slide1.xml",
         "<?xml version=\"1.0\"?>"
         "<p:sld xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\""
         " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
         " xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\">"
         "<p:cSld><p:spTree>" + sp_tree + "</p:spTree></p:cSld></p:sld>"},
    };
    for (const auto& e : extra) entries.push_back(e);
    return make_zip(entries);
}

// A shape whose text body holds a single paragraph.
static std::string text_shape(const std::string& tag, const std::string& text) {
    return "<p:" + tag + "><p:txBody><a:p><a:r><a:t>" + text +
           "</a:t></a:r></a:p></p:txBody></p:" + tag + ">";
}

static std::string convert_pptx(const std::string& pptx) {
    return jdoc::office_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(pptx.data()), pptx.size(), "deck.pptx");
}

// An object inserted into a deck or document is kept as its own package part
// under <ppt|word|xl>/embeddings/. jdoc does not open those payloads, so they
// used to vanish without a trace.
void test_ooxml_embedded_parts() {
    std::cerr << "\nOOXML embedded object parts:\n";

    TEST(embedded_parts_listed)
        auto md = convert_pptx(make_pptx(
            text_shape("sp", "slide body"),
            {{"ppt/embeddings/Microsoft_Excel_Worksheet1.xlsx",
              std::string(2048, 'x')},
             {"ppt/embeddings/plan.dwg", "AC1032" + std::string(1024, '\0')}}));
        ASSERT(md.find("## Attachments") != std::string::npos);
        ASSERT(md.find("Microsoft_Excel_Worksheet1.xlsx (2.0 KB)") != std::string::npos);
        ASSERT(md.find("plan.dwg (1.0 KB)") != std::string::npos);
        // The slide's own text is untouched.
        ASSERT(count_occurrences(md, "slide body") == 1);
        // Only the leaf name is listed, not the package path.
        ASSERT(md.find("ppt/embeddings/") == std::string::npos);
    TEST_END

    TEST(no_attachments_section_without_embeddings)
        auto md = convert_pptx(make_pptx(text_shape("sp", "slide body")));
        ASSERT(md.find("## Attachments") == std::string::npos);
    TEST_END

    TEST(part_name_cannot_forge_document_structure)
        // Zip entry names are attacker-controlled and may hold newlines. One
        // carrying markdown would otherwise fabricate headings in the output.
        std::string evil = "ppt/embeddings/benign.xlsx\n\n## Table of Contents"
                           "\n\n- INJECTED HEADING\n\nmore.bin";
        auto md = convert_pptx(make_pptx(text_shape("sp", "slide body"),
                                         {{evil, std::string(1234, 'x')}}));
        // The name is still reported in full — it is only prevented from
        // starting a line, which is what makes markdown structure.
        ASSERT(md.find("INJECTED HEADING") != std::string::npos);
        ASSERT(md.find("\n## Table of Contents") == std::string::npos);
        ASSERT(count_occurrences(md, "\n## Attachments") == 1);
        // Collapsed to a single list item.
        ASSERT(count_occurrences(md, "\n- ") == 1);
    TEST_END

    TEST(chunk_api_agrees_with_whole_document)
        // A chunk pipeline must not be told less than convert() is: the
        // listing mirrors into the last chunk, the way the header/footer and
        // master-layout trailers already do.
        auto pptx = make_pptx(text_shape("sp", "slide body"),
                              {{"ppt/embeddings/plan.dwg", std::string(2048, 'd')}});
        auto md = convert_pptx(pptx);
        auto chunks = jdoc::office_to_markdown_chunks_mem(
            reinterpret_cast<const uint8_t*>(pptx.data()), pptx.size(), "deck.pptx");
        ASSERT(md.find("plan.dwg (2.0 KB)") != std::string::npos);
        ASSERT(!chunks.empty());
        std::string joined;
        for (auto& c : chunks) joined += c.text;
        ASSERT(joined.find("## Attachments") != std::string::npos);
        ASSERT(joined.find("plan.dwg (2.0 KB)") != std::string::npos);
    TEST_END

    TEST(part_name_bytes_cannot_break_utf8)
        // make_zip sets the UTF-8 name flag, so ZipReader passes these bytes
        // through untouched. A caller that demands UTF-8 (pybind11 does) would
        // fail to decode the whole conversion over one bad part name.
        std::string bad = "ppt/embeddings/";
        bad += "\xff\xfe\x80"; bad += "bad.xlsx";
        auto md = convert_pptx(make_pptx(text_shape("sp", "slide body"),
                                         {{bad, std::string(512, 'z')}}));
        // Well-formed already, so repairing it changes nothing. (is_valid_utf8
        // is the wrong oracle here — it reports U+FFFD itself as invalid, and
        // repair is exactly what puts one there.)
        ASSERT(jdoc::util::sanitize_utf8(md) == md);
        ASSERT(md.find('\xff') == std::string::npos);
        ASSERT(md.find("bad.xlsx") != std::string::npos);
        ASSERT(md.find("slide body") != std::string::npos);
    TEST_END

    TEST(only_direct_children_of_embeddings_listed)
        // A substring match on "/embeddings/" would also claim these.
        auto md = convert_pptx(make_pptx(
            text_shape("sp", "slide body"),
            {{"ppt/media/embeddings/logo.png", std::string(4096, 'p')},
             {"ppt/embeddings/nested/deep.bin", std::string(512, 'd')},
             {"ppt/embeddings/", ""}}));
        ASSERT(md.find("## Attachments") == std::string::npos);
    TEST_END
}

// DWFx is an XPS package: it carries [Content_Types].xml like an OOXML
// document, so without a separate check it lands in the office layer and is
// rejected as an unsupported document.
void test_dwfx_not_office() {
    std::cerr << "\nDWFx vs OOXML package:\n";

    auto detect_zip = [](const std::string& zip) {
        return jdoc::detect(zip.data(), zip.size(), "");
    };
    const std::string ct =
        "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/"
        "package/2006/content-types\"/>";

    TEST(dwfx_3d_without_fdseq_detected)
        // Autodesk names the sequence part FixedDocumentSequence.fdseq in 2D
        // packages but DWFDocumentSequence.dwfseq in 3D ones, so only the
        // dwf/ parts are a reliable marker.
        auto zip = make_zip({{"[Content_Types].xml", ct},
                             {"DWFDocumentSequence.dwfseq", "<x/>"},
                             {"dwf/documents/1/manifest.xml", "<m/>"}});
        auto info = detect_zip(zip);
        ASSERT(info.format == "DWFX");
        ASSERT(!info.convertible);
    TEST_END

    TEST(plain_xps_stays_office)
        auto zip = make_zip({{"[Content_Types].xml", ct},
                             {"FixedDocumentSequence.fdseq", "<x/>"},
                             {"Documents/1/Pages/1.fpage", "<p/>"}});
        ASSERT(detect_zip(zip).format != "DWFX");
    TEST_END

    TEST(office_document_carrying_a_dwf_part_stays_convertible)
        // Misreading a real document as a drawing would drop its whole body.
        auto zip = make_zip({{"[Content_Types].xml", ct},
                             {"word/document.xml",
                              "<?xml version=\"1.0\"?><w:document xmlns:w=\"http://"
                              "schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                              "<w:body><w:p><w:r><w:t>quarterly report body</w:t>"
                              "</w:r></w:p></w:body></w:document>"},
                             {"dwf/documents/1/manifest.xml", "<m/>"}});
        ASSERT(detect_zip(zip).format != "DWFX");
        auto md = jdoc::office_to_markdown_mem(
            reinterpret_cast<const uint8_t*>(zip.data()), zip.size(), "report.docx");
        ASSERT(md.find("quarterly report body") != std::string::npos);
    TEST_END

    TEST(ordinary_zip_with_dwf_folder_stays_archive)
        // No OPC marker: a user's zip that happens to hold a dwf/ directory
        // must still be walked as an archive.
        auto zip = make_zip({{"dwf/notes.txt", "hello"}, {"readme.txt", "hi"}});
        auto info = detect_zip(zip);
        ASSERT(info.format == "ZIP");
        ASSERT(info.convertible);
    TEST_END
}

void test_pptx_shape_tree() {
    std::cerr << "\nPPTX shape tree:\n";

    TEST(connector_shape_text_extracted)
        // A cxnSp is a connector; PowerPoint lets it carry a label
        auto md = convert_pptx(make_pptx(text_shape("sp", "plain shape") +
                                         text_shape("cxnSp", "connector label")));
        ASSERT(count_occurrences(md, "plain shape") == 1);
        ASSERT(count_occurrences(md, "connector label") == 1);
    TEST_END

    TEST(alternate_content_choice_extracted)
        // The shape lives in mc:Choice; mc:Fallback restates it and must not
        // be emitted a second time.
        auto md = convert_pptx(make_pptx(
            "<mc:AlternateContent>"
            "<mc:Choice Requires=\"a14\">" + text_shape("sp", "choice text") +
            "</mc:Choice>"
            "<mc:Fallback>" + text_shape("sp", "choice text") + "</mc:Fallback>"
            "</mc:AlternateContent>"));
        ASSERT(count_occurrences(md, "choice text") == 1);
    TEST_END

    TEST(grouped_shape_text_not_duplicated)
        auto md = convert_pptx(make_pptx(
            "<p:grpSp>" + text_shape("sp", "grouped one") +
            "<p:grpSp>" + text_shape("sp", "grouped two") + "</p:grpSp>"
            "</p:grpSp>"));
        ASSERT(count_occurrences(md, "grouped one") == 1);
        ASSERT(count_occurrences(md, "grouped two") == 1);
    TEST_END

    TEST(notes_text_box_extracted)
        // Speaker notes typed into a plain text box carry no placeholder, while
        // the slide-image and slide-number placeholders must stay out.
        std::string notes =
            "<?xml version=\"1.0\"?>"
            "<p:notes xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\""
            " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
            "<p:cSld><p:spTree>"
            "<p:sp><p:nvSpPr><p:nvPr><p:ph type=\"sldNum\"/></p:nvPr></p:nvSpPr>"
            "<p:txBody><a:p><a:r><a:t>page chrome</a:t></a:r></a:p></p:txBody></p:sp>"
            "<p:sp><p:txBody><a:p><a:r><a:t>free form note</a:t></a:r></a:p>"
            "</p:txBody></p:sp>"
            "</p:spTree></p:cSld></p:notes>";
        std::string rels =
            "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas."
            "openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"notesSlide\" "
            "Target=\"../notesSlides/notesSlide1.xml\"/></Relationships>";
        auto md = convert_pptx(make_pptx(
            text_shape("sp", "slide body"),
            {{"ppt/notesSlides/notesSlide1.xml", notes},
             {"ppt/slides/_rels/slide1.xml.rels", rels}}));
        ASSERT(count_occurrences(md, "slide body") == 1);
        ASSERT(count_occurrences(md, "free form note") == 1);
        ASSERT(count_occurrences(md, "page chrome") == 0);
    TEST_END
}

// ── PPTX master / layout text ───────────────────────────────

// A master or layout part wrapping the given shape-tree markup.
static std::string layout_part(const std::string& sp_tree) {
    return "<?xml version=\"1.0\"?>"
           "<p:sldLayout xmlns:p=\"http://schemas.openxmlformats.org/"
           "presentationml/2006/main\" xmlns:a=\"http://schemas."
           "openxmlformats.org/drawingml/2006/main\">"
           "<p:cSld><p:spTree>" + sp_tree + "</p:spTree></p:cSld></p:sldLayout>";
}

// A placeholder shape of the given type carrying prompt text.
static std::string ph_shape(const std::string& type, const std::string& text) {
    std::string ph = type.empty() ? "<p:ph/>" : "<p:ph type=\"" + type + "\"/>";
    return "<p:sp><p:nvSpPr><p:nvPr>" + ph +
           "</p:nvPr></p:nvSpPr><p:txBody><a:p><a:r><a:t>" + text +
           "</a:t></a:r></a:p></p:txBody></p:sp>";
}

void test_pptx_master_layout() {
    std::cerr << "\nPPTX master/layout:\n";

    TEST(authored_layout_text_extracted)
        // A plain text box on the layout renders on every slide but lives in no
        // slide part — a common place to park a team name or contact.
        auto md = convert_pptx(make_pptx(
            text_shape("sp", "slide body"),
            {{"ppt/slideLayouts/slideLayout1.xml",
              layout_part(text_shape("sp", "designed by Hong Gildong"))}}));
        ASSERT(count_occurrences(md, "slide body") == 1);
        ASSERT(count_occurrences(md, "designed by Hong Gildong") == 1);
    TEST_END

    TEST(prompt_placeholders_excluded)
        // Template furniture in every locale — keyed on placeholder type, not
        // on a list of prompt strings.
        auto md = convert_pptx(make_pptx(
            text_shape("sp", "slide body"),
            {{"ppt/slideMasters/slideMaster1.xml",
              layout_part(ph_shape("title", "Click to edit Master title style") +
                          ph_shape("body", "Click to edit Master text styles") +
                          ph_shape("ctrTitle", "마스터 제목 스타일 편집") +
                          ph_shape("subTitle", "클릭하여 마스터 부제목 스타일 편집") +
                          ph_shape("", "마스터 텍스트 스타일 편집"))}}));
        ASSERT(md.find("Click to edit") == std::string::npos);
        ASSERT(md.find("마스터") == std::string::npos);
        ASSERT(md.find("Slide Master") == std::string::npos);
    TEST_END

    TEST(footer_placeholder_kept)
        // ftr is an authored slot, not a prompt: real text lives there
        auto md = convert_pptx(make_pptx(
            text_shape("sp", "slide body"),
            {{"ppt/slideLayouts/slideLayout1.xml",
              layout_part(ph_shape("ftr", "Hanbat University team") +
                          ph_shape("title", "Click to edit Master title style"))}}));
        ASSERT(count_occurrences(md, "Hanbat University team") == 1);
        ASSERT(md.find("Click to edit") == std::string::npos);
    TEST_END

    TEST(layout_text_not_repeated_from_slide)
        // If the slide already states it, the layout copy is redundant
        auto md = convert_pptx(make_pptx(
            text_shape("sp", "shared footer text"),
            {{"ppt/slideLayouts/slideLayout1.xml",
              layout_part(ph_shape("ftr", "shared footer text"))}}));
        ASSERT(count_occurrences(md, "shared footer text") == 1);
    TEST_END

    TEST(same_text_across_layouts_emitted_once)
        auto md = convert_pptx(make_pptx(
            text_shape("sp", "slide body"),
            {{"ppt/slideLayouts/slideLayout1.xml",
              layout_part(text_shape("sp", "repeated logo caption"))},
             {"ppt/slideLayouts/slideLayout2.xml",
              layout_part(text_shape("sp", "repeated logo caption"))}}));
        ASSERT(count_occurrences(md, "repeated logo caption") == 1);
    TEST_END

    TEST(no_block_when_layouts_hold_only_prompts)
        auto md = convert_pptx(make_pptx(
            text_shape("sp", "slide body"),
            {{"ppt/slideLayouts/slideLayout1.xml",
              layout_part(ph_shape("title", "Click to edit Master title style"))}}));
        ASSERT(md.find("Slide Master / Layout") == std::string::npos);
    TEST_END
}

// ── PPTX shared media deduplication ─────────────────────────

// A 1x1 PNG. Real encoded bytes so the dimension probe and the format sniffer
// behave as they do on a document from the wild.
static std::string png_bytes() {
    static const unsigned char kPng[] = {
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A, 0x00,0x00,0x00,0x0D,
        0x49,0x48,0x44,0x52, 0x00,0x00,0x00,0x01, 0x00,0x00,0x00,0x01,
        0x08,0x06,0x00,0x00,0x00, 0x1F,0x15,0xC4,0x89,
        0x00,0x00,0x00,0x0A, 0x49,0x44,0x41,0x54,
        0x78,0x9C,0x63,0x00,0x01,0x00,0x00,0x05,0x00,0x01,
        0x0D,0x0A,0x2D,0xB4,
        0x00,0x00,0x00,0x00, 0x49,0x45,0x4E,0x44, 0xAE,0x42,0x60,0x82};
    return std::string(reinterpret_cast<const char*>(kPng), sizeof(kPng));
}

// A clean directory for tests that need to inspect what was written to disk.
static std::string temp_image_dir(const std::string& tag) {
    std::string dir = (std::filesystem::temp_directory_path() /
                       ("jdoc_test_" + tag)).string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// A picture shape pulling in relationship `rid`.
static std::string pic_shape(const std::string& rid) {
    return "<p:pic><p:blipFill><a:blip r:embed=\"" + rid +
           "\"/></p:blipFill></p:pic>";
}

static std::string slide_part(const std::string& sp_tree) {
    return "<?xml version=\"1.0\"?>"
           "<p:sld xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\""
           " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
           " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
           "<p:cSld><p:spTree>" + sp_tree + "</p:spTree></p:cSld></p:sld>";
}

static std::string rels_part(const std::string& rid, const std::string& target) {
    return "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas."
           "openxmlformats.org/package/2006/relationships\"><Relationship Id=\"" +
           rid + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/"
           "2006/relationships/image\" Target=\"" + target + "\"/></Relationships>";
}

// Three slides that all show ppt/media/image1.png, the way a deck carries one
// logo on every page. Slide 3 additionally shows a second, distinct picture.
static std::string make_shared_media_pptx() {
    std::vector<std::pair<std::string, std::string>> entries = {
        {"[Content_Types].xml",
         "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/"
         "package/2006/content-types\"/>"},
        {"ppt/presentation.xml", "<?xml version=\"1.0\"?><p:presentation/>"},
        {"ppt/slides/slide1.xml", slide_part(pic_shape("rId1"))},
        {"ppt/slides/slide2.xml", slide_part(pic_shape("rId1"))},
        {"ppt/slides/slide3.xml", slide_part(pic_shape("rId1") + pic_shape("rId2"))},
        {"ppt/slides/_rels/slide1.xml.rels", rels_part("rId1", "../media/image1.png")},
        {"ppt/slides/_rels/slide2.xml.rels", rels_part("rId1", "../media/image1.png")},
        {"ppt/slides/_rels/slide3.xml.rels",
         "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas."
         "openxmlformats.org/package/2006/relationships\">"
         "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
         "officeDocument/2006/relationships/image\" Target=\"../media/image1.png\"/>"
         "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/"
         "officeDocument/2006/relationships/image\" Target=\"../media/image2.png\"/>"
         "</Relationships>"},
        {"ppt/media/image1.png", png_bytes()},
        {"ppt/media/image2.png", png_bytes() + std::string(4, '\0')},
    };
    return make_zip(entries);
}

void test_pptx_shared_media() {
    std::cerr << "\nPPTX shared media:\n";

    // The defect this guards: a media part referenced from N slides used to be
    // written to disk N times, under N names, all holding the same bytes.
    TEST(repeated_media_part_extracted_once)
        auto deck = make_shared_media_pptx();
        jdoc::ConvertOptions opts;
        ASSERT(opts.images);
        opts.min_image_size = 0;
        auto chunks = jdoc::office_to_markdown_chunks_mem(
            reinterpret_cast<const uint8_t*>(deck.data()), deck.size(),
            "deck.pptx", opts);

        ASSERT(chunks.size() >= 3);
        std::set<std::string> distinct;
        for (auto& c : chunks)
            for (auto& img : c.images) distinct.insert(img.name);
        // image1.png shared by all three slides, image2.png on slide 3 alone
        ASSERT(distinct.size() == 2);
    TEST_END

    TEST(repeated_media_part_written_to_disk_once)
        // The original defect, measured where it hurt: one media part shown on
        // three slides used to leave three identical files in the image
        // directory. Counts files actually written, not just names handed back.
        auto deck = make_shared_media_pptx();
        std::string dir = temp_image_dir("pptx_shared_media");

        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;
        auto chunks = jdoc::office_to_markdown_chunks_mem(
            reinterpret_cast<const uint8_t*>(deck.data()), deck.size(),
            "deck.pptx", opts);
        (void)chunks;

        size_t files = 0;
        std::set<std::string> contents;
        for (auto& e : std::filesystem::directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            files++;
            std::ifstream in(e.path(), std::ios::binary);
            contents.insert(std::string(std::istreambuf_iterator<char>(in),
                                        std::istreambuf_iterator<char>()));
        }
        std::filesystem::remove_all(dir);

        // Two distinct media parts, two files — no copy per reference
        ASSERT(files == 2);
        // and the files that landed hold distinct bytes
        ASSERT(contents.size() == 2);
    TEST_END

    TEST(every_page_lists_the_image_it_shows)
        // Deduplicating the extraction must not cost the page-to-image link:
        // a chunk consumer asking "what does slide 2 show?" still gets an answer
        auto deck = make_shared_media_pptx();
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        auto chunks = jdoc::office_to_markdown_chunks_mem(
            reinterpret_cast<const uint8_t*>(deck.data()), deck.size(),
            "deck.pptx", opts);

        ASSERT(chunks[0].images.size() == 1);
        ASSERT(chunks[1].images.size() == 1);
        ASSERT(chunks[2].images.size() == 2);
        // All three slides point at the one extracted image, by name
        ASSERT(chunks[0].images[0].name == chunks[1].images[0].name);
        ASSERT(chunks[1].images[0].name == chunks[2].images[0].name);
        // and each records the page it appears on
        ASSERT(chunks[0].images[0].page_number == 1);
        ASSERT(chunks[1].images[0].page_number == 2);
        ASSERT(chunks[2].images[0].page_number == 3);
    TEST_END

    TEST(shared_bytes_held_once_in_memory_mode)
        // With no image_dir the payload travels in ImageData::data. The repeat
        // references must not each carry their own copy of the same bytes.
        auto deck = make_shared_media_pptx();
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        auto chunks = jdoc::office_to_markdown_chunks_mem(
            reinterpret_cast<const uint8_t*>(deck.data()), deck.size(),
            "deck.pptx", opts);

        size_t with_payload = 0;
        for (auto& c : chunks)
            for (auto& img : c.images)
                if (!img.data.empty()) with_payload++;
        // Two distinct images, so two payloads — not four
        ASSERT(with_payload == 2);
    TEST_END

    TEST(every_reference_rendered_in_markdown)
        // Deduplication is about the file on disk, not the prose: a slide that
        // shows the logo must still say so, or the reader loses the picture.
        auto deck = make_shared_media_pptx();
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        auto md = jdoc::office_to_markdown_mem(
            reinterpret_cast<const uint8_t*>(deck.data()), deck.size(),
            "deck.pptx", opts);

        // Four picture references across the deck: 1 + 1 + 2
        ASSERT(count_occurrences(md, "![") == 4);
        // and they converge on two distinct targets
        std::set<std::string> targets;
        for (size_t p = md.find("]("); p != std::string::npos;
             p = md.find("](", p + 2)) {
            size_t end = md.find(')', p);
            if (end == std::string::npos) break;
            targets.insert(md.substr(p + 2, end - p - 2));
        }
        ASSERT(targets.size() == 2);
    TEST_END

    TEST(distinct_parts_are_not_collapsed)
        // Guard the opposite error: two different media parts must survive as
        // two images even when a naive key would merge them.
        auto deck = make_shared_media_pptx();
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        auto chunks = jdoc::office_to_markdown_chunks_mem(
            reinterpret_cast<const uint8_t*>(deck.data()), deck.size(),
            "deck.pptx", opts);
        ASSERT(chunks[2].images[0].name != chunks[2].images[1].name);
    TEST_END
}

// ── DOCX header / footer ────────────────────────────────────

// A header/footer part holding one paragraph per supplied string.
static std::string wordml_part(const std::string& root,
                               const std::vector<std::string>& paras) {
    std::string s =
        "<?xml version=\"1.0\"?><w:" + root +
        " xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">";
    for (const auto& p : paras)
        s += "<w:p><w:r><w:t>" + p + "</w:t></w:r></w:p>";
    return s + "</w:" + root + ">";
}

static std::string make_docx(
    const std::vector<std::string>& body_paras,
    const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    std::vector<std::pair<std::string, std::string>> entries = {
        {"[Content_Types].xml",
         "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/"
         "package/2006/content-types\"><Default Extension=\"xml\" ContentType=\""
         "application/vnd.openxmlformats-officedocument.wordprocessingml.document"
         ".main+xml\"/></Types>"},
        {"word/document.xml",
         "<?xml version=\"1.0\"?><w:document xmlns:w=\"http://schemas."
         "openxmlformats.org/wordprocessingml/2006/main\"><w:body>" +
             [&] {
                 std::string s;
                 for (const auto& p : body_paras)
                     s += "<w:p><w:r><w:t>" + p + "</w:t></w:r></w:p>";
                 return s;
             }() +
             "</w:body></w:document>"},
    };
    for (const auto& e : extra) entries.push_back(e);
    return make_zip(entries);
}

static std::string convert_docx(const std::string& docx) {
    return jdoc::office_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(docx.data()), docx.size(), "doc.docx");
}

void test_docx_header_footer() {
    std::cerr << "\nDOCX header/footer:\n";

    TEST(header_and_footer_extracted)
        // Renders on every page but lives outside word/document.xml
        auto md = convert_docx(make_docx(
            {"body paragraph"},
            {{"word/header1.xml", wordml_part("hdr", {"Internal review only"})},
             {"word/footer1.xml", wordml_part("ftr", {"drafted by Hong Gildong"})}}));
        ASSERT(count_occurrences(md, "body paragraph") == 1);
        ASSERT(count_occurrences(md, "Internal review only") == 1);
        ASSERT(count_occurrences(md, "drafted by Hong Gildong") == 1);
    TEST_END

    TEST(repeated_header_parts_emitted_once)
        // Word writes one header part per section and per first/even variant,
        // all carrying the same text.
        auto md = convert_docx(make_docx(
            {"body paragraph"},
            {{"word/header1.xml", wordml_part("hdr", {"same header text"})},
             {"word/header2.xml", wordml_part("hdr", {"same header text"})},
             {"word/header3.xml", wordml_part("hdr", {"same header text"})}}));
        ASSERT(count_occurrences(md, "same header text") == 1);
    TEST_END

    TEST(header_matching_body_not_repeated)
        auto md = convert_docx(make_docx(
            {"the document title"},
            {{"word/header1.xml", wordml_part("hdr", {"the document title"})}}));
        ASSERT(count_occurrences(md, "the document title") == 1);
    TEST_END

    TEST(no_block_without_header_parts)
        auto md = convert_docx(make_docx({"body paragraph"}));
        ASSERT(md.find("Header / Footer") == std::string::npos);
    TEST_END
}

// ── XLSX rels/comment fixes ──────────────────────────────────

static std::string convert_xlsx(const std::string& xlsx) {
    return jdoc::office_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(xlsx.data()), xlsx.size(), "book.xlsx");
}

// Minimal one-sheet workbook. rel_target is the workbook->sheet relationship
// Target (varied to exercise absolute vs relative paths). sheet_body is the
// <sheetData> content; extra adds further parts (comments, persons, sheet rels).
static std::string make_xlsx(
    const std::string& rel_target, const std::string& sheet_body,
    const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    std::vector<std::pair<std::string, std::string>> entries = {
        {"[Content_Types].xml",
         "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/"
         "package/2006/content-types\"><Default Extension=\"xml\" "
         "ContentType=\"application/xml\"/></Types>"},
        {"xl/workbook.xml",
         "<?xml version=\"1.0\"?><workbook xmlns=\"http://schemas.openxmlformats.org/"
         "spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/"
         "officeDocument/2006/relationships\"><sheets>"
         "<sheet name=\"Sheet1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>"},
        {"xl/_rels/workbook.xml.rels",
         "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/"
         "package/2006/relationships\"><Relationship Id=\"rId1\" Type=\""
         "http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\""
         " Target=\"" + rel_target + "\"/></Relationships>"},
        {"xl/worksheets/sheet1.xml",
         "<?xml version=\"1.0\"?><worksheet xmlns=\"http://schemas.openxmlformats.org/"
         "spreadsheetml/2006/main\"><sheetData>" + sheet_body +
         "</sheetData></worksheet>"},
    };
    for (const auto& e : extra) entries.push_back(e);
    return make_zip(entries);
}

void test_xlsx_fixes() {
    std::cerr << "\nXLSX rels/comments:\n";

    TEST(absolute_rels_target_inlinestr)
        // openpyxl-style absolute Target ("/xl/...") + inlineStr cells used to
        // yield an empty sheet; the path must normalize and the value appear.
        auto md = convert_xlsx(make_xlsx(
            "/xl/worksheets/sheet1.xml",
            "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>InlineHello</t></is></c></row>"));
        ASSERT(md.find("InlineHello") != std::string::npos);
        ASSERT(md.find("Empty sheet") == std::string::npos);
    TEST_END

    TEST(comment_on_empty_cell)
        // A comment anchored to a cell with no <c> element must still be emitted.
        std::string sheet =
            "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>Val</t></is></c></row>";
        std::string sheet_rels =
            "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/"
            "package/2006/relationships\"><Relationship Id=\"rIdC\" Type=\""
            "http://schemas.openxmlformats.org/officeDocument/2006/relationships/comments\""
            " Target=\"../comments1.xml\"/></Relationships>";
        std::string comments =
            "<?xml version=\"1.0\"?><comments xmlns=\"http://schemas.openxmlformats.org/"
            "spreadsheetml/2006/main\"><authors><author>Rev</author></authors>"
            "<commentList><comment ref=\"E1\" authorId=\"0\"><text><t>EmptyCellMemo"
            "</t></text></comment></commentList></comments>";
        auto md = convert_xlsx(make_xlsx(
            "worksheets/sheet1.xml", sheet,
            {{"xl/worksheets/_rels/sheet1.xml.rels", sheet_rels},
             {"xl/comments1.xml", comments}}));
        ASSERT(md.find("EmptyCellMemo") != std::string::npos);
    TEST_END

    TEST(threaded_comment_with_author)
        // Threaded comments resolve personId -> displayName and join a thread.
        std::string sheet =
            "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>Val</t></is></c></row>";
        std::string sheet_rels =
            "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/"
            "package/2006/relationships\"><Relationship Id=\"rIdT\" Type=\""
            "http://schemas.microsoft.com/office/2017/10/relationships/threadedComment\""
            " Target=\"../threadedComments/threadedComment1.xml\"/></Relationships>";
        std::string persons =
            "<?xml version=\"1.0\"?><personList xmlns=\"http://schemas.microsoft.com/"
            "office/spreadsheetml/2018/threadedcomments\">"
            "<person displayName=\"Kim\" id=\"{P1}\"/></personList>";
        std::string tc =
            "<?xml version=\"1.0\"?><ThreadedComments xmlns=\"http://schemas.microsoft.com/"
            "office/spreadsheetml/2018/threadedcomments\">"
            "<threadedComment ref=\"B2\" personId=\"{P1}\" id=\"{T1}\"><text>needs review"
            "</text></threadedComment></ThreadedComments>";
        auto md = convert_xlsx(make_xlsx(
            "worksheets/sheet1.xml", sheet,
            {{"xl/worksheets/_rels/sheet1.xml.rels", sheet_rels},
             {"xl/persons/person1.xml", persons},
             {"xl/threadedComments/threadedComment1.xml", tc}}));
        ASSERT(md.find("Kim: needs review") != std::string::npos);
    TEST_END
}

// ── Image references name the file that was written ──────────

// Every "![alt](target)" in the markdown, in document order.
static std::vector<std::string> image_targets(const std::string& md) {
    std::vector<std::string> targets;
    for (size_t p = md.find("]("); p != std::string::npos;
         p = md.find("](", p + 2)) {
        size_t end = md.find(')', p);
        if (end == std::string::npos) break;
        targets.push_back(md.substr(p + 2, end - p - 2));
    }
    return targets;
}

// Fail unless every reference resolves to a file that is actually in dir.
static void assert_targets_exist(const std::string& md, const std::string& dir) {
    auto targets = image_targets(md);
    if (targets.empty()) throw std::runtime_error("no image references emitted");
    for (const auto& t : targets) {
        std::string base = t.substr(t.find_last_of('/') + 1);
        if (!std::filesystem::exists(std::filesystem::path(dir) / base))
            throw std::runtime_error("reference points at a missing file: " + t);
    }
}

static std::string make_media_xlsx() {
    return make_xlsx(
        "worksheets/sheet1.xml",
        "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>Cell</t></is></c></row>",
        {{"xl/media/image1.png", png_bytes()}});
}

void test_image_reference_matches_file() {
    std::cerr << "\nImage reference names the written file:\n";

    TEST(xlsx_reference_carries_the_extension)
        // The reference used to be the bare stem ("page1_img0"), which resolves
        // to nothing: the file on disk is "page1_img0.png".
        auto book = make_media_xlsx();
        std::string dir = temp_image_dir("xlsx_image_ref");
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;
        auto md = jdoc::office_to_markdown_mem(
            reinterpret_cast<const uint8_t*>(book.data()), book.size(),
            "book.xlsx", opts);
        assert_targets_exist(md, dir);
        std::filesystem::remove_all(dir);
    TEST_END

    TEST(xlsx_reference_follows_the_collision_suffix)
        // A second conversion into the same image_dir cannot overwrite the
        // first document's file, so it is written as "page1_img0_1.png" — and
        // the markdown has to say so, or it points at the other document.
        auto book = make_media_xlsx();
        std::string dir = temp_image_dir("xlsx_image_ref_shared");
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;

        auto first = jdoc::office_to_markdown_mem(
            reinterpret_cast<const uint8_t*>(book.data()), book.size(),
            "book.xlsx", opts);
        auto second = jdoc::office_to_markdown_mem(
            reinterpret_cast<const uint8_t*>(book.data()), book.size(),
            "book.xlsx", opts);

        assert_targets_exist(first, dir);
        assert_targets_exist(second, dir);
        ASSERT(image_targets(first) != image_targets(second));
        std::filesystem::remove_all(dir);
    TEST_END

    TEST(xlsx_chunk_reference_matches_markdown)
        // The chunk API emits its own references; they used to drop both the
        // extension and image_ref_prefix that the markdown API applies.
        auto book = make_media_xlsx();
        std::string dir = temp_image_dir("xlsx_image_ref_chunks");
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;
        opts.image_ref_prefix = "media/";

        auto chunks = jdoc::office_to_markdown_chunks_mem(
            reinterpret_cast<const uint8_t*>(book.data()), book.size(),
            "book.xlsx", opts);
        std::string text;
        for (const auto& c : chunks) text += c.text;
        auto targets = image_targets(text);
        ASSERT(targets.size() == 1);
        ASSERT(targets[0].rfind("media/", 0) == 0);
        assert_targets_exist(text, dir);
        std::filesystem::remove_all(dir);
    TEST_END

    TEST(declared_extension_is_kept_verbatim)
        // The extension the package declared is what the extracted file gets.
        // It used to round-trip through the format name, which is lossy:
        // ".jpeg" came back ".jpg", ".tif" came back ".tiff", and anything
        // jdoc has no format name for (".webp") came back ".bin".
        auto book = make_xlsx(
            "worksheets/sheet1.xml",
            "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>C</t></is></c></row>",
            {{"xl/media/image1.jpeg", png_bytes()},
             {"xl/media/image2.webp", png_bytes()},
             {"xl/media/image3.tif",  png_bytes()}});
        std::string dir = temp_image_dir("xlsx_declared_ext");
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;

        auto md = jdoc::office_to_markdown_mem(
            reinterpret_cast<const uint8_t*>(book.data()), book.size(),
            "book.xlsx", opts);
        assert_targets_exist(md, dir);
        std::set<std::string> written;
        for (auto& e : std::filesystem::directory_iterator(dir))
            written.insert(e.path().filename().string());
        ASSERT(written == std::set<std::string>({"page1_img0.jpeg",
                                                 "page1_img1.webp",
                                                 "page1_img2.tif"}));
        std::filesystem::remove_all(dir);
    TEST_END

    TEST(a_failed_write_emits_no_reference)
        // Every parser now routes through store_image, so a write that cannot
        // happen costs the reference too — a link to a file that does not exist
        // is worse than no link. image_dir points inside a regular file here,
        // so the directory cannot be created and no image can land.
        std::string base = temp_image_dir("write_failure");
        std::string blocker = base + "/not_a_dir";
        { std::ofstream f(blocker); f << "x"; }

        auto book = make_media_xlsx();
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = blocker + "/images";

        auto md = jdoc::office_to_markdown_mem(
            reinterpret_cast<const uint8_t*>(book.data()), book.size(),
            "book.xlsx", opts);
        ASSERT(md.find("![") == std::string::npos);
        std::filesystem::remove_all(base);
    TEST_END

    TEST(archive_member_keeps_its_own_name_and_leaves_no_empty_dirs)
        // An archive member is a file with a name of its own, so it keeps it
        // rather than becoming page1_img0 — and a member that holds no images
        // must not leave an empty directory behind for itself.
        std::string dir = temp_image_dir("archive_members");
        auto zip = make_zip({{"photo.jpeg", png_bytes()},
                             {"note.txt", "hello"}});
        std::string zip_path = (std::filesystem::temp_directory_path() /
                                "jdoc_test_members.zip").string();
        { std::ofstream f(zip_path, std::ios::binary); f << zip; }

        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;
        opts.image_ref_prefix = "out/";

        std::string md;
        jdoc::convert_archive(zip_path,
            [&](jdoc::MemberResult&& m) { md += m.markdown; return true; },
            opts);

        ASSERT(std::filesystem::exists(std::filesystem::path(dir) / "photo.jpeg"));
        ASSERT(!std::filesystem::exists(std::filesystem::path(dir) / "note.txt"));
        ASSERT(md.find("out/photo.jpeg") != std::string::npos);
        std::filesystem::remove(zip_path);
        std::filesystem::remove_all(dir);
    TEST_END

    TEST(pptx_reference_follows_the_collision_suffix)
        // Guard the paths that already got this right against regressing.
        auto deck = make_shared_media_pptx();
        std::string dir = temp_image_dir("pptx_image_ref_shared");
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;

        auto first = jdoc::office_to_markdown_mem(
            reinterpret_cast<const uint8_t*>(deck.data()), deck.size(),
            "deck.pptx", opts);
        auto second = jdoc::office_to_markdown_mem(
            reinterpret_cast<const uint8_t*>(deck.data()), deck.size(),
            "deck.pptx", opts);

        assert_targets_exist(first, dir);
        assert_targets_exist(second, dir);
        ASSERT(image_targets(first) != image_targets(second));
        std::filesystem::remove_all(dir);
    TEST_END
}

// ── Legacy .xls image extraction ─────────────────────────────

// A compound file holding one "Workbook" stream — all the .xls image scanner
// reads. The stream is padded past the mini-stream cutoff so the regular FAT
// chain carries it and no mini-FAT is needed.
static std::string make_ole_workbook(const std::string& workbook) {
    constexpr size_t SEC = 512;
    constexpr uint32_t FREESECT = 0xFFFFFFFF;
    constexpr uint32_t ENDOFCHAIN = 0xFFFFFFFE;
    constexpr uint32_t FATSECT = 0xFFFFFFFD;
    constexpr uint32_t MINI_CUTOFF = 4096;

    auto set_u16_at = [](std::string& b, size_t off, uint16_t v) {
        b[off] = static_cast<char>(v & 0xFF);
        b[off + 1] = static_cast<char>(v >> 8);
    };
    auto set_u32_at = [&](std::string& b, size_t off, uint32_t v) {
        set_u16_at(b, off, static_cast<uint16_t>(v & 0xFFFF));
        set_u16_at(b, off + 2, static_cast<uint16_t>(v >> 16));
    };

    std::string data = workbook;
    size_t want = std::max<size_t>(data.size(), MINI_CUTOFF);
    want = ((want + SEC - 1) / SEC) * SEC;
    data.resize(want, '\0');
    // Declare the padded size: a stream under the mini-stream cutoff would
    // be looked for in the root mini-stream, which this fixture has not got.
    const uint32_t stream_size = static_cast<uint32_t>(data.size());

    const uint32_t fat_sec = 0, dir_sec = 1, first_data = 2;
    const uint32_t sectors = static_cast<uint32_t>(data.size() / SEC);

    std::vector<uint32_t> fat(SEC / 4, FREESECT);
    fat[fat_sec] = FATSECT;
    fat[dir_sec] = ENDOFCHAIN;
    for (uint32_t k = 0; k < sectors; k++)
        fat[first_data + k] =
            (k + 1 < sectors) ? (first_data + k + 1) : ENDOFCHAIN;

    // Directory: Root Entry, then the Workbook stream.
    std::string dir(SEC, '\0');
    struct Ent { const char* name; uint8_t type; uint32_t start, size; };
    const Ent ents[] = {
        {"Root Entry", 5, 0, 0},
        {"Workbook",   2, first_data, stream_size},
    };
    for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
        size_t off = i * 128;
        size_t len = std::strlen(ents[i].name);
        for (size_t c = 0; c < len; c++)
            set_u16_at(dir, off + c * 2, static_cast<uint16_t>(ents[i].name[c]));
        set_u16_at(dir, off + 0x40, static_cast<uint16_t>((len + 1) * 2));
        dir[off + 0x42] = static_cast<char>(ents[i].type);
        set_u32_at(dir, off + 0x44, FREESECT);  // left sibling
        set_u32_at(dir, off + 0x48, FREESECT);  // right sibling
        set_u32_at(dir, off + 0x4C, i == 0 ? 1u : FREESECT);  // child
        set_u32_at(dir, off + 0x74, ents[i].start);
        set_u32_at(dir, off + 0x78, ents[i].size);
    }
    for (size_t i = 2; i < SEC / 128; i++) {
        set_u32_at(dir, i * 128 + 0x44, FREESECT);
        set_u32_at(dir, i * 128 + 0x48, FREESECT);
        set_u32_at(dir, i * 128 + 0x4C, FREESECT);
    }

    std::string hdr(SEC, '\0');
    const unsigned char sig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    std::memcpy(&hdr[0], sig, 8);
    set_u16_at(hdr, 0x18, 0x003E);
    set_u16_at(hdr, 0x1A, 3);
    set_u16_at(hdr, 0x1C, 0xFFFE);
    set_u16_at(hdr, 0x1E, 9);
    set_u16_at(hdr, 0x20, 6);
    set_u32_at(hdr, 0x2C, 1);
    set_u32_at(hdr, 0x30, dir_sec);
    set_u32_at(hdr, 0x38, MINI_CUTOFF);
    set_u32_at(hdr, 0x3C, ENDOFCHAIN);
    set_u32_at(hdr, 0x40, 0);
    set_u32_at(hdr, 0x44, ENDOFCHAIN);
    set_u32_at(hdr, 0x48, 0);
    set_u32_at(hdr, 0x4C, fat_sec);
    for (int i = 1; i < 109; i++) set_u32_at(hdr, 0x4C + i * 4, FREESECT);

    std::string fat_bytes(SEC, '\0');
    for (size_t i = 0; i < fat.size(); i++) set_u32_at(fat_bytes, i * 4, fat[i]);

    return hdr + fat_bytes + dir + data;
}

// A Workbook stream carrying one MSODRAWING record whose OfficeArt payload is
// a PNG BLIP (record type 0xF01E, 17-byte header before the payload).
static std::string workbook_with_png_blip() {
    const std::string png = png_bytes();

    std::string blip;
    put_u16(blip, 0x6E00);   // ver 0, inst 0x6E0 — the 17-byte-header flavour
    put_u16(blip, 0xF01E);   // BLIP: PNG
    put_u32(blip, static_cast<uint32_t>(17 + png.size()));
    blip.append(16, '\0');   // BLIP UID
    blip.push_back('\0');    // tag
    blip += png;

    std::string wb;
    put_u16(wb, 0x0809);     // BOF — workbook globals
    put_u16(wb, 4);
    put_u16(wb, 0x0600);
    put_u16(wb, 0x0005);

    const std::string sheet_name = "Sheet1";
    put_u16(wb, 0x0085);     // BOUNDSHEET
    put_u16(wb, static_cast<uint16_t>(8 + sheet_name.size()));
    put_u32(wb, 0);          // lbPlyPos
    put_u16(wb, 0);          // grbit: visible worksheet
    wb.push_back(static_cast<char>(sheet_name.size()));
    wb.push_back(0);         // 8-bit name
    wb += sheet_name;

    put_u16(wb, 0x00EC);     // MSODRAWING
    put_u16(wb, static_cast<uint16_t>(blip.size()));
    wb += blip;
    put_u16(wb, 0x000A);     // EOF
    put_u16(wb, 0);
    return wb;
}

void test_xls_images() {
    std::cerr << "\nLegacy .xls images:\n";

    TEST(blip_is_written_to_image_dir)
        // .xls used to emit an image reference without ever writing the file:
        // extract_images() ran, nothing called the writer.
        auto book = make_ole_workbook(workbook_with_png_blip());
        std::string dir = temp_image_dir("xls_images");
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;

        auto md = jdoc::office_to_markdown_mem(
            reinterpret_cast<const uint8_t*>(book.data()), book.size(),
            "book.xls", opts);
        ASSERT(md.find("![") != std::string::npos);
        assert_targets_exist(md, dir);
        std::filesystem::remove_all(dir);
    TEST_END

    TEST(chunk_images_are_written_too)
        auto book = make_ole_workbook(workbook_with_png_blip());
        std::string dir = temp_image_dir("xls_images_chunks");
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;

        auto chunks = jdoc::office_to_markdown_chunks_mem(
            reinterpret_cast<const uint8_t*>(book.data()), book.size(),
            "book.xls", opts);
        size_t saved = 0;
        for (const auto& c : chunks)
            for (const auto& img : c.images)
                if (!img.saved_path.empty() &&
                    std::filesystem::exists(img.saved_path))
                    saved++;
        ASSERT(saved == 1);
        std::filesystem::remove_all(dir);
    TEST_END

    TEST(memory_mode_keeps_the_bytes)
        // With no image_dir the payload has to stay on the chunk.
        auto book = make_ole_workbook(workbook_with_png_blip());
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;

        auto chunks = jdoc::office_to_markdown_chunks_mem(
            reinterpret_cast<const uint8_t*>(book.data()), book.size(),
            "book.xls", opts);
        size_t with_payload = 0;
        for (const auto& c : chunks)
            for (const auto& img : c.images)
                if (!img.data.empty()) with_payload++;
        ASSERT(with_payload == 1);
    TEST_END
}

// ── RTF images ───────────────────────────────────────────────

// An RTF holding `count` \pngblip pictures, hex-encoded the way Word writes
// them.
static std::string make_rtf_with_pictures(int count) {
    static const char* kHex = "0123456789abcdef";
    const std::string raw = png_bytes();
    std::string hex;
    for (unsigned char c : raw) { hex += kHex[c >> 4]; hex += kHex[c & 0x0F]; }

    std::string rtf = "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Times;}}\n"
                      "\\pard body text\\par\n";
    for (int i = 0; i < count; i++)
        rtf += "{\\pict\\pngblip\\picw1\\pich1 " + hex + "}\n";
    rtf += "}";
    return rtf;
}

static std::string convert_rtf_with(const std::string& rtf,
                                    const jdoc::ConvertOptions& opts) {
    return jdoc::office_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(rtf.data()), rtf.size(), "doc.rtf",
        opts);
}

void test_rtf_images() {
    std::cerr << "\nRTF images:\n";

    TEST(pictures_are_written_to_image_dir)
        // RTF referenced its pictures without ever writing one: nothing in
        // rtf_parser.cpp called the writer.
        std::string dir = temp_image_dir("rtf_images");
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;

        auto md = convert_rtf_with(make_rtf_with_pictures(2), opts);
        ASSERT(count_occurrences(md, "![") == 2);
        assert_targets_exist(md, dir);
        std::filesystem::remove_all(dir);
    TEST_END

    TEST(names_match_every_other_parser)
        // to_markdown used to name them "rtf_image_1" while its own to_chunks
        // called the same picture "page1_img0".
        std::string dir = temp_image_dir("rtf_image_names");
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;

        auto md = convert_rtf_with(make_rtf_with_pictures(1), opts);
        auto chunks = jdoc::office_to_markdown_chunks_mem(
            reinterpret_cast<const uint8_t*>(make_rtf_with_pictures(1).data()),
            make_rtf_with_pictures(1).size(), "doc.rtf", opts);
        ASSERT(image_targets(md).size() == 1);
        ASSERT(image_targets(md)[0].find("page1_img0.png") != std::string::npos);
        ASSERT(chunks.size() == 1);
        ASSERT(chunks[0].images.size() == 1);
        ASSERT(chunks[0].images[0].name == "page1_img0");
        ASSERT(!chunks[0].images[0].saved_path.empty());
        std::filesystem::remove_all(dir);
    TEST_END

    TEST(memory_mode_keeps_the_bytes)
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;

        std::string rtf = make_rtf_with_pictures(1);
        auto chunks = jdoc::office_to_markdown_chunks_mem(
            reinterpret_cast<const uint8_t*>(rtf.data()), rtf.size(), "doc.rtf",
            opts);
        ASSERT(chunks[0].images.size() == 1);
        ASSERT(!chunks[0].images[0].data.empty());
    TEST_END
}

// ── XLSX streaming (SAX) path ────────────────────────────────

// A workbook exercising every construct the streaming scanner must replicate
// from the DOM path: shared strings (entities, rich runs, CRLF), inlineStr,
// bold styles, date formats, comments (incl. one on a cell with no <c>),
// row gaps, and a <dimension> extent.
static std::string make_streaming_xlsx() {
    std::string sst =
        "<?xml version=\"1.0\"?><sst xmlns=\"http://schemas.openxmlformats.org/"
        "spreadsheetml/2006/main\" count=\"4\" uniqueCount=\"4\">"
        "<si><t>plain &amp; escaped &lt;x&gt;</t></si>"
        "<si><r><t>rich</t></r><r><t xml:space=\"preserve\"> runs</t></r></si>"
        "<si><t>line1\r\nline2</t></si>"
        "<si><t/></si></sst>";
    std::string styles =
        "<?xml version=\"1.0\"?><styleSheet xmlns=\"http://schemas.openxmlformats.org/"
        "spreadsheetml/2006/main\">"
        "<fonts count=\"2\"><font/><font><b/></font></fonts>"
        "<cellXfs count=\"3\"><xf numFmtId=\"0\" fontId=\"0\"/>"
        "<xf numFmtId=\"0\" fontId=\"1\"/>"
        "<xf numFmtId=\"14\" fontId=\"0\"/></cellXfs></styleSheet>";
    std::string sheet =
        "<row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c>"
        "<c r=\"B1\" t=\"s\" s=\"1\"><v>1</v></c>"
        "<c r=\"C1\" t=\"s\"><v>2</v></c></row>"
        // gap: rows 2-4 empty
        "<row r=\"5\"><c r=\"B5\" s=\"2\"><v>45108</v></c>"
        "<c r=\"D5\" t=\"inlineStr\"><is><t>inline&#65;</t></is></c>"
        "<c r=\"E5\" t=\"b\"><v>1</v></c></row>";
    std::string sheet_rels =
        "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/"
        "package/2006/relationships\"><Relationship Id=\"rIdC\" Type=\""
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/comments\""
        " Target=\"../comments1.xml\"/></Relationships>";
    std::string comments =
        "<?xml version=\"1.0\"?><comments xmlns=\"http://schemas.openxmlformats.org/"
        "spreadsheetml/2006/main\"><authors><author>R</author></authors>"
        "<commentList><comment ref=\"A1\" authorId=\"0\"><text><t>on cell</t></text>"
        "</comment><comment ref=\"C7\" authorId=\"0\"><text><t>past end</t></text>"
        "</comment></commentList></comments>";
    // dimension present; sheet body wrapped manually to include it
    std::string sheet_xml =
        "<?xml version=\"1.0\"?><worksheet xmlns=\"http://schemas.openxmlformats.org/"
        "spreadsheetml/2006/main\"><dimension ref=\"A1:E5\"/><sheetData>" + sheet +
        "</sheetData></worksheet>";
    std::vector<std::pair<std::string, std::string>> entries = {
        {"[Content_Types].xml",
         "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/"
         "package/2006/content-types\"><Default Extension=\"xml\" "
         "ContentType=\"application/xml\"/></Types>"},
        {"xl/workbook.xml",
         "<?xml version=\"1.0\"?><workbook xmlns=\"http://schemas.openxmlformats.org/"
         "spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/"
         "officeDocument/2006/relationships\"><sheets>"
         "<sheet name=\"Sheet1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>"},
        {"xl/_rels/workbook.xml.rels",
         "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/"
         "package/2006/relationships\"><Relationship Id=\"rId1\" Type=\""
         "http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\""
         " Target=\"worksheets/sheet1.xml\"/></Relationships>"},
        {"xl/sharedStrings.xml", sst},
        {"xl/styles.xml", styles},
        {"xl/worksheets/sheet1.xml", sheet_xml},
        {"xl/worksheets/_rels/sheet1.xml.rels", sheet_rels},
        {"xl/comments1.xml", comments},
    };
    return make_zip(entries);
}

void test_xlsx_streaming() {
    std::cerr << "\nXLSX streaming path:\n";

    std::string book = make_streaming_xlsx();

    TEST(streamed_output_matches_dom)
        unset_env("JDOC_XLSX_STREAM_THRESHOLD");
        auto dom = convert_xlsx(book);
        set_env("JDOC_XLSX_STREAM_THRESHOLD", "0");
        auto sax = convert_xlsx(book);
        unset_env("JDOC_XLSX_STREAM_THRESHOLD");
        ASSERT(!dom.empty());
        ASSERT(dom == sax);
    TEST_END

    TEST(streamed_content_correct)
        set_env("JDOC_XLSX_STREAM_THRESHOLD", "0");
        auto md = convert_xlsx(book);
        unset_env("JDOC_XLSX_STREAM_THRESHOLD");
        // shared string with entities + merged comment
        ASSERT(md.find("plain & escaped <x> [on cell]") != std::string::npos);
        // bold shared string
        ASSERT(md.find("**rich runs**") != std::string::npos);
        // CRLF normalized then sanitized to a space
        ASSERT(md.find("line1 line2") != std::string::npos);
        // date format applied to numeric cell (45108 = 2023-07-01)
        ASSERT(md.find("2023-07-01") != std::string::npos);
        // inlineStr with numeric char ref, boolean
        ASSERT(md.find("inlineA") != std::string::npos);
        ASSERT(md.find("TRUE") != std::string::npos);
        // comment anchored past the last row appears
        ASSERT(md.find("[past end]") != std::string::npos);
        ASSERT(md.find("Empty sheet") == std::string::npos);
    TEST_END

    TEST(streamed_empty_sheet)
        set_env("JDOC_XLSX_STREAM_THRESHOLD", "0");
        auto md = convert_xlsx(make_xlsx("worksheets/sheet1.xml", ""));
        unset_env("JDOC_XLSX_STREAM_THRESHOLD");
        ASSERT(md.find("Empty sheet") != std::string::npos);
    TEST_END
}

// ── XLS SST CONTINUE-boundary decoding ───────────────────────

void test_xls_sst_continue() {
    std::cerr << "\nXLS SST continuation:\n";

    // 1000 unique strings — the SST spans multiple CONTINUE records, and a
    // string straddles nearly every boundary. Flat concatenation used to feed
    // the continuation's option-flags byte into the character stream,
    // corrupting or dropping everything after the first split (~450 strings).
    TEST(sst_survives_continue_boundaries)
        auto md = jdoc::office_to_markdown("test/fixtures/xls/sst_continue.xls");
        ASSERT(count_occurrences(md, "unique_string_number_") == 1000);
        ASSERT(md.find("unique_string_number_0000@example.com") != std::string::npos);
        ASSERT(md.find("unique_string_number_0446@example.com") != std::string::npos);
        ASSERT(md.find("unique_string_number_0999@example.com") != std::string::npos);
    TEST_END
}

// ── XLSB sparse-cell storage ────────────────────────────────

static void put_xlsb_varint(std::string& out, uint32_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        out.push_back(static_cast<char>(byte | (value ? 0x80 : 0)));
    } while (value);
}

static void put_xlsb_widestring(std::string& out, const std::string& text) {
    put_u32(out, static_cast<uint32_t>(text.size()));
    for (unsigned char ch : text) put_u16(out, ch);
}

static void put_xlsb_record(std::string& out, uint16_t type,
                            const std::string& payload) {
    put_xlsb_varint(out, type);
    put_xlsb_varint(out, static_cast<uint32_t>(payload.size()));
    out += payload;
}

void test_xlsb_sparse_cells() {
    std::cerr << "\nXLSB sparse cells:\n";

    std::string bundle(8, '\0');
    put_xlsb_widestring(bundle, "rId1");
    put_xlsb_widestring(bundle, "Sheet1");
    std::string workbook;
    put_xlsb_record(workbook, 0x9C, bundle);  // BrtBundleSh

    std::string sheet;
    std::string row(4, '\0');
    put_xlsb_record(sheet, 0x00, row);  // BrtRowHdr, row 0
    auto add_string = [&](uint32_t col, const std::string& value) {
        std::string cell;
        put_u32(cell, col);
        put_u32(cell, 0);  // style
        put_xlsb_widestring(cell, value);
        put_xlsb_record(sheet, 0x08, cell);  // BrtFmlaString
    };
    add_string(1, "old");
    add_string(0, "first");  // deliberately out of order
    add_string(1, "last");   // duplicate: last record wins

    std::string rels =
        "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/"
        "package/2006/relationships\"><Relationship Id=\"rId1\" Target=\""
        "worksheets/sheet1.bin\"/></Relationships>";
    auto package = make_zip({
        {"xl/workbook.bin", workbook},
        {"xl/_rels/workbook.bin.rels", rels},
        {"xl/worksheets/sheet1.bin", sheet},
    });

    TEST(flat_cells_keep_row_order_and_last_duplicate)
        jdoc::ZipReader zip(reinterpret_cast<const uint8_t*>(package.data()),
                            package.size());
        jdoc::XlsbParser parser(zip);
        auto md = parser.to_markdown({});
        ASSERT(md.find("| first | last |") != std::string::npos);
        ASSERT(md.find("old") == std::string::npos);
    TEST_END
}

// ── HTML charset detection ───────────────────────────────────

static std::string convert_html(const std::string& html) {
    return jdoc::office_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(html.data()), html.size(), "page.html");
}

// ── HTML embedded images ─────────────────────────────────────

// Base64 for the 1x1 PNG, so a data: URI can carry it the way a browser-saved
// page or an HTML mail body does.
static std::string png_base64() {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const std::string raw = png_bytes();
    std::string out;
    for (size_t i = 0; i < raw.size(); i += 3) {
        uint32_t v = static_cast<unsigned char>(raw[i]) << 16;
        if (i + 1 < raw.size()) v |= static_cast<unsigned char>(raw[i + 1]) << 8;
        if (i + 2 < raw.size()) v |= static_cast<unsigned char>(raw[i + 2]);
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += (i + 1 < raw.size()) ? kAlphabet[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < raw.size()) ? kAlphabet[v & 0x3F] : '=';
    }
    return out;
}

static std::string convert_html_with(const std::string& html,
                                     const jdoc::ConvertOptions& opts) {
    return jdoc::office_to_markdown_mem(
        reinterpret_cast<const uint8_t*>(html.data()), html.size(),
        "page.html", opts);
}

void test_html_images() {
    std::cerr << "\nHTML embedded images:\n";

    const std::string data_img =
        "<img src=\"data:image/png;base64," + png_base64() + "\" alt=\"logo\">";

    TEST(data_uri_is_extracted_to_image_dir)
        // The picture lives in the document, so it is extracted like any other
        // embedded media — and the reference names the file that was written
        // instead of inlining a base64 blob into the markdown.
        std::string dir = temp_image_dir("html_data_uri");
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;

        auto md = convert_html_with("<html><body>" + data_img + "</body></html>",
                                    opts);
        ASSERT(md.find("base64") == std::string::npos);
        assert_targets_exist(md, dir);
        std::filesystem::remove_all(dir);
    TEST_END

    TEST(data_uri_reference_honours_the_prefix)
        std::string dir = temp_image_dir("html_data_uri_prefix");
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;
        opts.image_ref_prefix = "media/";

        auto md = convert_html_with("<html><body>" + data_img + "</body></html>",
                                    opts);
        auto targets = image_targets(md);
        ASSERT(targets.size() == 1);
        ASSERT(targets[0] == "media/page1_img0.png");
        assert_targets_exist(md, dir);
        std::filesystem::remove_all(dir);
    TEST_END

    TEST(external_src_is_left_alone)
        // An http(s) or relative src names a file outside the document. There
        // are no bytes to write, and the reference stays as the author wrote it.
        std::string dir = temp_image_dir("html_external_src");
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;

        auto md = convert_html_with(
            "<html><body><img src=\"https://example.com/a.png\" alt=\"x\">"
            "<img src=\"local.jpg\" alt=\"y\"></body></html>", opts);
        auto targets = image_targets(md);
        ASSERT(targets.size() == 2);
        ASSERT(targets[0] == "https://example.com/a.png");
        ASSERT(targets[1] == "local.jpg");
        // and nothing was invented on disk for them
        ASSERT(std::filesystem::is_empty(dir));
        std::filesystem::remove_all(dir);
    TEST_END

    TEST(each_image_is_referenced_once)
        // The old appendix emitted a second reference per image, pointing at a
        // file that never existed.
        std::string dir = temp_image_dir("html_single_reference");
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;
        opts.image_dir = dir;

        auto md = convert_html_with("<html><body>" + data_img + "</body></html>",
                                    opts);
        ASSERT(count_occurrences(md, "![") == 1);
        std::filesystem::remove_all(dir);
    TEST_END

    TEST(memory_mode_keeps_the_bytes)
        // With no image_dir the decoded payload travels on the chunk.
        jdoc::ConvertOptions opts;
        opts.images = true;
        opts.min_image_size = 0;

        std::string html = "<html><body>" + data_img + "</body></html>";
        auto chunks = jdoc::office_to_markdown_chunks_mem(
            reinterpret_cast<const uint8_t*>(html.data()), html.size(),
            "page.html", opts);
        size_t with_payload = 0;
        for (const auto& c : chunks)
            for (const auto& img : c.images)
                if (!img.data.empty()) with_payload++;
        ASSERT(with_payload == 1);
    TEST_END
}

void test_html_charset() {
    std::cerr << "\nHTML charset:\n";

    // "주민번호" in EUC-KR bytes and in UTF-8 bytes.
    const std::string euckr = "\xC1\xD6\xB9\xCE\xB9\xF8\xC8\xA3";
    const std::string utf8  = "\xEC\xA3\xBC\xEB\xAF\xBC\xEB\xB2\x88\xED\x98\xB8";
    const std::string replacement = "\xEF\xBF\xBD";  // U+FFFD

    TEST(euckr_no_declaration_decoded)
        // charset-less EUC-KR (the real sample) is rescued by the heuristic.
        std::string html = "<html><body><p>" + euckr + "</p></body></html>";
        auto md = convert_html(html);
        ASSERT(md.find(utf8) != std::string::npos);
        ASSERT(md.find(replacement) == std::string::npos);
    TEST_END

    TEST(euckr_meta_declaration_decoded)
        std::string html =
            "<html><head><meta http-equiv=\"Content-Type\" "
            "content=\"text/html; charset=euc-kr\"></head><body><p>" + euckr +
            "</p></body></html>";
        auto md = convert_html(html);
        ASSERT(md.find(utf8) != std::string::npos);
    TEST_END

    TEST(utf8_passthrough)
        // Valid UTF-8 must pass through unchanged (fast path, no double-decode).
        std::string html =
            "<html><head><meta charset=\"utf-8\"></head><body><p>" + utf8 +
            "</p></body></html>";
        auto md = convert_html(html);
        ASSERT(md.find(utf8) != std::string::npos);
        ASSERT(md.find(replacement) == std::string::npos);
    TEST_END

    TEST(utf8_mislabeled_euckr_not_corrupted)
        // A UTF-8 page carrying a stale charset=euc-kr meta must not be double-decoded.
        std::string html =
            "<html><head><meta charset=\"euc-kr\"></head><body><p>" + utf8 +
            "</p></body></html>";
        auto md = convert_html(html);
        ASSERT(md.find(utf8) != std::string::npos);
    TEST_END
}

// ── PPTX soft line breaks (<a:br>) ───────────────────────────

void test_pptx_linebreak() {
    std::cerr << "\nPPTX line breaks:\n";

    TEST(explicit_break_splits_values)
        // A single <a:p> whose lines are separated only by <a:br> (the shape
        // PowerPoint's Shift+Enter produces) must not run together.
        std::string body =
            "<p:sp><p:txBody><a:p>"
            "<a:r><a:t>label</a:t></a:r>"
            "<a:br/>"
            "<a:r><a:t>03-5595-6395</a:t></a:r>"
            "<a:br/>"
            "<a:r><a:t>03-6495-7208</a:t></a:r>"
            "</a:p></p:txBody></p:sp>";
        auto md = convert_pptx(make_pptx(body));
        ASSERT(md.find("label\n03-5595-6395\n03-6495-7208") != std::string::npos);
        ASSERT(md.find("label03-5595-6395") == std::string::npos);
    TEST_END

    TEST(runs_in_paragraph_not_broken)
        // Consecutive runs in one paragraph (no <a:br>) stay on one line.
        std::string body =
            "<p:sp><p:txBody><a:p>"
            "<a:r><a:t>#1. </a:t></a:r>"
            "<a:r><a:t>national id</a:t></a:r>"
            "</a:p></p:txBody></p:sp>";
        auto md = convert_pptx(make_pptx(body));
        ASSERT(md.find("#1. national id") != std::string::npos);
        ASSERT(md.find("#1. \nnational id") == std::string::npos);
    TEST_END

    TEST(paragraphs_still_separated)
        // Each value in its own <a:p> keeps splitting by newline (regression).
        std::string body =
            "<p:sp><p:txBody>"
            "<a:p><a:r><a:t>first line</a:t></a:r></a:p>"
            "<a:p><a:r><a:t>second line</a:t></a:r></a:p>"
            "</p:txBody></p:sp>";
        auto md = convert_pptx(make_pptx(body));
        ASSERT(md.find("first line\nsecond line") != std::string::npos);
    TEST_END

    TEST(table_cell_break_becomes_space)
        // <a:br> inside a table cell collapses to a space so the row stays intact.
        std::string body =
            "<p:graphicFrame><a:graphic><a:graphicData>"
            "<a:tbl>"
            "<a:tr><a:tc><a:txBody><a:p>"
            "<a:r><a:t>x</a:t></a:r><a:br/><a:r><a:t>y</a:t></a:r>"
            "</a:p></a:txBody></a:tc></a:tr>"
            "</a:tbl>"
            "</a:graphicData></a:graphic></p:graphicFrame>";
        auto md = convert_pptx(make_pptx(body));
        ASSERT(md.find("x y") != std::string::npos);   // joined by space in cell
        ASSERT(md.find("x\ny") == std::string::npos);   // no newline leaks into row
    TEST_END
}

// ── Main ────────────────────────────────────────────────────

// The two APIs must describe the same conversion: the same references in the
// text, and the same files on disk. They used to drift — .doc stripped the
// image markers out of its chunk text while its markdown kept them.
static void assert_apis_agree(const std::string& doc, const std::string& hint,
                              const std::string& tag) {
    std::string d1 = temp_image_dir(tag + "_md");
    std::string d2 = temp_image_dir(tag + "_ch");
    jdoc::ConvertOptions o1;
    o1.images = true;
    o1.min_image_size = 0;
    o1.image_ref_prefix = "x/";
    o1.image_dir = d1;
    jdoc::ConvertOptions o2 = o1;
    o2.image_dir = d2;

    const auto* bytes = reinterpret_cast<const uint8_t*>(doc.data());
    std::string md = jdoc::office_to_markdown_mem(bytes, doc.size(), hint, o1);
    std::string ch;
    for (auto& c : jdoc::office_to_markdown_chunks_mem(bytes, doc.size(), hint, o2))
        ch += c.text;

    auto md_refs = image_targets(md), ch_refs = image_targets(ch);
    std::set<std::string> a(md_refs.begin(), md_refs.end());
    std::set<std::string> b(ch_refs.begin(), ch_refs.end());
    if (a != b) throw std::runtime_error("APIs disagree on image references");

    std::set<std::string> f1, f2;
    for (auto& e : std::filesystem::directory_iterator(d1))
        f1.insert(e.path().filename().string());
    for (auto& e : std::filesystem::directory_iterator(d2))
        f2.insert(e.path().filename().string());
    if (f1 != f2) throw std::runtime_error("APIs disagree on files written");
    if (f1.empty()) throw std::runtime_error("no image was written at all");

    std::filesystem::remove_all(d1);
    std::filesystem::remove_all(d2);
}

void test_markdown_and_chunk_apis_agree() {
    std::cerr << "\nMarkdown and chunk APIs agree:\n";

    TEST(xlsx)
        assert_apis_agree(make_media_xlsx(), "book.xlsx", "agree_xlsx");
    TEST_END

    TEST(pptx)
        assert_apis_agree(make_shared_media_pptx(), "deck.pptx", "agree_pptx");
    TEST_END

    TEST(rtf)
        assert_apis_agree(make_rtf_with_pictures(2), "doc.rtf", "agree_rtf");
    TEST_END

    TEST(xls)
        assert_apis_agree(make_ole_workbook(workbook_with_png_blip()),
                          "book.xls", "agree_xls");
    TEST_END

    TEST(html)
        assert_apis_agree(
            "<html><body><img src=\"data:image/png;base64," + png_base64() +
            "\" alt=\"a\"></body></html>", "page.html", "agree_html");
    TEST_END
}

int main() {
    std::cerr << "=== jdoc office tests ===\n\n";

    test_format_detection();
    test_zip_reader();
    test_ole_reader();
    test_rtf_parser();
    test_ooxml_embedded_parts();
    test_dwfx_not_office();
    test_pptx_shape_tree();
    test_pptx_master_layout();
    test_pptx_shared_media();
    test_docx_header_footer();
    test_xlsx_fixes();
    test_image_reference_matches_file();
    test_xls_images();
    test_rtf_images();
    test_markdown_and_chunk_apis_agree();
    test_xlsx_streaming();
    test_xls_sst_continue();
    test_xlsb_sparse_cells();
    test_html_charset();
    test_html_images();
    test_pptx_linebreak();

    std::cerr << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===\n";

    return tests_failed > 0 ? 1 : 0;
}
