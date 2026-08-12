// jpx.cpp — JPEG 2000 baseline decoder (ITU-T T.800) for PDF JPXDecode.
//
// Scope: what PDF producers actually emit for photographic content — single
// or multi-tile codestreams, 5/3 (reversible) and 9/7 (irreversible)
// wavelets, RCT/ICT component transforms, LRCP/RLCP progressions (the
// position-first orders only in their single-precinct degenerate form),
// scalar quantization, one codeword segment per code-block. Packed packet
// headers (PPM/PPT), bypass/termall/vertically-causal code-block styles,
// component subsampling and POC are rejected, so the caller keeps its
// no-decode fallback.
//
// The MQ coder is shared with JBIG2 (mq_decoder.h). Tag trees, packet
// headers and the bit-plane coder follow Annex B/C/D of the standard; the
// openjpeg and pdf.js implementations served as behavioral references for
// the underspecified corners (bit-stuffing alignment, context numbering).
#include "jpx.h"
#include "pdf_limits.h"
#include "mq_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace jdoc { namespace pdf_detail {

namespace {

inline int ceil_shift(int a, int s) { return (a + (1 << s) - 1) >> s; }
inline int ceil_ratio(int a, int b) { // b > 0, a may be negative
    return a >= 0 ? (a + b - 1) / b : -((-a) / b);
}
inline int floor_ratio(int a, int b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// ── Codestream byte reader ───────────────────────────────

struct Reader {
    const uint8_t* p;
    size_t len;
    size_t pos = 0;

    bool ok(size_t n) const { return pos + n <= len; }
    uint32_t u8() { return pos < len ? p[pos++] : 0; }
    uint32_t u16() { uint32_t v = u8(); return (v << 8) | u8(); }
    uint32_t u32() { uint32_t v = u16(); return (v << 16) | u16(); }
    void skip(size_t n) { pos = std::min(len, pos + n); }
};

// ── Packet-header bit reader (B.10.1 bit stuffing) ───────

struct Bio {
    const uint8_t* p;
    size_t len;
    size_t pos = 0;
    uint32_t buf = 0;
    int ct = 0;

    void bytein() {
        buf = (buf << 8) & 0xFFFF;
        ct = (buf == 0xFF00) ? 7 : 8;
        if (pos < len) buf |= p[pos++];
    }
    int bit() {
        if (ct == 0) bytein();
        ct--;
        return (buf >> ct) & 1;
    }
    uint32_t bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) v = (v << 1) | static_cast<uint32_t>(bit());
        return v;
    }
    void align() {
        // A packet header ending in 0xFF carries one stuffed byte (B.10.1)
        if ((buf & 0xFF) == 0xFF) bytein();
        ct = 0;
    }
};

// ── Tag tree (B.10.2) ────────────────────────────────────

struct TagTree {
    struct Node { int parent; int low; bool known; };
    std::vector<Node> nodes;
    int leaves_w = 0;

    void build(int w, int h) {
        leaves_w = w;
        nodes.clear();
        if (w <= 0 || h <= 0) return;
        std::vector<int> level_off;
        int lw = w, lh = h, total = 0;
        while (true) {
            level_off.push_back(total);
            total += lw * lh;
            if (lw == 1 && lh == 1) break;
            lw = (lw + 1) / 2;
            lh = (lh + 1) / 2;
        }
        nodes.assign(total, {-1, 0, false});
        lw = w; lh = h;
        for (size_t lv = 0; lv + 1 < level_off.size(); lv++) {
            int pw = (lw + 1) / 2;
            for (int y = 0; y < lh; y++)
                for (int x = 0; x < lw; x++)
                    nodes[level_off[lv] + y * lw + x].parent =
                        level_off[lv + 1] + (y / 2) * pw + (x / 2);
            lw = pw;
            lh = (lh + 1) / 2;
        }
    }

    // Decode the leaf's value under the given threshold. Returns true when
    // the value is fully determined (and thus < threshold).
    bool decode(Bio& bio, int leaf_x, int leaf_y, int threshold, int& value) {
        int path[32];
        int n = 0;
        int id = leaf_y * leaves_w + leaf_x;
        while (id >= 0 && n < 32) {
            path[n++] = id;
            id = nodes[id].parent;
        }
        int low = 0;
        for (int i = n - 1; i >= 0; i--) {
            Node& nd = nodes[path[i]];
            if (nd.low < low) nd.low = low;
            while (!nd.known && nd.low < threshold) {
                if (bio.bit()) nd.known = true;
                else nd.low++;
            }
            low = nd.low;
        }
        value = low;
        return nodes[path[0]].known;
    }

    int decode_full(Bio& bio, int leaf_x, int leaf_y) {
        // Bounded threshold: exhausted input feeds 0-bits, which would
        // otherwise walk the lower bound up forever on corrupt data.
        int v = 0;
        decode(bio, leaf_x, leaf_y, 4096, v);
        return v; // >= 4096 means undetermined; callers range-check
    }
};

// ── Structure ────────────────────────────────────────────

struct CodingStyle {
    int levels = 5;
    int cb_xexp = 6, cb_yexp = 6; // log2 code-block size
    int cb_style = 0;
    int transform = 0; // 0 = 9/7 irreversible, 1 = 5/3 reversible
    bool sop = false, eph = false;
    uint8_t precinct_size[34]; // PPx | PPy<<4 per resolution

    CodingStyle() { std::memset(precinct_size, 0xFF, sizeof(precinct_size)); }
};

struct QuantStyle {
    int style = 0; // 0 = none (reversible), 1 = scalar derived, 2 = expounded
    int guard = 2;
    std::vector<uint16_t> vals; // (eps << 11) | mu
};

struct CodeBlock {
    int x0, y0, x1, y1; // band-absolute
    bool included = false;
    int lblock = 3;
    int zbp = 0;
    int passes = 0;
    std::vector<uint8_t> data;
};

struct Band {
    int type; // 0=LL 1=HL 2=LH 3=HH
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int Mb = 0;
    float delta = 1.0f;
    int ncbx = 0, ncby = 0;
    std::vector<CodeBlock> cbs;
    TagTree incl, zbpt;
    std::vector<float> coef; // dequantized after Tier-1

    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
};

struct Resolution {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int nbands = 0;
    Band bands[3];
};

