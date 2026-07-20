// test_archive.cpp - Tests for archive conversion (convert_archive)
// Fixtures are synthesized in-process (minimal zip/tar/gz writers), so the
// suite needs no checked-in binaries and can build hostile inputs
// (zip bombs, lying headers, corrupt members) safely.
// License: MIT

#include "jdoc/archive.h"
#include "jdoc/jdoc.h"

#include <zlib.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
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

static std::string g_tmpdir;

static std::string write_tmp(const std::string& name, const std::string& data) {
    std::string path = g_tmpdir + "/" + name;
    std::ofstream f(path, std::ios::binary);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return path;
}

// ── Minimal writers (fixture synthesis) ─────────────────────

static void put_u16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}
static void put_u32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
static void put_u64(std::string& s, uint64_t v) {
    for (int i = 0; i < 8; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
static uint32_t crc_of(const std::string& d) {
    return static_cast<uint32_t>(
        crc32(0, reinterpret_cast<const Bytef*>(d.data()), static_cast<uInt>(d.size())));
}

static std::string raw_deflate(const std::string& data, int level = 9) {
    z_stream zs = {};
    deflateInit2(&zs, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    std::string out(deflateBound(&zs, static_cast<uLong>(data.size())), '\0');
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());
    zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = static_cast<uInt>(out.size());
    deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

struct ZipEntrySpec {
    std::string name;
    std::string data;
    bool deflate = true;
    bool utf8_flag = true;
    // Hostile-fixture knobs:
    uint32_t lie_uncompressed_size = 0;  // nonzero: write this instead of the truth
    size_t truncate_data = 0;            // nonzero: drop compressed bytes
};

// Minimal zip writer (no zip64), mirrors what ZipReader supports.
static std::string make_zip(const std::vector<ZipEntrySpec>& entries) {
    std::string out;
    struct CdInfo { ZipEntrySpec spec; uint32_t crc, csize, usize, offset; uint16_t method, flags; };
    std::vector<CdInfo> cd;

    for (const auto& e : entries) {
        uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(e.data.data()),
                             static_cast<uInt>(e.data.size()));
        std::string payload = e.deflate ? raw_deflate(e.data) : e.data;
        if (e.truncate_data > 0 && payload.size() > e.truncate_data)
            payload.resize(payload.size() - e.truncate_data);
        uint32_t usize = e.lie_uncompressed_size
                             ? e.lie_uncompressed_size
                             : static_cast<uint32_t>(e.data.size());
        uint16_t method = e.deflate ? 8 : 0;
        uint16_t flags = e.utf8_flag ? 0x0800 : 0;
        uint32_t offset = static_cast<uint32_t>(out.size());

        put_u32(out, 0x04034b50);
        put_u16(out, 20);
        put_u16(out, flags);
        put_u16(out, method);
        put_u32(out, 0);  // time/date
        put_u32(out, crc);
        put_u32(out, static_cast<uint32_t>(payload.size()));
        put_u32(out, usize);
        put_u16(out, static_cast<uint16_t>(e.name.size()));
        put_u16(out, 0);
        out += e.name;
        out += payload;

        cd.push_back({e, crc, static_cast<uint32_t>(payload.size()), usize,
                      offset, method, flags});
    }

    uint32_t cd_offset = static_cast<uint32_t>(out.size());
    for (const auto& c : cd) {
        put_u32(out, 0x02014b50);
        put_u16(out, 20);
        put_u16(out, 20);
        put_u16(out, c.flags);
        put_u16(out, c.method);
        put_u32(out, 0);  // time/date
        put_u32(out, c.crc);
        put_u32(out, c.csize);
        put_u32(out, c.usize);
        put_u16(out, static_cast<uint16_t>(c.spec.name.size()));
        put_u16(out, 0);  // extra
        put_u16(out, 0);  // comment
        put_u16(out, 0);  // disk
        put_u16(out, 0);  // internal attrs
        put_u32(out, 0);  // external attrs
        put_u32(out, c.offset);
        out += c.spec.name;
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

static std::string make_tar(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out;
    for (const auto& [name, data] : files) {
        char hdr[512] = {};
        snprintf(hdr, 100, "%s", name.c_str());
        snprintf(hdr + 100, 8, "%07o", 0644);
        snprintf(hdr + 108, 8, "%07o", 0);
        snprintf(hdr + 116, 8, "%07o", 0);
        snprintf(hdr + 124, 12, "%011llo", static_cast<unsigned long long>(data.size()));
        snprintf(hdr + 136, 12, "%011o", 0);
        memset(hdr + 148, ' ', 8);  // checksum computed over spaces
        hdr[156] = '0';
        memcpy(hdr + 257, "ustar", 5);
        hdr[262] = '0'; hdr[263] = '0';
        unsigned sum = 0;
        for (unsigned char c : hdr) sum += c;
        snprintf(hdr + 148, 8, "%06o", sum);
        hdr[155] = ' ';

        out.append(hdr, 512);
        out += data;
        size_t pad = (512 - data.size() % 512) % 512;
        out.append(pad, '\0');
    }
    out.append(1024, '\0');  // end-of-archive
    return out;
}

static std::string make_gz(const std::string& data) {
    z_stream zs = {};
    deflateInit2(&zs, 9, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    std::string out(deflateBound(&zs, static_cast<uLong>(data.size())) + 32, '\0');
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());
    zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = static_cast<uInt>(out.size());
    deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

// ── alz / egg fixture synthesis ─────────────────────────────
// Layouts follow docs/alz-egg-format-notes.md; synthesized fixtures were
// cross-validated against The Unarchiver (lsar/unar) once at development
// time to confirm real-world tools accept them.

struct AlzEntrySpec {
    std::string name;       // raw name bytes as stored (CP949 or UTF-8)
    std::string content;    // uncompressed content (crc/usize derived)
    int method = 2;         // 0=store, 1=bzip2, 2=deflate
    std::string payload;    // compressed bytes; empty → derived from content
    bool encrypted = false;
    uint32_t crc_override = 0;  // nonzero: write a lying CRC
};

static std::string make_alz(const std::vector<AlzEntrySpec>& entries) {
    std::string out;
    put_u32(out, 0x015A4C41);  // "ALZ\x01"
    put_u16(out, 10);          // version
    put_u16(out, 0);           // header id
    for (const auto& e : entries) {
        std::string payload = e.payload;
        if (payload.empty())
            payload = (e.method == 2) ? raw_deflate(e.content) : e.content;
        put_u32(out, 0x015A4C42);  // "BLZ\x01"
        put_u16(out, static_cast<uint16_t>(e.name.size()));
        out.push_back(0x20);       // attribute: archive
        put_u32(out, 0);           // time
        uint16_t flags = 0x40 | (e.encrypted ? 0x01 : 0);  // 4-byte size fields
        put_u16(out, flags);
        put_u16(out, static_cast<uint16_t>(e.method));
        put_u32(out, e.crc_override ? e.crc_override : crc_of(e.content));
        put_u32(out, static_cast<uint32_t>(payload.size()));
        put_u32(out, static_cast<uint32_t>(e.content.size()));
        out += e.name;
        if (e.encrypted) out.append(12, '\0');  // encryption header stub
        out += payload;
    }
    put_u32(out, 0x025A4C43);  // "CLZ\x02"
    return out;
}

struct EggBlockSpec {
    std::string content;    // uncompressed content of this block
    int method = 1;         // 0=store, 1=deflate, 2=bzip2, 3=azo, 4=lzma
    std::string payload;    // compressed bytes; empty → derived from content
};

struct EggEntrySpec {
    std::string name;               // UTF-8
    std::vector<EggBlockSpec> blocks;
    bool encrypted = false;

    uint64_t total_size() const {
        uint64_t t = 0;
        for (const auto& b : blocks) t += b.content.size();
        return t;
    }
};

static void put_egg_block(std::string& out, const EggBlockSpec& b) {
    std::string payload = b.payload;
    if (payload.empty())
        payload = (b.method == 1) ? raw_deflate(b.content) : b.content;
    put_u32(out, 0x02B50C13);  // block header
    put_u16(out, static_cast<uint16_t>(b.method));
    put_u32(out, static_cast<uint32_t>(b.content.size()));
    put_u32(out, static_cast<uint32_t>(payload.size()));
    put_u32(out, crc_of(b.content));
    put_u32(out, 0x08E28222);  // end of block header
    out += payload;
}

static void put_egg_file_header(std::string& out, const EggEntrySpec& e,
                                uint32_t fid) {
    const uint32_t kEofArc = 0x08E28222;
    put_u32(out, 0x0A8590E3);  // file header
    put_u32(out, fid);
    put_u64(out, e.total_size());
    put_u32(out, 0x0A8591AC);  // filename header
    out.push_back(0);          // flags
    put_u16(out, static_cast<uint16_t>(e.name.size()));
    out += e.name;
    if (e.encrypted) {
        put_u32(out, 0x08D1470F);  // encrypt header
        out.push_back(0);
        put_u16(out, 4);
        out.append(4, '\0');
    }
    put_u32(out, kEofArc);     // end of file header section
}

// Normal layout: each file's headers are followed by its blocks.
// Solid layout: every file header first, then one shared block stream
// covering the concatenated contents (spec section 3, "Solid Archive").
static std::string make_egg(const std::vector<EggEntrySpec>& entries,
                            bool solid = false) {
    const uint32_t kEofArc = 0x08E28222;
    std::string out;
    put_u32(out, 0x41474745);  // "EGGA"
    put_u16(out, 0x0100);      // version
    put_u32(out, 1);           // header id
    put_u32(out, 0);           // reserved
    if (solid) {
        put_u32(out, 0x24E5A060);  // solid extra field
        out.push_back(0);          // bit flag
        put_u16(out, 0);           // size
    }
    put_u32(out, kEofArc);

    uint32_t fid = 1;
    if (solid) {
        std::string all;
        for (const auto& e : entries) {
            put_egg_file_header(out, e, fid++);
            for (const auto& b : e.blocks) all += b.content;
        }
        put_egg_block(out, EggBlockSpec{all, 1});
    } else {
        for (const auto& e : entries) {
            put_egg_file_header(out, e, fid++);
            for (const auto& b : e.blocks) put_egg_block(out, b);
        }
    }
    put_u32(out, kEofArc);  // end of archive
    return out;
}

// Pre-compressed blobs so bzip2/lzma members can be synthesized without an
// encoder. Payload text: "bzip2 archive member payload" (28 bytes,
// crc 0x42C54452) / "lzma egg member payload text" (28 bytes, crc 0x8737E492).
static const char kBzip2Content[] = "bzip2 archive member payload";
static const unsigned char kBzip2Blob[] = {
    0x42, 0x5a, 0x68, 0x39, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0x13, 0x30,
    0x15, 0x53, 0x00, 0x00, 0x04, 0x19, 0x80, 0x40, 0x00, 0x10, 0x00, 0x3e,
    0x66, 0xd1, 0x30, 0x20, 0x00, 0x22, 0x98, 0x9a, 0x0c, 0x6a, 0x3d, 0x42,
    0x86, 0x9a, 0x60, 0x01, 0x32, 0x00, 0x75, 0x10, 0xfb, 0xee, 0x22, 0x48,
    0x22, 0x89, 0x56, 0x96, 0x4e, 0x7c, 0x5d, 0xc9, 0x14, 0xe1, 0x42, 0x40,
    0x4c, 0xc0, 0x55, 0x4c};
// bzip2 of a make_tar-style tar holding one file "t.txt" containing
// "tar bz2 member payload" — a tar.bz2 fixture needs an encoder, so the
// compressed bytes are checked in like the blobs above.
static const char kTarBz2Content[] = "tar bz2 member payload";
static const unsigned char kTarBz2Blob[] = {
    0x42, 0x5a, 0x68, 0x39, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0x48, 0x9d,
    0xe9, 0xa2, 0x00, 0x00, 0x36, 0x5b, 0x81, 0xca, 0x80, 0x40, 0x01, 0x55,
    0x80, 0x00, 0x08, 0x76, 0x06, 0xde, 0x70, 0x00, 0x08, 0x04, 0x08, 0x20,
    0x00, 0x54, 0x51, 0x34, 0xf5, 0x30, 0x99, 0x18, 0x09, 0xea, 0x61, 0xa0,
    0x94, 0x50, 0x64, 0x33, 0x53, 0x40, 0x34, 0x1a, 0x13, 0x7f, 0xca, 0xa1,
    0x9d, 0x82, 0x03, 0x45, 0xe3, 0x61, 0x73, 0x90, 0x14, 0x16, 0x78, 0xac,
    0x1a, 0xd1, 0x2b, 0x51, 0x09, 0x39, 0x0a, 0xa9, 0x28, 0xc1, 0xfd, 0xa9,
    0x40, 0x09, 0x37, 0xce, 0x30, 0x8f, 0x95, 0xe2, 0xd9, 0x10, 0xfd, 0x1d,
    0x25, 0x18, 0xce, 0x88, 0xb4, 0x5e, 0xad, 0xf1, 0x39, 0xf6, 0x2e, 0xe4,
    0x8a, 0x70, 0xa1, 0x20, 0x91, 0x3b, 0xd3, 0x44};

static const char kLzmaContent[] = "lzma egg member payload text";
// .lzma (LZMA-alone) output of xz: props(5) + size(8) + raw stream. The egg
// block payload is 4 reserved bytes + props + raw stream.
static const unsigned char kLzmaAlone[] = {
    0x5d, 0x00, 0x00, 0x80, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x00, 0x36, 0x1e, 0x89, 0xdd, 0x7d, 0x49, 0x29, 0x50, 0xab, 0x1c,
    0x4a, 0xac, 0xeb, 0xf5, 0x0e, 0xef, 0x77, 0x00, 0x3c, 0xd5, 0x19, 0x07,
    0x2c, 0xcc, 0x7d, 0x96, 0x2f, 0x39, 0x52, 0xdf, 0xe2, 0xe1, 0xff, 0xf8,
    0x4d, 0xa0, 0x00};

static std::string egg_lzma_payload() {
    std::string p(4, '\0');  // reserved(2) + property size u16
    p[2] = 5;                // LZMA props are 5 bytes
    p.append(reinterpret_cast<const char*>(kLzmaAlone), 5);        // props
    p.append(reinterpret_cast<const char*>(kLzmaAlone) + 13,
             sizeof(kLzmaAlone) - 13);                             // raw stream
    return p;
}

// ── rar fixture synthesis (4.x, store only) ─────────────────
// rar 7.x can no longer create the 4.x format, so RAR4 fixtures are
// synthesized. Layout follows the public RARLAB technote; the synthesized
// output was cross-validated against unrar/lsar/unar once at development
// time to confirm real-world tools accept it.

struct RarEntrySpec {
    std::string name;       // raw name bytes as stored (CP949/encoded ok)
    std::string content;    // member data (store: pack == unp == size)
    uint8_t method = 0x30;  // 0x30 = store
    uint16_t flags_extra = 0;    // ORed into HEAD_FLAGS (0x04 enc, 0x200 uni)
    uint32_t crc_override = 0;   // nonzero: write a lying CRC
};

static void put_rar4_block(std::string& out, uint8_t type, uint16_t flags,
                           const std::string& body) {
    std::string hdr;
    hdr.push_back(static_cast<char>(type));
    put_u16(hdr, flags);
    put_u16(hdr, static_cast<uint16_t>(7 + body.size()));
    hdr += body;
    put_u16(out, static_cast<uint16_t>(crc_of(hdr) & 0xFFFF));  // HEAD_CRC
    out += hdr;
}

static std::string make_rar4(const std::vector<RarEntrySpec>& entries) {
    std::string out("Rar!\x1A\x07\x00", 7);
    {
        std::string body;                       // main header
        put_u16(body, 0);                       // HighPosAV
        put_u32(body, 0);                       // PosAV
        put_rar4_block(out, 0x73, 0x0000, body);
    }
    for (const auto& e : entries) {
        std::string body;
        put_u32(body, static_cast<uint32_t>(e.content.size()));  // PACK_SIZE
        put_u32(body, static_cast<uint32_t>(e.content.size()));  // UNP_SIZE
        body.push_back(2);                                       // HOST_OS
        put_u32(body, e.crc_override ? e.crc_override : crc_of(e.content));
        put_u32(body, 0);                                        // FTIME
        body.push_back(20);                                      // UNP_VER
        body.push_back(static_cast<char>(e.method));
        put_u16(body, static_cast<uint16_t>(e.name.size()));
        put_u32(body, 0x20);                                     // ATTR
        body += e.name;
        put_rar4_block(out, 0x74,
                       static_cast<uint16_t>(0x8000 | e.flags_extra), body);
        out += e.content;
    }
    put_rar4_block(out, 0x7B, 0x4000, "");      // end of archive
    return out;
}

// Checked-in 7z fixtures (test/fixtures/7z). 7z containers cannot be
// synthesized with a few lines of code the way zip/tar/gz are, so small
// p7zip-generated binaries are used instead.
static std::string g_fixdir;

static std::string fixture(const std::string& name) {
    return g_fixdir + "/" + name;
}

// Checked-in rar 5.x fixtures (test/fixtures/rar), written by the real
// rar CLI (7.23).
static std::string g_rar_fixdir;

static std::string rar_fixture(const std::string& name) {
    return g_rar_fixdir + "/" + name;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read fixture: " + path);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static const jdoc::MemberResult* find_member(
        const std::vector<jdoc::MemberResult>& rs, const std::string& path) {
    for (const auto& r : rs)
        if (r.member_path == path) return &r;
    return nullptr;
}

// ── Tests ───────────────────────────────────────────────────

static void test_basic_zip() {
    std::cerr << "Basic zip:\n";

    TEST(zip_with_text_members)
        auto zip = make_zip({{"a.txt", "hello A"}, {"dir/b.md", "# hello B"}});
        auto path = write_tmp("basic.zip", zip);
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 2);
        auto* a = find_member(rs, "a.txt");
        auto* b = find_member(rs, "dir/b.md");
        ASSERT(a && a->ok() && a->markdown == "hello A");
        ASSERT(b && b->ok() && b->markdown == "# hello B");
        ASSERT(a->format == "TXT");
    TEST_END

    TEST(store_method)
        ZipEntrySpec e{"s.txt", "stored data"};
        e.deflate = false;
        auto path = write_tmp("store.zip", make_zip({e}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].ok() && rs[0].markdown == "stored data");
    TEST_END

    TEST(rtf_member_parsed)
        std::string rtf = "{\\rtf1\\ansi Hello RTF}";
        auto path = write_tmp("rtf.zip", make_zip({{"doc.rtf", rtf}}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].format == "OFFICE");
        ASSERT(rs[0].markdown.find("Hello RTF") != std::string::npos);
    TEST_END

    TEST(cp949_filename_converted)
        // "한글.txt" in CP949: C7 D1 B1 DB
        ZipEntrySpec e{std::string("\xC7\xD1\xB1\xDB") + ".txt", "korean name"};
        e.utf8_flag = false;
        auto path = write_tmp("cp949.zip", make_zip({e}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].member_path == "\xED\x95\x9C\xEA\xB8\x80.txt");  // UTF-8 한글
    TEST_END

    TEST(macos_metadata_skipped)
        auto zip = make_zip({{"__MACOSX/._x.txt", "junk"},
                             {"._y.txt", "junk"},
                             {".DS_Store", "junk"},
                             {"real.txt", "real"}});
        auto path = write_tmp("meta.zip", zip);
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].member_path == "real.txt");
    TEST_END

    TEST(unsupported_member_skipped_by_default)
        std::string binary(64, '\0');
        auto zip = make_zip({{"blob.bin", binary}, {"ok.txt", "fine"}});
        auto path = write_tmp("unsup.zip", zip);
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].member_path == "ok.txt");

        jdoc::ConvertOptions opts;
        opts.archive.include_unsupported = true;
        rs = jdoc::convert_archive(path, opts);
        ASSERT(rs.size() == 2);
        auto* blob = find_member(rs, "blob.bin");
        ASSERT(blob && !blob->ok() && blob->error == "unsupported format");
    TEST_END

    TEST(non_archive_input_single_result)
        auto path = write_tmp("plain.txt", "just text");
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].member_path == "plain.txt");
        ASSERT(rs[0].markdown == "just text");
    TEST_END

    TEST(missing_file_throws)
        bool threw = false;
        try { jdoc::convert_archive(g_tmpdir + "/nope.zip"); }
        catch (const std::exception&) { threw = true; }
        ASSERT(threw);
    TEST_END
}

