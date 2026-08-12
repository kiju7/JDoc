// test_codecs.cpp — JBIG2 and JPEG 2000 decoder tests against fixtures.
//
// circle.jbig2: arithmetic generic region (template 0, TPGDON), the stencil
// shape of a circular logo mask; expected output cross-checked bit-for-bit
// against jbig2dec. jpx_lossless.j2k / jpx_lossy.j2k: opj_compress encodings
// of jpx_src.ppm (5/3+RCT reversible must decode byte-exact; 9/7+ICT within
// the encoder's own quality loss).
#include "pdf/jbig2.h"
#include "pdf/jpx.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

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

int main() {
    std::cout << "=== jdoc codec tests ===\n";

    // [1] JBIG2 generic region
    {
        auto stream = slurp("test/fixtures/pdf/circle.jbig2");
        int w = 0, h = 0;
        auto bits = jdoc::pdf_detail::jbig2_decode(stream.data(), stream.size(),
                                                   nullptr, 0, w, h);
        assert(!bits.empty());
        assert(w == 215 && h == 215);
        auto expect = pnm_payload(slurp("test/fixtures/pdf/circle_expected.pbm"), 3);
        assert(bits.size() == expect.size());
        assert(std::memcmp(bits.data(), expect.data(), bits.size()) == 0);
        std::cout << "[1] jbig2 generic region: OK (" << w << "x" << h << ")\n";
    }

    // [2] JPX reversible (5/3 + RCT): byte-exact round trip
    {
        auto stream = slurp("test/fixtures/pdf/jpx_lossless.j2k");
        auto img = jdoc::pdf_detail::jpx_decode(stream.data(), stream.size());
        assert(img.width == 21 && img.height == 17 && img.components == 3);
        auto src = pnm_payload(slurp("test/fixtures/pdf/jpx_src.ppm"), 4);
        assert(img.pixels.size() == src.size());
        assert(std::memcmp(img.pixels.data(), src.data(), src.size()) == 0);
        std::cout << "[2] jpx 5/3 lossless: OK (byte-exact)\n";
    }

    // [3] JPX irreversible (9/7 + ICT): within the encoder's quality loss
    {
        auto stream = slurp("test/fixtures/pdf/jpx_lossy.j2k");
        auto img = jdoc::pdf_detail::jpx_decode(stream.data(), stream.size());
        assert(img.width == 21 && img.height == 17 && img.components == 3);
        auto src = pnm_payload(slurp("test/fixtures/pdf/jpx_src.ppm"), 4);
        assert(img.pixels.size() == src.size());
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
        assert(maxdiff <= 20);
        assert(psnr > 32.0);
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

    std::cout << "\nAll codec tests passed.\n";
    return 0;
}