struct TileComp {
    int x0, y0, x1, y1;
    CodingStyle cs;
    QuantStyle qs;
    std::vector<Resolution> res;
    std::vector<float> plane; // reconstructed tile-component samples
};

struct Tile {
    int x0, y0, x1, y1;
    int progression = 0, nlayers = 1; // COD in a tile header overrides these
    std::vector<TileComp> comps;
    std::vector<uint8_t> body; // concatenated tile-part bitstreams
};

struct Codestream {
    int xsiz = 0, ysiz = 0, xo = 0, yo = 0;
    int xt = 0, yt = 0, xto = 0, yto = 0;
    int ncomp = 0;
    int progression = 0, nlayers = 1, mct = 0;
    std::vector<int> depth;
    std::vector<bool> sgned;
    CodingStyle cs;
    QuantStyle qs;
    std::vector<CodingStyle> comp_cs;
    std::vector<QuantStyle> comp_qs;
    std::vector<Tile> tiles;
};

// ── Header parsing ───────────────────────────────────────

static bool parse_cod_style(Reader& r, CodingStyle& cs, int scod) {
    cs.levels = static_cast<int>(r.u8());
    cs.cb_xexp = static_cast<int>(r.u8() & 0x0F) + 2;
    cs.cb_yexp = static_cast<int>(r.u8() & 0x0F) + 2;
    cs.cb_style = static_cast<int>(r.u8());
    cs.transform = static_cast<int>(r.u8());
    // The geometry below uses 32-bit coordinates and power-of-two divisors.
    // Thirty decomposition levels already exceed any image admitted by the
    // decoded-pixel budget; rejecting larger values also avoids 1 << 32 UB.
    if (cs.levels > 30) return false;
    if (scod & 1) {
        for (int i = 0; i <= cs.levels && i < 34; i++)
            cs.precinct_size[i] = static_cast<uint8_t>(r.u8());
    }
    // Bypass, reset, termall, vertically-causal and predictable termination
    // change pass segmentation or context flow; only the segmentation
    // symbol (0x20) is tolerated — it is decoded and discarded.
    if (cs.cb_style & ~0x20) return false;
    return true;
}

static bool parse_quant(Reader& r, QuantStyle& q, size_t body_len) {
    if (body_len < 1) return false;
    uint32_t s = r.u8();
    q.style = s & 0x1F;
    q.guard = static_cast<int>(s >> 5);
    q.vals.clear();
    size_t rest = body_len - 1;
    if (q.style == 0) {
        for (size_t i = 0; i < rest; i++)
            q.vals.push_back(static_cast<uint16_t>((r.u8() >> 3) << 11));
    } else {
        for (size_t i = 0; i + 2 <= rest; i += 2)
            q.vals.push_back(static_cast<uint16_t>(r.u16()));
    }
    return !q.vals.empty();
}

// Quantization parameters for one band. level = decomposition level nb
// (1..NL for detail bands, NL for the LL band), type 0..3.
static void band_quant(const QuantStyle& q, int nl, int level, int type,
                       int depth, int transform, int& Mb, float& delta) {
    int idx;
    if (q.style == 1 || type == 0) idx = 0;
    else idx = 3 * (nl - level) + type;
    if (idx >= static_cast<int>(q.vals.size()))
        idx = static_cast<int>(q.vals.size()) - 1;
    if (idx < 0) { Mb = 0; delta = 1.0f; return; }
    int eps = q.vals[idx] >> 11;
    int mu = q.vals[idx] & 0x7FF;
    if (q.style == 1 && type != 0) {
        eps = eps - nl + level; // scalar derived (E.1.1)
        if (eps < 0) eps = 0;
    }
    Mb = q.guard + eps - 1;
    if (transform == 1) {
        delta = 1.0f;
    } else {
        int gain = (type == 0) ? 0 : (type == 3 ? 2 : 1);
        delta = static_cast<float>(
            std::pow(2.0, depth + gain - eps) * (1.0 + mu / 2048.0));
    }
}

// ── Tier-1 bit-plane decoder (Annex D) ───────────────────

// Context numbering follows pdf.js: 0..8 significance, 9..13 sign,
// 14..16 magnitude refinement, 17 uniform, 18 run-length.
enum { CTX_UNIFORM = 17, CTX_RL = 18, CTX_COUNT = 19 };

static inline int sig_ctx(int h, int v, int d, int type) {
    if (type == 1) std::swap(h, v); // HL: transposed neighborhood
    if (type != 3) {                // LL, LH (and transposed HL)
        if (h == 2) return 8;
        if (h == 1) return v >= 1 ? 7 : (d >= 1 ? 6 : 5);
        if (v == 2) return 4;
        if (v == 1) return 3;
        return d >= 2 ? 2 : d;
    }
    if (d >= 3) return 8;
    int hv = h + v;
    if (d == 2) return hv >= 1 ? 7 : 6;
    if (d == 1) return hv >= 2 ? 5 : (hv == 1 ? 4 : 3);
    return hv >= 2 ? 2 : (hv == 1 ? 1 : 0);
}

struct T1Flags {
    uint8_t sig : 1, visited : 1, refined : 1, sign : 1;
};

struct T1Out {
    std::vector<uint32_t> mag;
    std::vector<T1Flags> flags;
    std::vector<uint8_t> bits; // bit-plane observations per coefficient
};