static void test_nested() {
    std::cerr << "Nesting and recursion:\n";

    TEST(zip_in_zip)
        auto inner = make_zip({{"inner.txt", "deep"}});
        ZipEntrySpec e{"inner.zip", inner};
        auto path = write_tmp("nested.zip", make_zip({e, {"top.txt", "top"}}));
        auto rs = jdoc::convert_archive(path);
        auto* deep = find_member(rs, "inner.zip/inner.txt");
        auto* top = find_member(rs, "top.txt");
        ASSERT(deep && deep->ok() && deep->markdown == "deep");
        ASSERT(top && top->ok());
    TEST_END

    TEST(depth_limit)
        auto l3 = make_zip({{"leaf.txt", "leaf"}});
        auto l2 = make_zip({{"l3.zip", l3}});
        auto l1 = make_zip({{"l2.zip", l2}});
        auto path = write_tmp("deep.zip", l1);

        jdoc::ConvertOptions opts;
        opts.archive.max_depth = 2;  // top=1, l2=2, l3 would be 3
        auto rs = jdoc::convert_archive(path, opts);
        auto* blocked = find_member(rs, "l2.zip/l3.zip");
        ASSERT(blocked && !blocked->ok());
        ASSERT(blocked->error.find("depth") != std::string::npos);
        ASSERT(blocked->error_code == jdoc::MemberErrorCode::DEPTH_LIMIT);

        opts.archive.max_depth = 3;
        rs = jdoc::convert_archive(path, opts);
        auto* leaf = find_member(rs, "l2.zip/l3.zip/leaf.txt");
        ASSERT(leaf && leaf->ok() && leaf->markdown == "leaf");
    TEST_END

    TEST(unlimited_depth_with_minus_one)
        // 5 levels deep — beyond the default cap of 3; max_depth = -1 lifts it.
        auto a = make_zip({{"leaf.txt", "deep leaf"}});
        for (int i = 4; i >= 1; i--)
            a = make_zip({{"l" + std::to_string(i) + ".zip", a}});
        auto path = write_tmp("verydeep.zip", a);

        auto rs = jdoc::convert_archive(path);  // default depth 3: blocked
        ASSERT(!rs.empty() && !rs[0].ok());
        ASSERT(rs[0].error.find("depth") != std::string::npos);

        jdoc::ConvertOptions opts;
        opts.archive.max_depth = -1;
        rs = jdoc::convert_archive(path, opts);
        auto* leaf = find_member(rs, "l1.zip/l2.zip/l3.zip/l4.zip/leaf.txt");
        ASSERT(leaf && leaf->ok() && leaf->markdown == "deep leaf");
    TEST_END

    TEST(unlimited_member_cap_with_minus_one)
        std::string big(2 << 20, 'x');  // 2 MiB member
        auto path = write_tmp("bigmember.zip", make_zip({{"big.txt", big}}));

        jdoc::ConvertOptions opts;
        opts.archive.max_member_bytes = 1 << 20;  // capped: skipped
        auto rs = jdoc::convert_archive(path, opts);
        ASSERT(rs.size() == 1 && !rs[0].ok());

        opts.archive.max_member_bytes = static_cast<uint64_t>(-1);  // unlimited
        rs = jdoc::convert_archive(path, opts);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].markdown.size() == big.size());
    TEST_END

    TEST(gz_in_zip)
        auto gz = make_gz("gzipped text");
        ZipEntrySpec e{"note.txt.gz", gz};
        auto path = write_tmp("gzinzip.zip", make_zip({e}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].member_path == "note.txt.gz/note.txt");
        ASSERT(rs[0].markdown == "gzipped text");
    TEST_END
}

