#include "pdf_extract.h"
#include "common/png_encode.h"
#include <jpeglib.h>
#include <csetjmp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jdoc { namespace pdf_detail {

struct HuffNode { short val; short nbits; };

// 2D mode codes
enum { PASS = -4, HORIZ = -5, VR3 = 0, VR2 = 1, VR1 = 2, V0 = 3, VL1 = 4, VL2 = 5, VL3 = 6, HUFF_ERROR = -1, HUFF_ZEROS = -2 };

// White Huffman table (8-bit initial lookup)
static const HuffNode kWhiteHuff[] = {
    {256,12},{272,12},{29,8},{30,8},{45,8},{46,8},{22,7},{22,7},
    {23,7},{23,7},{47,8},{48,8},{13,6},{13,6},{13,6},{13,6},{20,7},
    {20,7},{33,8},{34,8},{35,8},{36,8},{37,8},{38,8},{19,7},{19,7},
    {31,8},{32,8},{1,6},{1,6},{1,6},{1,6},{12,6},{12,6},{12,6},{12,6},
    {53,8},{54,8},{26,7},{26,7},{39,8},{40,8},{41,8},{42,8},{43,8},
    {44,8},{21,7},{21,7},{28,7},{28,7},{61,8},{62,8},{63,8},{0,8},
    {320,8},{384,8},{10,5},{10,5},{10,5},{10,5},{10,5},{10,5},{10,5},
    {10,5},{11,5},{11,5},{11,5},{11,5},{11,5},{11,5},{11,5},{11,5},
    {27,7},{27,7},{59,8},{60,8},{288,9},{290,9},{18,7},{18,7},{24,7},
    {24,7},{49,8},{50,8},{51,8},{52,8},{25,7},{25,7},{55,8},{56,8},
    {57,8},{58,8},{192,6},{192,6},{192,6},{192,6},{1664,6},{1664,6},
    {1664,6},{1664,6},{448,8},{512,8},{292,9},{640,8},{576,8},{294,9},
    {296,9},{298,9},{300,9},{302,9},{256,7},{256,7},{2,4},{2,4},{2,4},
    {2,4},{2,4},{2,4},{2,4},{2,4},{2,4},{2,4},{2,4},{2,4},{2,4},{2,4},
    {2,4},{2,4},{3,4},{3,4},{3,4},{3,4},{3,4},{3,4},{3,4},{3,4},{3,4},
    {3,4},{3,4},{3,4},{3,4},{3,4},{3,4},{3,4},{128,5},{128,5},{128,5},
    {128,5},{128,5},{128,5},{128,5},{128,5},{8,5},{8,5},{8,5},{8,5},
    {8,5},{8,5},{8,5},{8,5},{9,5},{9,5},{9,5},{9,5},{9,5},{9,5},{9,5},
    {9,5},{16,6},{16,6},{16,6},{16,6},{17,6},{17,6},{17,6},{17,6},
    {4,4},{4,4},{4,4},{4,4},{4,4},{4,4},{4,4},{4,4},{4,4},{4,4},{4,4},
    {4,4},{4,4},{4,4},{4,4},{4,4},{5,4},{5,4},{5,4},{5,4},{5,4},{5,4},
    {5,4},{5,4},{5,4},{5,4},{5,4},{5,4},{5,4},{5,4},{5,4},{5,4},
    {14,6},{14,6},{14,6},{14,6},{15,6},{15,6},{15,6},{15,6},{64,5},
    {64,5},{64,5},{64,5},{64,5},{64,5},{64,5},{64,5},{6,4},{6,4},
    {6,4},{6,4},{6,4},{6,4},{6,4},{6,4},{6,4},{6,4},{6,4},{6,4},{6,4},
    {6,4},{6,4},{6,4},{7,4},{7,4},{7,4},{7,4},{7,4},{7,4},{7,4},{7,4},
    {7,4},{7,4},{7,4},{7,4},{7,4},{7,4},{7,4},{7,4},{-2,3},{-2,3},
    {-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},
    {-1,0},{-1,0},{-1,0},{-1,0},{-3,4},{1792,3},{1792,3},{1984,4},
    {2048,4},{2112,4},{2176,4},{2240,4},{2304,4},{1856,3},{1856,3},
    {1920,3},{1920,3},{2368,4},{2432,4},{2496,4},{2560,4},{1472,1},
    {1536,1},{1600,1},{1728,1},{704,1},{768,1},{832,1},{896,1},
    {960,1},{1024,1},{1088,1},{1152,1},{1216,1},{1280,1},{1344,1},
    {1408,1}
};

// Black Huffman table (7-bit initial lookup)
static const HuffNode kBlackHuff[] = {
    {128,12},{160,13},{224,12},{256,12},{10,7},{11,7},{288,12},{12,7},
    {9,6},{9,6},{8,6},{8,6},{7,5},{7,5},{7,5},{7,5},{6,4},{6,4},{6,4},
    {6,4},{6,4},{6,4},{6,4},{6,4},{5,4},{5,4},{5,4},{5,4},{5,4},{5,4},
    {5,4},{5,4},{1,3},{1,3},{1,3},{1,3},{1,3},{1,3},{1,3},{1,3},{1,3},
    {1,3},{1,3},{1,3},{1,3},{1,3},{1,3},{1,3},{4,3},{4,3},{4,3},{4,3},
    {4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},
    {4,3},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},
    {3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},
    {3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},{3,2},
    {2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},
    {2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},
    {2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},
    {-2,4},{-2,4},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},
    {-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-3,5},{1792,4},
    {1792,4},{1984,5},{2048,5},{2112,5},{2176,5},{2240,5},{2304,5},
    {1856,4},{1856,4},{1920,4},{1920,4},{2368,5},{2432,5},{2496,5},
    {2560,5},{18,3},{18,3},{18,3},{18,3},{18,3},{18,3},{18,3},{18,3},
    {52,5},{52,5},{640,6},{704,6},{768,6},{832,6},{55,5},{55,5},
    {56,5},{56,5},{1280,6},{1344,6},{1408,6},{1472,6},{59,5},{59,5},
    {60,5},{60,5},{1536,6},{1600,6},{24,4},{24,4},{24,4},{24,4},
    {25,4},{25,4},{25,4},{25,4},{1664,6},{1728,6},{320,5},{320,5},
    {384,5},{384,5},{448,5},{448,5},{512,6},{576,6},{53,5},{53,5},
    {54,5},{54,5},{896,6},{960,6},{1024,6},{1088,6},{1152,6},{1216,6},
    {64,3},{64,3},{64,3},{64,3},{64,3},{64,3},{64,3},{64,3},{13,1},
    {13,1},{13,1},{13,1},{13,1},{13,1},{13,1},{13,1},{13,1},{13,1},
    {13,1},{13,1},{13,1},{13,1},{13,1},{13,1},{23,4},{23,4},{50,5},
    {51,5},{44,5},{45,5},{46,5},{47,5},{57,5},{58,5},{61,5},{256,5},
    {16,3},{16,3},{16,3},{16,3},{17,3},{17,3},{17,3},{17,3},{48,5},
    {49,5},{62,5},{63,5},{30,5},{31,5},{32,5},{33,5},{40,5},{41,5},
    {22,4},{22,4},{14,1},{14,1},{14,1},{14,1},{14,1},{14,1},{14,1},
    {14,1},{14,1},{14,1},{14,1},{14,1},{14,1},{14,1},{14,1},{14,1},
    {15,2},{15,2},{15,2},{15,2},{15,2},{15,2},{15,2},{15,2},{128,5},
    {192,5},{26,5},{27,5},{28,5},{29,5},{19,4},{19,4},{20,4},{20,4},
    {34,5},{35,5},{36,5},{37,5},{38,5},{39,5},{21,4},{21,4},{42,5},
    {43,5},{0,3},{0,3},{0,3},{0,3}
};

