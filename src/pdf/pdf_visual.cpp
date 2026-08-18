#include "pdf_extract.h"
#include "pdf_limits.h"
#include "jbig2.h"
#include "jpx.h"
#include "common/png_encode.h"
#include <chrono>
#include <jpeglib.h>
#include "common/image_utils.h"
#include <csetjmp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
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
    bool inverted_cmyk = false; // Adobe APP14 CMYK: libjpeg leaves samples inverted
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
    result.inverted_cmyk = (cinfo.output_components == 4 &&
                            cinfo.saw_Adobe_marker &&
                            cinfo.Adobe_transform != 2);
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

// Axis-aligned rotation (degrees, clockwise) a placement CTM applies to the
// raster, or 0 for upright/mirrored/skewed placements. The CTM is in viewing
// coordinates, so this covers both rotated placements and page /Rotate: either
// way the stored raster is sideways relative to what the viewer shows.
static int ctm_axis_rotation(const double* m) {
    double diag = std::abs(m[0]) + std::abs(m[3]);
    double off = std::abs(m[1]) + std::abs(m[2]);
    if (off > diag * 50.0) {
        if (m[1] < 0 && m[2] > 0) return 90;
        if (m[1] > 0 && m[2] < 0) return 270;
        return 0; // rotation + mirror: leave untouched
    }
    if (diag > off * 50.0 && m[0] < 0 && m[3] < 0) return 180;
    return 0;
}

// In-place raster rotation by 90/180/270 degrees clockwise.
static void rotate_raster(std::vector<uint8_t>& px, unsigned& w, unsigned& h,
                          int comps, int deg) {
    if (comps <= 0) return;
    const size_t n = static_cast<size_t>(w) * h * comps;
    if (px.size() < n) return;
    std::vector<uint8_t> out(n);
    const unsigned ow = (deg == 180) ? w : h;
    const unsigned oh = (deg == 180) ? h : w;
    for (unsigned y = 0; y < oh; y++) {
        for (unsigned x = 0; x < ow; x++) {
            unsigned sx, sy;
            if (deg == 90)       { sx = y;         sy = h - 1 - x; }
            else if (deg == 270) { sx = w - 1 - y; sy = x; }
            else                 { sx = w - 1 - x; sy = h - 1 - y; }
            const uint8_t* s = px.data() + (static_cast<size_t>(sy) * w + sx) * comps;
            uint8_t* d = out.data() + (static_cast<size_t>(y) * ow + x) * comps;
            for (int c = 0; c < comps; c++) d[c] = s[c];
        }
    }
    px = std::move(out);
    w = ow;
    h = oh;
}

// PDF /Decode array (spec 8.9.5.2): a per-component linear remap of image
// samples declared on the image dict — e.g. [1 0 1 0 1 0 1 0] re-inverts a
// CMYK image stored inverted. Applied to byte-expanded samples; Indexed
// images are excluded (their Decode remaps palette indices, default covers
// the palette) as are ImageMasks (bit-sense handled at unpack).
static void apply_decode_array(PdfDoc& doc, const PdfObj& xobj,
                               std::vector<uint8_t>& pixels, int components) {
    if (components < 1 || components > 4) return;
    auto dec = doc.resolve(xobj.get("Decode"));
    if (!dec.is_arr() || dec.arr.size() < static_cast<size_t>(components) * 2)
        return;
    bool nontrivial = false;
    uint8_t lut[4][256];
    for (int c = 0; c < components; c++) {
        double d0 = dec.arr[c * 2].as_num();
        double d1 = dec.arr[c * 2 + 1].as_num();
        if (d0 != 0.0 || d1 != 1.0) nontrivial = true;
        for (int v = 0; v < 256; v++) {
            double out = (d0 + (d1 - d0) * v / 255.0) * 255.0 + 0.5;
            lut[c][v] = static_cast<uint8_t>(std::min(255.0, std::max(0.0, out)));
        }
    }
    if (!nontrivial) return;
    for (size_t i = 0; i + components <= pixels.size(); i += components)
        for (int c = 0; c < components; c++)
            pixels[i + c] = lut[c][pixels[i + c]];
}

// Indexed images may pack their palette indices at 1/2/4 bits per sample;
// expand them to one index byte per pixel before palette lookup.
static bool unpack_subbyte_indices(std::vector<uint8_t>& data, int w, int h, int bpc) {
    if (bpc != 1 && bpc != 2 && bpc != 4) return false;
    size_t row_bytes = (static_cast<size_t>(w) * bpc + 7) / 8;
    if (w <= 0 || h <= 0 || data.size() < row_bytes * h) return false;
    std::vector<uint8_t> out(static_cast<size_t>(w) * h);
    int per = 8 / bpc;
    int mask = (1 << bpc) - 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t byte = data[static_cast<size_t>(y) * row_bytes + x / per];
            int shift = 8 - bpc * ((x % per) + 1);
            out[static_cast<size_t>(y) * w + x] = (byte >> shift) & mask;
        }
    data = std::move(out);
    return true;
}

// Decode an image's /SMask into an 8-bit alpha plane (0 = transparent).
static bool decode_smask(PdfDoc& doc, const PdfObj& xobj,
                         std::vector<uint8_t>& alpha, int& aw, int& ah) {
    auto sm = doc.resolve(xobj.get("SMask"));
    if (!sm.is_stream()) return false;
    int w = sm.get("Width").as_int();
    int h = sm.get("Height").as_int();
    if (w <= 0 || h <= 0) return false;
    auto data = doc.decode_stream(sm);
    if (data.size() >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
        auto jr = jpeg_decode(data.data(), data.size());
        if (jr.pixels.empty()) return false;
        w = jr.width;
        h = jr.height;
        if (jr.components == 1) {
            data = std::move(jr.pixels);
        } else {
            data.resize(static_cast<size_t>(w) * h);
            for (size_t i = 0; i < data.size(); i++)
                data[i] = jr.pixels[i * jr.components];
        }
    } else {
        int bpc = sm.get("BitsPerComponent").as_int();
        if (bpc == 1) {
            if (!unpack_subbyte_indices(data, w, h, 1)) return false;
            for (auto& v : data) v = v ? 255 : 0;
        } else if (data.size() < static_cast<size_t>(w) * h) {
            return false;
        }
        data.resize(static_cast<size_t>(w) * h);
    }
    apply_decode_array(doc, sm, data, 1); // honors /Decode [1 0]
    alpha = std::move(data);
    aw = w;
    ah = h;
    return true;
}

// Decode an explicit stencil /Mask (an image XObject with ImageMask true)
// into an 8-bit alpha plane (255 = base image shows). The sample the Decode
// array maps to 1 is the visible area — the polarity Acrobat and mupdf
// render for the JBIG2-masked logos in Korean office documents.
static bool decode_stencil_mask(PdfDoc& doc, const PdfObj& xobj,
                                std::vector<uint8_t>& alpha, int& aw, int& ah) {
    auto mk = doc.resolve(xobj.get("Mask"));
    if (!mk.is_stream() || !mk.get("ImageMask").bool_val) return false;
    int w = mk.get("Width").as_int();
    int h = mk.get("Height").as_int();
    if (w <= 0 || h <= 0) return false;
    auto data = doc.decode_stream(mk);
    size_t row_bytes = (static_cast<size_t>(w) + 7) / 8;
    if (data.size() < row_bytes * h) return false;
    auto mdec = doc.resolve(mk.get("Decode"));
    bool flip = mdec.is_arr() && mdec.arr.size() >= 2 &&
                mdec.arr[0].as_num() >= 0.5;
    alpha.assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            bool bit = (data[static_cast<size_t>(y) * row_bytes + (x >> 3)]
                        >> (7 - (x & 7))) & 1;
            alpha[static_cast<size_t>(y) * w + x] = (bit != flip) ? 255 : 0;
        }
    aw = w;
    ah = h;
    return true;
}

// CIE Lab samples (D50 white, the PDF default) to sRGB in place. L is stored
// 0..255 for 0..100; a and b are byte-mapped over /Range (default ±100).
static void lab_pixels_to_srgb(std::vector<uint8_t>& px, size_t count,
                               double amin, double amax,
                               double bmin, double bmax) {
    auto finv = [](double t) {
        constexpr double d = 6.0 / 29.0;
        return t > d ? t * t * t : 3.0 * d * d * (t - 4.0 / 29.0);
    };
    auto gam = [](double v) {
        v = std::max(0.0, std::min(1.0, v));
        return v <= 0.0031308 ? 12.92 * v
                              : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    };
    for (size_t i = 0; i + 1 < count * 3; i += 3) {
        double L = px[i] * 100.0 / 255.0;
        double a = amin + px[i + 1] * (amax - amin) / 255.0;
        double b = bmin + px[i + 2] * (bmax - bmin) / 255.0;
        double fy = (L + 16.0) / 116.0;
        double fx = fy + a / 500.0;
        double fz = fy - b / 200.0;
        double X = 0.9642 * finv(fx);
        double Y = finv(fy);
        double Z = 0.8249 * finv(fz);
        // XYZ (D50) → linear sRGB, Bradford-adapted matrix.
        double R = 3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z;
        double G = -0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z;
        double B = 0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z;
        px[i] = static_cast<uint8_t>(gam(R) * 255.0 + 0.5);
        px[i + 1] = static_cast<uint8_t>(gam(G) * 255.0 + 0.5);
        px[i + 2] = static_cast<uint8_t>(gam(B) * 255.0 + 0.5);
    }
}