static void test_tar_gz() {
    std::cerr << "tar / gz containers:\n";

    TEST(tar_members)
        auto tar = make_tar({{"a.txt", "tar A"}, {"b.txt", "tar B"}});
        auto path = write_tmp("plain.tar", tar);
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 2);
        ASSERT(find_member(rs, "a.txt")->markdown == "tar A");
        ASSERT(find_member(rs, "b.txt")->markdown == "tar B");
    TEST_END

    TEST(tar_gz_streaming)
        auto tar = make_tar({{"x.txt", "from targz"}});
        auto path = write_tmp("arch.tar.gz", make_gz(tar));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].member_path == "x.txt");
        ASSERT(rs[0].markdown == "from targz");
    TEST_END

    TEST(single_gz_member)
        auto path = write_tmp("solo.txt.gz", make_gz("gz solo"));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].member_path == "solo.txt");
        ASSERT(rs[0].markdown == "gz solo");
    TEST_END
}

static void test_bz2() {
    std::cerr << "bz2 containers:\n";
    // With JDOC_WITH_BZIP2 these convert; without it, the file reports a
    // single "not built" error result — both builds must pass.
    std::string blob(reinterpret_cast<const char*>(kBzip2Blob),
                     sizeof(kBzip2Blob));

    TEST(single_bz2_member)
        auto path = write_tmp("solo.txt.bz2", blob);
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1);
        if (rs[0].ok()) {
            ASSERT(rs[0].member_path == "solo.txt");
            ASSERT(rs[0].markdown == kBzip2Content);
        } else {
            ASSERT(rs[0].error.find("bzip2 support not built") != std::string::npos);
        }
    TEST_END

    TEST(tar_bz2_streaming)
        auto path = write_tmp("arch.tar.bz2",
                              std::string(reinterpret_cast<const char*>(kTarBz2Blob),
                                          sizeof(kTarBz2Blob)));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1);
        if (rs[0].ok()) {
            ASSERT(rs[0].member_path == "t.txt");
            ASSERT(rs[0].markdown == kTarBz2Content);
        } else {
            ASSERT(rs[0].error.find("bzip2 support not built") != std::string::npos);
        }
    TEST_END

    TEST(bz2_in_zip)
        auto path = write_tmp("bzinzip.zip", make_zip({{"note.txt.bz2", blob}}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1);
        if (rs[0].ok()) {
            ASSERT(rs[0].member_path == "note.txt.bz2/note.txt");
            ASSERT(rs[0].markdown == kBzip2Content);
        } else {
            ASSERT(rs[0].error.find("bzip2 support not built") != std::string::npos);
        }
    TEST_END
}