// 2D mode Huffman table (7-bit initial lookup)
static const HuffNode k2dHuff[] = {
    {128,11},{144,10},{6,7},{0,7},{5,6},{5,6},{1,6},{1,6},{-4,4},
    {-4,4},{-4,4},{-4,4},{-4,4},{-4,4},{-4,4},{-4,4},{-5,3},{-5,3},
    {-5,3},{-5,3},{-5,3},{-5,3},{-5,3},{-5,3},{-5,3},{-5,3},{-5,3},
    {-5,3},{-5,3},{-5,3},{-5,3},{-5,3},{4,3},{4,3},{4,3},{4,3},{4,3},
    {4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},{4,3},
    {2,3},{2,3},{2,3},{2,3},{2,3},{2,3},{2,3},{2,3},{2,3},{2,3},{2,3},
    {2,3},{2,3},{2,3},{2,3},{2,3},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},
    {3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},
    {3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},
    {3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},
    {3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},
    {3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},{3,1},
    {3,1},{3,1},{3,1},{-2,4},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},
    {-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},
    {-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-1,0},{-3,3}
};

static const uint8_t kClzTable[256] = {
    8,7,6,6,5,5,5,5,4,4,4,4,4,4,4,4,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
static const uint8_t kTailMask[8] = {0x7F,0x3F,0x1F,0x0F,0x07,0x03,0x01,0x00};
static const uint8_t kLmask[8] = {0xFF,0x7F,0x3F,0x1F,0x0F,0x07,0x03,0x01};
static const uint8_t kRmask[8] = {0x00,0x80,0xC0,0xE0,0xF0,0xF8,0xFC,0xFE};

static inline int get_bit(const uint8_t* buf, int x) {
    return (buf[x >> 3] >> (7 - (x & 7))) & 1;
}

static inline void set_bits(uint8_t* line, int x0, int x1) {
    if (x1 <= x0) return;
    int a0 = x0 >> 3, a1 = x1 >> 3, b0 = x0 & 7, b1 = x1 & 7;
    if (a0 == a1) { if (b1) line[a0] |= kLmask[b0] & kRmask[b1]; }
    else {
        line[a0] |= kLmask[b0];
        for (int a = a0 + 1; a < a1; a++) line[a] = 0xFF;
        if (b1) line[a1] |= kRmask[b1];
    }
}

static int next_edge(const uint8_t* line, int x, int w) {
    if (!line) return w;
    int m;
    if (x < 0) { x = 0; m = 0xFF; }
    else { m = kTailMask[x & 7]; }
    int W = w >> 3;
    int xb = x >> 3;
    int a = line[xb];
    int b = (a ^ (a >> 1)) & m;
    if (xb >= W) { int r = (xb << 3) + kClzTable[b]; return r > w ? w : r; }
    while (b == 0) {
        if (++xb >= W) goto nearend;
        int prev_lsb = a & 1;
        a = line[xb];
        b = (prev_lsb << 7) ^ a ^ (a >> 1);
    }
    return (xb << 3) + kClzTable[b];
nearend:
    if ((xb << 3) == w) return w;
    { int prev_lsb = a & 1; a = line[xb]; b = (prev_lsb << 7) ^ a ^ (a >> 1); }
    { int r = (xb << 3) + kClzTable[b]; return r > w ? w : r; }
}

static int next_color_edge(const uint8_t* line, int x, int w, int color) {
    if (!line || x >= w) return w;
    x = next_edge(line, (x > 0 || !color) ? x : -1, w);
    if (x < w && get_bit(line, x) != color)
        x = next_edge(line, x, w);
    return x;
}

// ── New CCITTFax decoder using lookup tables ──

struct BitStream {
    const uint8_t* src;
    size_t src_len;
    uint32_t word;
    int bidx; // bits consumed from word (32-bidx = bits available)
    size_t byte_pos;

    BitStream(const uint8_t* s, size_t l) : src(s), src_len(l), word(0), bidx(32), byte_pos(0) {
        fill();
    }

    void fill() {
        while (bidx > (32 - 13) && byte_pos < src_len) {
            bidx -= 8;
            word |= static_cast<uint32_t>(src[byte_pos++]) << bidx;
        }
    }

    void eat(int n) { word <<= n; bidx += n; }

    int get_code(const HuffNode* table, int initial_bits) {
        fill();
        int tidx = word >> (32 - initial_bits);
        int val = table[tidx].val;
        int nbits = table[tidx].nbits;
        if (nbits > initial_bits) {
            uint32_t wordmask = (1u << (32 - initial_bits)) - 1;
            tidx = val + ((word & wordmask) >> (32 - nbits));
            val = table[tidx].val;
            nbits = initial_bits + table[tidx].nbits;
        }
        eat(nbits);
        return val;
    }

    int get_run(int color) {
        // Decode one 1D run (makeup + terminating)
        int total = 0;
        for (;;) {
            int code;
            if (color == 0) // white
                code = get_code(kWhiteHuff, 8);
            else
                code = get_code(kBlackHuff, 7);
            if (code < 0) return total; // error
            total += code;
            if (code < 64) break; // terminating code
        }
        return total;
    }
};

std::vector<uint8_t> decode_ccitt(const uint8_t* src, size_t src_len,
                                    int k_param, int columns, bool black_is_1) {
    if (columns <= 0) columns = 1728;
    int stride = (columns + 7) / 8;
    std::vector<uint8_t> out;
    BitStream st(src, src_len);

    std::vector<uint8_t> ref(stride, 0);  // reference line (all white)
    std::vector<uint8_t> dst(stride, 0);  // current line

    int max_rows = 100000;

    if (k_param == 0) {
        // Group 3, 1D
        while (max_rows-- > 0 && st.byte_pos < st.src_len) {
            std::memset(dst.data(), 0, stride);
            int a = 0, c = 0; // position, color (0=white)
            while (a < columns) {
                int run = st.get_run(c);
                if (run < 0) break;
                if (c) set_bits(dst.data(), a, std::min(a + run, columns));
                a += run;
                c = !c;
            }
            out.insert(out.end(), dst.begin(), dst.end());
        }
    } else if (k_param < 0) {
        // Group 4, 2D
        while (max_rows-- > 0 && st.byte_pos < st.src_len) {
            std::memset(dst.data(), 0, stride);
            int a = 0, c = 0; // position, color (0=white, 1=black)

            while (a < columns) {
                st.fill();

                int code = st.get_code(k2dHuff, 7);

                if (code == HORIZ) {
                    // Horizontal mode: read two 1D runs
                    if (a < 0) a = 0;
                    int run1 = st.get_run(c);
                    if (c) set_bits(dst.data(), a, std::min(a + run1, columns));
                    a += run1;
                    if (run1 < 64 || (run1 >= 64 && a <= columns)) c = !c;
                    else continue;

                    int run2 = st.get_run(c);
                    if (c) set_bits(dst.data(), a, std::min(a + run2, columns));
                    a += run2;
                    if (run2 < 64 || (run2 >= 64 && a <= columns)) c = !c;
                    else continue;
                    // After H mode: color is back to original
                    // (toggled twice = same as start)
                    continue; // don't toggle again below
                }

                if (code == PASS) {
                    // Pass mode
                    int b1 = next_color_edge(ref.data(), a, columns, !c);
                    int b2 = (b1 >= columns) ? columns : next_edge(ref.data(), b1, columns);
                    if (c) set_bits(dst.data(), a, b2);
                    a = b2;
                    continue;
                }

                // Vertical modes: V0, VR1-3, VL1-3
                int offset = 0;
                switch (code) {
                    case V0:  offset = 0; break;
                    case VR1: offset = 1; break;
                    case VR2: offset = 2; break;
                    case VR3: offset = 3; break;
                    case VL1: offset = -1; break;
                    case VL2: offset = -2; break;
                    case VL3: offset = -3; break;
                    default: goto done_line; // error/EOL
                }

                {
                    int b1 = next_color_edge(ref.data(), a, columns, !c) + offset;
                    if (b1 < 0) b1 = 0;
                    if (b1 > columns) b1 = columns;
                    if (c) set_bits(dst.data(), a, b1);
                    a = b1;
                    c = !c;
                }
            }

            done_line:
            out.insert(out.end(), dst.begin(), dst.end());
            std::memcpy(ref.data(), dst.data(), stride);
        }
    }

    // Output convention: 1-bits = black pixels (ITU-T standard).
    // Caller handles BlackIs1 and ImageMask interpretation.
    return out;
}

// ── JPEG Decoder ─────────────────────────────────────────
// (moved here — old CCITTFax tables removed)

// Marker: OLD_CCITT_START — everything below until OLD_CCITT_END was removed
// ── JPEG Decoder ─────────────────────────────────────────

struct JpegResult {
    std::vector<uint8_t> pixels;
    int width = 0, height = 0, components = 0;
};

struct JpegErrorMgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void jpeg_error_exit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    char buf[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buf);
    longjmp(err->setjmp_buffer, 1);
}

