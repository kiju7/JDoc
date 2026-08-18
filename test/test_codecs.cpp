// test_codecs.cpp — JBIG2 and JPEG 2000 decoder tests against fixtures.
//
// circle.jbig2: arithmetic generic region (template 0, TPGDON), the stencil
// shape of a circular logo mask; expected output cross-checked bit-for-bit
// against jbig2dec. jpx_lossless.j2k / jpx_lossy.j2k: opj_compress encodings
// of jpx_src.ppm (5/3+RCT reversible must decode byte-exact; 9/7+ICT within
// the encoder's own quality loss).
#include "pdf/jbig2.h"
#include "pdf/jpx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

// CHECK() vanishes under the default Release build (-DNDEBUG), which would
// let a decoder regression print OK and exit 0 — so checks stay live always.
#define CHECK(cond) do { \
        if (!(cond)) { \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ \
                      << ": " #cond "\n"; \
            exit(1); \
        } \
    } while (0)

static std::vector<uint8_t> slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "fixture not found: " << path << "\n";
        exit(1);
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// P4/P5/P6 with a single whitespace after maxval/dims; returns payload
static std::vector<uint8_t> pnm_payload(const std::vector<uint8_t>& d, int fields) {
    size_t pos = 0;
    int seen = 0;
    while (pos < d.size() && seen < fields) {
        while (pos < d.size() && (d[pos] == ' ' || d[pos] == '\n' || d[pos] == '\t')) pos++;
        while (pos < d.size() && d[pos] != ' ' && d[pos] != '\n' && d[pos] != '\t') pos++;
        seen++;
    }
    pos++; // single whitespace before payload
    return std::vector<uint8_t>(d.begin() + pos, d.end());
}

static size_t find_bytes(const std::vector<uint8_t>& data,
                         const std::vector<uint8_t>& needle) {
    auto it = std::search(data.begin(), data.end(), needle.begin(), needle.end());
    return it == data.end() ? data.size() :
           static_cast<size_t>(it - data.begin());
}

static size_t find_marker(const std::vector<uint8_t>& data, uint8_t marker) {
    return find_bytes(data, {0xFF, marker});
}

static void put_be32(std::vector<uint8_t>& data, size_t pos, uint32_t value) {
    CHECK(pos + 4 <= data.size());
    data[pos] = static_cast<uint8_t>(value >> 24);
    data[pos + 1] = static_cast<uint8_t>(value >> 16);
    data[pos + 2] = static_cast<uint8_t>(value >> 8);
    data[pos + 3] = static_cast<uint8_t>(value);
}