// /Range for a [/Lab <<…>>] colorspace; defaults per spec.
static void lab_range(PdfDoc& doc, const PdfObj& cs_obj, double range[4]) {
    range[0] = -100;
    range[1] = 100;
    range[2] = -100;
    range[3] = 100;
    if (!cs_obj.is_arr() || cs_obj.arr.size() < 2) return;
    auto params = doc.resolve(cs_obj.arr[1]);
    auto rng = doc.resolve(params.get("Range"));
    if (rng.is_arr() && rng.arr.size() >= 4)
        for (int i = 0; i < 4; i++) range[i] = rng.arr[i].as_num();
}

// 16-bit samples fold to their high bytes; downstream reads bytes, and
// leaving both bytes in place shears the sample grid.
static void fold_16bpc(std::vector<uint8_t>& samples) {
    for (size_t i = 0; i * 2 < samples.size(); i++) samples[i] = samples[i * 2];
    samples.resize(samples.size() / 2);
}

std::vector<ExtractedImage> extract_page_images(PdfDoc& doc, const PdfObj& resources,
                                                 const ContentParseResult& parse_result,
                                                 int page_num,
                                                 const std::string& output_dir,
                                                 unsigned min_image_size,
                                                 PageRenderDiag* diag,
                                                 const std::vector<size_t>* only,
                                                 int name_base) {
    std::vector<ExtractedImage> images;
    int img_idx = name_base;
    int considered = 0;   // image placements this call attempted
    int policy_skips = 0; // dedup and min_image_size: deliberate, not failures

    const PdfObj& res = resources;

    // One file per distinct source image: a logo or rule stamped N times
    // across the page used to write N identical files.
    std::unordered_set<int> seen_refs;
    std::unordered_set<std::string> seen_names;

    const size_t total = only ? only->size() : parse_result.images.size();
    for (size_t oi = 0; oi < total; oi++) {
        const size_t pi_idx = only ? (*only)[oi] : oi;
        if (pi_idx >= parse_result.images.size()) continue;
        auto& ip = parse_result.images[pi_idx];
        PdfObj xobj_res;
        if (ip.inline_img) {
            // Inline image: synthesized stream, no /Subtype to check.
        } else if (ip.xobj_ref >= 0) {
            xobj_res = doc.get_obj(ip.xobj_ref);
        } else if (!ip.xobj_name.empty()) {
            auto& xobjects = res.get("XObject");
            auto xd = doc.resolve(xobjects);
            if (xd.is_dict()) xobj_res = doc.resolve(xd.get(ip.xobj_name));
        }
        const PdfObj& xobj = ip.inline_img ? *ip.inline_img : xobj_res;

        if (!xobj.is_stream()) continue;

        auto& subtype = xobj.get("Subtype");
        if (!ip.inline_img &&
            (!subtype.is_name() || subtype.str_val != "Image")) continue;

        considered++;
        if (diag) diag->images_total++;

        if (!ip.inline_img) {
            bool dup = ip.xobj_ref >= 0
                ? !seen_refs.insert(ip.xobj_ref).second
                : (!ip.xobj_name.empty() &&
                   !seen_names.insert(ip.xobj_name).second);
            if (dup) {
                policy_skips++;
                continue;
            }
        }

        int w = xobj.get("Width").as_int();
        int h = xobj.get("Height").as_int();
        if (w <= 0 || h <= 0) continue;
        if (min_image_size > 0 &&
            (static_cast<unsigned>(w) < min_image_size ||
             static_cast<unsigned>(h) < min_image_size)) {
            policy_skips++;
            continue;
        }

        ImageData img;
        img.page_number = page_num;
        img.name = "page" + std::to_string(page_num + 1) + "_img" + std::to_string(img_idx);
        img.width = static_cast<unsigned>(w);
        img.height = static_cast<unsigned>(h);
        // Set when 1-bit samples were unpacked in stencil sense (255 =
        // painted, /Decode honored) — the CCITT branch unpacks black/white
        // instead and must not feed the fill-colored RGBA conversion.
        bool mask_alpha_pixels = false;

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
            if (single_filter && doc.crypt.active) {
                // Encrypted document: the raw stream bytes are ciphertext.
                // decode_stream decrypts and leaves the JPEG payload intact.
                auto decoded = doc.decode_stream(xobj);
                if (decoded.size() < 2 || decoded[0] != 0xFF ||
                    decoded[1] != 0xD8)
                    continue;
                img.format = "jpeg";
                img.data.assign(reinterpret_cast<const char*>(decoded.data()),
                                reinterpret_cast<const char*>(decoded.data()) +
                                    decoded.size());
            } else if (single_filter) {
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
        } else if (last_filter == "JPXDecode") {
            // decode_stream applies decryption and pre-filters, leaving the
            // JPEG 2000 codestream raw for the decoder.
            auto decoded = doc.decode_stream(xobj);
            if (decoded.empty()) continue;
            auto jr = jpx_decode(decoded.data(), decoded.size());
            if (!jr.pixels.empty() && jr.src_depth <= 8) {
                img.format = "raw";
                img.width = static_cast<unsigned>(jr.width);
                img.height = static_cast<unsigned>(jr.height);
                img.components = jr.components;
                img.pixels = std::move(jr.pixels);
            } else if (single_filter) {
                // Feature outside the baseline decoder, or a >8-bit source
                // the decoder would flatten: extraction stays byte-exact.
                img.format = "jp2";
                img.data.assign(reinterpret_cast<const char*>(decoded.data()),
                                reinterpret_cast<const char*>(decoded.data()) + decoded.size());
            } else {
                if (diag) diag->unsupported_filter++;
                continue;
            }
        } else if (last_filter == "CCITTFaxDecode") {
            auto parms = doc.resolve(xobj.get("DecodeParms"));
            int k = parms.get("K").as_int();
            int cols = parms.get("Columns").as_int();
            if (cols <= 0) cols = w;
            bool black_is_1 = parms.get("BlackIs1").bool_val;

            const uint8_t* csrc = xobj.raw_stream_data();
            size_t clen = xobj.raw_stream_size();
            std::vector<uint8_t> dec_buf;
            if (doc.crypt.active) {
                // decode_stream decrypts and leaves the CCITT payload raw.
                dec_buf = doc.decode_stream(xobj);
                csrc = dec_buf.data();
                clen = dec_buf.size();
            }
            auto decoded = decode_ccitt(csrc, clen, k, cols, black_is_1);
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
            if (decoded.empty()) {
                // JBIG2 variant reject, or an unknown filter left nothing.
                if (diag) diag->unsupported_filter++;
                continue;
            }

            int bpc = xobj.get("BitsPerComponent").as_int();
            if (bpc <= 0) bpc = 8;
            if (bpc == 16) {
                fold_16bpc(decoded);
                bpc = 8;
            }

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
            // A stencil has no ColorSpace at all; without this it keeps the
            // RGB default and its 1-bit unpack below never runs.
            if (xobj.get("ImageMask").bool_val) components = 1;

            const size_t width = static_cast<size_t>(w);
            const size_t height = static_cast<size_t>(h);
            if (width > std::numeric_limits<size_t>::max() / height)
                continue;
            const size_t pixel_count = width * height;
            const size_t component_count =
                components > 0 ? static_cast<size_t>(components) : 0;
            const bool byte_aligned =
                bpc == 8 && component_count > 0 &&
                pixel_count <=
                    std::numeric_limits<size_t>::max() / component_count;
            const size_t expected = byte_aligned
                ? pixel_count * component_count : 0;
            if (expected > 0 && decoded.size() < expected) {
                // Try to infer components
                if (pixel_count > 0 && decoded.size() % pixel_count == 0)
                    components =
                        static_cast<int>(decoded.size() / pixel_count);
            }

            // Apply Indexed palette expansion
            if (is_indexed && bpc != 8 && components == 1)
                unpack_subbyte_indices(decoded, w, h, bpc);
            if (is_indexed && !indexed_lookup.empty() && components == 1) {
                if (indexed_base_comps != 1 && indexed_base_comps != 3 &&
                    indexed_base_comps != 4)
                    continue;
                size_t lut_stride = static_cast<size_t>(indexed_base_comps);
                if (pixel_count >
                    std::numeric_limits<size_t>::max() / lut_stride)
                    continue;
                std::vector<uint8_t> expanded(pixel_count * lut_stride);
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

            // Unpack 1-bit rows to bytes: Flate-compressed bitonal scans
            // otherwise fail the PNG encoder's size check and are dropped.
            // ImageMask samples keep the painted/clear sense (255 = painted,
            // honoring /Decode [1 0]); JBIG2 gray delivers 1 = black ink.
            if (bpc == 1 && components == 1 && !is_indexed) {
                bool is_image_mask = xobj.get("ImageMask").bool_val;
                bool mask_flip = false;
                if (is_image_mask) {
                    auto mdec = doc.resolve(xobj.get("Decode"));
                    mask_flip = mdec.is_arr() && mdec.arr.size() >= 2 &&
                                mdec.arr[0].as_num() >= 0.5;
                }
                bool ink_is_one = (last_filter == "JBIG2Decode");
                size_t row_bytes = (width + 7) / 8;
                if (decoded.size() >= row_bytes * height) {
                    std::vector<uint8_t> unpacked(pixel_count);
                    for (size_t uy = 0; uy < height; uy++)
                        for (size_t ux = 0; ux < width; ux++) {
                            bool bit = (decoded[uy * row_bytes + ux / 8]
                                        >> (7 - (ux & 7))) & 1;
                            uint8_t v;
                            if (is_image_mask) v = (bit == mask_flip) ? 255 : 0;
                            else if (ink_is_one) v = bit ? 0 : 255;
                            else v = bit ? 255 : 0;
                            unpacked[uy * width + ux] = v;
                        }
                    decoded = std::move(unpacked);
                    mask_alpha_pixels = is_image_mask;
                }
            }

            if (!is_indexed && bpc == 8)
                apply_decode_array(doc, xobj, decoded, components);

            // Ink-tint samples (Separation, single-ink DeviceN) expand to RGB
            // through the shared tint ramp — a raw tint byte read as gray has
            // its ink sense inverted. Lab converts to sRGB in place.
            if ((cs_name == "Separation" || cs_name == "DeviceN") &&
                components == 1) {
                auto csi = load_colorspace(doc, cs_obj);
                if (csi && csi->kind == CsInfo::TINT && !csi->lut.empty()) {
                    std::vector<uint8_t> rgb(pixel_count * 3);
                    for (size_t pi = 0;
                         pi < pixel_count && pi < decoded.size(); pi++) {
                        const auto& e = csi->lut[decoded[pi]];
                        rgb[pi * 3] = e[0];
                        rgb[pi * 3 + 1] = e[1];
                        rgb[pi * 3 + 2] = e[2];
                    }
                    decoded = std::move(rgb);
                    components = 3;
                }
            } else if (cs_name == "Lab") {
                double range[4];
                lab_range(doc, cs_obj, range);
                lab_pixels_to_srgb(decoded, decoded.size() / 3,
                                   range[0], range[1], range[2], range[3]);
            }

            img.format = "raw";
            img.components = components;
            img.pixels = std::move(decoded);
        }

        // A stencil-mask image (stamp, signature) delivers no color of its
        // own — it paints the fill color in force at its Do. Deliver it as
        // fill-colored RGBA with a transparent background instead of an
        // unreadable black-and-white block.
        bool img_rgba = false;
        if (mask_alpha_pixels && img.format == "raw" &&
            img.components == 1 && !img.pixels.empty()) {
            uint8_t fr = static_cast<uint8_t>(std::min(255.0, std::max(0.0, ip.fill_r * 255)));
            uint8_t fg = static_cast<uint8_t>(std::min(255.0, std::max(0.0, ip.fill_g * 255)));
            uint8_t fb = static_cast<uint8_t>(std::min(255.0, std::max(0.0, ip.fill_b * 255)));
            std::vector<uint8_t> rgba(img.pixels.size() * 4);
            for (size_t i = 0; i < img.pixels.size(); i++) {
                uint8_t* dp = rgba.data() + i * 4;
                dp[0] = fr; dp[1] = fg; dp[2] = fb;
                dp[3] = img.pixels[i]; // 255 where painted
            }
            img.pixels = std::move(rgba);
            img.components = 4;
            img_rgba = true;
        }

        // Soft-masked images (logo/watermark transparency): merge the alpha
        // into an RGBA raster. A passthrough JPEG is decoded first — its
        // pixels beneath transparent areas hold arbitrary color (often
        // black) that must not show as background. An explicit stencil
        // /Mask (JBIG2 logo cutouts) merges the same way.
        {
            std::vector<uint8_t> alpha;
            int amw = 0, amh = 0;
            if (!img_rgba &&
                (decode_smask(doc, xobj, alpha, amw, amh) ||
                 decode_stencil_mask(doc, xobj, alpha, amw, amh))) {
                if (img.format == "jpeg" && !img.data.empty()) {
                    auto jr = jpeg_decode(
                        reinterpret_cast<const uint8_t*>(img.data.data()),
                        img.data.size());
                    if (!jr.pixels.empty() && jr.components != 4) {
                        img.format = "raw";
                        img.width = static_cast<unsigned>(jr.width);
                        img.height = static_cast<unsigned>(jr.height);
                        img.components = jr.components;
                        img.pixels = std::move(jr.pixels);
                        img.data.clear();
                    }
                }
                if (img.format == "raw" && !img.pixels.empty() &&
                    (img.components == 1 || img.components == 3)) {
                    int iw = static_cast<int>(img.width);
                    int ih = static_cast<int>(img.height);
                    std::vector<uint8_t> rgba(static_cast<size_t>(iw) * ih * 4);
                    for (int y = 0; y < ih; y++) {
                        int ay = ih > 1 ? static_cast<int>(
                            static_cast<int64_t>(y) * amh / ih) : 0;
                        if (ay >= amh) ay = amh - 1;
                        for (int x = 0; x < iw; x++) {
                            int ax = iw > 1 ? static_cast<int>(
                                static_cast<int64_t>(x) * amw / iw) : 0;
                            if (ax >= amw) ax = amw - 1;
                            const uint8_t* sp = img.pixels.data() +
                                (static_cast<size_t>(y) * iw + x) * img.components;
                            uint8_t* dp = rgba.data() +
                                (static_cast<size_t>(y) * iw + x) * 4;
                            if (img.components == 3) {
                                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
                            } else {
                                dp[0] = dp[1] = dp[2] = sp[0];
                            }
                            dp[3] = alpha[static_cast<size_t>(ay) * amw + ax];
                        }
                    }
                    img.pixels = std::move(rgba);
                    img.components = 4;
                    img_rgba = true;
                }
            }
        }

        // A stencil or soft-masked image whose alpha never rises above zero
        // paints nothing on the page; HWP-to-PDF prints emit such whiteout
        // masks freely, and each would export as a blank file.
        if (img_rgba && img.components == 4 && !img.pixels.empty()) {
            bool visible = false;
            for (size_t ai = 3; ai < img.pixels.size(); ai += 4)
                if (img.pixels[ai] != 0) {
                    visible = true;
                    break;
                }
            if (!visible) {
                policy_skips++;
                continue;
            }
        }

        if (!img.data.empty() || !img.pixels.empty()) {
            // Save in the viewer's orientation: a raster stored sideways
            // (rotated placement, or an unrotated placement on a /Rotate
            // page) is rotated upright first. JPEG passthrough only survives
            // when no rotation is needed; JP2 stays as-is (no decoder).
            int save_rot = ctm_axis_rotation(ip.ctm);
            if (save_rot != 0) {
                if (img.format == "jpeg" && !img.data.empty()) {
                    auto jr = jpeg_decode(
                        reinterpret_cast<const uint8_t*>(img.data.data()),
                        img.data.size());
                    if (jr.inverted_cmyk)
                        for (auto& v : jr.pixels) v = 255 - v;
                    if (!jr.pixels.empty()) {
                        img.format = "raw";
                        img.width = static_cast<unsigned>(jr.width);
                        img.height = static_cast<unsigned>(jr.height);
                        img.components = jr.components;
                        img.pixels = std::move(jr.pixels);
                        img.data.clear();
                    }
                }
                if (img.format == "raw" && !img.pixels.empty())
                    rotate_raster(img.pixels, img.width, img.height,
                                  img.components, save_rot);
            }

            // Encode raw pixels to PNG for in-memory delivery
            if (img.format == "raw" && img.data.empty() && !img.pixels.empty()) {
                auto png = pixels_to_png(img.pixels.data(), img.pixels.size(),
                                         img.width, img.height, img.components,
                                         Z_BEST_SPEED, img_rgba);
                if (!png.empty()) {
                    img.data = std::move(png);
                    img.format = "png";
                    img.pixels = {};
                } else {
                    // Unsupported component layout or truncated sample data.
                    // Never expose a "raw" image whose declared dimensions
                    // exceed its backing buffer.
                    if (diag) diag->decode_size_mismatch++;
                    continue;
                }
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
                    discard_image_payload(img);
                }
            }

            ExtractedImage ei;
            ei.img = std::move(img);
            std::memcpy(ei.ctm, ip.ctm, sizeof(ip.ctm));
            images.push_back(std::move(ei));
            img_idx++;
        }
    }

    // Anything considered that neither hit a policy skip nor produced an
    // image was lost to a decode failure, whichever branch dropped it.
    if (diag) {
        int failed = considered - policy_skips -
                     static_cast<int>(images.size());
        if (failed > 0) diag->images_failed += failed;
    }
    return images;
}