JpegResult jpeg_decode(const uint8_t* data, size_t len) {
    JpegResult result;
    struct jpeg_decompress_struct cinfo;
    JpegErrorMgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return result;
    }

    jpeg_create_decompress(&cinfo);
    // Custom memory source manager for all libjpeg versions
    {
        auto* src = static_cast<struct jpeg_source_mgr*>(
            (*cinfo.mem->alloc_small)(reinterpret_cast<j_common_ptr>(&cinfo),
                                       JPOOL_PERMANENT, sizeof(struct jpeg_source_mgr)));
        cinfo.src = src;
        src->next_input_byte = data;
        src->bytes_in_buffer = len;
        src->init_source = [](j_decompress_ptr) {};
        src->fill_input_buffer = [](j_decompress_ptr cinfo) -> boolean {
            // Insert fake EOI marker
            static const JOCTET eoi[2] = {0xFF, JPEG_EOI};
            cinfo->src->next_input_byte = eoi;
            cinfo->src->bytes_in_buffer = 2;
            return TRUE;
        };
        src->skip_input_data = [](j_decompress_ptr cinfo, long num_bytes) {
            if (num_bytes > 0) {
                size_t skip = static_cast<size_t>(num_bytes);
                if (skip > cinfo->src->bytes_in_buffer) skip = cinfo->src->bytes_in_buffer;
                cinfo->src->next_input_byte += skip;
                cinfo->src->bytes_in_buffer -= skip;
            }
        };
        src->resync_to_restart = jpeg_resync_to_restart;
        src->term_source = [](j_decompress_ptr) {};
    }
    jpeg_read_header(&cinfo, TRUE);

    if (cinfo.num_components == 4)
        cinfo.out_color_space = JCS_CMYK;
    else
        cinfo.out_color_space = JCS_RGB;

    jpeg_start_decompress(&cinfo);

    result.width = cinfo.output_width;
    result.height = cinfo.output_height;
    result.components = cinfo.output_components;
    int row_stride = result.width * result.components;
    result.pixels.resize(static_cast<size_t>(row_stride) * result.height);

    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* row_ptr = result.pixels.data() + cinfo.output_scanline * row_stride;
        jpeg_read_scanlines(&cinfo, &row_ptr, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return result;
}

// ── PNG Writer ───────────────────────────────────────────

// pixels_to_png moved to common/png_encode.h
using util::pixels_to_png;

// ── BMP Writer ───────────────────────────────────────────

static std::vector<char> pixels_to_bmp(const uint8_t* pixels, int w, int h,
                                        int components) {
    if (!pixels || w <= 0 || h <= 0) return {};

    int out_stride = ((w * 3 + 3) / 4) * 4;
    int pixel_data_size = out_stride * h;
    int file_size = 14 + 40 + pixel_data_size;

    std::vector<char> bmp(file_size, 0);
    auto write16 = [&](int off, uint16_t v) { memcpy(&bmp[off], &v, 2); };
    auto write32 = [&](int off, uint32_t v) { memcpy(&bmp[off], &v, 4); };

    bmp[0] = 'B'; bmp[1] = 'M';
    write32(2, static_cast<uint32_t>(file_size));
    write32(10, 14 + 40);
    write32(14, 40);
    write32(18, static_cast<uint32_t>(w));
    write32(22, static_cast<uint32_t>(h));
    write16(26, 1);
    write16(28, 24);
    write32(34, static_cast<uint32_t>(pixel_data_size));

    for (int y = 0; y < h; y++) {
        const uint8_t* src_row = pixels + (h - 1 - y) * w * components;
        char* dst_row = &bmp[14 + 40 + y * out_stride];
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
            if (components == 1) {
                r = g = b = src_row[x];
            } else if (components == 3) {
                r = src_row[x * 3]; g = src_row[x * 3 + 1]; b = src_row[x * 3 + 2];
            } else if (components == 4) {
                int c = src_row[x*4], m = src_row[x*4+1], yy = src_row[x*4+2], k = src_row[x*4+3];
                r = static_cast<uint8_t>(255 - std::min(255, c + k));
                g = static_cast<uint8_t>(255 - std::min(255, m + k));
                b = static_cast<uint8_t>(255 - std::min(255, yy + k));
            } else {
                r = g = b = src_row[x * components];
            }
            dst_row[x * 3 + 0] = static_cast<char>(b);
            dst_row[x * 3 + 1] = static_cast<char>(g);
            dst_row[x * 3 + 2] = static_cast<char>(r);
        }
    }
    return bmp;
}

// ── Image Extraction ─────────────────────────────────────



