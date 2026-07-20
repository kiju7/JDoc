// test_hwp5.cpp - Tests for HWP 5.x drawing object text extraction
// Builds synthetic OLE2 compound documents (FileHeader + DocInfo +
// BodyText/Section0 record stream) and verifies that text held inside
// drawing objects — text boxes and figure captions stored under a
// CTRL_HEADER('gso ') > LIST_HEADER > paragraph subtree — is extracted
// without duplicating table cell text.
// License: MIT

#include "jdoc/jdoc.h"
#include "jdoc/hwp.h"
#include "jdoc/hwp_types.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

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

// ── Fixture builder ─────────────────────────────────────────

namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t SEC_SIZE     = 512;
constexpr uint32_t MINI_CUTOFF  = 4096;
constexpr uint32_t FREESECT     = 0xFFFFFFFF;
constexpr uint32_t ENDOFCHAIN   = 0xFFFFFFFE;
constexpr uint32_t FATSECT      = 0xFFFFFFFD;

void put_u16(Bytes& b, uint16_t v) {
    b.push_back(uint8_t(v & 0xFF));
    b.push_back(uint8_t(v >> 8));
}

void put_u32(Bytes& b, uint32_t v) {
    put_u16(b, uint16_t(v & 0xFFFF));
    put_u16(b, uint16_t(v >> 16));
}

void set_u16(Bytes& b, size_t off, uint16_t v) {
    b[off]     = uint8_t(v & 0xFF);
    b[off + 1] = uint8_t(v >> 8);
}

void set_u32(Bytes& b, size_t off, uint32_t v) {
    set_u16(b, off, uint16_t(v & 0xFFFF));
    set_u16(b, off + 2, uint16_t(v >> 16));
}

// ── Record stream builder ───────────────────────────────────

// Emit one HWP record: 32-bit header (tag | level<<10 | size<<20) + payload.
void put_record(Bytes& b, uint16_t tag, uint16_t level, const Bytes& payload) {
    uint32_t size = uint32_t(payload.size());
    put_u32(b, uint32_t(tag & 0x3FF) | (uint32_t(level & 0x3FF) << 10) | (size << 20));
    b.insert(b.end(), payload.begin(), payload.end());
}

// Encode UTF-8 (ASCII + hangul syllables) as the UTF-16LE payload of PARA_TEXT.
Bytes para_text_payload(const std::u16string& text) {
    Bytes b;
    for (char16_t ch : text) put_u16(b, uint16_t(ch));
    return b;
}

// PARA_HEADER payload: charCount(4) + controlMask(2) + paraShapeId(2) + styleId(2)
Bytes para_header_payload(uint32_t char_count) {
    Bytes b;
    put_u32(b, char_count);
    put_u16(b, 0);   // control mask
    put_u16(b, 0);   // paraShapeId — index 0, absent from our empty DocInfo
    put_u16(b, 0);   // styleId
    return b;
}

// CTRL_HEADER payload: ctrl_id is read big-endian-ish (byte 3 is the first
// character), so "gso " is stored as ' ','o','s','g'.
Bytes ctrl_header_payload(const char id[5]) {
    Bytes b;
    for (int i = 3; i >= 0; i--) b.push_back(uint8_t(id[i]));
    return b;
}

// LIST_HEADER payload: paraCount(4) + property(4) + colAddr(2) + rowAddr(2)
// + colSpan(2) + rowSpan(2). Table cells use the address fields; drawing
// object text lists leave them zero.
Bytes list_header_payload(uint16_t col_addr, uint16_t row_addr) {
    Bytes b;
    put_u32(b, 1);   // paraCount
    put_u32(b, 0);   // property
    put_u16(b, col_addr);
    put_u16(b, row_addr);
    put_u16(b, 1);   // colSpan
    put_u16(b, 1);   // rowSpan
    return b;
}

// TABLE payload: property(4) + rowCount(2) + colCount(2) + cellSpacing(2)
// + margins(8) then the cellsPerRow array at offset 18.
Bytes table_payload(uint16_t rows, uint16_t cols) {
    Bytes b;
    put_u32(b, 0);
    put_u16(b, rows);
    put_u16(b, cols);
    put_u16(b, 0);
    for (int i = 0; i < 4; i++) put_u16(b, 0);  // margins → offset 18
    for (uint16_t r = 0; r < rows; r++) put_u16(b, cols);
    return b;
}

