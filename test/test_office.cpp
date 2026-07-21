// test_office.cpp - Basic tests for Office document to Markdown conversion
// License: MIT

#include "jdoc/office.h"
#include "zip_reader.h"
#include "legacy/ole_reader.h"
#include <iostream>
#include <cassert>
#include <fstream>
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
        opts.images = true;
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

// ── Main ────────────────────────────────────────────────────

int main() {
    std::cerr << "=== jdoc office tests ===\n\n";

    test_format_detection();
    test_zip_reader();
    test_ole_reader();
    test_rtf_parser();
    test_pptx_shape_tree();
    test_pptx_master_layout();
    test_pptx_shared_media();
    test_docx_header_footer();

    std::cerr << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===\n";

    return tests_failed > 0 ? 1 : 0;
}