std::vector<ExtractedImage> extract_page_images(PdfDoc& doc, const PdfObj& page_obj,
                                                 const ContentParseResult& parse_result,
                                                 int page_num,
                                                 const std::string& output_dir,
                                                 unsigned min_image_size) {
    std::vector<ExtractedImage> images;
    int img_idx = 0;

    auto res = doc.resolve(page_obj.get("Resources"));

    for (auto& ip : parse_result.images) {
        PdfObj xobj;
        if (ip.xobj_ref >= 0) {
            xobj = doc.get_obj(ip.xobj_ref);
        } else if (!ip.xobj_name.empty()) {
            auto& xobjects = res.get("XObject");
            auto xd = doc.resolve(xobjects);
            if (xd.is_dict()) xobj = doc.resolve(xd.get(ip.xobj_name));
        }

        if (!xobj.is_stream()) continue;

        auto& subtype = xobj.get("Subtype");
        if (!subtype.is_name() || subtype.str_val != "Image") continue;

        int w = xobj.get("Width").as_int();
        int h = xobj.get("Height").as_int();
        if (w <= 0 || h <= 0) continue;
        if (min_image_size > 0 &&
            (static_cast<unsigned>(w) < min_image_size ||
             static_cast<unsigned>(h) < min_image_size))
            continue;

        ImageData img;
        img.page_number = page_num;
        img.name = "page" + std::to_string(page_num + 1) + "_img" + std::to_string(img_idx);
        img.width = w;
        img.height = h;

        // Determine filter chain
        auto filter_obj = doc.resolve(xobj.get("Filter"));
        std::string last_filter;
        bool single_filter = false;
        if (filter_obj.is_name()) {
            last_filter = filter_obj.str_val;
            single_filter = true;
        } else if (filter_obj.is_arr() && !filter_obj.arr.empty()) {
            auto last = doc.resolve(filter_obj.arr.back());
            if (last.is_name()) last_filter = last.str_val;
            single_filter = (filter_obj.arr.size() == 1);
        }

        if (last_filter == "DCTDecode") {
            if (single_filter) {
                // JPEG passthrough — raw bytes are valid JPEG
                if (!xobj.raw_stream_data() || xobj.raw_stream_size() == 0) continue;
                img.format = "jpeg";
                img.data.assign(reinterpret_cast<const char*>(xobj.raw_stream_data()),
                               reinterpret_cast<const char*>(xobj.raw_stream_data()) + xobj.raw_stream_size());
            } else {
                // Multi-stage: decode preceding filters, result is JPEG bytes
                auto decoded = doc.decode_stream(xobj);
                if (decoded.empty()) continue;
                // Check if result is valid JPEG
                if (decoded.size() >= 2 && decoded[0] == 0xFF && decoded[1] == 0xD8) {
                    img.format = "jpeg";
                    img.data.assign(reinterpret_cast<const char*>(decoded.data()),
                                   reinterpret_cast<const char*>(decoded.data()) + decoded.size());
                } else {
                    // Decode JPEG to raw pixels
                    auto jr = jpeg_decode(decoded.data(), decoded.size());
                    if (jr.pixels.empty()) continue;
                    img.format = "raw";
                    img.width = jr.width; img.height = jr.height;
                    img.components = jr.components;
                    img.pixels = std::move(jr.pixels);
                }
            }
        } else if (last_filter == "JPXDecode" && single_filter) {
            if (!xobj.raw_stream_data() || xobj.raw_stream_size() == 0) continue;
            img.format = "jp2";
            img.data.assign(reinterpret_cast<const char*>(xobj.raw_stream_data()),
                           reinterpret_cast<const char*>(xobj.raw_stream_data()) + xobj.raw_stream_size());
        } else if (last_filter == "CCITTFaxDecode") {
            auto parms = doc.resolve(xobj.get("DecodeParms"));
            int k = parms.get("K").as_int();
            int cols = parms.get("Columns").as_int();
            if (cols <= 0) cols = w;
            bool black_is_1 = parms.get("BlackIs1").bool_val;

            auto decoded = decode_ccitt(xobj.raw_stream_data(), xobj.raw_stream_size(),
                                         k, cols, black_is_1);
            // Convert 1-bit to grayscale
            int row_bytes = (cols + 7) / 8;
            int expected_rows = (int)decoded.size() / row_bytes;
            if (expected_rows <= 0) continue;

            std::vector<uint8_t> gray(static_cast<size_t>(cols) * expected_rows);
            for (int y = 0; y < expected_rows; y++) {
                for (int x = 0; x < cols; x++) {
                    int byte_idx = y * row_bytes + x / 8;
                    int bit_idx = 7 - (x % 8);
                    bool bit_set = (decoded[byte_idx] >> bit_idx) & 1;
                    // Decoder uses 1=black convention; if BlackIs1=false, invert
                    bool is_black = black_is_1 ? bit_set : !bit_set;
                    gray[y * cols + x] = is_black ? 0 : 255;
                }
            }

            img.format = "raw";
            img.width = cols;
            img.height = expected_rows;
            img.components = 1;
            img.pixels = std::move(gray);
        } else {
            // FlateDecode or other — decode to raw pixels
            auto decoded = doc.decode_stream(xobj);
            if (decoded.empty()) continue;

            int bpc = xobj.get("BitsPerComponent").as_int();
            if (bpc <= 0) bpc = 8;

            auto cs_obj = doc.resolve(xobj.get("ColorSpace"));
            std::string cs_name;
            if (cs_obj.is_name()) cs_name = cs_obj.str_val;
            else if (cs_obj.is_arr() && !cs_obj.arr.empty()) {
                auto first = doc.resolve(cs_obj.arr[0]);
                if (first.is_name()) cs_name = first.str_val;
            }

            int components = 3;
            bool is_indexed = false;
            int indexed_hival = 0;
            std::vector<uint8_t> indexed_lookup;
            int indexed_base_comps = 3;

            if (cs_name == "DeviceGray" || cs_name == "CalGray") components = 1;
            else if (cs_name == "DeviceCMYK") components = 4;
            else if (cs_name == "DeviceRGB" || cs_name == "CalRGB") components = 3;
            else if (cs_name == "ICCBased") {
                if (cs_obj.is_arr() && cs_obj.arr.size() >= 2) {
                    auto icc_stream = doc.resolve(cs_obj.arr[1]);
                    int n = icc_stream.get("N").as_int();
                    if (n > 0) components = n;
                }
            } else if (cs_name == "Indexed" || cs_name == "I") {
                // Indexed (palette) color space: [/Indexed base hival lookup]
                is_indexed = true;
                components = 1; // index values are single-byte
                if (cs_obj.is_arr() && cs_obj.arr.size() >= 4) {
                    // Base color space
                    auto base_cs = doc.resolve(cs_obj.arr[1]);
                    if (base_cs.is_name()) {
                        if (base_cs.str_val == "DeviceRGB" || base_cs.str_val == "CalRGB")
                            indexed_base_comps = 3;
                        else if (base_cs.str_val == "DeviceCMYK")
                            indexed_base_comps = 4;
                        else if (base_cs.str_val == "DeviceGray" || base_cs.str_val == "CalGray")
                            indexed_base_comps = 1;
                    } else if (base_cs.is_arr() && !base_cs.arr.empty()) {
                        auto bn = doc.resolve(base_cs.arr[0]);
                        if (bn.is_name() && bn.str_val == "ICCBased" && base_cs.arr.size() >= 2) {
                            auto icc = doc.resolve(base_cs.arr[1]);
                            int n = icc.get("N").as_int();
                            if (n > 0) indexed_base_comps = n;
                        }
                    }
                    // hival
                    indexed_hival = doc.resolve(cs_obj.arr[2]).as_int();
                    // lookup table (string or stream)
                    auto lut = doc.resolve(cs_obj.arr[3]);
                    if (lut.is_str()) {
                        indexed_lookup.assign(lut.str_val.begin(), lut.str_val.end());
                    } else if (lut.is_stream()) {
                        indexed_lookup = doc.decode_stream(lut);
                    }
                }
            } else if (cs_name == "Separation") {
                // Separation: treat as grayscale for extraction
                components = 1;
            } else if (cs_name == "DeviceN") {
                // DeviceN: use N parameter if available
                if (cs_obj.is_arr() && cs_obj.arr.size() >= 2) {
                    auto names_arr = doc.resolve(cs_obj.arr[1]);
                    if (names_arr.is_arr())
                        components = static_cast<int>(names_arr.arr.size());
                }
            }

            size_t expected = static_cast<size_t>(w) * h * components * bpc / 8;
            if (decoded.size() < expected && decoded.size() > 0) {
                // Try to infer components
                size_t total = static_cast<size_t>(w) * h;
                if (total > 0 && decoded.size() % total == 0)
                    components = static_cast<int>(decoded.size() / total);
            }

            // Apply Indexed palette expansion
            if (is_indexed && !indexed_lookup.empty() && components == 1) {
                size_t pixel_count = static_cast<size_t>(w) * h;
                std::vector<uint8_t> expanded(pixel_count * indexed_base_comps);
                size_t lut_stride = static_cast<size_t>(indexed_base_comps);
                for (size_t px = 0; px < pixel_count && px < decoded.size(); px++) {
                    int idx = decoded[px];
                    if (idx > indexed_hival) idx = indexed_hival;
                    size_t lut_off = static_cast<size_t>(idx) * lut_stride;
                    for (int c = 0; c < indexed_base_comps; c++) {
                        expanded[px * lut_stride + c] =
                            (lut_off + c < indexed_lookup.size()) ? indexed_lookup[lut_off + c] : 0;
                    }
                }
                decoded = std::move(expanded);
                components = indexed_base_comps;
            }

            img.format = "raw";
            img.components = components;
            img.pixels = std::move(decoded);
        }

        if (!img.data.empty() || !img.pixels.empty()) {
            // Encode raw pixels to PNG for in-memory delivery
            if (img.format == "raw" && img.data.empty() && !img.pixels.empty()) {
                auto png = pixels_to_png(img.pixels.data(), img.width, img.height, img.components);
                img.data.assign(png.begin(), png.end());
                img.format = "png";
                img.pixels.clear();
                img.pixels.shrink_to_fit();
            }

            if (!output_dir.empty()) {
                std::string ext, path;
                if (img.format == "jpeg") ext = ".jpg";
                else if (img.format == "jp2") ext = ".jp2";
                else ext = ".png";
                path = output_dir + "/" + img.name + ext;

                std::ofstream ofs(path, std::ios::binary);
                if (ofs) {
                    ofs.write(img.data.data(), img.data.size());
                    img.saved_path = path;
                }
                if (!img.saved_path.empty()) {
                    img.data.clear();
                    img.data.shrink_to_fit();
                }
            }

            ExtractedImage ei;
            ei.img = std::move(img);
            std::memcpy(ei.ctm, ip.ctm, sizeof(ip.ctm));
            images.push_back(std::move(ei));
            img_idx++;
        }
    }

    return images;
}