static void t1_decode_block(const CodeBlock& cb, int band_type, int Mb,
                            bool segsym, T1Out& out) {
    const int w = cb.x1 - cb.x0, h = cb.y1 - cb.y0;
    const size_t n = static_cast<size_t>(w) * h;
    out.mag.assign(n, 0);
    out.flags.assign(n, {});
    out.bits.assign(n, 0);
    if (cb.passes <= 0 || cb.data.empty()) return;

    auto& mag = out.mag;
    auto& flags = out.flags;
    auto& bits_decoded = out.bits;

    MQDecoder mq(cb.data.data(), cb.data.size());
    uint8_t cx[CTX_COUNT];
    std::memset(cx, 0, sizeof(cx));
    cx[0] = 4 << 1;
    cx[CTX_UNIFORM] = 46 << 1;
    cx[CTX_RL] = 3 << 1;

    auto sig_at = [&](int x, int y) -> int {
        if (static_cast<unsigned>(x) >= static_cast<unsigned>(w) ||
            static_cast<unsigned>(y) >= static_cast<unsigned>(h))
            return 0;
        return flags[static_cast<size_t>(y) * w + x].sig;
    };
    auto sign_at = [&](int x, int y) -> int {
        if (static_cast<unsigned>(x) >= static_cast<unsigned>(w) ||
            static_cast<unsigned>(y) >= static_cast<unsigned>(h))
            return 0;
        const T1Flags& f = flags[static_cast<size_t>(y) * w + x];
        return f.sig ? (f.sign ? -1 : 1) : 0;
    };
    auto neighborhood = [&](int x, int y, int& hn, int& vn, int& dn) {
        hn = sig_at(x - 1, y) + sig_at(x + 1, y);
        vn = sig_at(x, y - 1) + sig_at(x, y + 1);
        dn = sig_at(x - 1, y - 1) + sig_at(x + 1, y - 1) +
             sig_at(x - 1, y + 1) + sig_at(x + 1, y + 1);
    };
    auto decode_sign = [&](int x, int y) -> uint8_t {
        int hc = sign_at(x - 1, y) + sign_at(x + 1, y);
        int vc = sign_at(x, y - 1) + sign_at(x, y + 1);
        hc = std::max(-1, std::min(1, hc));
        vc = std::max(-1, std::min(1, vc));
        int ctx, xorbit;
        if (hc == 0) {
            ctx = vc == 0 ? 9 : 10;
            xorbit = vc < 0 ? 1 : 0;
        } else {
            ctx = vc == 0 ? 12 : (hc * vc > 0 ? 13 : 11);
            xorbit = hc < 0 ? 1 : 0;
        }
        return static_cast<uint8_t>(mq.decode(&cx[ctx]) ^ xorbit);
    };

    int planes_left = Mb - cb.zbp;
    if (planes_left <= 0) return;

    int pass_kind = 2; // first pass of the MSB plane is a cleanup pass
    for (int p = 0; p < cb.passes; p++) {
        if (pass_kind == 0) {
            // Significance propagation
            for (int y0 = 0; y0 < h; y0 += 4)
                for (int x = 0; x < w; x++)
                    for (int y = y0; y < std::min(y0 + 4, h); y++) {
                        size_t i = static_cast<size_t>(y) * w + x;
                        T1Flags& f = flags[i];
                        if (f.sig) continue;
                        int hn, vn, dn;
                        neighborhood(x, y, hn, vn, dn);
                        if (hn + vn + dn == 0) continue;
                        f.visited = 1;
                        bits_decoded[i]++;
                        if (mq.decode(&cx[sig_ctx(hn, vn, dn, band_type)])) {
                            f.sign = decode_sign(x, y);
                            f.sig = 1;
                            mag[i] = 1;
                        }
                    }
        } else if (pass_kind == 1) {
            // Magnitude refinement
            for (int y0 = 0; y0 < h; y0 += 4)
                for (int x = 0; x < w; x++)
                    for (int y = y0; y < std::min(y0 + 4, h); y++) {
                        size_t i = static_cast<size_t>(y) * w + x;
                        T1Flags& f = flags[i];
                        if (!f.sig || f.visited) continue;
                        int ctx;
                        if (f.refined) ctx = 16;
                        else {
                            int hn, vn, dn;
                            neighborhood(x, y, hn, vn, dn);
                            ctx = (hn + vn + dn) ? 15 : 14;
                        }
                        mag[i] = (mag[i] << 1) |
                                 static_cast<uint32_t>(mq.decode(&cx[ctx]));
                        f.refined = 1;
                        bits_decoded[i]++;
                    }
        } else {
            // Cleanup
            for (int y0 = 0; y0 < h; y0 += 4)
                for (int x = 0; x < w; x++) {
                    int y = y0;
                    const int ylim = std::min(y0 + 4, h);
                    if (ylim - y0 == 4) {
                        // Run-length mode: full stripe column, all four
                        // coefficients unvisited with all-zero contexts
                        bool rl = true;
                        for (int yy = y0; yy < ylim && rl; yy++) {
                            size_t i = static_cast<size_t>(yy) * w + x;
                            if (flags[i].sig || flags[i].visited) { rl = false; break; }
                            int hn, vn, dn;
                            neighborhood(x, yy, hn, vn, dn);
                            if (hn + vn + dn) rl = false;
                        }
                        if (rl) {
                            if (!mq.decode(&cx[CTX_RL])) {
                                for (int yy = y0; yy < ylim; yy++)
                                    bits_decoded[static_cast<size_t>(yy) * w + x]++;
                                continue;
                            }
                            int first = static_cast<int>(mq.decode(&cx[CTX_UNIFORM])) << 1;
                            first |= static_cast<int>(mq.decode(&cx[CTX_UNIFORM]));
                            for (int yy = y0; yy <= y0 + first; yy++)
                                bits_decoded[static_cast<size_t>(yy) * w + x]++;
                            y = y0 + first;
                            size_t i = static_cast<size_t>(y) * w + x;
                            flags[i].sign = decode_sign(x, y);
                            flags[i].sig = 1;
                            mag[i] = 1;
                            y++;
                        }
                    }
                    for (; y < ylim; y++) {
                        size_t i = static_cast<size_t>(y) * w + x;
                        T1Flags& f = flags[i];
                        if (f.visited) { f.visited = 0; continue; }
                        if (f.sig) continue;
                        int hn, vn, dn;
                        neighborhood(x, y, hn, vn, dn);
                        bits_decoded[i]++;
                        if (mq.decode(&cx[sig_ctx(hn, vn, dn, band_type)])) {
                            f.sign = decode_sign(x, y);
                            f.sig = 1;
                            mag[i] = 1;
                        }
                    }
                }
            for (auto& f : flags) f.visited = 0;
            if (segsym)
                for (int k = 0; k < 4; k++) mq.decode(&cx[CTX_UNIFORM]);
        }
        if (pass_kind < 2) {
            pass_kind++;
        } else {
            pass_kind = 0;
            if (--planes_left <= 0) break; // no more magnitude planes coded
        }
    }
}

// ── Inverse DWT (Annex F) ────────────────────────────────

static inline int reflect(int i, int i0, int i1) {
    while (i < i0 || i >= i1) {
        if (i < i0) i = 2 * i0 - i;
        if (i >= i1) i = 2 * (i1 - 1) - i;
    }
    return i;
}