// Append a complete paragraph (header + text) at the given record level.
// The text record sits one level deeper, matching real HWP output.
void put_paragraph(Bytes& b, uint16_t level, const std::u16string& text) {
    put_record(b, jdoc::hwp::PARA_HEADER, level,
               para_header_payload(uint32_t(text.size())));
    put_record(b, jdoc::hwp::PARA_TEXT, uint16_t(level + 1),
               para_text_payload(text));
}

// ── OLE2 compound file builder ──────────────────────────────

struct OleStream {
    std::string name;
    Bytes data;
};

// Build a minimal CFB holding FileHeader, DocInfo and BodyText/Section0.
// Every stream is padded past the 4096-byte mini-stream cutoff so the
// regular FAT chain is used and no mini-FAT is needed.
Bytes build_ole(const Bytes& file_header, const Bytes& doc_info,
                const Bytes& section0) {
    std::vector<OleStream> streams = {
        {"FileHeader", file_header},
        {"DocInfo", doc_info},
        {"BodyText/Section0", section0},
    };

    // Pad each stream to a whole number of sectors, at least MINI_CUTOFF.
    for (auto& s : streams) {
        size_t want = std::max<size_t>(s.data.size(), MINI_CUTOFF);
        want = ((want + SEC_SIZE - 1) / SEC_SIZE) * SEC_SIZE;
        s.data.resize(want, 0);
    }

    // Sector layout: 0 = FAT, 1..2 = directory, then the stream chains.
    constexpr uint32_t FAT_SEC = 0;
    constexpr uint32_t DIR_SEC = 1;
    constexpr uint32_t DIR_SECTORS = 2;  // 5 entries x 128 bytes = 640 > 512

    uint32_t next_sec = DIR_SEC + DIR_SECTORS;
    std::vector<uint32_t> start_sec(streams.size());
    std::vector<uint32_t> sec_count(streams.size());
    for (size_t i = 0; i < streams.size(); i++) {
        start_sec[i] = next_sec;
        sec_count[i] = uint32_t(streams[i].data.size() / SEC_SIZE);
        next_sec += sec_count[i];
    }
    uint32_t total_sectors = next_sec;

    // FAT: one sector holds 128 entries, enough for this fixture.
    std::vector<uint32_t> fat(SEC_SIZE / 4, FREESECT);
    fat[FAT_SEC] = FATSECT;
    fat[DIR_SEC] = DIR_SEC + 1;
    fat[DIR_SEC + 1] = ENDOFCHAIN;
    for (size_t i = 0; i < streams.size(); i++) {
        for (uint32_t k = 0; k < sec_count[i]; k++) {
            uint32_t s = start_sec[i] + k;
            fat[s] = (k + 1 < sec_count[i]) ? (s + 1) : ENDOFCHAIN;
        }
    }

    // Directory entries: Root, FileHeader, DocInfo, BodyText, Section0.
    // find_entry/find_storage scan entries linearly by name, so only the
    // root and storage child links need to be set.
    struct DirEnt {
        std::string name;
        uint8_t type;       // 1 = storage, 2 = stream, 5 = root
        int32_t child;
        uint32_t start;
        uint32_t size;
    };
    std::vector<DirEnt> ents = {
        {"Root Entry", 5, 1, 0, 0},
        {"FileHeader", 2, -1, start_sec[0], uint32_t(streams[0].data.size())},
        {"DocInfo",    2, -1, start_sec[1], uint32_t(streams[1].data.size())},
        {"BodyText",   1,  4, 0, 0},
        {"Section0",   2, -1, start_sec[2], uint32_t(streams[2].data.size())},
    };

    Bytes dir(DIR_SECTORS * SEC_SIZE, 0);
    for (size_t i = 0; i < ents.size(); i++) {
        size_t off = i * 128;
        // Name as UTF-16LE, null terminated
        for (size_t c = 0; c < ents[i].name.size(); c++)
            set_u16(dir, off + c * 2, uint16_t(ents[i].name[c]));
        set_u16(dir, off + 0x40, uint16_t((ents[i].name.size() + 1) * 2));
        dir[off + 0x42] = ents[i].type;
        set_u32(dir, off + 0x44, FREESECT);  // left sibling
        set_u32(dir, off + 0x48, FREESECT);  // right sibling
        set_u32(dir, off + 0x4C, ents[i].child < 0 ? FREESECT
                                                   : uint32_t(ents[i].child));
        set_u32(dir, off + 0x74, ents[i].start);
        set_u32(dir, off + 0x78, ents[i].size);
    }
    // Unused directory slots are marked empty (type 0).
    for (size_t i = ents.size(); i < DIR_SECTORS * SEC_SIZE / 128; i++) {
        set_u32(dir, i * 128 + 0x44, FREESECT);
        set_u32(dir, i * 128 + 0x48, FREESECT);
        set_u32(dir, i * 128 + 0x4C, FREESECT);
    }

    // Header (sector "-1"): stream data starts at (sector + 1) * SEC_SIZE.
    Bytes hdr(SEC_SIZE, 0);
    const uint8_t sig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    std::memcpy(hdr.data(), sig, 8);
    set_u16(hdr, 0x18, 0x003E);       // minor version
    set_u16(hdr, 0x1A, 3);            // major version → 32-bit stream sizes
    set_u16(hdr, 0x1C, 0xFFFE);       // little endian
    set_u16(hdr, 0x1E, 9);            // sector shift → 512
    set_u16(hdr, 0x20, 6);            // mini sector shift → 64
    set_u32(hdr, 0x2C, 1);            // number of FAT sectors
    set_u32(hdr, 0x30, DIR_SEC);      // first directory sector
    set_u32(hdr, 0x38, MINI_CUTOFF);
    set_u32(hdr, 0x3C, ENDOFCHAIN);   // first mini-FAT sector
    set_u32(hdr, 0x40, 0);            // number of mini-FAT sectors
    set_u32(hdr, 0x44, ENDOFCHAIN);   // first DIFAT sector
    set_u32(hdr, 0x48, 0);            // number of DIFAT sectors
    set_u32(hdr, 0x4C, FAT_SEC);      // DIFAT[0]
    for (int i = 1; i < 109; i++) set_u32(hdr, 0x4C + i * 4, FREESECT);

    // Assemble: header, then every sector in order.
    Bytes out;
    out.reserve(SEC_SIZE * (total_sectors + 1));
    out.insert(out.end(), hdr.begin(), hdr.end());

    Bytes fat_sec(SEC_SIZE, 0);
    for (size_t i = 0; i < fat.size(); i++) set_u32(fat_sec, i * 4, fat[i]);
    out.insert(out.end(), fat_sec.begin(), fat_sec.end());
    out.insert(out.end(), dir.begin(), dir.end());
    for (auto& s : streams) out.insert(out.end(), s.data.begin(), s.data.end());

    return out;
}