// ── Canvas / Image Compositing ───────────────────────────

struct Canvas {
    int width, height;
    std::vector<uint8_t> pixels; // RGB

    Canvas(int w, int h) : width(w), height(h), pixels(static_cast<size_t>(w) * h * 3, 255) {}

    void blend_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (static_cast<unsigned>(x) >= static_cast<unsigned>(width) ||
            static_cast<unsigned>(y) >= static_cast<unsigned>(height)) return;
        uint8_t* p = pixels.data() + (static_cast<size_t>(y) * width + x) * 3;
        if (a >= 255) {
            p[0] = r; p[1] = g; p[2] = b;
        } else if (a > 0) {
            // Alpha blend onto opaque white background (no dst alpha needed)
            unsigned inv = 255 - a;
            p[0] = static_cast<uint8_t>((r * a + p[0] * inv + 127) >> 8);
            p[1] = static_cast<uint8_t>((g * a + p[1] * inv + 127) >> 8);
            p[2] = static_cast<uint8_t>((b * a + p[2] * inv + 127) >> 8);
        }
    }

    void blit_image(const uint8_t* src, int sw, int sh, int scomp,
                     const double ctm[6], double page_h, double scale) {
        // CTM maps image space [0,1]×[0,1] to page space
        // Scale converts page space to canvas space
        bool axis_aligned = (std::abs(ctm[1]) < 0.001 && std::abs(ctm[2]) < 0.001);

        if (axis_aligned) {
            // Fast path: direct pixel copy
            double px = ctm[4] * scale;
            double py = (page_h - ctm[5] - ctm[3]) * scale;
            double pw = ctm[0] * scale;
            double ph = ctm[3] * scale;

            int dx = static_cast<int>(px);
            int dy = static_cast<int>(py);
            int dw = static_cast<int>(std::abs(pw));
            int dh = static_cast<int>(std::abs(ph));
            if (dw <= 0 || dh <= 0) return;

            bool downscale = (dw < sw || dh < sh);
            for (int y = 0; y < dh; y++) {
                for (int x = 0; x < dw; x++) {
                    uint8_t r, g, b;
                    if (downscale) {
                        // Area sampling for downscale
                        int sy0 = y * sh / dh, sy1 = (y + 1) * sh / dh;
                        int sx0 = x * sw / dw, sx1 = (x + 1) * sw / dw;
                        if (sy1 <= sy0) sy1 = sy0 + 1;
                        if (sx1 <= sx0) sx1 = sx0 + 1;
                        if (sy1 > sh) sy1 = sh;
                        if (sx1 > sw) sx1 = sw;
                        int sr = 0, sg = 0, sb = 0, cnt = 0;
                        for (int ry = sy0; ry < sy1; ry++)
                            for (int rx = sx0; rx < sx1; rx++) {
                                const uint8_t* sp = src + (ry * sw + rx) * scomp;
                                if (scomp >= 3) { sr += sp[0]; sg += sp[1]; sb += sp[2]; }
                                else { sr += sp[0]; sg += sp[0]; sb += sp[0]; }
                                cnt++;
                            }
                        r = static_cast<uint8_t>(sr / cnt);
                        g = static_cast<uint8_t>(sg / cnt);
                        b = static_cast<uint8_t>(sb / cnt);
                    } else {
                        // Nearest-neighbor for upscale
                        int sy = y * sh / dh; if (sy >= sh) sy = sh - 1;
                        int sx = x * sw / dw; if (sx >= sw) sx = sw - 1;
                        const uint8_t* sp = src + (sy * sw + sx) * scomp;
                        if (scomp >= 3) { r = sp[0]; g = sp[1]; b = sp[2]; }
                        else { r = g = b = sp[0]; }
                    }
                    blend_pixel(dx + x, dy + y, r, g, b, 255);
                }
            }
        } else {
            // General case: inverse transform
            // For each destination pixel, find source pixel
            double inv_det = ctm[0]*ctm[3] - ctm[1]*ctm[2];
            if (std::abs(inv_det) < 1e-10) return;

            // Bounding box in canvas space
            double corners[4][2];
            for (int i = 0; i < 4; i++) {
                double ix = (i & 1) ? 1.0 : 0.0;
                double iy = (i & 2) ? 1.0 : 0.0;
                double pgx = ctm[0]*ix + ctm[2]*iy + ctm[4];
                double pgy = ctm[1]*ix + ctm[3]*iy + ctm[5];
                corners[i][0] = pgx * scale;
                corners[i][1] = (page_h - pgy) * scale;
            }
            double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
            for (auto& c : corners) {
                if (c[0] < min_x) min_x = c[0];
                if (c[0] > max_x) max_x = c[0];
                if (c[1] < min_y) min_y = c[1];
                if (c[1] > max_y) max_y = c[1];
            }

            int x0 = std::max(0, static_cast<int>(min_x));
            int x1 = std::min(width - 1, static_cast<int>(max_x) + 1);
            int y0 = std::max(0, static_cast<int>(min_y));
            int y1 = std::min(height - 1, static_cast<int>(max_y) + 1);

            for (int dy = y0; dy <= y1; dy++) {
                for (int dx = x0; dx <= x1; dx++) {
                    double pgx = dx / scale;
                    double pgy = page_h - dy / scale;
                    double lx = pgx - ctm[4];
                    double ly = pgy - ctm[5];
                    double ix = (ctm[3]*lx - ctm[2]*ly) / inv_det;
                    double iy = (-ctm[1]*lx + ctm[0]*ly) / inv_det;
                    if (ix < 0 || ix > 1 || iy < 0 || iy > 1) continue;
                    int sx = static_cast<int>(ix * (sw - 1));
                    int sy = static_cast<int>((1 - iy) * (sh - 1));
                    if (sx < 0) sx = 0; if (sx >= sw) sx = sw - 1;
                    if (sy < 0) sy = 0; if (sy >= sh) sy = sh - 1;
                    const uint8_t* sp = src + (sy * sw + sx) * scomp;
                    uint8_t r, g, b;
                    if (scomp >= 3) { r = sp[0]; g = sp[1]; b = sp[2]; }
                    else { r = g = b = sp[0]; }
                    blend_pixel(dx, dy, r, g, b, 255);
                }
            }
        }
    }

    std::vector<char> to_png(int level = Z_BEST_SPEED) const {
        return pixels_to_png(pixels.data(), width, height, 3, level);
    }
};