// 1D synthesis over absolute range [i0, i1); buf[k] holds the sample at
// absolute index i0+k. Even absolute indices are low-pass samples.
static void synth_53(float* buf, int i0, int i1) {
    const int n = i1 - i0;
    if (n <= 0) return;
    if (n == 1) {
        if (i0 & 1) buf[0] = std::floor(buf[0] / 2.0f);
        return;
    }
    auto at = [&](int i) -> float& { return buf[reflect(i, i0, i1) - i0]; };
    const int even0 = (i0 & 1) ? i0 + 1 : i0;
    const int odd0 = (i0 & 1) ? i0 : i0 + 1;
    for (int i = even0; i < i1; i += 2)
        at(i) -= std::floor((at(i - 1) + at(i + 1) + 2.0f) / 4.0f);
    for (int i = odd0; i < i1; i += 2)
        at(i) += std::floor((at(i - 1) + at(i + 1)) / 2.0f);
}

static void synth_97(float* buf, int i0, int i1) {
    const int n = i1 - i0;
    if (n <= 0) return;
    constexpr float alpha = -1.586134342059924f;
    constexpr float beta = -0.052980118572961f;
    constexpr float gamma = 0.882911075530934f;
    constexpr float delta = 0.443506852043971f;
    constexpr float K = 1.230174104914001f;
    if (n == 1) {
        buf[0] *= (i0 & 1) ? K : (1.0f / K);
        return;
    }
    auto at = [&](int i) -> float& { return buf[reflect(i, i0, i1) - i0]; };
    const int even0 = (i0 & 1) ? i0 + 1 : i0;
    const int odd0 = (i0 & 1) ? i0 : i0 + 1;
    // Undo the analysis scaling, then the four lifting steps in reverse.
    for (int i = even0; i < i1; i += 2) at(i) *= K;
    for (int i = odd0; i < i1; i += 2) at(i) *= 1.0f / K;
    for (int i = even0; i < i1; i += 2) at(i) -= delta * (at(i - 1) + at(i + 1));
    for (int i = odd0; i < i1; i += 2) at(i) -= gamma * (at(i - 1) + at(i + 1));
    for (int i = even0; i < i1; i += 2) at(i) -= beta * (at(i - 1) + at(i + 1));
    for (int i = odd0; i < i1; i += 2) at(i) -= alpha * (at(i - 1) + at(i + 1));
}

// ── Tile geometry / packet parsing / reconstruction ──────

static bool setup_tile(Codestream& cs, Tile& tile) {
    for (int c = 0; c < cs.ncomp; c++) {
        TileComp& tc = tile.comps[c];
        tc.x0 = tile.x0; tc.y0 = tile.y0;
        tc.x1 = tile.x1; tc.y1 = tile.y1;
        const CodingStyle& st = tc.cs;
        const int nl = st.levels;
        tc.res.assign(nl + 1, {});
        for (int r = 0; r <= nl; r++) {
            Resolution& res = tc.res[r];
            const int s = nl - r;
            res.x0 = ceil_shift(tc.x0, s);
            res.y0 = ceil_shift(tc.y0, s);
            res.x1 = ceil_shift(tc.x1, s);
            res.y1 = ceil_shift(tc.y1, s);

            // Reject multi-precinct layouts: tag trees here are per band.
            int ppx = st.precinct_size[r] & 0x0F;
            int ppy = (st.precinct_size[r] >> 4) & 0x0F;
            if (res.x1 > res.x0 &&
                ceil_ratio(res.x1, 1 << ppx) - floor_ratio(res.x0, 1 << ppx) > 1)
                return false;
            if (res.y1 > res.y0 &&
                ceil_ratio(res.y1, 1 << ppy) - floor_ratio(res.y0, 1 << ppy) > 1)
                return false;

            // T.800 Table A.21 permits precinct exponent 0 only at
            // resolution 0; accepting it above would drive the code-block
            // exponents below to -1, a negative (undefined) shift count.
            if (r > 0 && (ppx == 0 || ppy == 0)) return false;

            int cbxe = std::min(st.cb_xexp, r == 0 ? ppx : ppx - 1);
            int cbye = std::min(st.cb_yexp, r == 0 ? ppy : ppy - 1);

            res.nbands = (r == 0) ? 1 : 3;
            for (int b = 0; b < res.nbands; b++) {
                Band& band = res.bands[b];
                if (r == 0) {
                    band.type = 0;
                    band.x0 = res.x0; band.y0 = res.y0;
                    band.x1 = res.x1; band.y1 = res.y1;
                } else {
                    band.type = b + 1; // HL, LH, HH
                    const int nb = nl - r + 1;
                    const int xob = (band.type == 1 || band.type == 3) ? 1 : 0;
                    const int yob = (band.type == 2 || band.type == 3) ? 1 : 0;
                    const int half = 1 << (nb - 1), full = 1 << nb;
                    band.x0 = ceil_ratio(tc.x0 - half * xob, full);
                    band.y0 = ceil_ratio(tc.y0 - half * yob, full);
                    band.x1 = ceil_ratio(tc.x1 - half * xob, full);
                    band.y1 = ceil_ratio(tc.y1 - half * yob, full);
                }
                band_quant(tc.qs, nl, r == 0 ? nl : nl - r + 1, band.type,
                           cs.depth[c], st.transform, band.Mb, band.delta);
                if (band.Mb <= 0 || band.Mb > 31) return false;
                if (band.w() <= 0 || band.h() <= 0) continue;
                const int gx0 = band.x0 >> cbxe, gy0 = band.y0 >> cbye;
                band.ncbx = ceil_shift(band.x1, cbxe) - gx0;
                band.ncby = ceil_shift(band.y1, cbye) - gy0;
                if (static_cast<int64_t>(band.ncbx) * band.ncby > 1 << 20)
                    return false;
                band.cbs.resize(static_cast<size_t>(band.ncbx) * band.ncby);
                for (int gy = 0; gy < band.ncby; gy++)
                    for (int gx = 0; gx < band.ncbx; gx++) {
                        CodeBlock& cb = band.cbs[static_cast<size_t>(gy) * band.ncbx + gx];
                        cb.x0 = std::max(band.x0, (gx0 + gx) << cbxe);
                        cb.y0 = std::max(band.y0, (gy0 + gy) << cbye);
                        cb.x1 = std::min(band.x1, (gx0 + gx + 1) << cbxe);
                        cb.y1 = std::min(band.y1, (gy0 + gy + 1) << cbye);
                    }
                band.incl.build(band.ncbx, band.ncby);
                band.zbpt.build(band.ncbx, band.ncby);
            }
        }
    }
    return true;
}