static void test_limits() {
    std::cerr << "Limits and hostile inputs:\n";

    TEST(member_size_cap_streaming)
        // 8 MiB of zeros compresses tiny; cap at 1 MiB. The lying header
        // claims 10 bytes, so enforcement must happen DURING inflate.
        std::string big(8 << 20, '\0');
        big.insert(0, "text ");  // keep it text-like (no NUL first) — irrelevant, cap hits first
        ZipEntrySpec e{"bomb.txt", big};
        e.lie_uncompressed_size = 10;
        auto zip = make_zip({e, {"after.txt", "still here"}});
        auto path = write_tmp("bomb.zip", zip);

        jdoc::ConvertOptions opts;
        opts.archive.max_member_bytes = 1 << 20;
        auto rs = jdoc::convert_archive(path, opts);
        auto* bomb = find_member(rs, "bomb.txt");
        auto* after = find_member(rs, "after.txt");
        ASSERT(bomb && !bomb->ok());
        ASSERT(bomb->error.find("limit") != std::string::npos);
        ASSERT(bomb->error_code == jdoc::MemberErrorCode::MEMBER_LIMIT);
        ASSERT(after && after->ok());  // walk continued
    TEST_END

    TEST(total_budget_stops_walk)
        std::string chunk(1 << 20, 'a');  // 1 MiB text per member
        auto zip = make_zip({{"m1.txt", chunk}, {"m2.txt", chunk}, {"m3.txt", chunk}});
        auto path = write_tmp("total.zip", zip);

        jdoc::ConvertOptions opts;
        opts.archive.max_total_bytes = (1 << 20) + (1 << 19);  // 1.5 MiB
        auto rs = jdoc::convert_archive(path, opts);
        // m1 converts; m2 blows the cumulative budget; m3 never visited
        ASSERT(find_member(rs, "m1.txt") && find_member(rs, "m1.txt")->ok());
        auto* m2 = find_member(rs, "m2.txt");
        ASSERT(m2 && !m2->ok() && m2->error.find("total") != std::string::npos);
        ASSERT(m2->error_code == jdoc::MemberErrorCode::TOTAL_LIMIT);
        ASSERT(!find_member(rs, "m3.txt"));
    TEST_END

    TEST(entry_count_limit)
        std::vector<ZipEntrySpec> entries;
        for (int i = 0; i < 10; i++)
            entries.push_back({"f" + std::to_string(i) + ".txt", "x"});
        auto path = write_tmp("many.zip", make_zip(entries));

        jdoc::ConvertOptions opts;
        opts.archive.max_entries = 5;
        auto rs = jdoc::convert_archive(path, opts);
        ASSERT(rs.size() == 6);  // 5 converted + 1 "walk stopped" error
        ASSERT(!rs.back().ok());
        ASSERT(rs.back().error.find("entry count") != std::string::npos);
        ASSERT(rs.back().error_code == jdoc::MemberErrorCode::ENTRY_LIMIT);
    TEST_END

    TEST(corrupt_member_walk_continues)
        ZipEntrySpec bad{"bad.txt", "this will be truncated, long enough to matter"};
        bad.truncate_data = 8;
        auto zip = make_zip({bad, {"good.txt", "good"}});
        auto path = write_tmp("corrupt.zip", zip);
        auto rs = jdoc::convert_archive(path);
        auto* b = find_member(rs, "bad.txt");
        auto* g = find_member(rs, "good.txt");
        ASSERT(b && !b->ok());
        ASSERT(g && g->ok() && g->markdown == "good");
    TEST_END

    TEST(callback_early_stop)
        auto zip = make_zip({{"1.txt", "one"}, {"2.txt", "two"}, {"3.txt", "three"}});
        auto path = write_tmp("stop.zip", zip);
        int seen = 0;
        jdoc::convert_archive(path, [&](jdoc::MemberResult&&) {
            return ++seen < 2;
        });
        ASSERT(seen == 2);
    TEST_END
}