// FileHeader stream: signature + version + uncompressed/unencrypted flags.
Bytes make_file_header() {
    Bytes b(256, 0);
    const char* sig = "HWP Document File";
    std::memcpy(b.data(), sig, std::strlen(sig));
    set_u32(b, 32, 0x05000300);  // version 5.0.3.0
    set_u32(b, 36, 0);           // properties: not compressed, not encrypted
    return b;
}

std::string convert(const Bytes& ole) {
    jdoc::ConvertOptions opts;
    opts.images = false;
    return jdoc::hwp_to_markdown_mem(ole.data(), ole.size(), opts);
}

// Count non-overlapping occurrences of needle in haystack.
int count_occurrences(const std::string& haystack, const std::string& needle) {
    int n = 0;
    for (size_t p = haystack.find(needle); p != std::string::npos;
         p = haystack.find(needle, p + needle.size()))
        n++;
    return n;
}

} // namespace

// ── Tests ───────────────────────────────────────────────────

// A gso control anchored in a body paragraph carries its caption in a
// LIST_HEADER whose paragraphs are *siblings* of the LIST_HEADER record.
// Before the fix these were skipped wholesale and the caption was lost.
static void test_gso_caption_extracted() {
    TEST(gso_caption_extracted)
    Bytes sec;
    put_paragraph(sec, 0, u"본문 문단");

    // Paragraph hosting the drawing object
    put_record(sec, jdoc::hwp::PARA_HEADER, 0, para_header_payload(0));
    put_record(sec, jdoc::hwp::PARA_TEXT, 1, para_text_payload(u""));
    put_record(sec, jdoc::hwp::CTRL_HEADER, 1, ctrl_header_payload("gso "));
    put_record(sec, jdoc::hwp::LIST_HEADER, 2, list_header_payload(0, 0));
    put_paragraph(sec, 2, u"그림 캡션 텍스트");

    auto md = convert(build_ole(make_file_header(), Bytes(64, 0), sec));
    ASSERT(md.find("본문 문단") != std::string::npos);
    ASSERT(md.find("그림 캡션 텍스트") != std::string::npos);
    TEST_END
}