// One packet for (component c, resolution r, layer). The single precinct per
// resolution was enforced during setup.
static bool decode_packet(Tile& tile, int c, int r, int layer, Reader& rd) {
    TileComp& tc = tile.comps[c];
    if (r >= static_cast<int>(tc.res.size())) return true;
    Resolution& res = tc.res[r];

    if (tc.cs.sop && rd.ok(2) && rd.p[rd.pos] == 0xFF && rd.p[rd.pos + 1] == 0x91)
        rd.skip(6);

    Bio bio{rd.p + rd.pos, rd.len - rd.pos};
    struct Sel { CodeBlock* cb; uint32_t len; };
    std::vector<Sel> included;

    if (bio.bit()) {
        for (int b = 0; b < res.nbands; b++) {
            Band& band = res.bands[b];
            if (band.w() <= 0 || band.h() <= 0) continue;
            for (int gy = 0; gy < band.ncby; gy++)
                for (int gx = 0; gx < band.ncbx; gx++) {
                    CodeBlock& cb = band.cbs[static_cast<size_t>(gy) * band.ncbx + gx];
                    bool incl;
                    if (!cb.included) {
                        int v;
                        incl = band.incl.decode(bio, gx, gy, layer + 1, v);
                    } else {
                        incl = bio.bit() != 0;
                    }
                    if (!incl) continue;
                    if (!cb.included) {
                        cb.included = true;
                        cb.zbp = band.zbpt.decode_full(bio, gx, gy);
                        if (cb.zbp > 62) return false;
                    }
                    int np = 1; // number of new passes (B.10.6)
                    if (bio.bit()) {
                        np = 2;
                        if (bio.bit()) {
                            uint32_t v = bio.bits(2);
                            if (v < 3) np = 3 + static_cast<int>(v);
                            else {
                                v = bio.bits(5);
                                if (v < 31) np = 6 + static_cast<int>(v);
                                else np = 37 + static_cast<int>(bio.bits(7));
                            }
                        }
                    }
                    while (bio.bit()) cb.lblock++;
                    if (cb.lblock > 24) return false;
                    int lg = 0;
                    while ((1 << (lg + 1)) <= np) lg++;
                    uint32_t seg_len = bio.bits(cb.lblock + lg);
                    if (seg_len > 1u << 28) return false;
                    cb.passes += np;
                    included.push_back({&cb, seg_len});
                }
        }
    }
    bio.align();
    rd.skip(bio.pos);
    if (tc.cs.eph && rd.ok(2) && rd.p[rd.pos] == 0xFF && rd.p[rd.pos + 1] == 0x92)
        rd.skip(2);

    for (auto& s : included) {
        size_t len = s.len;
        if (!rd.ok(len)) len = rd.len - rd.pos;
        s.cb->data.insert(s.cb->data.end(), rd.p + rd.pos, rd.p + rd.pos + len);
        rd.skip(len);
    }
    return true;
}

static bool decode_tile_packets(Codestream& cs, Tile& tile) {
    Reader rd{tile.body.data(), tile.body.size(), 0};
    int maxres = 0;
    for (auto& tc : tile.comps)
        maxres = std::max(maxres, static_cast<int>(tc.res.size()));

    switch (tile.progression) {
        case 0: // LRCP
            for (int l = 0; l < tile.nlayers; l++)
                for (int r = 0; r < maxres; r++)
                    for (int c = 0; c < cs.ncomp; c++)
                        if (!decode_packet(tile, c, r, l, rd)) return false;
            break;
        case 1: // RLCP
            for (int r = 0; r < maxres; r++)
                for (int l = 0; l < tile.nlayers; l++)
                    for (int c = 0; c < cs.ncomp; c++)
                        if (!decode_packet(tile, c, r, l, rd)) return false;
            break;
        case 2: // RPCL (single precinct: position loop is degenerate)
            for (int r = 0; r < maxres; r++)
                for (int c = 0; c < cs.ncomp; c++)
                    for (int l = 0; l < tile.nlayers; l++)
                        if (!decode_packet(tile, c, r, l, rd)) return false;
            break;
        case 3: // PCRL
        case 4: // CPRL
            for (int c = 0; c < cs.ncomp; c++)
                for (int r = 0; r < maxres; r++)
                    for (int l = 0; l < tile.nlayers; l++)
                        if (!decode_packet(tile, c, r, l, rd)) return false;
            break;
        default:
            return false;
    }
    return true;
}