static void test_seven_zip() {
    std::cerr << "7z containers:\n";

    TEST(lzma_7z)
        auto rs = jdoc::convert_archive(fixture("lzma.7z"));
        ASSERT(rs.size() == 2);
        auto* a = find_member(rs, "a.txt");
        auto* b = find_member(rs, "docs/b.md");
        ASSERT(a && a->ok() && a->markdown == "hello from 7z");
        ASSERT(b && b->ok() && b->markdown == "# markdown in 7z");
    TEST_END

    TEST(lzma2_7z_with_rtf)
        auto rs = jdoc::convert_archive(fixture("lzma2.7z"));
        ASSERT(rs.size() == 3);
        auto* rtf = find_member(rs, "doc.rtf");
        ASSERT(rtf && rtf->ok() && rtf->format == "OFFICE");
        ASSERT(rtf->markdown.find("Seven Zip RTF") != std::string::npos);
        ASSERT(find_member(rs, "a.txt")->ok());
    TEST_END

    TEST(store_method_7z)
        auto rs = jdoc::convert_archive(fixture("store.7z"));
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].markdown == "hello from 7z");
    TEST_END

    TEST(solid_block_members)
        auto rs = jdoc::convert_archive(fixture("solid.7z"));
        ASSERT(rs.size() == 5);
        for (int i = 1; i <= 5; i++) {
            auto* m = find_member(rs, "solid/m" + std::to_string(i) + ".txt");
            ASSERT(m && m->ok());
            ASSERT(m->markdown.find("solid member " + std::to_string(i)) !=
                   std::string::npos);
        }
    TEST_END

    TEST(korean_member_name_utf8)
        auto rs = jdoc::convert_archive(fixture("korean.7z"));
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].member_path ==
               "\xED\x95\x9C\xEA\xB8\x80\xEC\x9D\xB4\xEB\xA6\x84.txt");  // 한글이름.txt
        ASSERT(rs[0].markdown == "korean name member");
    TEST_END

    TEST(seven_zip_in_zip)
        auto inner = read_file(fixture("lzma.7z"));
        auto path = write_tmp("7zinzip.zip", make_zip({{"inner.7z", inner}}));
        auto rs = jdoc::convert_archive(path);
        auto* a = find_member(rs, "inner.7z/a.txt");
        ASSERT(a && a->ok() && a->markdown == "hello from 7z");
    TEST_END

    TEST(oversized_solid_folder_skipped)
        // bigsolid.7z: one solid block decoding to 3 MiB. With a 1 MiB member
        // cap, the decode must be rejected up front — every member of that
        // block reports the error and none is materialized.
        jdoc::ConvertOptions opts;
        opts.archive.max_member_bytes = 1 << 20;
        opts.archive.include_unsupported = true;
        auto rs = jdoc::convert_archive(fixture("bigsolid.7z"), opts);
        ASSERT(rs.size() == 3);
        for (const auto& r : rs) {
            ASSERT(!r.ok());
            ASSERT(r.error.find("solid block") != std::string::npos);
        }
    TEST_END

    TEST(ppmd_7z)
        auto rs = jdoc::convert_archive(fixture("ppmd.7z"));
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].markdown == "ppmd compressed text member");
    TEST_END

    TEST(encrypted_member_reports_error)
        auto rs = jdoc::convert_archive(fixture("encrypted.7z"));
        ASSERT(rs.size() == 1 && !rs[0].ok());
        ASSERT(rs[0].error.find("unsupported") != std::string::npos);
    TEST_END

    TEST(corrupt_7z_reports_open_failure)
        auto good = read_file(fixture("lzma.7z"));
        // Keep the signature, wreck the header tail.
        auto bad = good.substr(0, good.size() / 2);
        auto path = write_tmp("corrupt.7z", bad);
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && !rs[0].ok());
    TEST_END
}