// A shape record following the text list must not be swallowed by it, and
// multi-paragraph text boxes keep their paragraph breaks.
static void test_gso_multi_paragraph_textbox() {
    TEST(gso_multi_paragraph_textbox)
    Bytes sec;
    put_record(sec, jdoc::hwp::PARA_HEADER, 0, para_header_payload(0));
    put_record(sec, jdoc::hwp::CTRL_HEADER, 1, ctrl_header_payload("gso "));
    put_record(sec, jdoc::hwp::LIST_HEADER, 2, list_header_payload(0, 0));
    put_paragraph(sec, 2, u"첫째 줄");
    put_paragraph(sec, 2, u"둘째 줄");
    // Sibling shape record terminating the text list
    Bytes sc(8, 0);
    std::memcpy(sc.data(), "cer$", 4);
    put_record(sec, jdoc::hwp::SHAPE_COMPONENT, 2, sc);

    auto md = convert(build_ole(make_file_header(), Bytes(64, 0), sec));
    ASSERT(md.find("첫째 줄") != std::string::npos);
    ASSERT(md.find("둘째 줄") != std::string::npos);
    // Separate paragraphs must not be glued onto one line
    ASSERT(md.find("첫째 줄 둘째 줄") == std::string::npos);
    TEST_END
}

// Table cell text must still be rendered exactly once — descending into
// control subtrees must not make cells appear both as a table row and as
// loose body text.
static void test_table_cell_not_duplicated() {
    TEST(table_cell_not_duplicated)
    Bytes sec;
    put_record(sec, jdoc::hwp::PARA_HEADER, 0, para_header_payload(0));
    put_record(sec, jdoc::hwp::CTRL_HEADER, 1, ctrl_header_payload("tbl "));
    put_record(sec, jdoc::hwp::TABLE, 2, table_payload(1, 1));
    put_record(sec, jdoc::hwp::LIST_HEADER, 2, list_header_payload(0, 0));
    put_paragraph(sec, 2, u"셀 텍스트");

    auto md = convert(build_ole(make_file_header(), Bytes(64, 0), sec));
    ASSERT(count_occurrences(md, "셀 텍스트") == 1);
    TEST_END
}

// A gso nested inside a table cell puts its caption in the cell, once.
static void test_gso_inside_table_cell() {
    TEST(gso_inside_table_cell)
    Bytes sec;
    put_record(sec, jdoc::hwp::PARA_HEADER, 0, para_header_payload(0));
    put_record(sec, jdoc::hwp::CTRL_HEADER, 1, ctrl_header_payload("tbl "));
    put_record(sec, jdoc::hwp::TABLE, 2, table_payload(1, 1));
    put_record(sec, jdoc::hwp::LIST_HEADER, 2, list_header_payload(0, 0));
    // Cell paragraph hosting a drawing object
    put_record(sec, jdoc::hwp::PARA_HEADER, 2, para_header_payload(0));
    put_record(sec, jdoc::hwp::PARA_TEXT, 3, para_text_payload(u""));
    put_record(sec, jdoc::hwp::CTRL_HEADER, 3, ctrl_header_payload("gso "));
    put_record(sec, jdoc::hwp::LIST_HEADER, 4, list_header_payload(0, 0));
    put_paragraph(sec, 4, u"셀 안 캡션");

    auto md = convert(build_ole(make_file_header(), Bytes(64, 0), sec));
    ASSERT(count_occurrences(md, "셀 안 캡션") == 1);
    // Rendered as part of the table, not as loose body text
    ASSERT(md.find("| 셀 안 캡션 |") != std::string::npos);
    TEST_END
}