// ── Canvas / Image Compositing ───────────────────────────

struct BlitAxis {
    int begin = 0;
    int end = 0;  // exclusive canvas coordinate
    double origin = 0;
    double length = 0;

    int source_edge(int canvas_edge, int source_length) const {
        long double position = static_cast<long double>(canvas_edge) - origin;
        if (!(position > 0)) return 0;  // also catches NaN
        if (position >= length) return source_length;

        // Preserve the old integer mapping where multiplication cannot
        // overflow. Extreme PDF coordinates use long double only after their
        // destination loop has already been clipped to the canvas.
        constexpr long double kInt64Max =
            std::numeric_limits<int64_t>::max();
        if (length < kInt64Max &&
            position <= kInt64Max / source_length) {
            auto numerator = static_cast<int64_t>(position) * source_length;
            return static_cast<int>(
                numerator / static_cast<int64_t>(length));
        }
        long double mapped = position * source_length / length;
        if (mapped <= 0) return 0;
        if (mapped >= source_length) return source_length;
        return static_cast<int>(mapped);
    }

    int source_pixel(int canvas_pixel, int source_length) const {
        return std::min(source_length - 1,
                        source_edge(canvas_pixel, source_length));
    }

    std::pair<int, int> source_interval(int canvas_pixel,
                                        int source_length) const {
        int first = source_pixel(canvas_pixel, source_length);
        int last = source_edge(canvas_pixel + 1, source_length);
        if (last <= first) last = first + 1;
        if (last > source_length) last = source_length;
        return {first, last};
    }
};

// PDF coordinates are untrusted doubles. Clip them to the actual raster before
// narrowing: a saturating int conversion alone can still leave billion-pixel
// loops for an image that only covers a few visible pixels.
static bool clip_blit_axis(double origin, double length, int canvas_length,
                           BlitAxis& out) {
    if (!std::isfinite(origin) || !std::isfinite(length) || canvas_length <= 0)
        return false;
    origin = std::trunc(origin);
    length = std::trunc(std::abs(length));
    if (length <= 0 || origin >= canvas_length) return false;

    double stop;
    if (origin < 0) {
        if (length <= -origin) return false;
        stop = origin + length;
    } else {
        stop = (length >= static_cast<double>(canvas_length) - origin)
                   ? static_cast<double>(canvas_length)
                   : origin + length;
    }
    double first = std::max(0.0, origin);
    double last = std::min(static_cast<double>(canvas_length), stop);
    if (first >= last) return false;
    out = {static_cast<int>(first), static_cast<int>(last), origin, length};
    return out.begin < out.end;
}