static void test_alz() {
    std::cerr << "alz containers:\n";

    TEST(alz_store_and_deflate)
        AlzEntrySpec store{"a.txt", "alz stored member", 0};
        AlzEntrySpec defl{"dir\\b.md", "# alz deflated", 2};
        auto path = write_tmp("basic.alz", make_alz({store, defl}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 2);
        auto* a = find_member(rs, "a.txt");
        auto* b = find_member(rs, "dir/b.md");  // backslash normalized
        ASSERT(a && a->ok() && a->markdown == "alz stored member");
        ASSERT(b && b->ok() && b->markdown == "# alz deflated");
    TEST_END

    TEST(alz_cp949_filename)
        // "한글.txt" in CP949: C7 D1 B1 DB
        AlzEntrySpec e{std::string("\xC7\xD1\xB1\xDB") + ".txt", "korean alz", 2};
        auto path = write_tmp("cp949.alz", make_alz({e}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].member_path == "\xED\x95\x9C\xEA\xB8\x80.txt");
    TEST_END

    TEST(alz_encrypted_member_error_walk_continues)
        AlzEntrySpec enc{"secret.txt", "irrelevant", 2};
        enc.encrypted = true;
        AlzEntrySpec ok{"open.txt", "readable", 2};
        auto path = write_tmp("enc.alz", make_alz({enc, ok}));
        auto rs = jdoc::convert_archive(path);
        auto* s = find_member(rs, "secret.txt");
        auto* o = find_member(rs, "open.txt");
        ASSERT(s && !s->ok());
        ASSERT(s->error.find("encrypted") != std::string::npos);
        ASSERT(o && o->ok() && o->markdown == "readable");
    TEST_END

    TEST(alz_bzip2_member)
        AlzEntrySpec e{"bz.txt", kBzip2Content, 1,
                       std::string(reinterpret_cast<const char*>(kBzip2Blob),
                                   sizeof(kBzip2Blob))};
        AlzEntrySpec after{"after.txt", "still walking", 2};
        auto path = write_tmp("bz.alz", make_alz({e, after}));
        auto rs = jdoc::convert_archive(path);
        auto* b = find_member(rs, "bz.txt");
        ASSERT(b);
        // With JDOC_WITH_BZIP2 the member converts; without it, the member
        // fails with a clear error and the walk continues either way.
        if (b->ok()) {
            ASSERT(b->markdown == kBzip2Content);
        } else {
            ASSERT(b->error.find("bzip2") != std::string::npos);
        }
        ASSERT(find_member(rs, "after.txt") && find_member(rs, "after.txt")->ok());
    TEST_END

    TEST(alz_crc_mismatch_detected)
        AlzEntrySpec e{"bad.txt", "content whose crc will lie", 2};
        e.crc_override = 0xDEADBEEF;
        AlzEntrySpec ok{"good.txt", "fine", 2};
        auto path = write_tmp("badcrc.alz", make_alz({e, ok}));
        auto rs = jdoc::convert_archive(path);
        auto* bad = find_member(rs, "bad.txt");
        ASSERT(bad && !bad->ok());
        ASSERT(bad->error.find("crc") != std::string::npos);
        ASSERT(find_member(rs, "good.txt")->ok());
    TEST_END

    TEST(alz_in_zip)
        auto alz = make_alz({{"inner.txt", "alz in zip", 2}});
        auto path = write_tmp("alzinzip.zip", make_zip({{"inner.alz", alz}}));
        auto rs = jdoc::convert_archive(path);
        auto* m = find_member(rs, "inner.alz/inner.txt");
        ASSERT(m && m->ok() && m->markdown == "alz in zip");
    TEST_END

    TEST(alz_truncated_stops_leniently)
        auto alz = make_alz({{"one.txt", "first member", 2},
                             {"two.txt", "second member", 2}});
        auto path = write_tmp("trunc.alz", alz.substr(0, alz.size() / 2));
        auto rs = jdoc::convert_archive(path);  // must not crash or hang
        ASSERT(rs.size() <= 2);
    TEST_END
}

static void test_egg() {
    std::cerr << "egg containers:\n";

    TEST(egg_store_and_deflate)
        EggEntrySpec store{"s.txt", {{"egg stored member", 0}}};
        EggEntrySpec defl{"dir/d.md", {{"# egg deflated", 1}}};
        auto path = write_tmp("basic.egg", make_egg({store, defl}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 2);
        ASSERT(find_member(rs, "s.txt")->markdown == "egg stored member");
        ASSERT(find_member(rs, "dir/d.md")->markdown == "# egg deflated");
    TEST_END

    TEST(egg_korean_utf8_name)
        EggEntrySpec e{"\xED\x95\x9C\xEA\xB8\x80.txt", {{"egg korean", 1}}};
        auto path = write_tmp("kr.egg", make_egg({e}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].member_path == "\xED\x95\x9C\xEA\xB8\x80.txt");
    TEST_END

    TEST(egg_lzma_member)
        EggEntrySpec e{"lz.txt", {{kLzmaContent, 4, egg_lzma_payload()}}};
        auto path = write_tmp("lz.egg", make_egg({e}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].markdown == kLzmaContent);
    TEST_END

    TEST(egg_bzip2_member)
        EggEntrySpec e{"bz.txt", {{kBzip2Content, 2,
                       std::string(reinterpret_cast<const char*>(kBzip2Blob),
                                   sizeof(kBzip2Blob))}}};
        EggEntrySpec after{"after.txt", {{"next member", 1}}};
        auto path = write_tmp("bz.egg", make_egg({e, after}));
        auto rs = jdoc::convert_archive(path);
        auto* b = find_member(rs, "bz.txt");
        ASSERT(b);
        if (b->ok()) {
            ASSERT(b->markdown == kBzip2Content);
        } else {
            ASSERT(b->error.find("bzip2") != std::string::npos);
        }
        ASSERT(find_member(rs, "after.txt") && find_member(rs, "after.txt")->ok());
    TEST_END

    TEST(egg_multi_block_member)
        // One file split across two independently deflated blocks.
        EggEntrySpec e{"split.txt", {{"first half + ", 1}, {"second half", 1}}};
        auto path = write_tmp("blocks.egg", make_egg({e}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].markdown == "first half + second half");
    TEST_END

    TEST(egg_encrypted_member_error_walk_continues)
        EggEntrySpec enc{"secret.txt", {{"hidden", 1}}};
        enc.encrypted = true;
        EggEntrySpec ok{"open.txt", {{"readable egg", 1}}};
        auto path = write_tmp("enc.egg", make_egg({enc, ok}));
        auto rs = jdoc::convert_archive(path);
        auto* s = find_member(rs, "secret.txt");
        auto* o = find_member(rs, "open.txt");
        ASSERT(s && !s->ok());
        ASSERT(s->error.find("encrypted") != std::string::npos);
        ASSERT(o && o->ok() && o->markdown == "readable egg");
    TEST_END

    TEST(egg_azo_member_error)
        EggEntrySpec e{"azo.txt", {{"whatever", 3, "\x01\x02\x03\x04"}}};
        auto path = write_tmp("azo.egg", make_egg({e}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && !rs[0].ok());
        ASSERT(rs[0].error.find("AZO") != std::string::npos);
    TEST_END

    TEST(egg_solid_extraction)
        // Three members share one solid block stream; each is demuxed out.
        EggEntrySpec a{"a.txt", {{"solid first member", 0}}};
        EggEntrySpec b{"dir/b.md", {{"# solid second", 0}}};
        EggEntrySpec c{"c.txt", {{"third one closes the stream", 0}}};
        auto path = write_tmp("solid.egg", make_egg({a, b, c}, /*solid=*/true));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 3);
        ASSERT(find_member(rs, "a.txt")->markdown == "solid first member");
        ASSERT(find_member(rs, "dir/b.md")->markdown == "# solid second");
        ASSERT(find_member(rs, "c.txt")->markdown == "third one closes the stream");
    TEST_END

    TEST(egg_solid_member_cap)
        // Middle member exceeds the cap: it is skipped with an error while
        // its stream bytes are drained, and the following member survives.
        std::string big(2 << 20, 'q');
        EggEntrySpec a{"ok1.txt", {{"first ok", 0}}};
        EggEntrySpec bomb{"big.txt", {{big, 0}}};
        EggEntrySpec c{"ok2.txt", {{"second ok", 0}}};
        auto path = write_tmp("solidcap.egg", make_egg({a, bomb, c}, true));
        jdoc::ConvertOptions opts;
        opts.archive.max_member_bytes = 1 << 20;
        auto rs = jdoc::convert_archive(path, opts);
        ASSERT(rs.size() == 3);
        ASSERT(find_member(rs, "ok1.txt")->ok());
        auto* bm = find_member(rs, "big.txt");
        ASSERT(bm && !bm->ok() && bm->error.find("limit") != std::string::npos);
        ASSERT(find_member(rs, "ok2.txt")->ok());
    TEST_END

    TEST(egg_areacode_cp949_filename)
        // Filename header with the area-code flag: locale(2) precedes the
        // name and the size field covers both (spec p13).
        std::string content = "areacode member";
        std::string payload = raw_deflate(content);
        std::string name949 = std::string("\xC7\xD1\xB1\xDB") + ".txt";  // 한글.txt
        std::string out;
        put_u32(out, 0x41474745); put_u16(out, 0x0100);
        put_u32(out, 1); put_u32(out, 0); put_u32(out, 0x08E28222);
        put_u32(out, 0x0A8590E3); put_u32(out, 1);
        put_u64(out, content.size());
        put_u32(out, 0x0A8591AC);
        out.push_back(0x08);  // area-code flag
        put_u16(out, static_cast<uint16_t>(2 + name949.size()));
        put_u16(out, 949);    // locale
        out += name949;
        put_u32(out, 0x08E28222);
        put_u32(out, 0x02B50C13); put_u16(out, 1);
        put_u32(out, static_cast<uint32_t>(content.size()));
        put_u32(out, static_cast<uint32_t>(payload.size()));
        put_u32(out, crc_of(content));
        put_u32(out, 0x08E28222);
        out += payload;
        put_u32(out, 0x08E28222);

        auto path = write_tmp("areacode.egg", out);
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].member_path == "\xED\x95\x9C\xEA\xB8\x80.txt");
        ASSERT(rs[0].markdown == content);
    TEST_END

    TEST(egg_global_comment_between_members)
        // A comment record between members must be skipped, not end the walk.
        EggEntrySpec a{"a.txt", {{"before comment", 1}}};
        EggEntrySpec b{"b.txt", {{"after comment", 1}}};
        auto part1 = make_egg({a});
        part1.resize(part1.size() - 4);  // drop end marker
        std::string comment;
        put_u32(comment, 0x04C63672);    // comment header
        comment.push_back(0);
        put_u16(comment, 5);
        comment += "hello";
        auto part2 = make_egg({b});
        part2.erase(0, 18);              // drop EGGA header + EOFARC
        auto path = write_tmp("comment.egg", part1 + comment + part2);
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 2);
        ASSERT(find_member(rs, "a.txt")->ok());
        ASSERT(find_member(rs, "b.txt")->markdown == "after comment");
    TEST_END

    TEST(egg_in_zip)
        auto egg = make_egg({{"inner.txt", {{"egg in zip", 1}}}});
        auto path = write_tmp("egginzip.zip", make_zip({{"inner.egg", egg}}));
        auto rs = jdoc::convert_archive(path);
        auto* m = find_member(rs, "inner.egg/inner.txt");
        ASSERT(m && m->ok() && m->markdown == "egg in zip");
    TEST_END

    TEST(egg_member_size_cap)
        // 4 MiB of zeros deflates tiny; a 1 MiB member cap must stop the
        // decode mid-stream and the walk must continue past the member.
        std::string big(4 << 20, 'z');
        EggEntrySpec bomb{"bomb.txt", {{big, 1}}};
        EggEntrySpec after{"after.txt", {{"survived", 1}}};
        auto path = write_tmp("cap.egg", make_egg({bomb, after}));
        jdoc::ConvertOptions opts;
        opts.archive.max_member_bytes = 1 << 20;
        auto rs = jdoc::convert_archive(path, opts);
        auto* b = find_member(rs, "bomb.txt");
        ASSERT(b && !b->ok());
        ASSERT(b->error.find("limit") != std::string::npos);
        ASSERT(find_member(rs, "after.txt") && find_member(rs, "after.txt")->ok());
    TEST_END
}

