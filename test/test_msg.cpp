// test_msg.cpp - Tests for Outlook .msg (MS-OXMSG) conversion
// License: MIT
//
// Builds a minimal synthetic CFB (Compound File Binary) holding the root
// __substg1.0_* property streams an .msg uses, so the tests need no external
// fixture. The layout mirrors test_hwp5.cpp's build_ole.

#include "jdoc/office.h"
#include "common/string_utils.h"
#include <cstdint>
#include <cstring>
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

using Bytes = std::vector<uint8_t>;

constexpr uint32_t SEC_SIZE = 512;
constexpr uint32_t MINI_CUTOFF = 4096;
constexpr uint32_t FREESECT = 0xFFFFFFFF;
constexpr uint32_t ENDOFCHAIN = 0xFFFFFFFE;
constexpr uint32_t FATSECT = 0xFFFFFFFD;

void set_u16(Bytes& b, size_t off, uint16_t v) {
    b[off] = v & 0xFF;
    b[off + 1] = (v >> 8) & 0xFF;
}
void set_u32(Bytes& b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; i++) b[off + i] = (v >> (8 * i)) & 0xFF;
}

struct OleStream { std::string name; Bytes data; };

// Build a CFB whose root storage directly holds the given streams.
Bytes build_cfb(std::vector<OleStream> streams) {
    for (auto& s : streams) {
        size_t want = std::max<size_t>(s.data.size(), MINI_CUTOFF);
        want = ((want + SEC_SIZE - 1) / SEC_SIZE) * SEC_SIZE;
        s.data.resize(want, 0);
    }

    constexpr uint32_t FAT_SEC = 0;
    constexpr uint32_t DIR_SEC = 1;
    const uint32_t entry_count = uint32_t(streams.size()) + 1;  // + Root Entry
    const uint32_t DIR_SECTORS = (entry_count * 128 + SEC_SIZE - 1) / SEC_SIZE;

    uint32_t next_sec = DIR_SEC + DIR_SECTORS;
    std::vector<uint32_t> start_sec(streams.size()), sec_count(streams.size());
    for (size_t i = 0; i < streams.size(); i++) {
        start_sec[i] = next_sec;
        sec_count[i] = uint32_t(streams[i].data.size() / SEC_SIZE);
        next_sec += sec_count[i];
    }
    uint32_t total_sectors = next_sec;

    std::vector<uint32_t> fat(SEC_SIZE / 4, FREESECT);
    fat[FAT_SEC] = FATSECT;
    for (uint32_t k = 0; k < DIR_SECTORS; k++)
        fat[DIR_SEC + k] = (k + 1 < DIR_SECTORS) ? (DIR_SEC + k + 1) : ENDOFCHAIN;
    for (size_t i = 0; i < streams.size(); i++)
        for (uint32_t k = 0; k < sec_count[i]; k++) {
            uint32_t s = start_sec[i] + k;
            fat[s] = (k + 1 < sec_count[i]) ? (s + 1) : ENDOFCHAIN;
        }

    // Directory: Root Entry (child -> first stream), then the streams. The OLE
    // reader scans entries linearly by name, so sibling links stay FREESECT.
    Bytes dir(DIR_SECTORS * SEC_SIZE, 0);
    auto write_entry = [&](size_t idx, const std::string& name, uint8_t type,
                           uint32_t left, uint32_t right, uint32_t child,
                           uint32_t start, uint32_t size) {
        size_t off = idx * 128;
        for (size_t c = 0; c < name.size(); c++)
            set_u16(dir, off + c * 2, uint16_t(static_cast<unsigned char>(name[c])));
        set_u16(dir, off + 0x40, uint16_t((name.size() + 1) * 2));
        dir[off + 0x42] = type;
        set_u32(dir, off + 0x44, left);
        set_u32(dir, off + 0x48, right);
        set_u32(dir, off + 0x4C, child);
        set_u32(dir, off + 0x74, start);
        set_u32(dir, off + 0x78, size);
    };
    // Root's child is the first stream; the streams are linked as a right-sibling
    // chain so the directory tree walk (list_streams) visits every one.
    write_entry(0, "Root Entry", 5, FREESECT, FREESECT, 1, 0, 0);
    for (size_t i = 0; i < streams.size(); i++) {
        uint32_t right = (i + 1 < streams.size()) ? uint32_t(i + 2) : FREESECT;
        write_entry(i + 1, streams[i].name, 2, FREESECT, right, FREESECT,
                    start_sec[i], uint32_t(streams[i].data.size()));
    }
    for (size_t i = entry_count; i < DIR_SECTORS * SEC_SIZE / 128; i++) {
        set_u32(dir, i * 128 + 0x44, FREESECT);
        set_u32(dir, i * 128 + 0x48, FREESECT);
        set_u32(dir, i * 128 + 0x4C, FREESECT);
    }

    Bytes hdr(SEC_SIZE, 0);
    const uint8_t sig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    std::memcpy(hdr.data(), sig, 8);
    set_u16(hdr, 0x18, 0x003E);
    set_u16(hdr, 0x1A, 3);
    set_u16(hdr, 0x1C, 0xFFFE);
    set_u16(hdr, 0x1E, 9);
    set_u16(hdr, 0x20, 6);
    set_u32(hdr, 0x2C, 1);            // FAT sector count
    set_u32(hdr, 0x30, DIR_SEC);
    set_u32(hdr, 0x38, MINI_CUTOFF);
    set_u32(hdr, 0x3C, ENDOFCHAIN);
    set_u32(hdr, 0x40, 0);
    set_u32(hdr, 0x44, ENDOFCHAIN);
    set_u32(hdr, 0x48, 0);
    set_u32(hdr, 0x4C, FAT_SEC);
    for (int i = 1; i < 109; i++) set_u32(hdr, 0x4C + i * 4, FREESECT);

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

// A node in a CFB directory tree. parent = index into nodes of the containing
// storage, or -1 for a direct child of the root.
struct Node {
    std::string name;
    uint8_t type;       // 1 = storage, 2 = stream
    std::string data;   // stream payload (ignored for storages)
    int parent;
};

// Build a CFB with an arbitrary storage/stream tree (streams padded past the
// mini-stream cutoff so the main FAT is used). Directory index of node i is i+1
// (index 0 is the Root Entry). Children of a parent are chained via right
// siblings, and the parent's child pointer targets the first of them.
Bytes build_cfb_tree(std::vector<Node> nodes) {
    const size_t N = nodes.size();
    // Pad stream data and lay out stream sectors.
    std::vector<uint32_t> start(N, 0), count(N, 0);
    const uint32_t entry_count = uint32_t(N) + 1;
    const uint32_t DIR_SEC = 1;
    const uint32_t DIR_SECTORS = (entry_count * 128 + SEC_SIZE - 1) / SEC_SIZE;
    uint32_t next_sec = DIR_SEC + DIR_SECTORS;
    for (size_t i = 0; i < N; i++) {
        if (nodes[i].type != 2) continue;
        size_t want = std::max<size_t>(nodes[i].data.size(), MINI_CUTOFF);
        want = ((want + SEC_SIZE - 1) / SEC_SIZE) * SEC_SIZE;
        nodes[i].data.resize(want, '\0');
        start[i] = next_sec;
        count[i] = uint32_t(want / SEC_SIZE);
        next_sec += count[i];
    }
    uint32_t total_sectors = next_sec;

    std::vector<uint32_t> fat(SEC_SIZE / 4, FREESECT);
    fat[0] = FATSECT;
    for (uint32_t k = 0; k < DIR_SECTORS; k++)
        fat[DIR_SEC + k] = (k + 1 < DIR_SECTORS) ? (DIR_SEC + k + 1) : ENDOFCHAIN;
    for (size_t i = 0; i < N; i++)
        for (uint32_t k = 0; k < count[i]; k++) {
            uint32_t s = start[i] + k;
            fat[s] = (k + 1 < count[i]) ? (s + 1) : ENDOFCHAIN;
        }

    // Per-parent child ordering (parent -1 == root).
    auto dir_idx = [](int node) { return uint32_t(node + 1); };
    std::vector<uint32_t> right(N, FREESECT), child(N + 1, FREESECT);  // child[0]=root
    std::vector<int> last(N + 1, -1);  // last child seen per parent (+1 offset)
    for (int i = 0; i < (int)N; i++) {
        int p = nodes[i].parent;      // -1 root
        int pk = p + 1;               // 0 == root
        if (last[pk] < 0) child[pk] = dir_idx(i);
        else right[last[pk]] = dir_idx(i);
        last[pk] = i;
    }

    Bytes dir(DIR_SECTORS * SEC_SIZE, 0);
    auto write_entry = [&](size_t idx, const std::string& name, uint8_t type,
                           uint32_t left, uint32_t rsib, uint32_t ch,
                           uint32_t st, uint32_t size) {
        size_t off = idx * 128;
        for (size_t c = 0; c < name.size(); c++)
            set_u16(dir, off + c * 2, uint16_t(static_cast<unsigned char>(name[c])));
        set_u16(dir, off + 0x40, uint16_t((name.size() + 1) * 2));
        dir[off + 0x42] = type;
        set_u32(dir, off + 0x44, left);
        set_u32(dir, off + 0x48, rsib);
        set_u32(dir, off + 0x4C, ch);
        set_u32(dir, off + 0x74, st);
        set_u32(dir, off + 0x78, size);
    };
    write_entry(0, "Root Entry", 5, FREESECT, FREESECT, child[0], 0, 0);
    for (size_t i = 0; i < N; i++)
        write_entry(i + 1, nodes[i].name, nodes[i].type, FREESECT, right[i],
                    nodes[i].type == 1 ? child[i + 1] : FREESECT,
                    nodes[i].type == 2 ? start[i] : 0,
                    nodes[i].type == 2 ? uint32_t(nodes[i].data.size()) : 0);
    for (size_t i = entry_count; i < DIR_SECTORS * SEC_SIZE / 128; i++) {
        set_u32(dir, i * 128 + 0x44, FREESECT);
        set_u32(dir, i * 128 + 0x48, FREESECT);
        set_u32(dir, i * 128 + 0x4C, FREESECT);
    }

    Bytes hdr(SEC_SIZE, 0);
    const uint8_t sig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    std::memcpy(hdr.data(), sig, 8);
    set_u16(hdr, 0x18, 0x003E); set_u16(hdr, 0x1A, 3); set_u16(hdr, 0x1C, 0xFFFE);
    set_u16(hdr, 0x1E, 9); set_u16(hdr, 0x20, 6); set_u32(hdr, 0x2C, 1);
    set_u32(hdr, 0x30, DIR_SEC); set_u32(hdr, 0x38, MINI_CUTOFF);
    set_u32(hdr, 0x3C, ENDOFCHAIN); set_u32(hdr, 0x40, 0);
    set_u32(hdr, 0x44, ENDOFCHAIN); set_u32(hdr, 0x48, 0); set_u32(hdr, 0x4C, 0);
    for (int i = 1; i < 109; i++) set_u32(hdr, 0x4C + i * 4, FREESECT);

    Bytes out;
    out.reserve(SEC_SIZE * (total_sectors + 1));
    out.insert(out.end(), hdr.begin(), hdr.end());
    Bytes fat_sec(SEC_SIZE, 0);
    for (size_t i = 0; i < fat.size(); i++) set_u32(fat_sec, i * 4, fat[i]);
    out.insert(out.end(), fat_sec.begin(), fat_sec.end());
    out.insert(out.end(), dir.begin(), dir.end());
    for (size_t i = 0; i < N; i++)
        if (nodes[i].type == 2)
            out.insert(out.end(), nodes[i].data.begin(), nodes[i].data.end());
    return out;
}

Bytes ascii(const std::string& s) { return Bytes(s.begin(), s.end()); }

// UTF-16LE bytes for an ASCII string (PT_UNICODE property value).
Bytes utf16le(const std::string& s) {
    Bytes b;
    for (char c : s) { b.push_back(static_cast<uint8_t>(c)); b.push_back(0); }
    return b;
}

std::string convert(const Bytes& cfb) {
    return jdoc::office_to_markdown_mem(cfb.data(), cfb.size(), "mail.msg");
}

} // namespace