struct PixelRange {
    int begin = 0;
    int end = 0;  // exclusive
};

static bool clip_pixel_range(double min_value, double max_value,
                             int canvas_length, PixelRange& out) {
    if (!std::isfinite(min_value) || !std::isfinite(max_value) ||
        canvas_length <= 0 || max_value < 0 ||
        min_value > canvas_length - 1.0)
        return false;
    min_value = std::max(0.0, std::min(canvas_length - 1.0, min_value));
    max_value = std::max(0.0, std::min(canvas_length - 1.0, max_value));
    out.begin = static_cast<int>(min_value);
    out.end = std::min(canvas_length, static_cast<int>(max_value) + 2);
    return out.begin < out.end;
}

static bool clip_axis_aligned_image(const double ctm[6], double page_height,
                                    double scale, int canvas_width,
                                    int canvas_height, BlitAxis& x_axis,
                                    BlitAxis& y_axis) {
    double left = ctm[4] * scale;
    double top = (page_height - ctm[5] - ctm[3]) * scale;
    return clip_blit_axis(left, ctm[0] * scale, canvas_width, x_axis) &&
           clip_blit_axis(top, ctm[3] * scale, canvas_height, y_axis);
}

struct InverseImageMap {
    double determinant = 0;
    PixelRange x;
    PixelRange y;
};

static bool make_inverse_image_map(const double ctm[6], double page_height,
                                   double scale, int canvas_width,
                                   int canvas_height, InverseImageMap& out) {
    out.determinant = ctm[0] * ctm[3] - ctm[1] * ctm[2];
    if (!std::isfinite(out.determinant) ||
        std::abs(out.determinant) < 1e-10)
        return false;

    double min_x = std::numeric_limits<double>::infinity();
    double max_x = -min_x;
    double min_y = min_x;
    double max_y = -min_x;
    for (int corner = 0; corner < 4; corner++) {
        double image_x = (corner & 1) ? 1.0 : 0.0;
        double image_y = (corner & 2) ? 1.0 : 0.0;
        double page_x = ctm[0] * image_x + ctm[2] * image_y + ctm[4];
        double page_y = ctm[1] * image_x + ctm[3] * image_y + ctm[5];
        double canvas_x = page_x * scale;
        double canvas_y = (page_height - page_y) * scale;
        min_x = std::min(min_x, canvas_x);
        max_x = std::max(max_x, canvas_x);
        min_y = std::min(min_y, canvas_y);
        max_y = std::max(max_y, canvas_y);
    }
    return clip_pixel_range(min_x, max_x, canvas_width, out.x) &&
           clip_pixel_range(min_y, max_y, canvas_height, out.y);
}

struct Canvas {
    int width, height;
    size_t stride; // PNG row layout: 1 filter byte + width*3 samples
    std::vector<uint8_t> pixels;

    // Rows carry their PNG filter byte (0 = none) so the finished canvas
    // deflates in place, skipping a full-page copy at encode time.
    Canvas(int w, int h)
        : width(w), height(h), stride(static_cast<size_t>(w) * 3 + 1),
          pixels(stride * h, 255) {
        for (int y = 0; y < h; y++) pixels[static_cast<size_t>(y) * stride] = 0;
    }

    void blend_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (static_cast<unsigned>(x) >= static_cast<unsigned>(width) ||
            static_cast<unsigned>(y) >= static_cast<unsigned>(height)) return;
        uint8_t* p = pixels.data() + static_cast<size_t>(y) * stride + 1 +
                     static_cast<size_t>(x) * 3;
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

    // 1/3-component sources are gray/RGB; 4-component is CMYK ink
    // (Adobe-inverted JPEGs are normalized before reaching the canvas).
    // tint_lut (256 RGB entries) maps 1-component ink tints instead.
    static void sample_rgb(const uint8_t* sp, int scomp,
                           uint8_t& r, uint8_t& g, uint8_t& b,
                           const std::array<uint8_t, 3>* tint_lut = nullptr) {
        if (tint_lut && scomp == 1) {
            const auto& px = tint_lut[sp[0]];
            r = px[0]; g = px[1]; b = px[2];
        } else if (scomp == 4) {
            util::cmyk_to_rgb(sp[0] / 255.0, sp[1] / 255.0,
                              sp[2] / 255.0, sp[3] / 255.0, r, g, b);
        } else if (scomp >= 3) {
            r = sp[0]; g = sp[1]; b = sp[2];
        } else {
            r = g = b = sp[0];
        }
    }

    void blit_image(const uint8_t* src, int sw, int sh, int scomp,
                     const double ctm[6], double page_h, double scale,
                     const uint8_t* amask = nullptr, int amw = 0, int amh = 0,
                     int base_alpha = 255, const int* clip_px = nullptr,
                     const std::array<uint8_t, 3>* tint_lut = nullptr) {
        // CTM maps image space [0,1]×[0,1] to page space
        // Scale converts page space to canvas space
        bool axis_aligned = (std::abs(ctm[1]) < 0.001 && std::abs(ctm[2]) < 0.001);
        auto alpha_at = [&](int sx, int sy) -> uint8_t {
            if (!amask) return static_cast<uint8_t>(base_alpha);
            int ax = (sw > 1) ? static_cast<int>(
                static_cast<int64_t>(sx) * amw / sw) : 0;
            int ay = (sh > 1) ? static_cast<int>(
                static_cast<int64_t>(sy) * amh / sh) : 0;
            if (ax >= amw) ax = amw - 1;
            if (ay >= amh) ay = amh - 1;
            if (ax < 0 || ay < 0) return static_cast<uint8_t>(base_alpha);
            return static_cast<uint8_t>(
                amask[static_cast<size_t>(ay) * amw + ax] * base_alpha / 255);
        };

        if (axis_aligned) {
            // Fast path: direct pixel copy
            BlitAxis x_axis, y_axis;
            if (!clip_axis_aligned_image(ctm, page_h, scale, width, height,
                                         x_axis, y_axis))
                return;
            if (clip_px) {
                x_axis.begin = std::max(x_axis.begin, clip_px[0]);
                x_axis.end = std::min(x_axis.end, clip_px[2]);
                y_axis.begin = std::max(y_axis.begin, clip_px[1]);
                y_axis.end = std::min(y_axis.end, clip_px[3]);
                if (x_axis.begin >= x_axis.end || y_axis.begin >= y_axis.end)
                    return;
            }

            bool downscale = (x_axis.length < sw || y_axis.length < sh);
            for (int dy = y_axis.begin; dy < y_axis.end; dy++) {
                for (int dx = x_axis.begin; dx < x_axis.end; dx++) {
                    uint8_t r, g, b;
                    if (downscale) {
                        // Area sampling for downscale
                        auto [sy0, sy1] = y_axis.source_interval(dy, sh);
                        auto [sx0, sx1] = x_axis.source_interval(dx, sw);
                        // A single destination pixel can cover millions of
                        // source pixels. 32-bit sums overflow above ~8.4 Mpx;
                        // use wide accumulators because image dimensions come
                        // from the PDF, not from a trusted renderer.
                        uint64_t sr = 0, sg = 0, sb = 0, sa = 0, cnt = 0;
                        for (int ry = sy0; ry < sy1; ry++)
                            for (int rx = sx0; rx < sx1; rx++) {
                                size_t src_idx =
                                    (static_cast<size_t>(ry) * sw + rx) * scomp;
                                const uint8_t* sp = src + src_idx;
                                uint8_t pr, pg, pb;
                                sample_rgb(sp, scomp, pr, pg, pb, tint_lut);
                                sr += pr; sg += pg; sb += pb;
                                // Alpha has to be averaged over the same box as
                                // the color. Rasterized-glyph strips carry a flat
                                // RGB plane and keep the letter shapes entirely in
                                // the soft mask, so point-sampling alpha here drops
                                // half of every stroke when a 600 DPI strip lands
                                // on a 300 DPI canvas — enough to turn 봉 into 농.
                                sa += alpha_at(rx, ry);
                                cnt++;
                            }
                        r = static_cast<uint8_t>(sr / cnt);
                        g = static_cast<uint8_t>(sg / cnt);
                        b = static_cast<uint8_t>(sb / cnt);
                        blend_pixel(dx, dy, r, g, b,
                                    static_cast<uint8_t>(sa / cnt));
                        continue;
                    }
                    // Nearest-neighbor for upscale
                    int sy = y_axis.source_pixel(dy, sh);
                    int sx = x_axis.source_pixel(dx, sw);
                    size_t src_idx =
                        (static_cast<size_t>(sy) * sw + sx) * scomp;
                    const uint8_t* sp = src + src_idx;
                    sample_rgb(sp, scomp, r, g, b, tint_lut);
                    blend_pixel(dx, dy, r, g, b, alpha_at(sx, sy));
                }
            }
        } else {
            // General case: inverse transform
            // For each destination pixel, find source pixel
            InverseImageMap image_map;
            if (!make_inverse_image_map(ctm, page_h, scale, width, height,
                                        image_map))
                return;
            if (clip_px) {
                image_map.x.begin = std::max(image_map.x.begin, clip_px[0]);
                image_map.x.end = std::min(image_map.x.end, clip_px[2]);
                image_map.y.begin = std::max(image_map.y.begin, clip_px[1]);
                image_map.y.end = std::min(image_map.y.end, clip_px[3]);
            }

            // Bilinear 4-tap sampling; nearest-neighbour aliased rotated
            // scans badly while the axis-aligned path box-filtered.
            auto bilinear = [&](double image_x, double image_y, double& r,
                                double& g, double& b, double& a) {
                double fx = image_x * (sw - 1);
                double fy = (1 - image_y) * (sh - 1);
                int sx0 = static_cast<int>(fx);
                int sy0 = static_cast<int>(fy);
                int sx1 = std::min(sx0 + 1, sw - 1);
                int sy1 = std::min(sy0 + 1, sh - 1);
                double tx = fx - sx0, ty = fy - sy0;
                const int xs[2] = {sx0, sx1}, ys[2] = {sy0, sy1};
                const double wx[2] = {1 - tx, tx}, wy[2] = {1 - ty, ty};
                r = g = b = a = 0;
                for (int j = 0; j < 2; j++)
                    for (int i = 0; i < 2; i++) {
                        double wgt = wx[i] * wy[j];
                        if (wgt <= 0) continue;
                        const uint8_t* sp =
                            src + (static_cast<size_t>(ys[j]) * sw + xs[i]) *
                                      scomp;
                        uint8_t pr, pg, pb;
                        sample_rgb(sp, scomp, pr, pg, pb, tint_lut);
                        r += wgt * pr;
                        g += wgt * pg;
                        b += wgt * pb;
                        a += wgt * alpha_at(xs[i], ys[j]);
                    }
            };
            // When the source is denser than the destination, one bilinear
            // tap skips texels; a 2×2 sub-pixel average keeps thin scan
            // strokes from stippling.
            const bool minify =
                sw > (image_map.x.end - image_map.x.begin) ||
                sh > (image_map.y.end - image_map.y.begin);

            for (int canvas_y = image_map.y.begin;
                 canvas_y < image_map.y.end; canvas_y++) {
                for (int canvas_x = image_map.x.begin;
                     canvas_x < image_map.x.end; canvas_x++) {
                    double racc = 0, gacc = 0, bacc = 0, aacc = 0, wacc = 0;
                    const double subs[2] = {0.25, 0.75};
                    const int taps = minify ? 2 : 1;
                    for (int jy = 0; jy < taps; jy++) {
                        for (int jx = 0; jx < taps; jx++) {
                            double cxs = canvas_x +
                                         (minify ? subs[jx] : 0.5);
                            double cys = canvas_y +
                                         (minify ? subs[jy] : 0.5);
                            double page_x = cxs / scale;
                            double page_y = page_h - cys / scale;
                            double local_x = page_x - ctm[4];
                            double local_y = page_y - ctm[5];
                            double image_x =
                                (ctm[3] * local_x - ctm[2] * local_y) /
                                image_map.determinant;
                            double image_y =
                                (-ctm[1] * local_x + ctm[0] * local_y) /
                                image_map.determinant;
                            if (!std::isfinite(image_x) ||
                                !std::isfinite(image_y) || image_x < 0 ||
                                image_x > 1 || image_y < 0 || image_y > 1)
                                continue;
                            double r, g, b, a;
                            bilinear(image_x, image_y, r, g, b, a);
                            racc += r;
                            gacc += g;
                            bacc += b;
                            aacc += a;
                            wacc += 1;
                        }
                    }
                    if (wacc <= 0) continue;
                    // Partially-covered edge pixels keep their full sampled
                    // alpha over the covered fraction only.
                    double cov = wacc / (taps * taps);
                    blend_pixel(canvas_x, canvas_y,
                                static_cast<uint8_t>(racc / wacc + 0.5),
                                static_cast<uint8_t>(gacc / wacc + 0.5),
                                static_cast<uint8_t>(bacc / wacc + 0.5),
                                static_cast<uint8_t>(aacc / wacc * cov +
                                                     0.5));
                }
            }
        }
    }