// Unknown control types are still skipped wholesale, and a truncated
// control subtree must not crash or hang the parser.
static void test_unknown_control_skipped() {
    TEST(unknown_control_skipped)
    Bytes sec;
    put_paragraph(sec, 0, u"보이는 본문");
    put_record(sec, jdoc::hwp::PARA_HEADER, 0, para_header_payload(0));
    put_record(sec, jdoc::hwp::CTRL_HEADER, 1, ctrl_header_payload("zzz "));
    put_record(sec, jdoc::hwp::LIST_HEADER, 2, list_header_payload(0, 0));
    put_paragraph(sec, 2, u"숨은 텍스트");

    auto md = convert(build_ole(make_file_header(), Bytes(64, 0), sec));
    ASSERT(md.find("보이는 본문") != std::string::npos);
    ASSERT(md.find("숨은 텍스트") == std::string::npos);
    TEST_END
}

// A gso whose LIST_HEADER is the last record in the stream must terminate.
static void test_truncated_gso_terminates() {
    TEST(truncated_gso_terminates)
    Bytes sec;
    put_paragraph(sec, 0, u"본문");
    put_record(sec, jdoc::hwp::PARA_HEADER, 0, para_header_payload(0));
    put_record(sec, jdoc::hwp::CTRL_HEADER, 1, ctrl_header_payload("gso "));
    put_record(sec, jdoc::hwp::LIST_HEADER, 2, list_header_payload(0, 0));

    auto md = convert(build_ole(make_file_header(), Bytes(64, 0), sec));
    ASSERT(md.find("본문") != std::string::npos);
    TEST_END
}

// A captioned table stores its caption in a LIST_HEADER placed *before* the
// TABLE record. read_table must consume it rather than treating the control
// as unrecognised — doing so previously discarded the entire table.
static void test_captioned_table_not_dropped() {
    TEST(captioned_table_not_dropped)
    Bytes sec;
    put_record(sec, jdoc::hwp::PARA_HEADER, 0, para_header_payload(0));
    put_record(sec, jdoc::hwp::CTRL_HEADER, 1, ctrl_header_payload("tbl "));
    // Caption list ahead of the TABLE record
    put_record(sec, jdoc::hwp::LIST_HEADER, 2, list_header_payload(0, 0));
    put_paragraph(sec, 2, u"표 1. 측정 결과");
    put_record(sec, jdoc::hwp::TABLE, 2, table_payload(1, 2));
    put_record(sec, jdoc::hwp::LIST_HEADER, 2, list_header_payload(0, 0));
    put_paragraph(sec, 2, u"항목");
    put_record(sec, jdoc::hwp::LIST_HEADER, 2, list_header_payload(1, 0));
    put_paragraph(sec, 2, u"값");

    auto md = convert(build_ole(make_file_header(), Bytes(64, 0), sec));
    ASSERT(count_occurrences(md, "표 1. 측정 결과") == 1);
    ASSERT(count_occurrences(md, "항목") == 1);
    ASSERT(count_occurrences(md, "값") == 1);
    // Caption precedes the rendered table
    ASSERT(md.find("표 1. 측정 결과") < md.find("| 항목 |"));
    TEST_END
}