int main() {
    std::cout << "=== jdoc codec tests ===\n";

    // [1] JBIG2 generic region
    {
        auto stream = slurp("test/fixtures/pdf/circle.jbig2");
        int w = 0, h = 0;
        auto bits = jdoc::pdf_detail::jbig2_decode(stream.data(), stream.size(),
                                                   nullptr, 0, w, h);
        CHECK(!bits.empty());
        CHECK(w == 215 && h == 215);
        auto expect = pnm_payload(slurp("test/fixtures/pdf/circle_expected.pbm"), 3);
        CHECK(bits.size() == expect.size());
        CHECK(std::memcmp(bits.data(), expect.data(), bits.size()) == 0);
        std::cout << "[1] jbig2 generic region: OK (" << w << "x" << h << ")\n";
    }

    // [1b] JBIG2 symbol dictionary + text region, in the PDF embedding
    // layout (dictionary in the globals stream, text region in the image
    // stream) — jbig2enc -s output, cross-checked against jbig2dec.
    {
        auto globals = slurp("test/fixtures/pdf/symtext.sym");
        auto stream = slurp("test/fixtures/pdf/symtext.jbig2");
        int w = 0, h = 0;
        auto bits = jdoc::pdf_detail::jbig2_decode(stream.data(), stream.size(),
                                                   globals.data(),
                                                   globals.size(), w, h);
        CHECK(!bits.empty());
        CHECK(w == 400 && h == 120);
        auto expect =
            pnm_payload(slurp("test/fixtures/pdf/symtext_expected.pbm"), 3);
        CHECK(bits.size() == expect.size());
        CHECK(std::memcmp(bits.data(), expect.data(), bits.size()) == 0);
        std::cout << "[1b] jbig2 symbol dictionary + text region: OK ("
                  << w << "x" << h << ")\n";
    }

    // [1c] Denser variant: 12 random glyph classes over 14 lines, TPGD on.
    {
        auto globals = slurp("test/fixtures/pdf/symtext2.sym");
        auto stream = slurp("test/fixtures/pdf/symtext2.jbig2");
        int w = 0, h = 0;
        auto bits = jdoc::pdf_detail::jbig2_decode(stream.data(), stream.size(),
                                                   globals.data(),
                                                   globals.size(), w, h);
        CHECK(!bits.empty());
        CHECK(w == 600 && h == 400);
        auto expect =
            pnm_payload(slurp("test/fixtures/pdf/symtext2_expected.pbm"), 3);
        CHECK(bits.size() == expect.size());
        CHECK(std::memcmp(bits.data(), expect.data(), bits.size()) == 0);
        std::cout << "[1c] jbig2 symbol text, dense multi-class: OK ("
                  << w << "x" << h << ")\n";
    }

    // [2] JPX reversible (5/3 + RCT): byte-exact round trip
    {
        auto stream = slurp("test/fixtures/pdf/jpx_lossless.j2k");
        auto img = jdoc::pdf_detail::jpx_decode(stream.data(), stream.size());
        CHECK(img.width == 21 && img.height == 17 && img.components == 3);
        auto src = pnm_payload(slurp("test/fixtures/pdf/jpx_src.ppm"), 4);
        CHECK(img.pixels.size() == src.size());
        CHECK(std::memcmp(img.pixels.data(), src.data(), src.size()) == 0);
        std::cout << "[2] jpx 5/3 lossless: OK (byte-exact)\n";
    }

    // [3] JPX irreversible (9/7 + ICT): within the encoder's quality loss
    {
        auto stream = slurp("test/fixtures/pdf/jpx_lossy.j2k");
        auto img = jdoc::pdf_detail::jpx_decode(stream.data(), stream.size());
        CHECK(img.width == 21 && img.height == 17 && img.components == 3);
        auto src = pnm_payload(slurp("test/fixtures/pdf/jpx_src.ppm"), 4);
        CHECK(img.pixels.size() == src.size());
        double se = 0;
        int maxdiff = 0;
        for (size_t i = 0; i < src.size(); i++) {
            int df = std::abs(static_cast<int>(src[i]) - img.pixels[i]);
            se += static_cast<double>(df) * df;
            if (df > maxdiff) maxdiff = df;
        }
        double psnr = 10.0 * std::log10(255.0 * 255.0 * src.size() / se);
        std::cout << "[3] jpx 9/7 lossy: maxdiff " << maxdiff
                  << ", PSNR " << psnr << " dB\n";
        CHECK(maxdiff <= 20);
        CHECK(psnr > 32.0);
    }

    // [4] Robustness: truncated / corrupt streams must fail cleanly
    {
        auto jb = slurp("test/fixtures/pdf/circle.jbig2");
        auto jp = slurp("test/fixtures/pdf/jpx_lossless.j2k");
        for (size_t cut : {size_t(0), size_t(3), jb.size() / 2}) {
            int w = 0, h = 0;
            jdoc::pdf_detail::jbig2_decode(jb.data(), cut, nullptr, 0, w, h);
        }
        for (size_t cut : {size_t(0), size_t(2), size_t(16), jp.size() / 2})
            jdoc::pdf_detail::jpx_decode(jp.data(), cut);
        std::cout << "[4] truncated streams: no crash\n";
    }

    // [5] Declared dimensions and decomposition levels are attacker-controlled.
    // Reject them before allocating page/component buffers or shifting by 32.
    {
        auto jb = slurp("test/fixtures/pdf/circle.jbig2");
        const size_t dimensions = find_bytes(
            jb, {0x00, 0x00, 0x00, 0xD7, 0x00, 0x00, 0x00, 0xD7});
        CHECK(dimensions != jb.size());
        put_be32(jb, dimensions, 1u << 20);
        put_be32(jb, dimensions + 4, 1u << 20);
        int w = 0, h = 0;
        CHECK(jdoc::pdf_detail::jbig2_decode(
                  jb.data(), jb.size(), nullptr, 0, w, h).empty());

        auto jp = slurp("test/fixtures/pdf/jpx_lossless.j2k");
        const size_t cod_pos = find_marker(jp, 0x52);
        CHECK(cod_pos != jp.size());
        CHECK(cod_pos + 9 < jp.size());
        jp[cod_pos + 9] = 32;
        CHECK(jdoc::pdf_detail::jpx_decode(jp.data(), jp.size()).pixels.empty());
        std::cout << "[5] oversized codec headers: rejected\n";
    }

    std::cout << "\nAll codec tests passed.\n";
    return 0;
}