    std::vector<char> to_png(int level = Z_BEST_SPEED) const {
        return util::prefiltered_to_png(pixels.data(), pixels.size(),
                                  static_cast<unsigned>(width),
                                  static_cast<unsigned>(height), level);
    }
};

// ── Page Rendering ───────────────────────────────────────

ImageData render_page_composite(PdfDoc& doc, const PdfObj& resources,
                                 const ContentParseResult& parse_result,
                                 int page_num, double page_w, double page_h,
                                 const std::string& output_dir,
                                 int img_idx,
                                 PageRenderDiag* diag) {
    constexpr double kMinDPI = 150.0;
    constexpr double kMaxDPI = 300.0;
    constexpr double kBase = 72.0;
    // Composite output was fixed at 150 DPI, which silently downsampled
    // high-resolution scans (drawings especially). Raise the render DPI to
    // the densest embedded image's effective resolution, within bounds.
    double dpi = kMinDPI;
    {
        const PdfObj& pre_res = resources;
        for (auto& ip : parse_result.images) {
            PdfObj xobj_res;
            if (ip.inline_img) {
                // Inline strips carry their own dims and filter.
            } else if (ip.xobj_ref >= 0) {
                xobj_res = doc.get_obj(ip.xobj_ref);
            } else if (!ip.xobj_name.empty()) {
                auto xd = doc.resolve(pre_res.get("XObject"));
                if (xd.is_dict()) xobj_res = doc.resolve(xd.get(ip.xobj_name));
            }
            const PdfObj& xobj = ip.inline_img ? *ip.inline_img : xobj_res;
            if (!xobj.is_stream()) continue;
            // Images the compositor may fail to decode must not inflate the
            // canvas. The in-tree JBIG2/JPX decoders cover subsets
            // (arithmetic generic regions; baseline codestreams), and a
            // variant they reject would leave a large, mostly blank canvas
            // still paid for in raster and PNG-encode time — while their
            // decodable payloads (stamps, logos, signatures) don't need a
            // DPI raise to read.
            auto pre_filter = doc.resolve(xobj.get("Filter"));
            std::string pre_last;
            if (pre_filter.is_name()) pre_last = pre_filter.str_val;
            else if (pre_filter.is_arr() && !pre_filter.arr.empty()) {
                auto last = doc.resolve(pre_filter.arr.back());
                if (last.is_name()) pre_last = last.str_val;
            }
            if (pre_last == "JPXDecode") continue;
            if (pre_last == "JBIG2Decode") {
                // The arithmetic subset now covers symbol text; raise the
                // DPI only when a header probe confirms the stream stays
                // inside it — a rejected variant would still pay for a
                // large, mostly blank canvas.
                bool single = pre_filter.is_name() ||
                              (pre_filter.is_arr() &&
                               pre_filter.arr.size() == 1);
                if (!single || !xobj.raw_stream_data()) continue;
                const uint8_t* jb = xobj.raw_stream_data();
                size_t jb_len = xobj.raw_stream_size();
                std::vector<uint8_t> jb_buf;
                if (doc.crypt.active) {
                    // Probe on plaintext; the raw bytes are ciphertext here.
                    jb_buf.assign(jb, jb + jb_len);
                    doc.crypt.decrypt_data(jb_buf, xobj.src_num,
                                           xobj.src_gen);
                    jb = jb_buf.data();
                    jb_len = jb_buf.size();
                }
                std::vector<uint8_t> gl;
                auto parms = doc.resolve(xobj.get("DecodeParms"));
                auto g = doc.resolve(parms.get("JBIG2Globals"));
                if (g.is_stream()) gl = doc.decode_stream(g);
                if (!jbig2_supported(jb, jb_len, gl.data(), gl.size()))
                    continue;
            }
            int iw = xobj.get("Width").as_int();
            int ih = xobj.get("Height").as_int();
            double pw_pt = std::hypot(ip.ctm[0], ip.ctm[1]);
            double ph_pt = std::hypot(ip.ctm[2], ip.ctm[3]);
            if (iw > 0 && pw_pt > 1.0) dpi = std::max(dpi, iw / (pw_pt / kBase));
            if (ih > 0 && ph_pt > 1.0) dpi = std::max(dpi, ih / (ph_pt / kBase));
        }
        if (dpi > kMaxDPI) dpi = kMaxDPI;
    }
    if (!std::isfinite(page_w) || !std::isfinite(page_h) ||
        page_w <= 0 || page_h <= 0)
        return {};
    double scale = dpi / kBase;
    // Bound total canvas pixels; a runaway page size must not exhaust memory.
    constexpr double kMaxPixels =
        static_cast<double>(limits::kMaxDecodedPixels);
    long double pixel_count = static_cast<long double>(page_w) * scale *
                              static_cast<long double>(page_h) * scale;
    if (pixel_count > kMaxPixels) {
        scale *= std::sqrt(kMaxPixels / pixel_count);
        double min_scale = kMinDPI / kBase / 8;
        if (scale < min_scale) {
            long double min_pixels = static_cast<long double>(page_w) * min_scale *
                                     static_cast<long double>(page_h) * min_scale;
            if (min_pixels > kMaxPixels) return {};
            scale = min_scale;
        }
    }
    long double raster_w = static_cast<long double>(page_w) * scale;
    long double raster_h = static_cast<long double>(page_h) * scale;
    if (!std::isfinite(scale) || raster_w > std::numeric_limits<int>::max() ||
        raster_h > std::numeric_limits<int>::max() ||
        raster_w * raster_h > kMaxPixels)
        return {};
    int rw = static_cast<int>(raster_w);
    int rh = static_cast<int>(raster_h);
    if (rw <= 0 || rh <= 0) return {};

    Canvas canvas(rw, rh);

    // Canvas-space clip rect (pixels, end-exclusive) from a recorded page-
    // space clip. Returns false when the op is clipped away entirely; the
    // sentinel bounds clamp to the full canvas at no cost.
    auto canvas_clip = [&](const float clip[4], int out[4]) -> bool {
        auto clamp_floor = [](double v, int hi) {
            v = std::max(0.0, std::min(static_cast<double>(hi), v));
            return static_cast<int>(std::floor(v));
        };
        auto clamp_ceil = [](double v, int hi) {
            v = std::max(0.0, std::min(static_cast<double>(hi), v));
            return static_cast<int>(std::ceil(v));
        };
        out[0] = clamp_floor(clip[0] * scale, rw);
        out[2] = clamp_ceil(clip[2] * scale, rw);
        out[1] = clamp_floor((page_h - clip[3]) * scale, rh);
        out[3] = clamp_ceil((page_h - clip[1]) * scale, rh);
        return out[0] < out[2] && out[1] < out[3];
    };

    // ── Rasterize vector paths (8× vertical AA + analytic horizontal coverage) ──
    constexpr int AA_V = 8;

    struct ScanEdge { double x_at_ymin; double inv_slope; int ymin, ymax; int dir; };
    std::vector<ScanEdge> edge_buf;
    struct Crossing { double x; int dir; };
    std::vector<Crossing> xs_buf;
    std::vector<int> cov_buf;

    // PDF fills default to the NONZERO winding rule; the even-odd pairing is
    // only for f*/B*. Overlapping stroke-shaped subpaths in bold display
    // glyphs cancel under even-odd, punching white holes at every joint.
    auto rasterize_edges = [&](std::vector<ScanEdge>& edges, int ymin, int ymax,
                               uint8_t cr, uint8_t cg, uint8_t cb,
                               bool nonzero = false, int alpha255 = 255,
                               int clip_x0 = 0,
                               int clip_x1 = std::numeric_limits<int>::max()) {
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
        // Clamp before narrowing, not after. These bounds come from path
        // coordinates the PDF supplies, and a degenerate one reaches ±1e11 —
        // converting that to int is undefined, and in practice it saturated to
        // INT_MIN, wrapped on the -1, and left xmin above xmax so the path was
        // dropped instead of clipped to the canvas. The bound below is far
        // outside any real raster (rw is a few thousand) so in-range geometry
        // narrows exactly as before.
        auto to_int = [](double v) {
            constexpr double kGuard = 1e9;
            return static_cast<int>(std::max(-kGuard, std::min(kGuard, v)));
        };
        int xmin = std::max({0, to_int(xmin_d) - 1, clip_x0});
        int xmax = std::min({rw, to_int(xmax_d) + 2, clip_x1});
        int xspan = xmax - xmin;
        if (xspan <= 0) return;

        cov_buf.assign(xspan + 1, 0);

        std::sort(edges.begin(), edges.end(),
                  [](const ScanEdge& a, const ScanEdge& b) { return a.ymin < b.ymin; });

        size_t next_edge = 0;
        struct ActiveEdge { double x; double inv_slope; int ymax; int dir; };
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
                        alpha = alpha * alpha255 / 255;
                        canvas.blend_pixel(x + xmin, prev_row, cr, cg, cb, static_cast<uint8_t>(alpha));
                        cov_buf[x] = 0;
                    }
                }
                prev_row = cur_row;
            }

            // Add newly active edges
            while (next_edge < edges.size() && edges[next_edge].ymin <= suby) {
                auto& e = edges[next_edge];
                active.push_back({e.x_at_ymin + (suby - e.ymin) * e.inv_slope, e.inv_slope, e.ymax, e.dir});
                next_edge++;
            }

            // Collect x-intersections, remove expired
            xs_buf.clear();
            size_t write = 0;
            for (size_t i = 0; i < active.size(); i++) {
                if (suby < active[i].ymax) {
                    Crossing cx{active[i].x, active[i].dir};
                    size_t pos = xs_buf.size();
                    xs_buf.push_back(cx);
                    while (pos > 0 && xs_buf[pos - 1].x > cx.x) {
                        xs_buf[pos] = xs_buf[pos - 1]; pos--;
                    }
                    xs_buf[pos] = cx;
                    active[i].x += active[i].inv_slope;
                    active[write++] = active[i];
                }
            }
            active.resize(write);

            auto add_span = [&](double fx0, double fx1) {
                // Clamp the endpoints, not just the pixel indices they narrow
                // to. They inherit the path's own coordinates, so a degenerate
                // one reaches ±1e11: the casts below would be undefined, and
                // the sub-pixel coverage terms — which stay in double until
                // after the clamp in the original — would scale a partial
                // pixel by 1e11 instead of by the fraction actually covered.
                // A span that runs off the canvas covers its edge pixel
                // fully, which is what clamping to [xmin, xmax] expresses.
                fx0 = std::max(static_cast<double>(xmin),
                               std::min(static_cast<double>(xmax), fx0));
                fx1 = std::max(static_cast<double>(xmin),
                               std::min(static_cast<double>(xmax), fx1));
                int ix0 = std::max(xmin, static_cast<int>(fx0));
                int ix1 = std::min(xmax - 1, static_cast<int>(fx1));
                if (ix0 > ix1) return;
                if (ix0 == ix1) {
                    cov_buf[ix0 - xmin] += static_cast<int>((fx1 - fx0) * 256 + 0.5);
                } else {
                    cov_buf[ix0 - xmin] += static_cast<int>((ix0 + 1 - fx0) * 256 + 0.5);
                    for (int x = ix0 + 1; x < ix1; x++) cov_buf[x - xmin] += 256;
                    cov_buf[ix1 - xmin] += static_cast<int>((fx1 - ix1) * 256 + 0.5);
                }
            };
            if (nonzero) {
                int wind = 0;
                double span_x = 0;
                for (auto& c : xs_buf) {
                    int prev = wind;
                    wind += c.dir;
                    if (prev == 0 && wind != 0) span_x = c.x;
                    else if (prev != 0 && wind == 0) add_span(span_x, c.x);
                }
            } else {
                for (size_t i = 0; i + 1 < xs_buf.size(); i += 2)
                    add_span(xs_buf[i].x, xs_buf[i + 1].x);
            }
        }
        // Flush last row
        for (int x = 0; x < xspan; x++) {
            if (cov_buf[x] > 0) {
                int alpha = cov_buf[x] / AA_V;
                if (alpha > 255) alpha = 255;
                alpha = alpha * alpha255 / 255;
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

    auto append_scan_edge = [&](double x0, double y0, double x1, double y1,
                                int& ymin, int& ymax) {
        if (!std::isfinite(x0) || !std::isfinite(y0) ||
            !std::isfinite(x1) || !std::isfinite(y1) || y0 == y1)
            return;

        int dir = 1;
        if (y0 > y1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
            dir = -1;
        }
        double iy0_d = std::round(y0 * AA_V);
        double iy1_d = std::round(y1 * AA_V);
        const double raster_bottom = static_cast<double>(rh) * AA_V;
        if (!std::isfinite(iy0_d) || !std::isfinite(iy1_d) ||
            iy1_d <= 0 || iy0_d >= raster_bottom)
            return;

        // Clip in floating point before narrowing. Besides avoiding UB, this
        // keeps the scanline loop proportional to the canvas rather than to a
        // malformed path coordinate.
        iy0_d = std::max(0.0, iy0_d);
        iy1_d = std::min(raster_bottom, iy1_d);
        int iy0 = static_cast<int>(iy0_d);
        int iy1 = static_cast<int>(iy1_d);
        if (iy0 >= iy1) return;

        double slope = (x1 - x0) / (y1 - y0);
        double inv_slope = slope / AA_V;
        double x_start = x0 + (iy0 / static_cast<double>(AA_V) - y0) * slope;
        if (!std::isfinite(inv_slope) || !std::isfinite(x_start)) return;
        edge_buf.push_back({x_start, inv_slope, iy0, iy1, dir});
        if (iy0 < ymin) ymin = iy0;
        if (iy1 > ymax) ymax = iy1;
    };

    // Reusable buffers for path flattening
    std::vector<std::vector<std::pair<double,double>>> subpaths;
    std::vector<std::pair<double,double>> cur_sub;

    auto draw_path = [&](const RenderPath& rp) {
        if (rp.points.empty()) return;
        int clip_px[4];
        if (!canvas_clip(rp.clip, clip_px)) return;

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
                // Open subpaths are implicitly closed when filling (spec
                // 8.5.3.2); without the closing edge, crossing parity breaks
                // and the fill floods the shape's concavities.
                size_t n = sp.size();
                for (size_t i = 0; i < n; i++) {
                    const auto& a = sp[i];
                    const auto& b = sp[(i + 1 == n) ? 0 : i + 1];
                    double sx0 = a.first * scale;
                    double sy0 = (page_h - a.second) * scale;
                    double sx1 = b.first * scale;
                    double sy1 = (page_h - b.second) * scale;
                    append_scan_edge(sx0, sy0, sx1, sy1, ymin, ymax);
                }
            }
            uint8_t fr = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.fill_r * 255)));
            uint8_t fg = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.fill_g * 255)));
            uint8_t fb = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.fill_b * 255)));
            int fa = static_cast<int>(std::min(1.0, std::max(0.0, rp.fill_alpha)) * 255 + 0.5);
            ymin = std::max(ymin, clip_px[1] * AA_V);
            ymax = std::min(ymax, clip_px[3] * AA_V);
            rasterize_edges(edge_buf, ymin, ymax, fr, fg, fb, !rp.even_odd, fa,
                            clip_px[0], clip_px[2]);
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
                    double len = std::hypot(dx, dy);
                    if (len < 0.01) continue;
                    double nx = -dy / len * half;
                    double ny = dx / len * half;
                    double qx[4] = {sx0+nx, sx1+nx, sx1-nx, sx0-nx};
                    double qy[4] = {sy0+ny, sy1+ny, sy1-ny, sy0-ny};
                    for (int e = 0; e < 4; e++) {
                        int e2 = (e + 1) % 4;
                        double ey0 = qy[e], ey1 = qy[e2];
                        double ex0 = qx[e], ex1 = qx[e2];
                        append_scan_edge(ex0, ey0, ex1, ey1, ymin, ymax);
                    }
                }
            }
            uint8_t sr = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.stroke_r * 255)));
            uint8_t sg = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.stroke_g * 255)));
            uint8_t sb = static_cast<uint8_t>(std::min(255.0, std::max(0.0, rp.stroke_b * 255)));
            int sa = static_cast<int>(std::min(1.0, std::max(0.0, rp.stroke_alpha)) * 255 + 0.5);
            ymin = std::max(ymin, clip_px[1] * AA_V);
            ymax = std::min(ymax, clip_px[3] * AA_V);
            rasterize_edges(edge_buf, ymin, ymax, sr, sg, sb, true, sa,
                            clip_px[0], clip_px[2]);
        }
    };

    const PdfObj& res = resources;

    int images_drawn = 0;
    auto draw_image = [&](const ImagePlacement& ip) {
        PdfObj xobj_res;
        if (ip.inline_img) {
            // Inline image: the synthesized stream carries dict and payload.
        } else if (ip.xobj_ref >= 0) {
            xobj_res = doc.get_obj(ip.xobj_ref);
        } else if (!ip.xobj_name.empty()) {
            auto& xobjects = res.get("XObject");
            auto xd = doc.resolve(xobjects);
            if (xd.is_dict()) xobj_res = doc.resolve(xd.get(ip.xobj_name));
        }
        const PdfObj& xobj = ip.inline_img ? *ip.inline_img : xobj_res;
        if (!xobj.is_stream()) return;

        auto& subtype = xobj.get("Subtype");
        if (subtype.is_name() && subtype.str_val != "Image") return;

        if (diag) diag->images_total++;

        int w = xobj.get("Width").as_int();
        int h = xobj.get("Height").as_int();
        if (w <= 0 || h <= 0) {
            if (diag) diag->images_failed++;
            return;
        }

        // Spec-clipped away entirely: not a failure, and no decode needed.
        int clip_px[4];
        if (!canvas_clip(ip.clip, clip_px)) return;

        // Check if this is an ImageMask (1-bit stencil)
        bool is_image_mask = xobj.get("ImageMask").bool_val;

        // Decode image to RGB pixels for compositing
        std::vector<uint8_t> pixels;
        int components = 3;
        // Separation/DeviceN samples are ink tints, not luminance; they map
        // through the same tint→RGB ramp the fill colors use.
        std::shared_ptr<const CsInfo> tint_cs;
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

                // Get the CCITT payload. decode_stream decrypts and applies
                // any preceding filters (Flate/LZW/ASCII), leaving CCITT raw
                // for the caller; the plain raw path stays zero-copy.
                const uint8_t* src = xobj.raw_stream_data();
                size_t src_len = xobj.raw_stream_size();
                std::vector<uint8_t> pre_decoded;
                if (doc.crypt.active ||
                    (filter_obj.is_arr() && filter_obj.arr.size() > 1)) {
                    pre_decoded = doc.decode_stream(xobj);
                    src = pre_decoded.data();
                    src_len = pre_decoded.size();
                }
                if (!src || src_len == 0) {
                    if (diag) diag->images_failed++;
                    return;
                }

                auto ccitt_data = decode_ccitt(src, src_len, k, cols, black_is_1);
                int row_bytes = (cols + 7) / 8;
                int rows = ccitt_data.empty() ? 0 : (int)ccitt_data.size() / row_bytes;
                if (rows <= 0) {
                    if (diag) { diag->images_failed++; diag->unsupported_filter++; }
                    return;
                }

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
                if (decoded.empty() && last_filter == "JBIG2Decode") {
                    // Variant outside the in-tree subset (MMR, symbol dicts…).
                    if (diag) { diag->images_failed++; diag->unsupported_filter++; }
                    return;
                }

                if (last_filter == "JPXDecode") {
                    auto jr = jpx_decode(decoded.data(), decoded.size());
                    if (jr.pixels.empty()) {
                        if (diag) { diag->images_failed++; diag->unsupported_filter++; }
                        return;
                    }
                    pixels = std::move(jr.pixels);
                    w = jr.width;
                    h = jr.height;
                    components = jr.components;
                } else
                // Check if result is JPEG (decode_stream leaves DCTDecode raw)
                if (decoded.size() >= 2 && decoded[0] == 0xFF && decoded[1] == 0xD8) {
                    auto jr = jpeg_decode(decoded.data(), decoded.size());
                    pixels = std::move(jr.pixels);
                    w = jr.width; h = jr.height; components = jr.components;
                    if (jr.inverted_cmyk)
                        for (auto& v : pixels) v = 255 - v;
                } else {
                    {
                        int bpc16 = xobj.get("BitsPerComponent").as_int();
                        if (bpc16 == 16 && !is_image_mask) fold_16bpc(decoded);
                    }
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
                        {
                            int bpc_i = xobj.get("BitsPerComponent").as_int();
                            if (bpc_i != 8)
                                unpack_subbyte_indices(decoded, w, h, bpc_i);
                        }
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
                        auto csi = load_colorspace(doc, cs_obj);
                        if (csi && csi->kind == CsInfo::TINT &&
                            !csi->lut.empty())
                            tint_cs = csi;
                    } else if (cs_name == "DeviceN") {
                        if (cs_obj.is_arr() && cs_obj.arr.size() >= 2) {
                            auto names_arr = doc.resolve(cs_obj.arr[1]);
                            if (names_arr.is_arr())
                                components =
                                    static_cast<int>(names_arr.arr.size());
                        }
                        // N-in tint transforms don't fit a 1-D ramp; N>=2
                        // reads through the alternate-space interpretation
                        // (4=CMYK, 3=RGB) below.
                        if (components == 1) {
                            auto csi = load_colorspace(doc, cs_obj);
                            if (csi && csi->kind == CsInfo::TINT &&
                                !csi->lut.empty())
                                tint_cs = csi;
                        }
                    } else if (cs_name == "Lab") {
                        components = 3;
                        double range[4];
                        lab_range(doc, cs_obj, range);
                        lab_pixels_to_srgb(decoded, decoded.size() / 3,
                                           range[0], range[1],
                                           range[2], range[3]);
                    }
                    if (is_image_mask) components = 1;
                    // Unpack 1-bit rows to bytes; CCITT masks arrive unpacked
                    // above, but Flate-compressed bitonal layers (stamps,
                    // signature masks) reach here still packed and previously
                    // fell out on the size check below, vanishing from the
                    // composite. ImageMask default /Decode [0 1]: sample 0
                    // paints; plain gray: bit 1 is white.
                    int bpc1 = xobj.get("BitsPerComponent").as_int();
                    if (bpc1 == 1 && components == 1 &&
                        cs_name != "Indexed" && cs_name != "I") {
                        // ImageMask default /Decode [0 1]: sample 0 paints;
                        // [1 0] flips the bit sense.
                        bool mask_flip = false;
                        if (is_image_mask) {
                            auto mdec = doc.resolve(xobj.get("Decode"));
                            mask_flip = mdec.is_arr() && mdec.arr.size() >= 2 &&
                                        mdec.arr[0].as_num() >= 0.5;
                        }
                        // JBIG2 gray (not a mask): decoded 1 = black ink
                        bool ink_is_one = (last_filter == "JBIG2Decode");
                        size_t row_bytes = (static_cast<size_t>(w) + 7) / 8;
                        if (decoded.size() >= row_bytes * h) {
                            std::vector<uint8_t> unpacked(static_cast<size_t>(w) * h);
                            for (int uy = 0; uy < h; uy++)
                                for (int ux = 0; ux < w; ux++) {
                                    bool bit = (decoded[uy * row_bytes + ux / 8]
                                                >> (7 - (ux & 7))) & 1;
                                    uint8_t v;
                                    if (is_image_mask) v = (bit != mask_flip) ? 0 : 255;
                                    else if (ink_is_one) v = bit ? 0 : 255;
                                    else v = bit ? 255 : 0;
                                    unpacked[static_cast<size_t>(uy) * w + ux] = v;
                                }
                            decoded = std::move(unpacked);
                        }
                    }
                    pixels = std::move(decoded);
                }
            }
        }
        {
            auto cs_probe = doc.resolve(xobj.get("ColorSpace"));
            std::string csn;
            if (cs_probe.is_name()) csn = cs_probe.str_val;
            else if (cs_probe.is_arr() && !cs_probe.arr.empty()) {
                auto first = doc.resolve(cs_probe.arr[0]);
                if (first.is_name()) csn = first.str_val;
            }
            if (!is_image_mask && csn != "Indexed" && csn != "I")
                apply_decode_array(doc, xobj, pixels, components);
        }

        size_t expected = static_cast<size_t>(w) * h * components;
        if (pixels.size() < expected) {
            if (diag) { diag->images_failed++; diag->decode_size_mismatch++; }
            return;
        }
        images_drawn++;

        int ip_alpha = static_cast<int>(
            std::min(1.0, std::max(0.0, ip.alpha)) * 255 + 0.5);
        if (is_image_mask && components == 1) {
            // Composite mask coverage using the current fill color.
            uint8_t fr = static_cast<uint8_t>(std::min(255.0, std::max(0.0, ip.fill_r * 255)));
            uint8_t fg = static_cast<uint8_t>(std::min(255.0, std::max(0.0, ip.fill_g * 255)));
            uint8_t fb = static_cast<uint8_t>(std::min(255.0, std::max(0.0, ip.fill_b * 255)));
            // Rotated placement (e.g. a /Rotate page normalized into viewing
            // coordinates): the axis-aligned coverage loop below would place
            // the mask wrong, so inverse-map each destination pixel instead.
            if (std::abs(ip.ctm[1]) >= 0.001 || std::abs(ip.ctm[2]) >= 0.001) {
                InverseImageMap image_map;
                if (!make_inverse_image_map(ip.ctm, page_h, scale,
                                            canvas.width, canvas.height,
                                            image_map))
                    return;
                image_map.x.begin = std::max(image_map.x.begin, clip_px[0]);
                image_map.x.end = std::min(image_map.x.end, clip_px[2]);
                image_map.y.begin = std::max(image_map.y.begin, clip_px[1]);
                image_map.y.end = std::min(image_map.y.end, clip_px[3]);
                for (int canvas_y = image_map.y.begin;
                     canvas_y < image_map.y.end; canvas_y++) {
                    for (int canvas_x = image_map.x.begin;
                         canvas_x < image_map.x.end; canvas_x++) {
                        double page_x = canvas_x / scale;
                        double page_y = page_h - canvas_y / scale;
                        double local_x = page_x - ip.ctm[4];
                        double local_y = page_y - ip.ctm[5];
                        double image_x =
                            (ip.ctm[3] * local_x - ip.ctm[2] * local_y) /
                            image_map.determinant;
                        double image_y =
                            (-ip.ctm[1] * local_x + ip.ctm[0] * local_y) /
                            image_map.determinant;
                        if (!std::isfinite(image_x) ||
                            !std::isfinite(image_y) || image_x < 0 ||
                            image_x > 1 || image_y < 0 || image_y > 1)
                            continue;
                        // Fractional coverage instead of a >128 threshold:
                        // rotated stencil edges anti-alias like the
                        // axis-aligned branch's area sampling.
                        double fx = image_x * (w - 1);
                        double fy = (1 - image_y) * (h - 1);
                        int sx0 = static_cast<int>(fx);
                        int sy0 = static_cast<int>(fy);
                        int sx1 = std::min(sx0 + 1, w - 1);
                        int sy1 = std::min(sy0 + 1, h - 1);
                        double tx = fx - sx0, ty = fy - sy0;
                        double cov =
                            (1 - tx) * (1 - ty) *
                                pixels[static_cast<size_t>(sy0) * w + sx0] +
                            tx * (1 - ty) *
                                pixels[static_cast<size_t>(sy0) * w + sx1] +
                            (1 - tx) * ty *
                                pixels[static_cast<size_t>(sy1) * w + sx0] +
                            tx * ty *
                                pixels[static_cast<size_t>(sy1) * w + sx1];
                        int a = static_cast<int>(cov / 255.0 * ip_alpha + 0.5);
                        if (a > 0)
                            canvas.blend_pixel(canvas_x, canvas_y, fr, fg, fb,
                                               static_cast<uint8_t>(a));
                    }
                }
                return;
            }
            // Blit with alpha — use Canvas blit for proper CTM handling
            BlitAxis x_axis, y_axis;
            if (!clip_axis_aligned_image(ip.ctm, page_h, scale, canvas.width,
                                         canvas.height, x_axis, y_axis))
                return;
            x_axis.begin = std::max(x_axis.begin, clip_px[0]);
            x_axis.end = std::min(x_axis.end, clip_px[2]);
            y_axis.begin = std::max(y_axis.begin, clip_px[1]);
            y_axis.end = std::min(y_axis.end, clip_px[3]);
            if (x_axis.begin >= x_axis.end || y_axis.begin >= y_axis.end)
                return;
            // Area sampling for ImageMask: compute coverage ratio in source region
            for (int dy = y_axis.begin; dy < y_axis.end; dy++) {
                auto [sy0, sy1] = y_axis.source_interval(dy, h);
                for (int dx = x_axis.begin; dx < x_axis.end; dx++) {
                    auto [sx0, sx1] = x_axis.source_interval(dx, w);
                    // Count set pixels in source region
                    uint64_t total = static_cast<uint64_t>(sy1 - sy0) *
                                     static_cast<uint64_t>(sx1 - sx0);
                    if (total <= 0) continue;
                    uint64_t set = 0;
                    for (int ry = sy0; ry < sy1; ry++)
                        for (int rx = sx0; rx < sx1; rx++)
                            if (pixels[ry * w + rx] > 128) set++;
                    if (set > 0) {
                        uint8_t a = static_cast<uint8_t>(
                            set * 255 / total * ip_alpha / 255);
                        canvas.blend_pixel(dx, dy, fr, fg, fb, a);
                    }
                }
            }
        } else {
            std::vector<uint8_t> smask;
            int smw = 0, smh = 0;
            if (!decode_smask(doc, xobj, smask, smw, smh))
                decode_stencil_mask(doc, xobj, smask, smw, smh);
            canvas.blit_image(pixels.data(), w, h, components, ip.ctm, page_h, scale,
                              smask.empty() ? nullptr : smask.data(), smw, smh,
                              ip_alpha, clip_px,
                              tint_cs ? tint_cs->lut.data() : nullptr);
        }
    };

    // Draw paths and images interleaved in content-stream order. Painting all
    // paths first buried them under later-composited opaque images: a
    // watermark background drawn below glyph outlines erased the whole body
    // of GDI print-to-PDF pages.
    {
        size_t pi = 0, ii = 0;
        while (pi < parse_result.paths.size() || ii < parse_result.images.size()) {
            bool take_path =
                ii >= parse_result.images.size() ||
                (pi < parse_result.paths.size() &&
                 parse_result.paths[pi].seq <= parse_result.images[ii].seq);
            if (take_path) draw_path(parse_result.paths[pi++]);
            else draw_image(parse_result.images[ii++]);
        }
    }

    // Every image placement failed to draw and no vector path was recorded:
    // the canvas is blank white, and shipping it would read as success.
    // Returning empty hands control back to the caller's extraction fallback.
    if (images_drawn == 0 && parse_result.paths.empty() &&
        !parse_result.images.empty())
        return {};

    ImageData img;
    img.page_number = page_num;
    img.name = "page" + std::to_string(page_num + 1) + "_img" +
               std::to_string(img_idx);
    img.format = "raw";
    img.width = rw;
    img.height = rh;
    img.components = 3;

    // Canvas rows are already in PNG layout — deflate them in place
    auto png = canvas.to_png(Z_BEST_SPEED);
    img.format = "png";
    img.data = std::move(png);

    if (!output_dir.empty()) {
        std::string path = output_dir + "/" + img.name + ".png";
        std::ofstream f(path, std::ios::binary);
        if (f) {
            f.write(img.data.data(), static_cast<std::streamsize>(img.data.size()));
            img.saved_path = path;
        }
        if (!img.saved_path.empty()) {
            discard_image_payload(img);
        }
    }
    return img;
}