static void test_rar() {
    std::cerr << "rar (4.x synthesized, 5.x fixtures):\n";

    TEST(rar4_store_members)
        auto path = write_tmp("s.rar", make_rar4({
            {"a.txt", "hello rar stored"},
            {"dir\\b.md", "# rar heading"}}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 2);
        auto* a = find_member(rs, "a.txt");
        auto* b = find_member(rs, "dir/b.md");
        ASSERT(a && a->ok() && a->markdown == "hello rar stored");
        ASSERT(b && b->ok() && b->markdown == "# rar heading");
        ASSERT(a->format == "TXT");
    TEST_END

    TEST(rar4_cp949_filename)
        std::string name = "\xC7\xD1\xB1\xDB.txt";  // "한글.txt" in CP949
        auto path = write_tmp("cp.rar", make_rar4({{name, "cp949 name"}}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].member_path == "\xED\x95\x9C\xEA\xB8\x80.txt");
    TEST_END

    TEST(rar4_unicode_filename)
        // ANSI fallback "?.txt", NUL, then the RAR4 compact UTF-16
        // encoding of "가.txt" (high byte 0x00; opcode 2 = full 16-bit,
        // opcode 0 = literal low byte).
        std::string raw = std::string("?.txt") + '\0';
        const unsigned char enc[] = {0x00, 0x80, 0x00, 0xAC,
                                     '.', 't', 'x', 0x00, 't'};
        raw.append(reinterpret_cast<const char*>(enc), sizeof(enc));
        RarEntrySpec e{raw, "unicode name"};
        e.flags_extra = 0x0200;  // LHD_UNICODE
        auto path = write_tmp("uni.rar", make_rar4({e}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].member_path == "\xEA\xB0\x80.txt");  // "가.txt"
    TEST_END

    TEST(rar4_compressed_member_error)
        RarEntrySpec e{"c.txt", "not really lzss data"};
        e.method = 0x33;  // normal compression
        auto path = write_tmp("comp4.rar", make_rar4({e, {"ok.txt", "still walked"}}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 2);
        ASSERT(!rs[0].ok() && rs[0].error == "rar compressed member unsupported");
        auto* ok = find_member(rs, "ok.txt");
        ASSERT(ok && ok->ok());  // framing survives the failed member
    TEST_END

    TEST(rar4_encrypted_member_error)
        RarEntrySpec e{"e.txt", "ciphertextpayload"};
        e.flags_extra = 0x0004;  // LHD_PASSWORD
        auto path = write_tmp("enc4.rar", make_rar4({e}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && !rs[0].ok());
        ASSERT(rs[0].error == "encrypted member unsupported");
    TEST_END

    TEST(rar4_crc_mismatch)
        RarEntrySpec e{"bad.txt", "content"};
        e.crc_override = 0xDEADBEEF;
        auto path = write_tmp("crc4.rar", make_rar4({e}));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && !rs[0].ok());
        ASSERT(rs[0].error == "member crc mismatch");
    TEST_END

    TEST(rar4_truncated_member)
        auto data = make_rar4({{"t.txt", "0123456789"}});
        data.resize(data.size() - 15);  // cut end block + part of the data
        auto path = write_tmp("trunc.rar", data);
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 1 && !rs[0].ok());
    TEST_END

    TEST(zip_nested_in_rar4)
        auto inner = make_zip({{"in.txt", "nested in rar"}});
        auto path = write_tmp("nest.rar", make_rar4({{"inner.zip", inner}}));
        auto rs = jdoc::convert_archive(path);
        auto* n = find_member(rs, "inner.zip/in.txt");
        ASSERT(n && n->ok() && n->markdown == "nested in rar");
    TEST_END

    TEST(rar5_store_fixture)
        auto rs = jdoc::convert_archive(rar_fixture("rar5_store.rar"));
        ASSERT(rs.size() == 3);
        auto* a = find_member(rs, "a.txt");
        ASSERT(a && a->ok() && a->markdown == "hello rar stored");
        // "한글이름.txt" — rar5 stores names as UTF-8 directly
        auto* k = find_member(rs,
            "\xED\x95\x9C\xEA\xB8\x80\xEC\x9D\xB4\xEB\xA6\x84.txt");
        ASSERT(k && k->ok());
    TEST_END

    TEST(rar5_compressed_member_error)
        auto rs = jdoc::convert_archive(rar_fixture("rar5_comp.rar"));
        ASSERT(rs.size() == 1 && !rs[0].ok());
        ASSERT(rs[0].error == "rar compressed member unsupported");
    TEST_END

    TEST(rar5_encrypted_member_error)
        auto rs = jdoc::convert_archive(rar_fixture("rar5_enc.rar"));
        ASSERT(rs.size() == 1 && !rs[0].ok());
        ASSERT(rs[0].error == "encrypted member unsupported");
    TEST_END

    TEST(rar5_encrypted_headers_error)
        auto rs = jdoc::convert_archive(rar_fixture("rar5_hdrenc.rar"));
        ASSERT(rs.size() == 1 && !rs[0].ok());
        ASSERT(rs[0].error == "encrypted rar archive unsupported");
    TEST_END

    TEST(rar_magic_detection_without_ext)
        auto path = write_tmp("noext_rar",
                              read_file(rar_fixture("rar5_store.rar")));
        auto rs = jdoc::convert_archive(path);
        ASSERT(rs.size() == 3);
    TEST_END

    // Real WinRAR-written RAR4 archives from the libarchive test suite
    // (BSD-2-Clause, github.com/libarchive/libarchive) — store members,
    // directories, symlinks, unicode filenames.
    TEST(rar4_real_winrar_store)
        auto rs = jdoc::convert_archive(rar_fixture("rar4_libarchive.rar"));
        auto* t = find_member(rs, "test.txt");
        auto* d = find_member(rs, "testdir/test.txt");
        ASSERT(t && t->ok() && t->markdown.find("test text document") == 0);
        ASSERT(d && d->ok());
    TEST_END

    TEST(rar4_real_winrar_unicode_names)
        auto rs = jdoc::convert_archive(
            rar_fixture("rar4_libarchive_unicode.rar"));
        // "表だよ/漢字長いファイル名long-filename-in-漢字.txt", store member
        const jdoc::MemberResult* k = nullptr;
        for (const auto& r : rs)
            if (r.member_path.find("long-filename-in-") != std::string::npos)
                k = &r;
        ASSERT(k && k->ok() && k->markdown.find("kanji") == 0);
        ASSERT(k->member_path.compare(0, 10,
                                      "\xE8\xA1\xA8\xE3\x81\xA0\xE3\x82\x88/") == 0);
        // the archive's one compressed member fails member-locally
        bool comp_err = false;
        for (const auto& r : rs)
            if (r.error == "rar compressed member unsupported") comp_err = true;
        ASSERT(comp_err);
    TEST_END
}

static void test_consistency() {
    std::cerr << "Path/memory conversion consistency:\n";

    TEST(archive_result_matches_direct_convert)
        std::string rtf = "{\\rtf1\\ansi Consistency Check}";
        auto direct_path = write_tmp("direct.rtf", rtf);
        auto direct = jdoc::convert(direct_path);

        auto zip_path = write_tmp("consist.zip", make_zip({{"direct.rtf", rtf}}));
        auto rs = jdoc::convert_archive(zip_path);
        ASSERT(rs.size() == 1 && rs[0].ok());
        ASSERT(rs[0].markdown == direct);
    TEST_END

    TEST(convert_bytes_api)
        std::string rtf = "{\\rtf1\\ansi Bytes API}";
        auto md = jdoc::convert(rtf.data(), rtf.size(), "x.rtf");
        ASSERT(md.find("Bytes API") != std::string::npos);
    TEST_END
}

int main(int argc, char* argv[]) {
    g_tmpdir = (argc > 1) ? argv[1] : "test_archive_tmp";
    g_fixdir = (argc > 2) ? argv[2] : "test/fixtures/7z";
    g_rar_fixdir = (argc > 3) ? argv[3] : "test/fixtures/rar";
    std::string mkdir_cmd = "mkdir -p '" + g_tmpdir + "'";
    if (system(mkdir_cmd.c_str()) != 0) {
        std::cerr << "cannot create tmp dir\n";
        return 1;
    }

    std::cerr << "=== jdoc Archive Test ===\n";
    test_basic_zip();
    test_nested();
    test_tar_gz();
    test_bz2();
    test_seven_zip();
    test_alz();
    test_egg();
    test_rar();
    test_limits();
    test_consistency();

    std::cerr << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===\n";
    return tests_failed == 0 ? 0 : 1;
}