static void reconstruct_component(TileComp& tc) {
    const int nl = tc.cs.levels;
    const bool reversible = tc.cs.transform == 1;

    // Tier-1 + dequantization into per-band coefficient planes
    T1Out t1;
    for (auto& res : tc.res)
        for (int b = 0; b < res.nbands; b++) {
            Band& band = res.bands[b];
            band.coef.assign(static_cast<size_t>(band.w()) * band.h(), 0.0f);
            const bool segsym = (tc.cs.cb_style & 0x20) != 0;
            for (auto& cb : band.cbs) {
                const int cw = cb.x1 - cb.x0, ch = cb.y1 - cb.y0;
                if (cw <= 0 || ch <= 0) continue;
                t1_decode_block(cb, band.type, band.Mb, segsym, t1);
                for (int y = 0; y < ch; y++)
                    for (int x = 0; x < cw; x++) {
                        const size_t i = static_cast<size_t>(y) * cw + x;
                        uint32_t m = t1.mag[i];
                        if (!m) continue;
                        // Shift up planes never observed; reconstruct at the
                        // middle of the uncertainty interval.
                        int missing = band.Mb - cb.zbp - t1.bits[i];
                        if (missing < 0) missing = 0;
                        float v = static_cast<float>(m << missing) +
                                  (missing > 0 ? static_cast<float>(1u << (missing - 1)) : 0.0f);
                        if (t1.flags[i].sign) v = -v;
                        band.coef[static_cast<size_t>(cb.y0 - band.y0 + y) * band.w() +
                                  (cb.x0 - band.x0 + x)] =
                            reversible ? v : v * band.delta;
                    }
            }
        }

    // Multi-level 2D synthesis: LL grows one resolution per step
    std::vector<float> cur = std::move(tc.res[0].bands[0].coef);
    int cx0 = tc.res[0].x0, cy0 = tc.res[0].y0;
    std::vector<float> next, rowbuf, colbuf;
    for (int r = 1; r <= nl; r++) {
        Resolution& res = tc.res[r];
        const int w = res.x1 - res.x0, h = res.y1 - res.y0;
        next.assign(static_cast<size_t>(w) * h, 0.0f);
        const Band& hl = res.bands[0];
        const Band& lh = res.bands[1];
        const Band& hh = res.bands[2];
        const int prev_w = tc.res[r - 1].x1 - tc.res[r - 1].x0;
        for (int y = res.y0; y < res.y1; y++) {
            for (int x = res.x0; x < res.x1; x++) {
                const int bx = x >> 1, by = y >> 1;
                float v = 0.0f;
                if ((x & 1) == 0 && (y & 1) == 0) {
                    if (bx >= cx0 && by >= cy0 &&
                        bx - cx0 < prev_w &&
                        static_cast<size_t>(by - cy0) * prev_w < cur.size())
                        v = cur[static_cast<size_t>(by - cy0) * prev_w + (bx - cx0)];
                } else if ((x & 1) == 1 && (y & 1) == 0) {
                    if (bx >= hl.x0 && bx < hl.x1 && by >= hl.y0 && by < hl.y1)
                        v = hl.coef[static_cast<size_t>(by - hl.y0) * hl.w() + (bx - hl.x0)];
                } else if ((x & 1) == 0) {
                    if (bx >= lh.x0 && bx < lh.x1 && by >= lh.y0 && by < lh.y1)
                        v = lh.coef[static_cast<size_t>(by - lh.y0) * lh.w() + (bx - lh.x0)];
                } else {
                    if (bx >= hh.x0 && bx < hh.x1 && by >= hh.y0 && by < hh.y1)
                        v = hh.coef[static_cast<size_t>(by - hh.y0) * hh.w() + (bx - hh.x0)];
                }
                next[static_cast<size_t>(y - res.y0) * w + (x - res.x0)] = v;
            }
        }
        rowbuf.resize(w);
        for (int y = 0; y < h; y++) {
            float* row = next.data() + static_cast<size_t>(y) * w;
            std::memcpy(rowbuf.data(), row, sizeof(float) * w);
            if (reversible) synth_53(rowbuf.data(), res.x0, res.x1);
            else synth_97(rowbuf.data(), res.x0, res.x1);
            std::memcpy(row, rowbuf.data(), sizeof(float) * w);
        }
        colbuf.resize(h);
        for (int x = 0; x < w; x++) {
            for (int y = 0; y < h; y++)
                colbuf[y] = next[static_cast<size_t>(y) * w + x];
            if (reversible) synth_53(colbuf.data(), res.y0, res.y1);
            else synth_97(colbuf.data(), res.y0, res.y1);
            for (int y = 0; y < h; y++)
                next[static_cast<size_t>(y) * w + x] = colbuf[y];
        }
        cur = std::move(next);
        cx0 = res.x0;
        cy0 = res.y0;
    }
    tc.plane = std::move(cur);
}

// ── Top level ────────────────────────────────────────────

static bool locate_codestream(const uint8_t*& data, size_t& len) {
    if (len >= 4 && data[0] == 0xFF && data[1] == 0x4F) return true; // SOC
    // JP2 container: walk boxes to jp2c
    Reader r{data, len, 0};
    while (r.ok(8)) {
        uint64_t box_len = r.u32();
        uint32_t type = r.u32();
        size_t header = 8;
        if (box_len == 1) {
            uint64_t hi = r.u32(), lo = r.u32();
            box_len = (hi << 32) | lo;
            header = 16;
        }
        if (type == 0x6A703263) { // 'jp2c'
            data += r.pos;
            len = (box_len == 0) ? len - r.pos
                                 : std::min<size_t>(len - r.pos,
                                                    static_cast<size_t>(box_len - header));
            return len >= 4 && data[0] == 0xFF && data[1] == 0x4F;
        }
        if (box_len == 0) return false;
        if (box_len < header || box_len - header > len - r.pos) return false;
        r.skip(static_cast<size_t>(box_len - header));
    }
    return false;
}

} // namespace