void test_msg() {
    std::cerr << "=== jdoc msg tests ===\n\n";

    TEST(detected_and_dispatched_as_msg)
        // A CFB carrying __properties_version1.0 + __substg1.0_* must route to
        // the MSG parser and yield the subject/body rather than throwing.
        Bytes cfb = build_cfb({
            {"__properties_version1.0", Bytes(64, 0)},
            {"__substg1.0_0037001E", ascii("Hello")},
            {"__substg1.0_1000001E", ascii("body text")},
        });
        auto md = convert(cfb);
        ASSERT(md.find("# Hello") != std::string::npos);
        ASSERT(md.find("body text") != std::string::npos);
    TEST_END

    TEST(headers_and_body_extracted)
        Bytes cfb = build_cfb({
            {"__properties_version1.0", Bytes(64, 0)},
            {"__substg1.0_0037001F", utf16le("Subject Line")},   // PT_UNICODE
            {"__substg1.0_0C1A001E", ascii("Alice")},            // sender name
            {"__substg1.0_0C1F001E", ascii("alice@example.com")},// sender email
            {"__substg1.0_0E04001E", ascii("Bob")},              // PR_DISPLAY_TO
            {"__substg1.0_1000001E", ascii("line1\r\nsecret42")},// body
        });
        auto md = convert(cfb);
        ASSERT(md.find("# Subject Line") != std::string::npos);
        ASSERT(md.find("**From:** Alice <alice@example.com>") != std::string::npos);
        ASSERT(md.find("**To:** Bob") != std::string::npos);
        ASSERT(md.find("secret42") != std::string::npos);
        ASSERT(jdoc::util::is_valid_utf8(md));
    TEST_END

    TEST(trailing_nul_trimmed)
        // PR_DISPLAY_TO often carries a trailing NUL pad that must be stripped.
        Bytes cfb = build_cfb({
            {"__properties_version1.0", Bytes(64, 0)},
            {"__substg1.0_0037001E", ascii("S")},
            {"__substg1.0_0E04001E", ascii(std::string("Carol\0", 6))},
            {"__substg1.0_1000001E", ascii("b")},
        });
        auto md = convert(cfb);
        ASSERT(md.find("**To:** Carol\n") != std::string::npos);
    TEST_END

    TEST(subject_falls_back_to_conversation_topic)
        Bytes cfb = build_cfb({
            {"__properties_version1.0", Bytes(64, 0)},
            {"__substg1.0_0070001E", ascii("Topic Only")},  // no 0037
            {"__substg1.0_1000001E", ascii("b")},
        });
        auto md = convert(cfb);
        ASSERT(md.find("# Topic Only") != std::string::npos);
    TEST_END

    TEST(multiple_recipient_storages_scoped)
        // Two recipient sub-storages reuse the same leaf names (3001/3003).
        // The OLE reader must scope each lookup to its storage, or both
        // recipients collapse to the first one's data.
        std::vector<Node> nodes = {
            {"__substg1.0_0037001E", 2, "Meeting", -1},
            {"__substg1.0_1000001E", 2, "body", -1},
            {"__properties_version1.0", 2, std::string(64, '\0'), -1},
            {"__recip_version1.0_#00000000", 1, "", -1},
            {"__recip_version1.0_#00000001", 1, "", -1},
            {"__substg1.0_3001001E", 2, "Alice", 3},
            {"__substg1.0_3003001E", 2, "alice@x.com", 3},
            {"__substg1.0_3001001E", 2, "Bob", 4},
            {"__substg1.0_3003001E", 2, "bob@y.com", 4},
        };
        auto cfb = build_cfb_tree(nodes);
        auto md = jdoc::office_to_markdown_mem(cfb.data(), cfb.size(), "m.msg");
        ASSERT(md.find("Alice <alice@x.com>") != std::string::npos);
        ASSERT(md.find("Bob <bob@y.com>") != std::string::npos);   // scope fix
        ASSERT(md.find("Bob <alice@x.com>") == std::string::npos);
    TEST_END

    std::cerr << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===\n";
}

int main() {
    test_msg();
    return tests_failed > 0 ? 1 : 0;
}
