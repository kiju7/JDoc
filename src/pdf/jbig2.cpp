// jbig2.cpp — embedded JBIG2 (ITU-T T.88), generic-region subset.
//
// PDF scanners and form printers (ePapyrus among them) emit stamps and
// stencil masks as a page-information segment plus one arithmetic-coded
// immediate generic region. That subset — MQ coding, templates 0..3, TPGDON —
// is implemented here; symbol dictionaries, MMR, refinement and halftone
// regions are not, and any stream using them decodes to empty so callers keep
// their skip behavior.
#include "jbig2.h"
#include "mq_decoder.h"

#include <algorithm>
#include <cstring>

namespace jdoc { namespace pdf_detail {

namespace {

struct ByteReader {
    const uint8_t* p;
    size_t len;
    size_t pos = 0;

    bool ok(size_t n) const { return pos + n <= len; }
    uint8_t u8() { return pos < len ? p[pos++] : 0; }
    uint32_t u16() { uint32_t v = u8(); return (v << 8) | u8(); }
    uint32_t u32() { uint32_t v = u16(); return (v << 16) | u16(); }
};

struct Bitmap {
    int w = 0, h = 0;
    size_t stride = 0;
    std::vector<uint8_t> rows;

    void init(int width, int height, int fill_bit) {
        w = width;
        h = height;
        stride = (static_cast<size_t>(w) + 7) / 8;
        rows.assign(stride * h, fill_bit ? 0xFF : 0x00);
    }
    int get(int x, int y) const {
        if (static_cast<unsigned>(x) >= static_cast<unsigned>(w) ||
            static_cast<unsigned>(y) >= static_cast<unsigned>(h))
            return 0;
        return (rows[static_cast<size_t>(y) * stride + (x >> 3)] >> (7 - (x & 7))) & 1;
    }
    void set(int x, int y, int v) {
        uint8_t& b = rows[static_cast<size_t>(y) * stride + (x >> 3)];
        uint8_t m = static_cast<uint8_t>(1u << (7 - (x & 7)));
        if (v) b |= m; else b &= static_cast<uint8_t>(~m);
    }
};

struct TplPixel { int x, y; };

// Fixed context pixels per generic template (T.88 6.2.5.3); the adaptive
// pixels are appended and the whole set sorted in raster order, which equals
// the spec's context layout for the nominal AT positions every known PDF
// producer uses.
static const TplPixel kTpl0[] = {{-1,-2},{0,-2},{1,-2},{-2,-1},{-1,-1},{0,-1},{1,-1},{2,-1},{-4,0},{-3,0},{-2,0},{-1,0}};
static const TplPixel kTpl1[] = {{-1,-2},{0,-2},{1,-2},{2,-2},{-2,-1},{-1,-1},{0,-1},{1,-1},{2,-1},{-3,0},{-2,0},{-1,0}};
static const TplPixel kTpl2[] = {{-1,-2},{0,-2},{1,-2},{-2,-1},{-1,-1},{0,-1},{1,-1},{-2,0},{-1,0}};
static const TplPixel kTpl3[] = {{-3,-1},{-2,-1},{-1,-1},{0,-1},{1,-1},{-4,0},{-3,0},{-2,0},{-1,0}};

// Pseudo-pixel context for the TPGDON typical-prediction bit, per template.
static const int kTpgdonCx[4] = {0x9B25, 0x0795, 0x00E5, 0x0195};

static bool decode_generic_region(Bitmap& bmp, int tmpl, const TplPixel* at,
                                  int at_count, bool tpgdon,
                                  const uint8_t* data, size_t len) {
    std::vector<TplPixel> tpl;
    switch (tmpl) {
        case 0: tpl.assign(kTpl0, kTpl0 + 12); break;
        case 1: tpl.assign(kTpl1, kTpl1 + 12); break;
        case 2: tpl.assign(kTpl2, kTpl2 + 9); break;
        default: tpl.assign(kTpl3, kTpl3 + 9); break;
    }
    for (int i = 0; i < at_count; i++) tpl.push_back(at[i]);
    std::stable_sort(tpl.begin(), tpl.end(), [](const TplPixel& a, const TplPixel& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    const int nbits = static_cast<int>(tpl.size());
    if (nbits > 16) return false;

    std::vector<uint8_t> cx(1u << nbits, 0);
    MQDecoder mq(data, len);
    int ltp = 0;

    for (int y = 0; y < bmp.h; y++) {
        if (tpgdon) {
            ltp ^= mq.decode(&cx[kTpgdonCx[tmpl]]);
            if (ltp) {
                // Typical row: identical to the one above (row 0: all zero)
                if (y > 0)
                    std::memcpy(&bmp.rows[static_cast<size_t>(y) * bmp.stride],
                                &bmp.rows[static_cast<size_t>(y - 1) * bmp.stride],
                                bmp.stride);
                continue;
            }
        }
        for (int x = 0; x < bmp.w; x++) {
            uint32_t ctx = 0;
            for (const auto& t : tpl)
                ctx = (ctx << 1) | static_cast<uint32_t>(bmp.get(x + t.x, y + t.y));
            if (mq.decode(&cx[ctx])) bmp.set(x, y, 1);
        }
    }
    return true;
}

struct SegmentHeader {
    uint32_t number = 0;
    int type = 0;
    uint32_t page = 0;
    uint32_t data_length = 0;
    bool valid = false;
};

static SegmentHeader read_segment_header(ByteReader& r) {
    SegmentHeader h;
    if (!r.ok(11)) return h;
    h.number = r.u32();
    uint8_t flags = r.u8();
    h.type = flags & 0x3F;
    bool page4 = (flags & 0x40) != 0;

    uint8_t rts = r.u8();
    uint32_t ref_count = rts >> 5;
    if (ref_count == 7) {
        // Long form: 29-bit count, then retain bits
        r.pos--;
        ref_count = r.u32() & 0x1FFFFFFF;
        r.pos += (ref_count + 8) / 8;
    }
    // Referred-to segment numbers, sized by this segment's number
    size_t ref_size = h.number <= 256 ? 1 : (h.number <= 65536 ? 2 : 4);
    r.pos += ref_count * ref_size;

    h.page = page4 ? r.u32() : r.u8();
    h.data_length = r.u32();
    h.valid = r.pos <= r.len;
    return h;
}

} // namespace

std::vector<uint8_t> jbig2_decode(const uint8_t* data, size_t len,
                                  const uint8_t* globals, size_t globals_len,
                                  int& w, int& h) {
    Bitmap page;
    int page_default = 0;
    bool page_ready = false;

    auto process = [&](const uint8_t* p, size_t n) -> bool {
        ByteReader r{p, n};
        while (r.pos < r.len) {
            SegmentHeader sh = read_segment_header(r);
            if (!sh.valid || sh.data_length == 0xFFFFFFFF) return false;
            size_t body = r.pos;
            size_t body_end = body + sh.data_length;
            if (body_end > r.len) return false;
            r.pos = body_end;

            switch (sh.type) {
                case 48: { // page information
                    ByteReader b{p + body, sh.data_length};
                    uint32_t pw = b.u32();
                    uint32_t ph = b.u32();
                    b.u32(); // x resolution
                    b.u32(); // y resolution
                    uint8_t flags = b.u8();
                    page_default = (flags >> 2) & 1;
                    if (pw == 0 || pw > 1u << 20) return false;
                    if (ph == 0xFFFFFFFF || ph == 0)
                        ph = 0; // striped page: grown by region extents below
                    if (ph > 1u << 20) return false;
                    page.init(static_cast<int>(pw), static_cast<int>(ph), page_default);
                    page_ready = true;
                    break;
                }
                case 36:   // intermediate generic region
                case 38:   // immediate generic region
                case 39: { // immediate lossless generic region
                    ByteReader b{p + body, sh.data_length};
                    if (!b.ok(18)) return false;
                    uint32_t rw = b.u32();
                    uint32_t rh = b.u32();
                    uint32_t rx = b.u32();
                    uint32_t ry = b.u32();
                    uint8_t comb_op = b.u8() & 7;
                    uint8_t gflags = b.u8();
                    if (gflags & 1) return false; // MMR unsupported
                    int tmpl = (gflags >> 1) & 3;
                    bool tpgdon = (gflags >> 3) & 1;
                    int at_count = tmpl == 0 ? 4 : 1;
                    TplPixel at[4];
                    for (int i = 0; i < at_count; i++) {
                        at[i].x = static_cast<int8_t>(b.u8());
                        at[i].y = static_cast<int8_t>(b.u8());
                    }
                    if (!b.ok(0) || b.pos > b.len) return false;
                    if (rw == 0 || rh == 0 || rw > 1u << 20 || rh > 1u << 20 ||
                        static_cast<uint64_t>(rw) * rh > 64ull << 20)
                        return false;

                    Bitmap region;
                    region.init(static_cast<int>(rw), static_cast<int>(rh), 0);
                    if (!decode_generic_region(region, tmpl, at, at_count, tpgdon,
                                               p + body + b.pos,
                                               sh.data_length - b.pos))
                        return false;

                    if (!page_ready) {
                        // Degenerate stream without page info: adopt the region
                        page.init(static_cast<int>(rw + rx), static_cast<int>(rh + ry), 0);
                        page_ready = true;
                    }
                    if (static_cast<int>(ry + rh) > page.h) {
                        // Striped/unknown-height page grows to fit each region
                        Bitmap grown;
                        grown.init(page.w, static_cast<int>(ry + rh), page_default);
                        std::memcpy(grown.rows.data(), page.rows.data(), page.rows.size());
                        page = std::move(grown);
                    }
                    for (uint32_t yy = 0; yy < rh; yy++) {
                        for (uint32_t xx = 0; xx < rw; xx++) {
                            int px = static_cast<int>(rx + xx);
                            int py = static_cast<int>(ry + yy);
                            if (px >= page.w || py >= page.h) continue;
                            int s = region.get(static_cast<int>(xx), static_cast<int>(yy));
                            int d = page.get(px, py);
                            int v;
                            switch (comb_op) {
                                case 1: v = d & s; break;  // AND
                                case 2: v = d ^ s; break;  // XOR
                                case 3: v = 1 - (d ^ s); break; // XNOR
                                case 4: v = s; break;      // REPLACE
                                default: v = d | s; break; // OR
                            }
                            page.set(px, py, v);
                        }
                    }
                    break;
                }
                case 49: case 50: case 51: case 62:
                    // end of page / stripe / file, extension: nothing to do
                    break;
                default:
                    // Symbol dictionaries, text/halftone/refinement regions
                    return false;
            }
        }
        return true;
    };

    if (globals && globals_len > 0 && !process(globals, globals_len)) return {};
    if (!process(data, len)) return {};
    if (!page_ready || page.h == 0) return {};

    w = page.w;
    h = page.h;
    return std::move(page.rows);
}

}} // namespace jdoc::pdf_detail