// ── Page Rendering ───────────────────────────────────────

ImageData render_page_composite(PdfDoc& doc, const PdfObj& page_obj,
                                 const ContentParseResult& parse_result,
                                 int page_num, double page_w, double page_h,
                                 const std::string& output_dir) {
    constexpr double kDPI = 150.0;
    constexpr double kBase = 72.0;
    double scale = kDPI / kBase;
    int rw = static_cast<int>(page_w * scale);
    int rh = static_cast<int>(page_h * scale);
    if (rw <= 0 || rh <= 0) return {};

    Canvas canvas(rw, rh);

    // ── Rasterize vector paths (8× vertical AA + analytic horizontal coverage) ──
    constexpr int AA_V = 8;

    struct ScanEdge { double x_at_ymin; double inv_slope; int ymin, ymax; };
    std::vector<ScanEdge> edge_buf;
    std::vector<double> xs_buf;
    std::vector<int> cov_buf;

    auto rasterize_edges = [&](std::vector<ScanEdge>& edges, int ymin, int ymax,
                               uint8_t cr, uint8_t cg, uint8_t cb) {
        if (edges.empty()) return;
        ymin = std::max(0, ymin);
        ymax = std::min(rh * AA_V, ymax);
        if (ymin >= ymax) return;

        // Find x-bounds
        double xmin_d = 1e9, xmax_d = -1e9;
        for (auto& e : edges) {
            double x0 = e.x_at_ymin;
            double x1 = x0 + (e.ymax - e.ymin) * e.inv_slope;
            if (std::min(x0, x1) < xmin_d) xmin_d = std::min(x0, x1);
            if (std::max(x0, x1) > xmax_d) xmax_d = std::max(x0, x1);
        }
        int xmin = std::max(0, static_cast<int>(xmin_d) - 1);
        int xmax = std::min(rw, static_cast<int>(xmax_d) + 2);
        int xspan = xmax - xmin;
        if (xspan <= 0) return;

        cov_buf.assign(xspan + 1, 0);

        std::sort(edges.begin(), edges.end(),
                  [](const ScanEdge& a, const ScanEdge& b) { return a.ymin < b.ymin; });

        size_t next_edge = 0;
        struct ActiveEdge { double x; double inv_slope; int ymax; };
        std::vector<ActiveEdge> active;
        int prev_row = ymin / AA_V;

        for (int suby = ymin; suby < ymax; suby++) {
            int cur_row = suby / AA_V;
            if (cur_row != prev_row) {
                // Flush row
                for (int x = 0; x < xspan; x++) {
                    if (cov_buf[x] > 0) {
                        int alpha = cov_buf[x] / AA_V;
                        if (alpha > 255) alpha = 255;
                        canvas.blend_pixel(x + xmin, prev_row, cr, cg, cb, static_cast<uint8_t>(alpha));
                        cov_buf[x] = 0;
                    }
                }
                prev_row = cur_row;
            }

            // Add newly active edges
            while (next_edge < edges.size() && edges[next_edge].ymin <= suby) {
                auto& e = edges[next_edge];
                active.push_back({e.x_at_ymin + (suby - e.ymin) * e.inv_slope, e.inv_slope, e.ymax});
                next_edge++;
            }

            // Collect x-intersections, remove expired
            xs_buf.clear();
            size_t write = 0;
            for (size_t i = 0; i < active.size(); i++) {
                if (suby < active[i].ymax) {
                    double xval = active[i].x;
                    size_t pos = xs_buf.size();
                    xs_buf.push_back(xval);
                    while (pos > 0 && xs_buf[pos - 1] > xval) {
                        xs_buf[pos] = xs_buf[pos - 1]; pos--;
                    }
                    xs_buf[pos] = xval;
                    active[i].x += active[i].inv_slope;
                    active[write++] = active[i];
                }
            }
            active.resize(write);

            // Even-odd fill
            for (size_t i = 0; i + 1 < xs_buf.size(); i += 2) {
                double fx0 = xs_buf[i], fx1 = xs_buf[i + 1];
                int ix0 = std::max(xmin, static_cast<int>(fx0));
                int ix1 = std::min(xmax - 1, static_cast<int>(fx1));
                if (ix0 > ix1) continue;
                if (ix0 == ix1) {
                    cov_buf[ix0 - xmin] += static_cast<int>((fx1 - fx0) * 256 + 0.5);
                } else {
                    cov_buf[ix0 - xmin] += static_cast<int>((ix0 + 1 - fx0) * 256 + 0.5);
                    for (int x = ix0 + 1; x < ix1; x++) cov_buf[x - xmin] += 256;
                    cov_buf[ix1 - xmin] += static_cast<int>((fx1 - ix1) * 256 + 0.5);
                }
            }
        }
        // Flush last row
        for (int x = 0; x < xspan; x++) {
            if (cov_buf[x] > 0) {
                int alpha = cov_buf[x] / AA_V;
                if (alpha > 255) alpha = 255;
                canvas.blend_pixel(x + xmin, prev_row, cr, cg, cb, static_cast<uint8_t>(alpha));
            }
        }
    };

    // Bezier flattening (non-recursive with explicit stack)
    struct BezierWork { double x0,y0,cx1,cy1,cx2,cy2,x3,y3; int depth; };
    std::vector<BezierWork> bez_stack;
    auto flatten_bezier = [&](double x0, double y0, double cx1, double cy1,
                              double cx2, double cy2, double x3, double y3,
                              std::vector<std::pair<double,double>>& pts, double tol) {
        bez_stack.clear();
        bez_stack.push_back({x0,y0,cx1,cy1,cx2,cy2,x3,y3,0});
        while (!bez_stack.empty()) {
            auto w = bez_stack.back(); bez_stack.pop_back();
            if (w.depth > 10) { pts.push_back({w.x3, w.y3}); continue; }
            double dmax = std::max({std::abs(w.cx1-w.x0), std::abs(w.cy1-w.y0),
                                    std::abs(w.cx2-w.x3), std::abs(w.cy2-w.y3)});
            if (dmax < tol) { pts.push_back({w.x3, w.y3}); continue; }
            double m01x=(w.x0+w.cx1)/2, m01y=(w.y0+w.cy1)/2;
            double m12x=(w.cx1+w.cx2)/2, m12y=(w.cy1+w.cy2)/2;
            double m23x=(w.cx2+w.x3)/2, m23y=(w.cy2+w.y3)/2;
            double m012x=(m01x+m12x)/2, m012y=(m01y+m12y)/2;
            double m123x=(m12x+m23x)/2, m123y=(m12y+m23y)/2;
            double mx=(m012x+m123x)/2, my=(m012y+m123y)/2;
            // Push right half first (processed second), left half last (processed first)
            bez_stack.push_back({mx,my,m123x,m123y,m23x,m23y,w.x3,w.y3,w.depth+1});
            bez_stack.push_back({w.x0,w.y0,m01x,m01y,m012x,m012y,mx,my,w.depth+1});
        }
    };

    // Reusable buffers for path flattening
    std::vector<std::vector<std::pair<double,double>>> subpaths;
    std::vector<std::pair<double,double>> cur_sub;

    for (auto& rp : parse_result.paths) {
        if (rp.points.empty()) continue;

        // Flatten path to line segments
        subpaths.clear();
        cur_sub.clear();
        double px = 0, py = 0;
        for (auto& pt : rp.points) {
            switch (pt.type) {
                case PathPoint::MOVE:
                    if (!cur_sub.empty()) subpaths.push_back(std::move(cur_sub));
                    cur_sub.clear();
                    cur_sub.push_back({pt.x, pt.y});
                    px = pt.x; py = pt.y; break;
                case PathPoint::LINE:
                    cur_sub.push_back({pt.x, pt.y});
                    px = pt.x; py = pt.y; break;
                case PathPoint::CURVE:
                    flatten_bezier(px, py, pt.cx1, pt.cy1, pt.cx2, pt.cy2, pt.x, pt.y, cur_sub, 0.25);
                    px = pt.x; py = pt.y; break;
                case PathPoint::CLOSE:
                    if (!cur_sub.empty()) { cur_sub.push_back(cur_sub[0]); px = cur_sub[0].first; py = cur_sub[0].second; }
                    break;
            }
        }
        if (!cur_sub.empty()) subpaths.push_back(std::move(cur_sub));

        // Fill
        if (rp.do_fill) {
            edge_buf.clear();
            int ymin = rh * AA_V, ymax = 0;
            for (auto& sp : subpaths) {
                for (size_t i = 0; i + 1 < sp.size(); i++) {
                    double sx0 = sp[i].first * scale;
                    double sy0 = (page_h - sp[i].second) * scale;
                    double sx1 = sp[i+1].first * scale;
                    double sy1 = (page_h - sp[i+1].second) * scale;
                    int iy0 = static_cast<int>(std::round(sy0 * AA_V));
                    int iy1 = static_cast<int>(std::round(sy1 * AA_V));
                    if (iy0 == iy1) continue;
                    if (iy0 > iy1) { std::swap(sx0, sx1); std::swap(sy0, sy1); std::swap(iy0, iy1); }
                    double inv_slope = (sx1 - sx0) / (sy1 - sy0) / AA_V;
                    double x_start = sx0 + (iy0 / (double)AA_V - sy0) * (sx1 - sx0) / (sy1 - sy0);
                    edge_buf.push_back({x_start, inv_slope, iy0, iy1});
                    if (iy0 < ymin) ymin = iy0;
                    if (iy1 > ymax) ymax = iy1;
                }
            }
            uint8_t fr = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.fill_r * 255)));
            uint8_t fg = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.fill_g * 255)));
            uint8_t fb = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.fill_b * 255)));
            rasterize_edges(edge_buf, ymin, ymax, fr, fg, fb);
        }

        // Stroke
        if (rp.do_stroke) {
            double lw = rp.line_width * scale;
            if (lw < 1.0) lw = 1.0;
            double half = lw / 2.0;
            edge_buf.clear();
            int ymin = rh * AA_V, ymax = 0;
            for (auto& sp : subpaths) {
                for (size_t i = 0; i + 1 < sp.size(); i++) {
                    double sx0 = sp[i].first * scale;
                    double sy0 = (page_h - sp[i].second) * scale;
                    double sx1 = sp[i+1].first * scale;
                    double sy1 = (page_h - sp[i+1].second) * scale;
                    double dx = sx1 - sx0, dy = sy1 - sy0;
                    double len = std::sqrt(dx*dx + dy*dy);
                    if (len < 0.01) continue;
                    double nx = -dy / len * half;
                    double ny = dx / len * half;
                    double qx[4] = {sx0+nx, sx1+nx, sx1-nx, sx0-nx};
                    double qy[4] = {sy0+ny, sy1+ny, sy1-ny, sy0-ny};
                    for (int e = 0; e < 4; e++) {
                        int e2 = (e + 1) % 4;
                        double ey0 = qy[e], ey1 = qy[e2];
                        double ex0 = qx[e], ex1 = qx[e2];
                        int iy0 = static_cast<int>(std::round(ey0 * AA_V));
                        int iy1 = static_cast<int>(std::round(ey1 * AA_V));
                        if (iy0 == iy1) continue;
                        if (iy0 > iy1) { std::swap(iy0, iy1); std::swap(ex0, ex1); std::swap(ey0, ey1); }
                        double inv_s = (ex1 - ex0) / (ey1 - ey0) / AA_V;
                        double x_s = ex0 + (iy0 / (double)AA_V - ey0) * (ex1 - ex0) / (ey1 - ey0);
                        edge_buf.push_back({x_s, inv_s, iy0, iy1});
                        if (iy0 < ymin) ymin = iy0;
                        if (iy1 > ymax) ymax = iy1;
                    }
                }
            }
            uint8_t sr = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.stroke_r * 255)));
            uint8_t sg = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.stroke_g * 255)));
            uint8_t sb = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.stroke_b * 255)));
            rasterize_edges(edge_buf, ymin, ymax, sr, sg, sb);
        }
    }

    auto res = doc.resolve(page_obj.get("Resources"));

    // Composite images in stream order
    for (auto& ip : parse_result.images) {
        PdfObj xobj;
        if (ip.xobj_ref >= 0) {
            xobj = doc.get_obj(ip.xobj_ref);
        } else if (!ip.xobj_name.empty()) {
            auto& xobjects = res.get("XObject");
            auto xd = doc.resolve(xobjects);
            if (xd.is_dict()) xobj = doc.resolve(xd.get(ip.xobj_name));
        }

        auto& subtype = xobj.get("Subtype");

        int w = xobj.get("Width").as_int();
        int h = xobj.get("Height").as_int();
        if (w <= 0 || h <= 0) continue;

        // Check if this is an ImageMask (1-bit stencil)
        bool is_image_mask = xobj.get("ImageMask").bool_val;

        // Decode image to RGB pixels for compositing
        std::vector<uint8_t> pixels;
        int components = 3;
        {
            // Determine last filter to decide decode strategy
            auto filter_obj = doc.resolve(xobj.get("Filter"));
            std::string last_filter;
            if (filter_obj.is_name()) last_filter = filter_obj.str_val;
            else if (filter_obj.is_arr() && !filter_obj.arr.empty()) {
                auto last = doc.resolve(filter_obj.arr.back());
                if (last.is_name()) last_filter = last.str_val;
            }

            if (last_filter == "CCITTFaxDecode") {
                // Decode preceding filters first, then CCITTFax
                // decode_stream skips CCITTFax, so we need manual handling
                auto parms_obj = doc.resolve(xobj.get("DecodeParms"));
                PdfObj ccitt_parms;
                if (parms_obj.is_dict()) ccitt_parms = parms_obj;
                else if (parms_obj.is_arr() && !parms_obj.arr.empty())
                    ccitt_parms = doc.resolve(parms_obj.arr.back());

                int k = ccitt_parms.get("K").as_int();
                int cols = ccitt_parms.get("Columns").as_int();
                if (cols <= 0) cols = w;
                bool black_is_1 = ccitt_parms.get("BlackIs1").bool_val;

                // Get raw stream data and apply pre-filters manually
                const uint8_t* src = xobj.raw_stream_data();
                size_t src_len = xobj.raw_stream_size();
                if (!src || src_len == 0) continue;

                // Apply preceding filters (e.g. FlateDecode before CCITTFax)
                std::vector<uint8_t> pre_decoded;
                if (filter_obj.is_arr() && filter_obj.arr.size() > 1) {
                    pre_decoded.assign(src, src + src_len);
                    for (size_t fi = 0; fi + 1 < filter_obj.arr.size(); fi++) {
                        auto fname = doc.resolve(filter_obj.arr[fi]);
                        if (fname.str_val == "FlateDecode")
                            pre_decoded = decode_flate(pre_decoded.data(), pre_decoded.size());
                    }
                    src = pre_decoded.data();
                    src_len = pre_decoded.size();
                }

                auto ccitt_data = decode_ccitt(src, src_len, k, cols, black_is_1);
                int row_bytes = (cols + 7) / 8;
                int rows = ccitt_data.empty() ? 0 : (int)ccitt_data.size() / row_bytes;
                if (rows <= 0) continue;

                // Convert 1-bit to grayscale
                pixels.resize(static_cast<size_t>(cols) * rows);
                for (int y = 0; y < rows; y++)
                    for (int x = 0; x < cols; x++) {
                        int bi = y * row_bytes + x / 8;
                        bool bit_set = (ccitt_data[bi] >> (7 - (x % 8))) & 1;
                        if (is_image_mask) {
                            // For ImageMask: store raw bit (1=paint, 0=transparent)
                            pixels[y * cols + x] = bit_set ? 255 : 0;
                        } else {
                            bool is_black = black_is_1 ? bit_set : !bit_set;
                            pixels[y * cols + x] = is_black ? 0 : 255;
                        }
                    }
                w = cols; h = rows; components = 1;
            } else {
                // Use decode_stream for everything else (handles filter chains)
                auto decoded = doc.decode_stream(xobj);

                // Check if result is JPEG (decode_stream leaves DCTDecode raw)
                if (decoded.size() >= 2 && decoded[0] == 0xFF && decoded[1] == 0xD8) {
                    auto jr = jpeg_decode(decoded.data(), decoded.size());
                    pixels = std::move(jr.pixels);
                    w = jr.width; h = jr.height; components = jr.components;
                } else {
                    auto cs_obj = doc.resolve(xobj.get("ColorSpace"));
                    std::string cs_name;
                    if (cs_obj.is_name()) cs_name = cs_obj.str_val;
                    else if (cs_obj.is_arr() && !cs_obj.arr.empty()) {
                        auto first = doc.resolve(cs_obj.arr[0]);
                        if (first.is_name()) cs_name = first.str_val;
                    }
                    if (cs_name == "DeviceGray" || cs_name == "CalGray") components = 1;
                    else if (cs_name == "DeviceCMYK") components = 4;
                    else if (cs_name == "ICCBased") {
                        if (cs_obj.is_arr() && cs_obj.arr.size() >= 2) {
                            auto icc = doc.resolve(cs_obj.arr[1]);
                            int n = icc.get("N").as_int();
                            if (n > 0) components = n;
                        }
                    } else if (cs_name == "Indexed" || cs_name == "I") {
                        // Indexed color space: expand palette
                        components = 1;
                        if (cs_obj.is_arr() && cs_obj.arr.size() >= 4) {
                            auto base_cs = doc.resolve(cs_obj.arr[1]);
                            int base_comps = 3;
                            if (base_cs.is_name()) {
                                if (base_cs.str_val == "DeviceGray" || base_cs.str_val == "CalGray") base_comps = 1;
                                else if (base_cs.str_val == "DeviceCMYK") base_comps = 4;
                            }
                            int hival = doc.resolve(cs_obj.arr[2]).as_int();
                            auto lut_obj = doc.resolve(cs_obj.arr[3]);
                            std::vector<uint8_t> lut;
                            if (lut_obj.is_str()) lut.assign(lut_obj.str_val.begin(), lut_obj.str_val.end());
                            else if (lut_obj.is_stream()) lut = doc.decode_stream(lut_obj);

                            if (!lut.empty()) {
                                size_t px_count = static_cast<size_t>(w) * h;
                                std::vector<uint8_t> expanded(px_count * base_comps);
                                for (size_t pi = 0; pi < px_count && pi < decoded.size(); pi++) {
                                    int idx = decoded[pi];
                                    if (idx > hival) idx = hival;
                                    size_t lo = static_cast<size_t>(idx) * base_comps;
                                    for (int c = 0; c < base_comps; c++)
                                        expanded[pi * base_comps + c] = (lo + c < lut.size()) ? lut[lo + c] : 0;
                                }
                                decoded = std::move(expanded);
                                components = base_comps;
                            }
                        }
                    } else if (cs_name == "Separation") {
                        components = 1;
                    }
                    pixels = std::move(decoded);
                }
            }
        }
        size_t expected = static_cast<size_t>(w) * h * components;
        if (pixels.size() < expected) continue;

        if (is_image_mask && components == 1) {
            // ImageMask: painted where mask bit is SET (pixel==0 means bit was 1 in decoder)
            // In our grayscale: 0=black(bit was set), 255=white(bit was clear)
            // Paint fill color (black) where bit was set (pixel==0),
            // transparent where bit was clear (pixel==255)
            uint8_t fr = static_cast<uint8_t>(std::min(255.0, std::max(0.0, ip.fill_r * 255)));
            uint8_t fg = static_cast<uint8_t>(std::min(255.0, std::max(0.0, ip.fill_g * 255)));
            uint8_t fb = static_cast<uint8_t>(std::min(255.0, std::max(0.0, ip.fill_b * 255)));
            // Blit with alpha — use Canvas blit for proper CTM handling
            double px = ip.ctm[4] * scale;
            double py = (page_h - ip.ctm[5] - ip.ctm[3]) * scale;
            double pw = std::abs(ip.ctm[0] * scale);
            double ph = std::abs(ip.ctm[3] * scale);
            int dx = static_cast<int>(px), dy = static_cast<int>(py);
            int dw = static_cast<int>(pw), dh = static_cast<int>(ph);
            if (dw <= 0 || dh <= 0) continue;
            // Area sampling for ImageMask: compute coverage ratio in source region
            for (int y = 0; y < dh && dy + y >= 0 && dy + y < canvas.height; y++) {
                int sy0 = y * h / dh;
                int sy1 = (y + 1) * h / dh;
                if (sy1 <= sy0) sy1 = sy0 + 1;
                if (sy1 > h) sy1 = h;
                for (int x = 0; x < dw && dx + x < canvas.width; x++) {
                    if (dx + x < 0) continue;
                    int sx0 = x * w / dw;
                    int sx1 = (x + 1) * w / dw;
                    if (sx1 <= sx0) sx1 = sx0 + 1;
                    if (sx1 > w) sx1 = w;
                    // Count set pixels in source region
                    int total = (sy1 - sy0) * (sx1 - sx0);
                    if (total <= 0) continue;
                    int set = 0;
                    for (int ry = sy0; ry < sy1; ry++)
                        for (int rx = sx0; rx < sx1; rx++)
                            if (pixels[ry * w + rx] > 128) set++;
                    if (set > 0) {
                        uint8_t a = static_cast<uint8_t>(set * 255 / total);
                        canvas.blend_pixel(dx + x, dy + y, fr, fg, fb, a);
                    }
                }
            }
        } else {
            canvas.blit_image(pixels.data(), w, h, components, ip.ctm, page_h, scale);
        }
    }

    ImageData img;
    img.page_number = page_num;
    img.name = "page" + std::to_string(page_num + 1) + "_img0";
    img.format = "raw";
    img.width = rw;
    img.height = rh;
    img.components = 3;

    // Canvas is already RGB — encode to PNG for in-memory delivery
    auto png = pixels_to_png(canvas.pixels.data(), rw, rh, 3, Z_BEST_SPEED);
    img.format = "png";
    img.data.assign(png.begin(), png.end());

    if (!output_dir.empty()) {
        std::string path = output_dir + "/" + img.name + ".png";
        std::ofstream f(path, std::ios::binary);
        if (f) {
            f.write(img.data.data(), static_cast<std::streamsize>(img.data.size()));
            img.saved_path = path;
        }
        if (!img.saved_path.empty()) {
            img.data.clear();
            img.data.shrink_to_fit();
        }
    }
    return img;
}

// ── Bookmark Extraction ──────────────────────────────────


}} // namespace jdoc::pdf_detail