JpxImage jpx_decode(const uint8_t* data, size_t len) {
    JpxImage img;
    if (!data || len < 4) return img;
    if (!locate_codestream(data, len)) return img;

    Reader r{data, len, 0};
    if (r.u16() != 0xFF4F) return img;

    Codestream cs;
    std::vector<bool> tile_started;

    // Main header. Marker order after SIZ is unconstrained, so a per-component
    // override (COC/QCC) may legally precede its default (COD/QCD); overrides
    // are flagged here and the defaults materialized only after the loop.
    bool have_siz = false, have_cod = false, have_qcd = false;
    std::vector<char> coc_set, qcc_set;
    while (r.ok(4)) {
        uint32_t marker = r.u16();
        if (marker == 0xFF90) { r.pos -= 2; break; } // SOT
        if (marker == 0xFFD9) return img;            // EOC before any tile
        uint32_t mlen = r.u16();
        if (mlen < 2 || !r.ok(mlen - 2)) return img;
        size_t end = r.pos + mlen - 2;
        switch (marker) {
            case 0xFF51: { // SIZ
                r.u16(); // Rsiz
                cs.xsiz = static_cast<int>(r.u32());
                cs.ysiz = static_cast<int>(r.u32());
                cs.xo = static_cast<int>(r.u32());
                cs.yo = static_cast<int>(r.u32());
                cs.xt = static_cast<int>(r.u32());
                cs.yt = static_cast<int>(r.u32());
                cs.xto = static_cast<int>(r.u32());
                cs.yto = static_cast<int>(r.u32());
                cs.ncomp = static_cast<int>(r.u16());
                if (cs.xsiz <= cs.xo || cs.ysiz <= cs.yo) return img;
                if (cs.xt <= 0 || cs.yt <= 0) return img;
                if (cs.ncomp < 1 || cs.ncomp > 8) return img;
                const int64_t image_w = static_cast<int64_t>(cs.xsiz) - cs.xo;
                const int64_t image_h = static_cast<int64_t>(cs.ysiz) - cs.yo;
                if (image_w <= 0 || image_h <= 0 ||
                    image_w > static_cast<int64_t>(limits::kMaxDecodedPixels) /
                                  image_h)
                    return img;
                const int64_t pixels = image_w * image_h;
                if (pixels > static_cast<int64_t>(limits::kMaxDecodedPixels) ||
                    pixels > static_cast<int64_t>(limits::kMaxDecodedSamples) /
                                 cs.ncomp)
                    return img;
                for (int c = 0; c < cs.ncomp; c++) {
                    uint32_t ssiz = r.u8();
                    cs.depth.push_back(static_cast<int>(ssiz & 0x7F) + 1);
                    cs.sgned.push_back((ssiz & 0x80) != 0);
                    int xr = static_cast<int>(r.u8());
                    int yr = static_cast<int>(r.u8());
                    if (xr != 1 || yr != 1) return img; // subsampling
                    if (cs.depth.back() > 16) return img;
                    if (cs.depth.back() > img.src_depth)
                        img.src_depth = cs.depth.back();
                }
                have_siz = true;
                coc_set.assign(static_cast<size_t>(cs.ncomp), 0);
                qcc_set.assign(static_cast<size_t>(cs.ncomp), 0);
                break;
            }
            case 0xFF52: { // COD
                uint32_t scod = r.u8();
                cs.cs.sop = (scod & 2) != 0;
                cs.cs.eph = (scod & 4) != 0;
                cs.progression = static_cast<int>(r.u8());
                cs.nlayers = static_cast<int>(r.u16());
                cs.mct = static_cast<int>(r.u8());
                if (cs.nlayers < 1 || cs.nlayers > 256) return img;
                if (!parse_cod_style(r, cs.cs, static_cast<int>(scod))) return img;
                have_cod = true;
                break;
            }
            case 0xFF53: { // COC
                if (!have_siz) return img;
                int c = static_cast<int>(cs.ncomp <= 256 ? r.u8() : r.u16());
                uint32_t scoc = r.u8();
                if (cs.comp_cs.empty()) {
                    cs.comp_cs.assign(cs.ncomp, cs.cs);
                }
                if (c < 0 || c >= cs.ncomp) return img;
                CodingStyle st = cs.cs;
                if (!parse_cod_style(r, st, static_cast<int>(scoc & 1))) return img;
                cs.comp_cs[c] = st;
                coc_set[static_cast<size_t>(c)] = 1;
                break;
            }
            case 0xFF5C: // QCD
                if (!parse_quant(r, cs.qs, mlen - 2)) return img;
                have_qcd = true;
                break;
            case 0xFF5D: { // QCC
                if (!have_siz) return img;
                size_t body = mlen - 2;
                int c;
                if (cs.ncomp <= 256) { c = static_cast<int>(r.u8()); body -= 1; }
                else { c = static_cast<int>(r.u16()); body -= 2; }
                if (c < 0 || c >= cs.ncomp) return img;
                if (cs.comp_qs.empty()) cs.comp_qs.assign(cs.ncomp, cs.qs);
                if (!parse_quant(r, cs.comp_qs[c], body)) return img;
                qcc_set[static_cast<size_t>(c)] = 1;
                break;
            }
            case 0xFF5F: // POC
            case 0xFF5E: // RGN
            case 0xFF60: // PPM
                return img; // unsupported
            default:
                break; // COM, TLM, PLM, CRG, ...
        }
        r.pos = end;
    }
    if (!have_siz || !have_cod || !have_qcd) return img;

    // Fill per-component defaults now that COD/QCD are final. Components an
    // early COC snapshotted before COD arrived still owe it the COD-only
    // fields (SOP/EPH markers); a QCC body is self-contained.
    if (cs.comp_cs.empty()) cs.comp_cs.assign(cs.ncomp, cs.cs);
    else
        for (int c = 0; c < cs.ncomp; c++) {
            if (!coc_set[static_cast<size_t>(c)]) cs.comp_cs[c] = cs.cs;
            else {
                cs.comp_cs[c].sop = cs.cs.sop;
                cs.comp_cs[c].eph = cs.cs.eph;
            }
        }
    if (cs.comp_qs.empty()) cs.comp_qs.assign(cs.ncomp, cs.qs);
    else
        for (int c = 0; c < cs.ncomp; c++)
            if (!qcc_set[static_cast<size_t>(c)]) cs.comp_qs[c] = cs.qs;

    const int ntx = ceil_ratio(cs.xsiz - cs.xto, cs.xt);
    const int nty = ceil_ratio(cs.ysiz - cs.yto, cs.yt);
    if (ntx <= 0 || nty <= 0 || static_cast<int64_t>(ntx) * nty > 4096) return img;
    cs.tiles.resize(static_cast<size_t>(ntx) * nty);
    tile_started.assign(cs.tiles.size(), false);
    for (int q = 0; q < nty; q++)
        for (int p = 0; p < ntx; p++) {
            Tile& t = cs.tiles[static_cast<size_t>(q) * ntx + p];
            t.x0 = std::max(cs.xto + p * cs.xt, cs.xo);
            t.y0 = std::max(cs.yto + q * cs.yt, cs.yo);
            t.x1 = std::min(cs.xto + (p + 1) * cs.xt, cs.xsiz);
            t.y1 = std::min(cs.yto + (q + 1) * cs.yt, cs.ysiz);
        }

    // Tile-parts
    while (r.ok(2)) {
        uint32_t marker = r.u16();
        if (marker == 0xFFD9) break; // EOC
        if (marker != 0xFF90) return img;
        size_t sot_start = r.pos - 2;
        if (!r.ok(10)) return img;
        r.u16(); // Lsot
        uint32_t isot = r.u16();
        uint32_t psot = r.u32();
        r.u8();  // TPsot
        r.u8();  // TNsot
        if (isot >= cs.tiles.size()) return img;
        Tile& tile = cs.tiles[isot];
        if (!tile_started[isot]) {
            tile_started[isot] = true;
            tile.progression = cs.progression;
            tile.nlayers = cs.nlayers;
            tile.comps.resize(cs.ncomp);
            for (int c = 0; c < cs.ncomp; c++) {
                tile.comps[c].cs = cs.comp_cs[c];
                tile.comps[c].qs = cs.comp_qs[c];
            }
        }
        // Tile-part header: markers until SOD. Same ordering rule as the main
        // header: a tile COC/QCC override survives a later tile COD/QCD
        // blanket write instead of being clobbered by it.
        bool ok_header = true;
        std::vector<char> t_coc(static_cast<size_t>(cs.ncomp), 0);
        std::vector<char> t_qcc(static_cast<size_t>(cs.ncomp), 0);
        while (r.ok(4)) {
            uint32_t m2 = r.u16();
            if (m2 == 0xFF93) break; // SOD
            uint32_t l2 = r.u16();
            if (l2 < 2 || !r.ok(l2 - 2)) { ok_header = false; break; }
            size_t end2 = r.pos + l2 - 2;
            switch (m2) {
                case 0xFF52: { // tile COD
                    uint32_t scod = r.u8();
                    tile.progression = static_cast<int>(r.u8());
                    tile.nlayers = static_cast<int>(r.u16());
                    cs.mct = static_cast<int>(r.u8());
                    if (tile.nlayers < 1 || tile.nlayers > 256) return img;
                    CodingStyle st;
                    st.sop = (scod & 2) != 0;
                    st.eph = (scod & 4) != 0;
                    if (!parse_cod_style(r, st, static_cast<int>(scod))) return img;
                    for (int c = 0; c < cs.ncomp; c++) {
                        if (t_coc[static_cast<size_t>(c)]) {
                            // COC carries no SOP/EPH; those follow the COD.
                            tile.comps[c].cs.sop = st.sop;
                            tile.comps[c].cs.eph = st.eph;
                        } else {
                            tile.comps[c].cs = st;
                        }
                    }
                    break;
                }
                case 0xFF53: { // tile COC
                    int c = static_cast<int>(cs.ncomp <= 256 ? r.u8() : r.u16());
                    uint32_t scoc = r.u8();
                    if (c < 0 || c >= cs.ncomp) return img;
                    CodingStyle st = tile.comps[c].cs;
                    if (!parse_cod_style(r, st, static_cast<int>(scoc & 1))) return img;
                    tile.comps[c].cs = st;
                    t_coc[static_cast<size_t>(c)] = 1;
                    break;
                }
                case 0xFF5C: { // tile QCD
                    QuantStyle q;
                    if (!parse_quant(r, q, l2 - 2)) return img;
                    for (int c = 0; c < cs.ncomp; c++)
                        if (!t_qcc[static_cast<size_t>(c)]) tile.comps[c].qs = q;
                    break;
                }
                case 0xFF5D: { // tile QCC
                    size_t body = l2 - 2;
                    int c;
                    if (cs.ncomp <= 256) { c = static_cast<int>(r.u8()); body -= 1; }
                    else { c = static_cast<int>(r.u16()); body -= 2; }
                    if (c < 0 || c >= cs.ncomp) return img;
                    if (!parse_quant(r, tile.comps[c].qs, body)) return img;
                    t_qcc[static_cast<size_t>(c)] = 1;
                    break;
                }
                case 0xFF61: // PPT
                case 0xFF5F: // POC
                    return img;
                default:
                    break; // PLT, COM, ...
            }
            r.pos = end2;
        }
        if (!ok_header) return img;
        // Body: Psot spans from the SOT marker to the end of the tile-part
        size_t body_end = psot ? sot_start + psot : len;
        if (body_end > len || body_end < r.pos) return img;
        tile.body.insert(tile.body.end(), r.p + r.pos, r.p + body_end);
        r.pos = body_end;
    }

    // Decode
    for (size_t t = 0; t < cs.tiles.size(); t++) {
        if (!tile_started[t]) return img;
        Tile& tile = cs.tiles[t];
        if (tile.x1 <= tile.x0 || tile.y1 <= tile.y0) continue;
        if (!setup_tile(cs, tile)) return img;
        if (!decode_tile_packets(cs, tile)) return img;
        for (auto& tc : tile.comps) reconstruct_component(tc);
    }

    // Assemble output (8-bit, component-interleaved)
    const int W = cs.xsiz - cs.xo, H = cs.ysiz - cs.yo;
    const int NC = cs.ncomp;
    img.width = W;
    img.height = H;
    img.components = NC;
    img.pixels.assign(static_cast<size_t>(W) * H * NC, 0);

    for (auto& tile : cs.tiles) {
        const int tw = tile.x1 - tile.x0, th = tile.y1 - tile.y0;
        if (tw <= 0 || th <= 0) continue;
        const bool rct = cs.mct && NC >= 3 && tile.comps[0].cs.transform == 1;
        const bool ict = cs.mct && NC >= 3 && tile.comps[0].cs.transform == 0;
        for (int y = 0; y < th; y++) {
            for (int x = 0; x < tw; x++) {
                const size_t si = static_cast<size_t>(y) * tw + x;
                float smp[8] = {};
                for (int c = 0; c < NC; c++)
                    smp[c] = si < tile.comps[c].plane.size()
                                 ? tile.comps[c].plane[si] : 0.0f;
                if (rct) {
                    float g = smp[0] - std::floor((smp[1] + smp[2]) / 4.0f);
                    float rr = smp[2] + g, bb = smp[1] + g;
                    smp[0] = rr; smp[1] = g; smp[2] = bb;
                } else if (ict) {
                    float yy = smp[0], cb = smp[1], cr = smp[2];
                    smp[0] = yy + 1.402f * cr;
                    smp[1] = yy - 0.34413f * cb - 0.71414f * cr;
                    smp[2] = yy + 1.772f * cb;
                }
                uint8_t* dst = img.pixels.data() +
                               (static_cast<size_t>(tile.y0 - cs.yo + y) * W +
                                (tile.x0 - cs.xo + x)) * NC;
                for (int c = 0; c < NC; c++) {
                    const int depth = cs.depth[c];
                    float v = smp[c];
                    if (!cs.sgned[c]) v += static_cast<float>(1 << (depth - 1));
                    int iv = static_cast<int>(std::lround(v));
                    const int maxv = (1 << depth) - 1;
                    if (iv < 0) iv = 0;
                    if (iv > maxv) iv = maxv;
                    if (depth > 8) iv >>= (depth - 8);
                    else if (depth < 8) iv = maxv ? iv * 255 / maxv : 0;
                    dst[c] = static_cast<uint8_t>(iv);
                }
            }
        }
    }
    return img;
}

}} // namespace jdoc::pdf_detail
