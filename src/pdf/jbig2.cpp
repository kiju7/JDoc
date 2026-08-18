// jbig2.cpp — embedded JBIG2 (ITU-T T.88), arithmetic subset.
//
// PDF scanners and form printers (ePapyrus among them) emit stamps and
// stencil masks as a page-information segment plus one arithmetic-coded
// immediate generic region; copier "compact PDF" modes add symbol
// dictionaries and text regions for the glyph layer. The arithmetic paths —
// MQ coding, generic templates 0..3 with TPGDON, symbol dictionaries and
// text regions — are implemented here; Huffman tables, MMR, refinement and
// halftone regions are not, and any stream using them decodes to empty so
// callers keep their skip behavior.
#include "jbig2.h"
#include "mq_decoder.h"
#include "pdf_limits.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

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

    bool init(int width, int height, int fill_bit) {
        if (width <= 0 || height < 0 ||
            static_cast<uint64_t>(width) * static_cast<uint64_t>(height) >
                limits::kMaxDecodedPixels)
            return false;
        w = width;
        h = height;
        stride = (static_cast<size_t>(w) + 7) / 8;
        rows.assign(stride * h, fill_bit ? 0xFF : 0x00);
        return true;
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

// Decode one generic-region bitmap using caller-owned arithmetic state.
// Symbol dictionaries decode every symbol of a segment through one shared
// MQ decoder and one shared context array, so both must live outside.
static bool decode_generic_bitmap(Bitmap& bmp, int tmpl, const TplPixel* at,
                                  int at_count, bool tpgdon, MQDecoder& mq,
                                  std::vector<uint8_t>& cx) {
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
    if (cx.size() < (1u << nbits)) cx.assign(1u << nbits, 0);

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

static bool decode_generic_region(Bitmap& bmp, int tmpl, const TplPixel* at,
                                  int at_count, bool tpgdon,
                                  const uint8_t* data, size_t len) {
    MQDecoder mq(data, len);
    std::vector<uint8_t> cx;
    return decode_generic_bitmap(bmp, tmpl, at, at_count, tpgdon, mq, cx);
}

// ── Integer arithmetic decoding (T.88 Annex A) ───────────

// One IAx statistics context (512 slots). decode() returns false on OOB.
struct ArithIntCtx {
    uint8_t cx[512] = {0};

    bool decode(MQDecoder& mq, int32_t& out) {
        uint32_t prev = 1;
        auto bit = [&]() {
            int b = mq.decode(&cx[prev]);
            prev = prev < 256
                       ? (prev << 1) | static_cast<uint32_t>(b)
                       : ((((prev << 1) | static_cast<uint32_t>(b)) & 511) |
                          256);
            return b;
        };
        auto bits = [&](int n) {
            int64_t v = 0;
            for (int i = 0; i < n; i++) v = (v << 1) | bit();
            return v;
        };
        int s = bit();
        int64_t v;
        if (!bit()) v = bits(2);
        else if (!bit()) v = bits(4) + 4;
        else if (!bit()) v = bits(6) + 20;
        else if (!bit()) v = bits(8) + 84;
        else if (!bit()) v = bits(12) + 340;
        else v = bits(32) + 4436;
        if (s && v == 0) return false; // OOB
        if (v > 0x7FFFFFFF) v = 0x7FFFFFFF;
        out = static_cast<int32_t>(s ? -v : v);
        return true;
    }
};

// Symbol-ID decoding (T.88 A.3): a code tree of SBSYMCODELEN bits.
struct ArithIidCtx {
    std::vector<uint8_t> cx;
    int codelen = 0;

    explicit ArithIidCtx(int len)
        : cx(static_cast<size_t>(1) << (len + 1), 0), codelen(len) {}

    uint32_t decode(MQDecoder& mq) {
        uint32_t prev = 1;
        for (int i = 0; i < codelen; i++)
            prev = (prev << 1) |
                   static_cast<uint32_t>(mq.decode(&cx[prev]));
        return prev - (1u << codelen);
    }
};

static int ceil_log2(uint32_t n) {
    int bits = 0;
    while ((1u << bits) < n) bits++;
    return bits;
}

struct SegmentHeader {
    uint32_t number = 0;
    int type = 0;
    uint32_t page = 0;
    uint32_t data_length = 0;
    std::vector<uint32_t> referred; // symbol dicts a text region draws from
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
    if (ref_count <= 1u << 16) {
        h.referred.reserve(ref_count);
        for (uint32_t i = 0; i < ref_count && r.ok(ref_size); i++) {
            uint32_t v = 0;
            for (size_t bnum = 0; bnum < ref_size; bnum++) v = (v << 8) | r.u8();
            h.referred.push_back(v);
        }
    } else {
        r.pos += ref_count * ref_size;
    }

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
    // Exported symbols per symbol-dictionary segment number; text regions
    // draw from the dictionaries their header refers to. Dictionaries in
    // the /JBIG2Globals stream persist into the per-image stream.
    std::map<uint32_t, std::vector<Bitmap>> sym_dicts;
    uint64_t sym_area_total = 0; // budget across all dictionaries

    // Combine a decoded region onto the page at (rx, ry), growing a striped
    // (unknown-height) page as regions arrive.
    auto compose_region = [&](const Bitmap& region, uint32_t rx, uint32_t ry,
                              uint8_t comb_op) -> bool {
        const uint32_t rw = static_cast<uint32_t>(region.w);
        const uint32_t rh = static_cast<uint32_t>(region.h);
        if (!page_ready) {
            // Degenerate stream without page info: adopt the region
            if (static_cast<uint64_t>(rw + rx) * (rh + ry) > 64ull << 20)
                return false;
            if (!page.init(static_cast<int>(rw + rx),
                           static_cast<int>(rh + ry), 0))
                return false;
            page_ready = true;
        }
        if (static_cast<int>(ry + rh) > page.h) {
            // Striped/unknown-height page grows to fit each region;
            // the grown area gets the same 64 Mpx budget as regions.
            if (static_cast<uint64_t>(page.w) * (ry + rh) > 64ull << 20)
                return false;
            Bitmap grown;
            if (!grown.init(page.w, static_cast<int>(ry + rh), page_default))
                return false;
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
        return true;
    };

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
                    if (!page.init(static_cast<int>(pw), static_cast<int>(ph),
                                   page_default))
                        return false;
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
                    // Offsets get the same range cap: a wild rx/ry would wrap
                    // the int casts below negative, sailing past the signed
                    // px/py guards into unchecked Bitmap::set writes.
                    if (rx > 1u << 20 || ry > 1u << 20) return false;

                    Bitmap region;
                    if (!region.init(static_cast<int>(rw), static_cast<int>(rh), 0))
                        return false;
                    if (!decode_generic_region(region, tmpl, at, at_count, tpgdon,
                                               p + body + b.pos,
                                               sh.data_length - b.pos))
                        return false;

                    if (!compose_region(region, rx, ry, comb_op)) return false;
                    break;
                }
                case 0: { // symbol dictionary (T.88 6.5, arithmetic only)
                    ByteReader b{p + body, sh.data_length};
                    if (!b.ok(2)) return false;
                    uint32_t flags = b.u16();
                    bool sdhuff = flags & 1;
                    bool sdrefagg = (flags >> 1) & 1;
                    int tmpl = (flags >> 10) & 3;
                    if (sdhuff || sdrefagg) return false; // Huffman/refinement
                    int at_count = tmpl == 0 ? 4 : 1;
                    TplPixel at[4];
                    for (int i = 0; i < at_count; i++) {
                        at[i].x = static_cast<int8_t>(b.u8());
                        at[i].y = static_cast<int8_t>(b.u8());
                    }
                    if (!b.ok(8)) return false;
                    uint32_t num_ex = b.u32();
                    uint32_t num_new = b.u32();
                    if (num_new > 1u << 16 || num_ex > 1u << 16) return false;

                    // Input symbols imported from referred dictionaries.
                    std::vector<const Bitmap*> input;
                    for (uint32_t rn : sh.referred) {
                        auto it = sym_dicts.find(rn);
                        if (it == sym_dicts.end()) continue;
                        for (auto& s : it->second) input.push_back(&s);
                    }
                    const uint32_t num_in =
                        static_cast<uint32_t>(input.size());
                    if (num_ex > num_in + num_new) return false;

                    // One decoder and one shared bitmap context span the
                    // whole segment (6.5.5).
                    MQDecoder mq(p + body + b.pos, sh.data_length - b.pos);
                    std::vector<uint8_t> gb_cx;
                    ArithIntCtx iadh, iadw, iaex, iaai;
                    std::vector<Bitmap> newsyms;
                    newsyms.reserve(num_new);
                    int32_t hcheight = 0;
                    while (newsyms.size() < num_new) {
                        int32_t dh;
                        if (!iadh.decode(mq, dh)) return false;
                        hcheight += dh;
                        if (hcheight <= 0 || hcheight > 1 << 14) return false;
                        int32_t symwidth = 0;
                        while (true) {
                            int32_t dw;
                            if (!iadw.decode(mq, dw)) break; // OOB: class end
                            symwidth += dw;
                            if (symwidth <= 0 || symwidth > 1 << 14)
                                return false;
                            if (newsyms.size() >= num_new) return false;
                            sym_area_total +=
                                static_cast<uint64_t>(symwidth) * hcheight;
                            if (sym_area_total > 64ull << 20) return false;
                            Bitmap sym;
                            if (!sym.init(symwidth, hcheight, 0)) return false;
                            if (!decode_generic_bitmap(sym, tmpl, at, at_count,
                                                       false, mq, gb_cx))
                                return false;
                            newsyms.push_back(std::move(sym));
                        }
                    }

                    // Export flags: alternating skip/export run lengths over
                    // input followed by new symbols (6.5.10).
                    std::vector<Bitmap> exported;
                    exported.reserve(num_ex);
                    uint32_t idx = 0, total = num_in + num_new;
                    bool exflag = false;
                    while (idx < total && exported.size() < num_ex) {
                        int32_t run;
                        if (!iaex.decode(mq, run) || run < 0) return false;
                        if (exflag) {
                            for (int32_t k = 0;
                                 k < run && idx < total &&
                                 exported.size() < num_ex;
                                 k++, idx++) {
                                exported.push_back(
                                    idx < num_in
                                        ? *input[idx]
                                        : newsyms[idx - num_in]);
                            }
                        } else {
                            idx += static_cast<uint32_t>(run);
                        }
                        exflag = !exflag;
                    }
                    sym_dicts[sh.number] = std::move(exported);
                    break;
                }
                case 4:   // intermediate text region
                case 6:   // immediate text region
                case 7: { // immediate lossless text region
                    ByteReader b{p + body, sh.data_length};
                    if (!b.ok(17 + 2 + 4)) return false;
                    uint32_t rw = b.u32();
                    uint32_t rh = b.u32();
                    uint32_t rx = b.u32();
                    uint32_t ry = b.u32();
                    uint8_t comb_op = b.u8() & 7;
                    uint32_t tflags = b.u16();
                    bool sbhuff = tflags & 1;
                    bool refine = (tflags >> 1) & 1;
                    int logstrips = (tflags >> 2) & 3;
                    int refcorner = (tflags >> 4) & 3;
                    bool transposed = (tflags >> 6) & 1;
                    uint8_t sb_comb = (tflags >> 7) & 3;
                    int defpixel = (tflags >> 9) & 1;
                    int dsoffset = (tflags >> 10) & 0x1F;
                    if (dsoffset > 15) dsoffset -= 32;
                    if (sbhuff || refine) return false; // outside the subset
                    uint32_t num_inst = b.u32();
                    if (rw == 0 || rh == 0 || rw > 1u << 20 || rh > 1u << 20 ||
                        static_cast<uint64_t>(rw) * rh > 64ull << 20)
                        return false;
                    if (rx > 1u << 20 || ry > 1u << 20) return false;
                    if (num_inst > 1u << 20) return false;

                    std::vector<const Bitmap*> syms;
                    for (uint32_t rn : sh.referred) {
                        auto it = sym_dicts.find(rn);
                        if (it == sym_dicts.end()) continue;
                        for (auto& s : it->second) syms.push_back(&s);
                    }
                    if (syms.empty()) return false;
                    const int codelen =
                        ceil_log2(static_cast<uint32_t>(syms.size()));

                    Bitmap region;
                    if (!region.init(static_cast<int>(rw),
                                     static_cast<int>(rh), defpixel))
                        return false;

                    const int strips = 1 << logstrips;
                    MQDecoder mq(p + body + b.pos, sh.data_length - b.pos);
                    ArithIntCtx iadt, iafs, iads, iait;
                    ArithIidCtx iaid(codelen);

                    auto draw_symbol = [&](const Bitmap& sym, int32_t sx,
                                           int32_t sy) {
                        for (int yy = 0; yy < sym.h; yy++) {
                            int py = sy + yy;
                            if (py < 0 || py >= region.h) continue;
                            for (int xx = 0; xx < sym.w; xx++) {
                                int px = sx + xx;
                                if (px < 0 || px >= region.w) continue;
                                int s = sym.get(xx, yy);
                                int d = region.get(px, py);
                                int v;
                                switch (sb_comb) {
                                    case 1: v = d & s; break;
                                    case 2: v = d ^ s; break;
                                    case 3: v = 1 - (d ^ s); break;
                                    default: v = d | s; break;
                                }
                                region.set(px, py, v);
                            }
                        }
                    };

                    // 6.4.5: strip loop. Symbols place their left edge at
                    // CURS either way (right-corner pre-advance and left-
                    // corner post-advance meet at the same spot); the T edge
                    // depends on top vs bottom reference corners.
                    int32_t stript, tmp;
                    if (!iadt.decode(mq, tmp)) return false;
                    stript = -tmp * strips;
                    int32_t firsts = 0;
                    uint32_t ninst = 0;
                    while (ninst < num_inst) {
                        if (!iadt.decode(mq, tmp)) return false;
                        stript += tmp * strips;
                        if (!iafs.decode(mq, tmp)) return false;
                        firsts += tmp;
                        int32_t curs = firsts;
                        bool first = true;
                        while (ninst < num_inst) {
                            if (!first) {
                                int32_t ids;
                                if (!iads.decode(mq, ids)) break; // strip end
                                curs += ids + dsoffset;
                            }
                            first = false;
                            int32_t curt = 0;
                            if (strips > 1) {
                                if (!iait.decode(mq, curt)) return false;
                            }
                            int32_t t = stript + curt;
                            uint32_t id = iaid.decode(mq);
                            if (id >= syms.size()) return false;
                            const Bitmap& sym = *syms[id];
                            if (transposed) {
                                // S runs down the page; right corners hang
                                // the symbol's width off T.
                                int32_t ox = (refcorner == 2 || refcorner == 3)
                                                 ? t - sym.w + 1 : t;
                                draw_symbol(sym, ox, curs);
                                curs += sym.h - 1;
                            } else {
                                int32_t oy = (refcorner == 0 || refcorner == 2)
                                                 ? t - sym.h + 1 : t;
                                draw_symbol(sym, curs, oy);
                                curs += sym.w - 1;
                            }
                            ninst++;
                        }
                    }

                    if (!compose_region(region, rx, ry, comb_op)) return false;
                    break;
                }
                case 49: case 50: case 51: case 62:
                    // end of page / stripe / file, extension: nothing to do
                    break;
                default:
                    // Halftone/refinement regions, pattern dictionaries
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

bool jbig2_supported(const uint8_t* data, size_t len,
                     const uint8_t* globals, size_t globals_len) {
    auto scan = [](const uint8_t* p, size_t n) -> bool {
        ByteReader r{p, n};
        while (r.pos < r.len) {
            SegmentHeader sh = read_segment_header(r);
            if (!sh.valid || sh.data_length == 0xFFFFFFFF) return false;
            size_t body = r.pos;
            if (body + sh.data_length > r.len) return false;
            r.pos = body + sh.data_length;
            switch (sh.type) {
                case 0: { // symbol dictionary: no Huffman, no refinement
                    ByteReader b{p + body, sh.data_length};
                    if (!b.ok(2)) return false;
                    uint32_t flags = b.u16();
                    if (flags & 3) return false;
                    break;
                }
                case 4: case 6: case 7: { // text region: same restrictions
                    ByteReader b{p + body, sh.data_length};
                    if (!b.ok(19)) return false;
                    b.pos = 17;
                    uint32_t tflags = b.u16();
                    if (tflags & 3) return false;
                    break;
                }
                case 36: case 38: case 39: { // generic region: no MMR
                    ByteReader b{p + body, sh.data_length};
                    if (!b.ok(18)) return false;
                    b.pos = 17;
                    if (b.u8() & 1) return false;
                    break;
                }
                case 48: case 49: case 50: case 51: case 62:
                    break;
                default:
                    return false;
            }
        }
        return true;
    };
    if (globals && globals_len > 0 && !scan(globals, globals_len))
        return false;
    return scan(data, len);
}

}} // namespace jdoc::pdf_detail