ImageData render_region_composite(PdfDoc& doc, const PdfObj& resources,
                                  const ContentParseResult& parse_result,
                                  const std::vector<size_t>& members,
                                  int page_num, const double region[4],
                                  const std::string& output_dir,
                                  int img_idx,
                                  PageRenderDiag* diag) {
    const double x0 = region[0], y0 = region[1];
    const double rgn_w = region[2] - region[0];
    const double rgn_h = region[3] - region[1];
    if (!std::isfinite(rgn_w) || !std::isfinite(rgn_h) ||
        rgn_w < 1.0 || rgn_h < 1.0)
        return {};

    // Translate the members and every intersecting path into region-local
    // coordinates; with page dims set to the region size, the page compositor
    // renders exactly the cropped window (its canvas math is origin-based).
    ContentParseResult sub;
    sub.images.reserve(members.size());
    for (size_t idx : members) {
        if (idx >= parse_result.images.size()) continue;
        ImagePlacement ip = parse_result.images[idx];
        ip.ctm[4] -= x0;
        ip.ctm[5] -= y0;
        ip.clip[0] -= static_cast<float>(x0);
        ip.clip[2] -= static_cast<float>(x0);
        ip.clip[1] -= static_cast<float>(y0);
        ip.clip[3] -= static_cast<float>(y0);
        sub.images.push_back(std::move(ip));
    }
    if (sub.images.empty()) return {};

    for (const auto& rp : parse_result.paths) {
        double bx0, by0, bx1, by1;
        bx0 = by0 = 1e300;
        bx1 = by1 = -1e300;
        for (const auto& pt : rp.points) {
            if (pt.type == PathPoint::CLOSE) continue;
            bx0 = std::min(bx0, pt.x); bx1 = std::max(bx1, pt.x);
            by0 = std::min(by0, pt.y); by1 = std::max(by1, pt.y);
            if (pt.type == PathPoint::CURVE) {
                bx0 = std::min({bx0, pt.cx1, pt.cx2});
                bx1 = std::max({bx1, pt.cx1, pt.cx2});
                by0 = std::min({by0, pt.cy1, pt.cy2});
                by1 = std::max({by1, pt.cy1, pt.cy2});
            }
        }
        if (bx0 > bx1) continue;
        double pad = rp.do_stroke ? rp.line_width * 0.5 : 0.0;
        if (bx1 + pad < region[0] || bx0 - pad > region[2] ||
            by1 + pad < region[1] || by0 - pad > region[3])
            continue;
        RenderPath sp = rp;
        for (auto& pt : sp.points) {
            pt.x -= x0; pt.y -= y0;
            pt.cx1 -= x0; pt.cy1 -= y0;
            pt.cx2 -= x0; pt.cy2 -= y0;
        }
        sp.clip[0] -= static_cast<float>(x0);
        sp.clip[2] -= static_cast<float>(x0);
        sp.clip[1] -= static_cast<float>(y0);
        sp.clip[3] -= static_cast<float>(y0);
        sub.paths.push_back(std::move(sp));
    }

    return render_page_composite(doc, resources, sub, page_num, rgn_w, rgn_h,
                                 output_dir, img_idx, diag);
}

// ── Bookmark Extraction ──────────────────────────────────


}} // namespace jdoc::pdf_detail