// Header and footer text is section-level page furniture: it must be
// extracted, but hoisted to the top of the section instead of being spliced
// into the middle of the prose at whichever paragraph anchors it.
static void test_header_footer_hoisted_to_section_start() {
    TEST(header_footer_hoisted_to_section_start)
    Bytes sec;
    put_paragraph(sec, 0, u"첫 번째 본문");
    // Header anchored on a later paragraph, mid-document
    put_record(sec, jdoc::hwp::PARA_HEADER, 0, para_header_payload(0));
    put_record(sec, jdoc::hwp::CTRL_HEADER, 1, ctrl_header_payload("head"));
    put_record(sec, jdoc::hwp::LIST_HEADER, 2, list_header_payload(0, 0));
    put_paragraph(sec, 2, u"학회 논문집 제24권");
    put_paragraph(sec, 0, u"두 번째 본문");

    auto md = convert(build_ole(make_file_header(), Bytes(64, 0), sec));
    ASSERT(count_occurrences(md, "학회 논문집 제24권") == 1);
    ASSERT(md.find("첫 번째 본문") != std::string::npos);
    ASSERT(md.find("두 번째 본문") != std::string::npos);
    // Hoisted ahead of the body, not left between the two body paragraphs
    ASSERT(md.find("학회 논문집 제24권") < md.find("첫 번째 본문"));
    TEST_END
}

// Footnote bodies stay with the paragraph that references them.
static void test_footnote_follows_referencing_paragraph() {
    TEST(footnote_follows_referencing_paragraph)
    Bytes sec;
    put_paragraph(sec, 0, u"앞 문단");
    // Paragraph carrying the footnote reference
    put_record(sec, jdoc::hwp::PARA_HEADER, 0, para_header_payload(0));
    put_record(sec, jdoc::hwp::PARA_TEXT, 1, para_text_payload(u"참조가 있는 문단"));
    put_record(sec, jdoc::hwp::CTRL_HEADER, 1, ctrl_header_payload("fn  "));
    put_record(sec, jdoc::hwp::LIST_HEADER, 2, list_header_payload(0, 0));
    put_paragraph(sec, 2, u"한밭대학교 컴퓨터공학과");
    put_paragraph(sec, 0, u"뒤 문단");

    auto md = convert(build_ole(make_file_header(), Bytes(64, 0), sec));
    ASSERT(count_occurrences(md, "한밭대학교 컴퓨터공학과") == 1);
    // Emitted after its referencing paragraph but before the next one
    ASSERT(md.find("참조가 있는 문단") < md.find("한밭대학교 컴퓨터공학과"));
    ASSERT(md.find("한밭대학교 컴퓨터공학과") < md.find("뒤 문단"));
    TEST_END
}

// A footnote hanging off a table cell paragraph is folded into that cell
// exactly once, not dropped and not also emitted as loose body text.
static void test_footnote_inside_table_cell() {
    TEST(footnote_inside_table_cell)
    Bytes sec;
    put_record(sec, jdoc::hwp::PARA_HEADER, 0, para_header_payload(0));
    put_record(sec, jdoc::hwp::CTRL_HEADER, 1, ctrl_header_payload("tbl "));
    put_record(sec, jdoc::hwp::TABLE, 2, table_payload(1, 1));
    put_record(sec, jdoc::hwp::LIST_HEADER, 2, list_header_payload(0, 0));
    put_record(sec, jdoc::hwp::PARA_HEADER, 2, para_header_payload(0));
    put_record(sec, jdoc::hwp::PARA_TEXT, 3, para_text_payload(u"셀 본문"));
    put_record(sec, jdoc::hwp::CTRL_HEADER, 3, ctrl_header_payload("fn  "));
    put_record(sec, jdoc::hwp::LIST_HEADER, 4, list_header_payload(0, 0));
    put_paragraph(sec, 4, u"셀 각주");

    auto md = convert(build_ole(make_file_header(), Bytes(64, 0), sec));
    ASSERT(count_occurrences(md, "셀 각주") == 1);
    ASSERT(count_occurrences(md, "셀 본문") == 1);
    TEST_END
}

int main() {
    std::cerr << "\n=== HWP 5.x drawing object tests ===\n";

    test_gso_caption_extracted();
    test_gso_multi_paragraph_textbox();
    test_table_cell_not_duplicated();
    test_gso_inside_table_cell();
    test_unknown_control_skipped();
    test_truncated_gso_terminates();
    test_captioned_table_not_dropped();
    test_header_footer_hoisted_to_section_start();
    test_footnote_follows_referencing_paragraph();
    test_footnote_inside_table_cell();

    std::cerr << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===\n";
    return tests_failed == 0 ? 0 : 1;
}
