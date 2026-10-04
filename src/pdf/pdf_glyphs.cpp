// Glyph outlines from embedded font programs for the compositor.
//
// TrueType (glyf quadratic contours, composite glyphs), CFF (Type2
// charstrings, CID-keyed fonts, OpenType 'CFF ') and Type1 (eexec-encrypted
// Type1 charstrings with flex and seac). Glyph selection follows PDF 32000-1
// 9.6.6 (simple fonts) and 9.7.4 (CIDFonts, Identity CMaps only).

#include "pdf_glyphs.h"
#include "glyph_names.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace jdoc { namespace pdf_detail {

namespace {

// ── Byte access ─────────────────────────────────────────

struct Bytes {
    const uint8_t* p = nullptr;
    size_t n = 0;
    uint8_t u8(size_t o) const { return o < n ? p[o] : 0; }
    uint16_t u16(size_t o) const { return static_cast<uint16_t>((u8(o) << 8) | u8(o + 1)); }
    int16_t s16(size_t o) const { return static_cast<int16_t>(u16(o)); }
    uint32_t u32(size_t o) const {
        return (static_cast<uint32_t>(u16(o)) << 16) | u16(o + 2);
    }
};

// ── Path building ───────────────────────────────────────

struct PathBuilder {
    std::vector<PathPoint>* out;
    double x = 0, y = 0;
    bool open = false;
    // Optional affine applied to every emitted point (composite glyphs, seac).
    double m[6] = {1, 0, 0, 1, 0, 0};

    void map(double ix, double iy, double& ox, double& oy) const {
        ox = m[0] * ix + m[2] * iy + m[4];
        oy = m[1] * ix + m[3] * iy + m[5];
    }
    void close() {
        if (open) { PathPoint p{}; p.type = PathPoint::CLOSE; out->push_back(p); open = false; }
    }
    void move_to(double nx, double ny) {
        close();
        x = nx; y = ny;
        PathPoint p{}; p.type = PathPoint::MOVE; map(x, y, p.x, p.y);
        out->push_back(p);
        open = true;
    }
    void line_to(double nx, double ny) {
        if (!open) move_to(x, y);
        x = nx; y = ny;
        PathPoint p{}; p.type = PathPoint::LINE; map(x, y, p.x, p.y);
        out->push_back(p);
    }
    void curve_to(double x1, double y1, double x2, double y2, double x3, double y3) {
        if (!open) move_to(x, y);
        PathPoint p{}; p.type = PathPoint::CURVE;
        map(x1, y1, p.cx1, p.cy1);
        map(x2, y2, p.cx2, p.cy2);
        map(x3, y3, p.x, p.y);
        x = x3; y = y3;
        out->push_back(p);
    }
};

// ── TrueType ────────────────────────────────────────────

struct TrueType {
    std::vector<uint8_t> data;
    Bytes b;
    size_t glyf = 0, glyf_len = 0, loca = 0, loca_len = 0, cff = 0, cff_len = 0;
    int upem = 1000, loc_fmt = 0, nglyphs = 0;
    std::unordered_map<uint32_t, uint16_t> cmap30, cmap10, cmap31;
    bool has30 = false, has10 = false, has31 = false;
    std::unordered_map<std::string, uint16_t> post_names;

    bool parse() {
        b = {data.data(), data.size()};
        if (data.size() < 12) return false;
        uint32_t ver = b.u32(0);
        if (ver != 0x00010000 && ver != 0x74727565 /*true*/ && ver != 0x4F54544F /*OTTO*/)
            return false;
        int ntab = b.u16(4);
        size_t head = 0, maxp = 0, cmap = 0, post = 0, post_len = 0;
        for (int i = 0; i < ntab; i++) {
            size_t r = 12 + 16 * static_cast<size_t>(i);
            if (r + 16 > data.size()) break;
            uint32_t tag = b.u32(r);
            size_t off = b.u32(r + 8), len = b.u32(r + 12);
            if (off >= data.size()) continue;
            len = std::min(len, data.size() - off);
            switch (tag) {
                case 0x68656164: head = off; break;               // head
                case 0x6D617870: maxp = off; break;               // maxp
                case 0x636D6170: cmap = off; break;               // cmap
                case 0x676C7966: glyf = off; glyf_len = len; break; // glyf
                case 0x6C6F6361: loca = off; loca_len = len; break; // loca
                case 0x706F7374: post = off; post_len = len; break; // post
                case 0x43464620: cff = off; cff_len = len; break;   // 'CFF '
            }
        }
        if (head) {
            upem = b.u16(head + 18);
            if (upem < 16 || upem > 16384) upem = 1000;
            loc_fmt = b.s16(head + 50);
        }
        if (maxp) nglyphs = b.u16(maxp + 4);
        if (cmap) parse_cmap(cmap);
        if (post) parse_post(post, post_len);
        return (glyf && loca) || cff;
    }

    void read_subtable(size_t st, std::unordered_map<uint32_t, uint16_t>& m) {
        int fmt = b.u16(st);
        if (fmt == 0) {
            for (int c = 0; c < 256; c++) if (b.u8(st + 6 + c)) m[c] = b.u8(st + 6 + c);
        } else if (fmt == 4) {
            int segx2 = b.u16(st + 6);
            size_t ends = st + 14, starts = ends + segx2 + 2, deltas = starts + segx2,
                   ranges = deltas + segx2;
            for (int s = 0; s < segx2 / 2; s++) {
                uint32_t end = b.u16(ends + 2 * s), start = b.u16(starts + 2 * s);
                int16_t delta = b.s16(deltas + 2 * s);
                uint16_t ro = b.u16(ranges + 2 * s);
                if (end == 0xFFFF && start == 0xFFFF) continue;
                if (end < start || end - start > 0xFFFF) continue;
                for (uint32_t c = start; c <= end; c++) {
                    uint16_t g;
                    if (ro == 0) g = static_cast<uint16_t>(c + delta);
                    else {
                        size_t gi = ranges + 2 * s + ro + 2 * (c - start);
                        g = b.u16(gi);
                        if (g) g = static_cast<uint16_t>(g + delta);
                    }
                    if (g) m[c] = g;
                }
            }
        } else if (fmt == 6) {
            uint32_t first = b.u16(st + 6), cnt = b.u16(st + 8);
            for (uint32_t i = 0; i < cnt; i++) {
                uint16_t g = b.u16(st + 10 + 2 * i);
                if (g) m[first + i] = g;
            }
        } else if (fmt == 12) {
            uint32_t ngroups = b.u32(st + 12);
            for (uint32_t i = 0; i < ngroups && i < 65536; i++) {
                size_t gr = st + 16 + 12 * static_cast<size_t>(i);
                uint32_t sc = b.u32(gr), ec = b.u32(gr + 4), sg = b.u32(gr + 8);
                if (ec < sc || ec - sc > 0x10000) continue;
                for (uint32_t c = sc; c <= ec; c++) m[c] = static_cast<uint16_t>(sg + (c - sc));
            }
        }
    }

    void parse_cmap(size_t cmap) {
        int n = b.u16(cmap + 2);
        for (int i = 0; i < n; i++) {
            size_t r = cmap + 4 + 8 * static_cast<size_t>(i);
            int pid = b.u16(r), eid = b.u16(r + 2);
            size_t st = cmap + b.u32(r + 4);
            if (st >= data.size()) continue;
            if (pid == 3 && eid == 0) { read_subtable(st, cmap30); has30 = true; }
            else if (pid == 1 && eid == 0) { read_subtable(st, cmap10); has10 = true; }
            else if ((pid == 3 && (eid == 1 || eid == 10)) || pid == 0) {
                read_subtable(st, cmap31); has31 = true;
            }
        }
    }

    void parse_post(size_t post, size_t len) {
        if (b.u32(post) != 0x00020000 || len < 34) return;
        int n = b.u16(post + 32);
        size_t idx = post + 34;
        size_t names = idx + 2 * static_cast<size_t>(n);
        std::vector<std::string> extra;
        size_t p = names;
        while (p < post + len && p < data.size()) {
            int l = b.u8(p);
            if (p + 1 + l > data.size()) break;
            extra.emplace_back(reinterpret_cast<const char*>(&data[p + 1]), l);
            p += 1 + l;
        }
        // Macintosh standard order = WinAnsi-independent; the 258 standard
        // names are not needed for subset fonts that name their glyphs.
        for (int g = 0; g < n; g++) {
            int k = b.u16(idx + 2 * g);
            if (k >= 258 && static_cast<size_t>(k - 258) < extra.size())
                post_names.emplace(extra[k - 258], static_cast<uint16_t>(g));
        }
    }

    bool glyph_range(int gid, size_t& off, size_t& len) const {
        if (gid < 0) return false;
        size_t a, e;
        if (loc_fmt == 0) {
            if (2 * static_cast<size_t>(gid) + 4 > loca_len) return false;
            a = 2 * static_cast<size_t>(b.u16(loca + 2 * gid));
            e = 2 * static_cast<size_t>(b.u16(loca + 2 * gid + 2));
        } else {
            if (4 * static_cast<size_t>(gid) + 8 > loca_len) return false;
            a = b.u32(loca + 4 * gid);
            e = b.u32(loca + 4 * gid + 4);
        }
        if (e <= a || e > glyf_len) return false;
        off = glyf + a; len = e - a;
        return true;
    }

    // Appends the glyph's contours (font units) through pb's transform.
    bool outline(int gid, PathBuilder& pb, int depth = 0) const {
        size_t off, len;
        if (depth > 8 || !glyph_range(gid, off, len) || len < 10) return false;
        int ncont = b.s16(off);
        if (ncont >= 0) return simple(off, len, ncont, pb);
        // Composite glyph
        size_t p = off + 10;
        bool any = false;
        for (int guard = 0; guard < 64; guard++) {
            uint16_t flags = b.u16(p), comp = b.u16(p + 2);
            p += 4;
            double dx, dy;
            if (flags & 1) { dx = b.s16(p); dy = b.s16(p + 2); p += 4; }
            else { dx = static_cast<int8_t>(b.u8(p)); dy = static_cast<int8_t>(b.u8(p + 1)); p += 2; }
            double a = 1, bb = 0, c = 0, d = 1;
            auto f2 = [&](size_t o) { return b.s16(o) / 16384.0; };
            if (flags & 0x08) { a = d = f2(p); p += 2; }
            else if (flags & 0x40) { a = f2(p); d = f2(p + 2); p += 4; }
            else if (flags & 0x80) { a = f2(p); bb = f2(p + 2); c = f2(p + 4); d = f2(p + 6); p += 8; }
            if (!(flags & 0x02)) { dx = 0; dy = 0; }   // point-matching offsets: ignored
            PathBuilder sub = pb;
            double cm[6] = {a, bb, c, d, dx, dy};
            // sub.m = cm × pb.m
            sub.m[0] = cm[0] * pb.m[0] + cm[1] * pb.m[2];
            sub.m[1] = cm[0] * pb.m[1] + cm[1] * pb.m[3];
            sub.m[2] = cm[2] * pb.m[0] + cm[3] * pb.m[2];
            sub.m[3] = cm[2] * pb.m[1] + cm[3] * pb.m[3];
            sub.m[4] = cm[4] * pb.m[0] + cm[5] * pb.m[2] + pb.m[4];
            sub.m[5] = cm[4] * pb.m[1] + cm[5] * pb.m[3] + pb.m[5];
            sub.open = false;
            any |= outline(comp, sub, depth + 1);
            if (!(flags & 0x20) || p >= off + len) break;
        }
        return any;
    }

    bool simple(size_t off, size_t len, int ncont, PathBuilder& pb) const {
        if (ncont == 0) return false;
        size_t end = off + len;
        std::vector<int> ends(ncont);
        for (int i = 0; i < ncont; i++) ends[i] = b.u16(off + 10 + 2 * i);
        int npts = ends.back() + 1;
        if (npts <= 0 || npts > 20000) return false;
        size_t p = off + 10 + 2 * static_cast<size_t>(ncont);
        p += 2 + b.u16(p);                         // instructions
        std::vector<uint8_t> fl(npts);
        for (int i = 0; i < npts && p < end;) {
            uint8_t f = b.u8(p++);
            fl[i++] = f;
            if (f & 8) {
                int rep = b.u8(p++);
                while (rep-- > 0 && i < npts) fl[i++] = f;
            }
        }
        std::vector<double> xs(npts), ys(npts);
        int v = 0;
        for (int i = 0; i < npts; i++) {
            uint8_t f = fl[i];
            if (f & 2) { int d = b.u8(p++); v += (f & 16) ? d : -d; }
            else if (!(f & 16)) { v += b.s16(p); p += 2; }
            xs[i] = v;
        }
        v = 0;
        for (int i = 0; i < npts; i++) {
            uint8_t f = fl[i];
            if (f & 4) { int d = b.u8(p++); v += (f & 32) ? d : -d; }
            else if (!(f & 32)) { v += b.s16(p); p += 2; }
            ys[i] = v;
        }
        int start = 0;
        for (int c = 0; c < ncont; c++) {
            int last = ends[c];
            if (last < start || last >= npts) break;
            int n = last - start + 1;
            auto on = [&](int k) { return (fl[start + (k % n)] & 1) != 0; };
            auto X = [&](int k) { return xs[start + (k % n)]; };
            auto Y = [&](int k) { return ys[start + (k % n)]; };
            // Start on an on-curve point, or at the midpoint of two off points.
            int s0 = -1;
            for (int k = 0; k < n; k++) if (on(k)) { s0 = k; break; }
            double sx, sy;
            if (s0 >= 0) { sx = X(s0); sy = Y(s0); }
            else { s0 = 0; sx = (X(0) + X(1)) / 2; sy = (Y(0) + Y(1)) / 2; }
            pb.move_to(sx, sy);
            double cx = sx, cy = sy;
            bool start_off = !on(s0);
            for (int k = 1; k <= n; k++) {
                int i = s0 + k;
                if (on(i)) { pb.line_to(X(i), Y(i)); cx = X(i); cy = Y(i); continue; }
                double qx = X(i), qy = Y(i), ex, ey;
                if (on(i + 1)) { ex = X(i + 1); ey = Y(i + 1); k++; }
                else { ex = (qx + X(i + 1)) / 2; ey = (qy + Y(i + 1)) / 2; }
                if (k > n && start_off) { ex = sx; ey = sy; }
                pb.curve_to(cx + 2.0 / 3 * (qx - cx), cy + 2.0 / 3 * (qy - cy),
                            ex + 2.0 / 3 * (qx - ex), ey + 2.0 / 3 * (qy - ey), ex, ey);
                cx = ex; cy = ey;
            }
            pb.close();
            start = last + 1;
        }
        return true;
    }
};

// ── CFF ─────────────────────────────────────────────────

struct CffIndex {
    std::vector<size_t> off;   // count+1 absolute offsets
    size_t count() const { return off.empty() ? 0 : off.size() - 1; }
};

struct Cff {
    std::vector<uint8_t> own;  // when the CFF is the whole program
    Bytes b;
    CffIndex strings, gsubrs, charstrings;
    std::vector<CffIndex> fd_subrs;      // per FD (CID-keyed) or one entry
    std::vector<uint8_t> fdselect;       // gid -> fd
    std::vector<uint16_t> charset;       // gid -> SID (or CID)
    std::unordered_map<uint32_t, uint16_t> cid_to_gid;
    std::unordered_map<uint32_t, uint16_t> enc_code_to_gid;
    bool cid_keyed = false;
    double scale = 0.001;               // FontMatrix [0]

    size_t read_index(size_t p, CffIndex& ix) const {
        int cnt = b.u16(p);
        if (cnt == 0) { ix.off.clear(); return p + 2; }
        int os = b.u8(p + 2);
        if (os < 1 || os > 4) { ix.off.clear(); return b.n; }
        size_t base = p + 3 + static_cast<size_t>(cnt + 1) * os - 1;
        ix.off.resize(cnt + 1);
        for (int i = 0; i <= cnt; i++) {
            size_t v = 0;
            for (int k = 0; k < os; k++) v = (v << 8) | b.u8(p + 3 + static_cast<size_t>(i) * os + k);
            ix.off[i] = base + v;
        }
        return ix.off.back();
    }

    // DICT: operator -> operands
    std::unordered_map<int, std::vector<double>> read_dict(size_t a, size_t e) const {
        std::unordered_map<int, std::vector<double>> d;
        std::vector<double> ops;
        size_t p = a;
        while (p < e && p < b.n) {
            int v = b.u8(p);
            if (v <= 21) {
                int key = v;
                p++;
                if (v == 12) { key = 1200 + b.u8(p); p++; }
                d[key] = ops;
                ops.clear();
            } else if (v == 28) { ops.push_back(b.s16(p + 1)); p += 3; }
            else if (v == 29) { ops.push_back(static_cast<int32_t>(b.u32(p + 1))); p += 5; }
            else if (v == 30) {
                std::string s; p++;
                bool done = false;
                while (!done && p < e) {
                    int byte = b.u8(p++);
                    for (int nib : {byte >> 4, byte & 15}) {
                        if (nib <= 9) s += static_cast<char>('0' + nib);
                        else if (nib == 0xa) s += '.';
                        else if (nib == 0xb) s += 'E';
                        else if (nib == 0xc) s += "E-";
                        else if (nib == 0xe) s += '-';
                        else if (nib == 0xf) { done = true; break; }
                    }
                }
                ops.push_back(std::strtod(s.c_str(), nullptr));
            } else if (v >= 32 && v <= 246) { ops.push_back(v - 139); p++; }
            else if (v >= 247 && v <= 250) { ops.push_back((v - 247) * 256 + b.u8(p + 1) + 108); p += 2; }
            else if (v >= 251 && v <= 254) { ops.push_back(-(v - 251) * 256 - b.u8(p + 1) - 108); p += 2; }
            else p++;
        }
        return d;
    }

    std::string sid_name(int sid) const {
        if (sid < jdoc_glyphnames::kCffStandardStringCount)
            return jdoc_glyphnames::kCffStandardStrings[sid];
        size_t i = static_cast<size_t>(sid - jdoc_glyphnames::kCffStandardStringCount);
        if (i >= strings.count()) return {};
        return std::string(reinterpret_cast<const char*>(b.p + strings.off[i]),
                           strings.off[i + 1] - strings.off[i]);
    }

    bool parse(const uint8_t* p, size_t n) {
        b = {p, n};
        if (n < 4) return false;
        size_t pos = b.u8(2);
        CffIndex names, tops;
        pos = read_index(pos, names);
        pos = read_index(pos, tops);
        pos = read_index(pos, strings);
        read_index(pos, gsubrs);
        if (tops.count() < 1) return false;
        auto top = read_dict(tops.off[0], tops.off[1]);
        if (!top.count(17)) return false;
        read_index(static_cast<size_t>(top[17][0]), charstrings);
        size_t ng = charstrings.count();
        if (ng == 0) return false;
        if (top.count(1207) && !top[1207].empty() && top[1207][0] != 0)
            scale = std::abs(top[1207][0]);
        cid_keyed = top.count(1230) > 0;
        // charset
        charset.assign(ng, 0);
        if (top.count(15) && top[15][0] > 2) {
            size_t cp = static_cast<size_t>(top[15][0]);
            int fmt = b.u8(cp);
            size_t g = 1;
            if (fmt == 0) {
                for (; g < ng; g++) charset[g] = b.u16(cp + 1 + 2 * (g - 1));
            } else if (fmt == 1 || fmt == 2) {
                size_t q = cp + 1;
                while (g < ng && q < n) {
                    uint16_t first = b.u16(q);
                    int left = fmt == 1 ? b.u8(q + 2) : b.u16(q + 2);
                    q += fmt == 1 ? 3 : 4;
                    for (int k = 0; k <= left && g < ng; k++) charset[g++] = static_cast<uint16_t>(first + k);
                }
            }
        } else {
            for (size_t g = 1; g < ng; g++) charset[g] = static_cast<uint16_t>(g);  // ISOAdobe order
        }
        if (cid_keyed) {
            for (size_t g = 0; g < ng; g++) cid_to_gid.emplace(charset[g], static_cast<uint16_t>(g));
            CffIndex fda;
            if (top.count(1236)) read_index(static_cast<size_t>(top[1236][0]), fda);
            double fd_scale = 0;
            for (size_t i = 0; i < fda.count(); i++) {
                auto fd = read_dict(fda.off[i], fda.off[i + 1]);
                if (i == 0 && fd.count(1207) && !fd[1207].empty()) fd_scale = std::abs(fd[1207][0]);
                fd_subrs.push_back(private_subrs(fd));
            }
            if (!top.count(1207) && fd_scale > 0) scale = fd_scale;
            fdselect.assign(ng, 0);
            if (top.count(1237)) {
                size_t fp = static_cast<size_t>(top[1237][0]);
                int fmt = b.u8(fp);
                if (fmt == 0) {
                    for (size_t g = 0; g < ng; g++) fdselect[g] = b.u8(fp + 1 + g);
                } else if (fmt == 3) {
                    int nr = b.u16(fp + 1);
                    for (int r = 0; r < nr; r++) {
                        size_t rp = fp + 3 + 3 * static_cast<size_t>(r);
                        uint16_t first = b.u16(rp), next = b.u16(rp + 3);
                        uint8_t fd = b.u8(rp + 2);
                        for (uint32_t g = first; g < next && g < ng; g++) fdselect[g] = fd;
                    }
                }
            }
        } else {
            fd_subrs.push_back(private_subrs(top));
            // Built-in encoding (format 0/1); 0 = Standard, 1 = Expert.
            if (top.count(16) && top[16][0] > 1) {
                size_t ep = static_cast<size_t>(top[16][0]);
                int fmt = b.u8(ep) & 0x7f;
                if (fmt == 0) {
                    int nc = b.u8(ep + 1);
                    for (int i = 0; i < nc; i++) enc_code_to_gid[b.u8(ep + 2 + i)] = static_cast<uint16_t>(i + 1);
                } else if (fmt == 1) {
                    int nr = b.u8(ep + 1);
                    uint16_t g = 1;
                    for (int r = 0; r < nr; r++) {
                        int first = b.u8(ep + 2 + 2 * r), left = b.u8(ep + 3 + 2 * r);
                        for (int k = 0; k <= left; k++) enc_code_to_gid[first + k] = g++;
                    }
                }
            }
        }
        return true;
    }

    CffIndex private_subrs(std::unordered_map<int, std::vector<double>>& d) const {
        CffIndex s;
        if (!d.count(18) || d[18].size() < 2) return s;
        size_t size = static_cast<size_t>(d[18][0]), off = static_cast<size_t>(d[18][1]);
        auto pd = read_dict(off, off + size);
        if (pd.count(19) && !pd[19].empty()) read_index(off + static_cast<size_t>(pd[19][0]), s);
        return s;
    }

    // Glyph names are indexed on first lookup: a scan of the charset per
    // shown glyph built a string for every glyph of the font.
    mutable std::unordered_map<std::string, int> name_to_gid;
    mutable bool names_indexed = false;

    int gid_for_name(const std::string& name) const {
        if (!names_indexed) {
            name_to_gid.reserve(charset.size());
            for (size_t g = 0; g < charset.size(); g++)
                name_to_gid.emplace(sid_name(charset[g]), static_cast<int>(g));  // first wins
            names_indexed = true;
        }
        auto it = name_to_gid.find(name);
        return it == name_to_gid.end() ? -1 : it->second;
    }

    static int bias(size_t count) { return count < 1240 ? 107 : count < 33900 ? 1131 : 32768; }

    bool outline(int gid, PathBuilder& pb) const {
        if (gid < 0 || static_cast<size_t>(gid) >= charstrings.count()) return false;
        int fd = cid_keyed && static_cast<size_t>(gid) < fdselect.size() ? fdselect[gid] : 0;
        const CffIndex* lsub = static_cast<size_t>(fd) < fd_subrs.size() ? &fd_subrs[fd] : nullptr;
        T2 st{this, lsub, &pb};
        st.run(charstrings.off[gid], charstrings.off[gid + 1], 0);
        pb.close();
        return st.drew;
    }

    // Type2 charstring interpreter.
    struct T2 {
        const Cff* f;
        const CffIndex* lsub;
        PathBuilder* pb;
        std::vector<double> s;
        int nstems = 0;
        bool width_done = false, drew = false, ended = false;
        double x = 0, y = 0;
        void width_check(bool odd) { if (!width_done) { if (odd && !s.empty()) s.erase(s.begin()); width_done = true; } }
        void move(double nx, double ny) { x = nx; y = ny; pb->move_to(x, y); }
        void line(double dx, double dy) { x += dx; y += dy; pb->line_to(x, y); drew = true; }
        void curve(double a, double b2, double c, double d, double e, double g) {
            double x1 = x + a, y1 = y + b2, x2 = x1 + c, y2 = y1 + d;
            x = x2 + e; y = y2 + g;
            pb->curve_to(x1, y1, x2, y2, x, y);
            drew = true;
        }
        void run(size_t p, size_t e, int depth) {
            const Bytes& b = f->b;
            if (depth > 10) return;
            while (p < e && !ended) {
                int v = b.u8(p);
                if (v >= 32 || v == 28) {
                    if (v == 28) { s.push_back(b.s16(p + 1)); p += 3; }
                    else if (v <= 246) { s.push_back(v - 139); p++; }
                    else if (v <= 250) { s.push_back((v - 247) * 256 + b.u8(p + 1) + 108); p += 2; }
                    else if (v <= 254) { s.push_back(-(v - 251) * 256 - b.u8(p + 1) - 108); p += 2; }
                    else { s.push_back(static_cast<int32_t>(b.u32(p + 1)) / 65536.0); p += 5; }
                    if (s.size() > 96) s.erase(s.begin());
                    continue;
                }
                p++;
                size_t k = 0;
                switch (v) {
                    case 1: case 3: case 18: case 23:          // stems
                        width_check(s.size() % 2 == 1);
                        nstems += static_cast<int>(s.size() / 2); s.clear(); break;
                    case 19: case 20:                          // hintmask / cntrmask
                        width_check(s.size() % 2 == 1);
                        nstems += static_cast<int>(s.size() / 2); s.clear();
                        p += (nstems + 7) / 8; break;
                    case 21: width_check(s.size() > 2); if (s.size() >= 2) move(x + s[0], y + s[1]); s.clear(); break;
                    case 22: width_check(s.size() > 1); if (!s.empty()) move(x + s[0], y); s.clear(); break;
                    case 4:  width_check(s.size() > 1); if (!s.empty()) move(x, y + s[0]); s.clear(); break;
                    case 5: for (; k + 1 < s.size(); k += 2) line(s[k], s[k + 1]); s.clear(); break;
                    case 6: case 7: {
                        bool horiz = v == 6;
                        for (; k < s.size(); k++, horiz = !horiz) horiz ? line(s[k], 0) : line(0, s[k]);
                        s.clear(); break;
                    }
                    case 8: for (; k + 5 < s.size(); k += 6) curve(s[k], s[k+1], s[k+2], s[k+3], s[k+4], s[k+5]); s.clear(); break;
                    case 24:                                   // rcurveline
                        if (s.size() >= 8) {
                            for (; k + 6 + 2 <= s.size(); k += 6) curve(s[k], s[k+1], s[k+2], s[k+3], s[k+4], s[k+5]);
                            if (k + 1 < s.size()) line(s[k], s[k + 1]);
                        }
                        s.clear(); break;
                    case 25:                                   // rlinecurve
                        if (s.size() >= 8) {
                            for (; k + 2 + 6 <= s.size(); k += 2) line(s[k], s[k + 1]);
                            if (k + 5 < s.size()) curve(s[k], s[k+1], s[k+2], s[k+3], s[k+4], s[k+5]);
                        }
                        s.clear(); break;
                    case 26: {                                 // vvcurveto
                        double dx1 = 0;
                        if (s.size() % 4 == 1) { dx1 = s[0]; k = 1; }
                        for (; k + 3 < s.size(); k += 4) { curve(dx1, s[k], s[k+1], s[k+2], 0, s[k+3]); dx1 = 0; }
                        s.clear(); break;
                    }
                    case 27: {                                 // hhcurveto
                        double dy1 = 0;
                        if (s.size() % 4 == 1) { dy1 = s[0]; k = 1; }
                        for (; k + 3 < s.size(); k += 4) { curve(s[k], dy1, s[k+1], s[k+2], s[k+3], 0); dy1 = 0; }
                        s.clear(); break;
                    }
                    case 30: case 31: {                        // vhcurveto / hvcurveto
                        bool h = v == 31;
                        const size_t n = s.size();
                        while (k + 4 <= n) {
                            double extra = (n - k == 5) ? s[k + 4] : 0;   // last curve's odd argument
                            if (h) curve(s[k], 0, s[k+1], s[k+2], extra, s[k+3]);
                            else curve(0, s[k], s[k+1], s[k+2], s[k+3], extra);
                            k += 4; h = !h;
                        }
                        s.clear(); break;
                    }
                    case 10: case 29: {                        // callsubr / callgsubr
                        if (s.empty()) break;
                        const CffIndex* ix = v == 10 ? lsub : &f->gsubrs;
                        if (!ix) { s.clear(); break; }
                        int i = static_cast<int>(s.back()) + bias(ix->count());
                        s.pop_back();
                        if (i >= 0 && static_cast<size_t>(i) < ix->count())
                            run(ix->off[i], ix->off[i + 1], depth + 1);
                        break;
                    }
                    case 11: return;                           // return
                    case 14:                                   // endchar (seac form)
                        width_check(s.size() == 1 || s.size() == 5);
                        if (s.size() >= 4) seac(s[s.size()-4], s[s.size()-3], static_cast<int>(s[s.size()-2]), static_cast<int>(s[s.size()-1]));
                        pb->close(); ended = true; return;
                    case 12: {
                        int w = b.u8(p++);
                        auto flex = [&](const std::vector<double>& d) {
                            curve(d[0], d[1], d[2], d[3], d[4], d[5]);
                            curve(d[6], d[7], d[8], d[9], d[10], d[11]);
                        };
                        if (w == 35 && s.size() >= 12) flex({s[0],s[1],s[2],s[3],s[4],s[5],s[6],s[7],s[8],s[9],s[10],s[11]});
                        else if (w == 34 && s.size() >= 7)    // hflex
                            flex({s[0],0,s[1],s[2],s[3],0, s[4],0,s[5],-s[2],s[6],0});
                        else if (w == 36 && s.size() >= 9)    // hflex1
                            flex({s[0],s[1],s[2],s[3],s[4],0, s[5],0,s[6],s[7],s[8],-(s[1]+s[3]+s[7])});
                        else if (w == 37 && s.size() >= 11) { // flex1
                            double dx = s[0]+s[2]+s[4]+s[6]+s[8], dy = s[1]+s[3]+s[5]+s[7]+s[9];
                            double l, m;
                            if (std::abs(dx) > std::abs(dy)) { l = s[10]; m = -dy; }
                            else { l = -dx; m = s[10]; }
                            flex({s[0],s[1],s[2],s[3],s[4],s[5],s[6],s[7],s[8],s[9],l,m});
                        }
                        s.clear(); break;
                    }
                    default: s.clear(); break;
                }
            }
        }
        void seac(double adx, double ady, int bchar, int achar) {
            if (bchar < 0 || bchar > 255 || achar < 0 || achar > 255) return;
            const char* bn = jdoc_glyphnames::kStandardEncoding[bchar];
            const char* an = jdoc_glyphnames::kStandardEncoding[achar];
            if (!bn || !an) return;
            PathBuilder base = *pb; base.open = false;
            f->outline(f->gid_for_name(bn), base);
            PathBuilder acc = *pb; acc.open = false;
            acc.m[4] += adx * pb->m[0] + ady * pb->m[2];
            acc.m[5] += adx * pb->m[1] + ady * pb->m[3];
            f->outline(f->gid_for_name(an), acc);
            drew = true;
        }
    };
};

// ── Type1 ───────────────────────────────────────────────

struct Type1 {
    std::vector<std::vector<uint8_t>> subrs;
    std::unordered_map<std::string, std::vector<uint8_t>> chars;
    double scale = 0.001;

    static void decrypt(std::vector<uint8_t>& d, uint16_t r, size_t skip) {
        std::vector<uint8_t> o;
        o.reserve(d.size());
        for (size_t i = 0; i < d.size(); i++) {
            uint8_t c = d[i];
            uint8_t pch = static_cast<uint8_t>(c ^ (r >> 8));
            r = static_cast<uint16_t>((c + r) * 52845 + 22719);
            if (i >= skip) o.push_back(pch);
        }
        d.swap(o);
    }

    bool parse(std::vector<uint8_t> data, size_t len1) {
        // PFB segments
        if (data.size() > 6 && data[0] == 0x80) {
            std::vector<uint8_t> clear, enc;
            size_t p = 0;
            while (p + 6 <= data.size() && data[p] == 0x80 && data[p + 1] != 3) {
                int type = data[p + 1];
                size_t l = data[p + 2] | (data[p + 3] << 8) | (data[p + 4] << 16) | (static_cast<size_t>(data[p + 5]) << 24);
                p += 6;
                if (p + l > data.size()) break;
                (type == 1 ? clear : enc).insert((type == 1 ? clear : enc).end(), data.begin() + p, data.begin() + p + l);
                p += l;
            }
            len1 = clear.size();
            clear.insert(clear.end(), enc.begin(), enc.end());
            data.swap(clear);
        }
        if (len1 == 0 || len1 >= data.size()) {
            static const char kEexec[] = "eexec";
            auto it = std::search(data.begin(), data.end(), kEexec, kEexec + 5);
            if (it == data.end()) return false;
            len1 = static_cast<size_t>(it - data.begin()) + 5;
            while (len1 < data.size() && (data[len1] == '\r' || data[len1] == '\n' || data[len1] == ' ')) len1++;
        }
        // FontMatrix from the cleartext part
        {
            std::string clear(data.begin(), data.begin() + len1);
            auto fm = clear.find("/FontMatrix");
            if (fm != std::string::npos) {
                auto br = clear.find_first_of("[{", fm);
                if (br != std::string::npos) {
                    double v = std::strtod(clear.c_str() + br + 1, nullptr);
                    if (v != 0) scale = std::abs(v);
                }
            }
        }
        std::vector<uint8_t> enc(data.begin() + len1, data.end());
        // Hex-encoded eexec section
        bool hex = enc.size() >= 4;
        for (size_t i = 0; i < 4 && i < enc.size(); i++) if (!std::isxdigit(enc[i])) hex = false;
        if (hex) {
            std::vector<uint8_t> bin;
            int hi = -1;
            for (uint8_t c : enc) {
                int v = std::isdigit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (v < 0) continue;
                if (hi < 0) hi = v; else { bin.push_back(static_cast<uint8_t>(hi * 16 + v)); hi = -1; }
            }
            enc.swap(bin);
        }
        decrypt(enc, 55665, 4);
        const std::string s(enc.begin(), enc.end());
        int leniv = 4;
        auto li = s.find("/lenIV");
        if (li != std::string::npos) leniv = std::atoi(s.c_str() + li + 6);
        auto read_bin = [&](size_t& p, int n, std::vector<uint8_t>& out) {
            // after "<n> RD " or "<n> -| "
            while (p < s.size() && s[p] == ' ') p++;
            while (p < s.size() && s[p] != ' ') p++;     // RD token
            p++;                                          // single space
            if (n < 0 || p + n > s.size()) return false;
            out.assign(enc.begin() + p, enc.begin() + p + n);
            if (leniv >= 0) decrypt(out, 4330, static_cast<size_t>(leniv));
            p += n;
            return true;
        };
        auto si = s.find("/Subrs");
        if (si != std::string::npos) {
            size_t p = si + 6;
            int cnt = std::atoi(s.c_str() + p);
            if (cnt > 0 && cnt < 65536) subrs.resize(cnt);
            for (int k = 0; k < cnt; k++) {
                size_t d = s.find("dup ", p);
                if (d == std::string::npos) break;
                p = d + 4;
                int idx = std::atoi(s.c_str() + p);
                while (p < s.size() && s[p] != ' ') p++;
                int n = std::atoi(s.c_str() + p);
                while (p < s.size() && s[p] == ' ') p++;
                while (p < s.size() && s[p] != ' ') p++;  // length digits
                std::vector<uint8_t> cs;
                if (!read_bin(p, n, cs)) break;
                if (idx >= 0 && idx < cnt) subrs[idx] = std::move(cs);
            }
        }
        auto ci = s.find("/CharStrings");
        if (ci == std::string::npos) return false;
        size_t p = s.find("begin", ci);
        if (p == std::string::npos) return false;
        p += 5;
        while (p < s.size()) {
            size_t sl = s.find('/', p);
            if (sl == std::string::npos) break;
            size_t ne = sl + 1;
            while (ne < s.size() && s[ne] != ' ' && s[ne] != '\n' && s[ne] != '\r') ne++;
            std::string name = s.substr(sl + 1, ne - sl - 1);
            p = ne;
            while (p < s.size() && s[p] == ' ') p++;
            if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) {
                if (s.compare(p, 3, "end") == 0) break;
                continue;
            }
            int n = std::atoi(s.c_str() + p);
            while (p < s.size() && s[p] != ' ') p++;
            std::vector<uint8_t> cs;
            if (!read_bin(p, n, cs)) break;
            chars.emplace(std::move(name), std::move(cs));
        }
        return !chars.empty();
    }

    bool outline(const std::string& name, PathBuilder& pb, int depth = 0) const {
        auto it = chars.find(name);
        if (it == chars.end() || depth > 2) return false;
        T1 st{this, &pb, depth};
        st.run(it->second, 0);
        pb.close();
        return st.drew;
    }

    struct T1 {
        const Type1* f;
        PathBuilder* pb;
        int depth;
        std::vector<double> s, ps;   // operand stack, PostScript stack (othersubr results)
        double x = 0, y = 0, sbx = 0;
        bool drew = false, ended = false, flexing = false;
        std::vector<std::pair<double, double>> flexpts;
        void moveto(double nx, double ny) {
            x = nx; y = ny;
            if (flexing) { flexpts.push_back({x, y}); return; }
            pb->move_to(x, y);
        }
        void run(const std::vector<uint8_t>& c, int lvl) {
            if (lvl > 10) return;
            size_t p = 0;
            while (p < c.size() && !ended) {
                int v = c[p++];
                if (v >= 32) {
                    if (v <= 246) s.push_back(v - 139);
                    else if (v <= 250 && p < c.size()) s.push_back((v - 247) * 256 + c[p++] + 108);
                    else if (v <= 254 && p < c.size()) s.push_back(-(v - 251) * 256 - c[p++] - 108);
                    else if (v == 255 && p + 4 <= c.size()) {
                        int32_t n = static_cast<int32_t>((c[p] << 24) | (c[p + 1] << 16) | (c[p + 2] << 8) | c[p + 3]);
                        s.push_back(n); p += 4;
                    }
                    continue;
                }
                auto arg = [&](size_t i) { return i < s.size() ? s[i] : 0.0; };
                switch (v) {
                    case 13: sbx = arg(0); x = sbx; y = 0; s.clear(); break;      // hsbw
                    case 21: moveto(x + arg(0), y + arg(1)); s.clear(); break;   // rmoveto
                    case 22: moveto(x + arg(0), y); s.clear(); break;            // hmoveto
                    case 4:  moveto(x, y + arg(0)); s.clear(); break;            // vmoveto
                    case 5: x += arg(0); y += arg(1); pb->line_to(x, y); drew = true; s.clear(); break;
                    case 6: x += arg(0); pb->line_to(x, y); drew = true; s.clear(); break;
                    case 7: y += arg(0); pb->line_to(x, y); drew = true; s.clear(); break;
                    case 8: {
                        double x1 = x + arg(0), y1 = y + arg(1), x2 = x1 + arg(2), y2 = y1 + arg(3);
                        x = x2 + arg(4); y = y2 + arg(5);
                        pb->curve_to(x1, y1, x2, y2, x, y); drew = true; s.clear(); break;
                    }
                    case 30: {                                                   // vhcurveto
                        double x1 = x, y1 = y + arg(0), x2 = x1 + arg(1), y2 = y1 + arg(2);
                        x = x2 + arg(3); y = y2;
                        pb->curve_to(x1, y1, x2, y2, x, y); drew = true; s.clear(); break;
                    }
                    case 31: {                                                   // hvcurveto
                        double x1 = x + arg(0), y1 = y, x2 = x1 + arg(1), y2 = y1 + arg(2);
                        x = x2; y = y2 + arg(3);
                        pb->curve_to(x1, y1, x2, y2, x, y); drew = true; s.clear(); break;
                    }
                    case 9: pb->close(); s.clear(); break;                       // closepath
                    case 10: {                                                   // callsubr
                        if (s.empty()) break;
                        int i = static_cast<int>(s.back()); s.pop_back();
                        if (i >= 0 && static_cast<size_t>(i) < f->subrs.size()) run(f->subrs[i], lvl + 1);
                        break;
                    }
                    case 11: return;                                             // return
                    case 14: pb->close(); ended = true; return;                  // endchar
                    case 1: case 3: s.clear(); break;                            // hstem / vstem
                    case 12: {
                        if (p >= c.size()) return;
                        int w = c[p++];
                        if (w == 7) { sbx = arg(0); x = sbx; y = arg(1); s.clear(); }            // sbw
                        else if (w == 12) {                                                      // div
                            if (s.size() >= 2) { double d = s.back(); s.pop_back(); s.back() = d != 0 ? s.back() / d : 0; }
                        } else if (w == 6) {                                                     // seac
                            seac(arg(0), arg(1), arg(2), static_cast<int>(arg(3)), static_cast<int>(arg(4)));
                            ended = true; return;
                        } else if (w == 16) {                                                    // callothersubr
                            if (s.size() < 2) { s.clear(); break; }
                            int other = static_cast<int>(s.back()); s.pop_back();
                            int n = static_cast<int>(s.back()); s.pop_back();
                            std::vector<double> a;
                            for (int k = 0; k < n && !s.empty(); k++) { a.push_back(s.back()); s.pop_back(); }
                            if (other == 1) { flexing = true; flexpts.clear(); }
                            else if (other == 0 && flexing) {
                                flexing = false;
                                if (flexpts.size() >= 7) {
                                    auto& q = flexpts;
                                    pb->curve_to(q[1].first, q[1].second, q[2].first, q[2].second, q[3].first, q[3].second);
                                    pb->curve_to(q[4].first, q[4].second, q[5].first, q[5].second, q[6].first, q[6].second);
                                    x = q[6].first; y = q[6].second; drew = true;
                                }
                                // othersubr 0 leaves the end point for two pops
                                ps.clear(); ps.push_back(y); ps.push_back(x);
                                break;
                            }
                            ps = a;   // others (hint replacement 3, 2): echo arguments to pop
                        } else if (w == 17) {                                                    // pop
                            if (!ps.empty()) { s.push_back(ps.back()); ps.pop_back(); }
                        } else if (w == 33) { x = arg(0); y = arg(1); s.clear(); }               // setcurrentpoint
                        else s.clear();
                        break;
                    }
                    default: s.clear(); break;
                }
            }
        }
        // seac asb adx ady bchar achar: the accent's side bearing point lands
        // at (adx - asb + base sbx, ady) relative to the base glyph's origin.
        void seac(double asb, double adx, double ady, int bchar, int achar) {
            if (bchar < 0 || bchar > 255 || achar < 0 || achar > 255) return;
            const char* bn = jdoc_glyphnames::kStandardEncoding[bchar];
            const char* an = jdoc_glyphnames::kStandardEncoding[achar];
            if (!bn || !an) return;
            PathBuilder base = *pb; base.open = false;
            f->outline(bn, base, depth + 1);
            PathBuilder acc = *pb; acc.open = false;
            double ox = adx - asb;
            acc.m[4] += ox * pb->m[0] + ady * pb->m[2];
            acc.m[5] += ox * pb->m[1] + ady * pb->m[3];
            f->outline(an, acc, depth + 1);
            drew = true;
        }
    };
};

}  // namespace

// ── Glyph selection ─────────────────────────────────────

struct GlyphSource::Impl {
    const PdfFont* font = nullptr;
    TrueType tt;
    Cff cff;
    Type1 t1;
    int kind = 0;                 // 1 Type1, 2 TrueType outlines, 3 CFF outlines
    bool tt_cmap = false;         // OpenType CFF selected through the sfnt cmap
    std::vector<uint16_t> cid_to_gid;

    std::string glyph_name(uint32_t code) const {
        auto d = font->differences.find(static_cast<int>(code));
        if (d != font->differences.end()) return d->second;
        if (code < 256) {
            const char* n = nullptr;
            if (font->named_base_encoding == 1) n = jdoc_glyphnames::kWinAnsiEncoding[code];
            else if (font->named_base_encoding == 2) n = jdoc_glyphnames::kMacRomanEncoding[code];
            else if (font->named_base_encoding == 3) n = jdoc_glyphnames::kStandardEncoding[code];
            if (n) return n;
            auto bi = font->builtin_names.find(static_cast<int>(code));
            if (bi != font->builtin_names.end()) return bi->second;
            if (font->named_base_encoding == 0 && jdoc_glyphnames::kStandardEncoding[code])
                return jdoc_glyphnames::kStandardEncoding[code];
        }
        return {};
    }

    int cid_gid(uint32_t code) const {
        uint32_t cid = code;
        if (!cid_to_gid.empty()) return cid < cid_to_gid.size() ? cid_to_gid[cid] : -1;
        return static_cast<int>(cid);
    }

    int truetype_gid(uint32_t code) const {
        auto look = [](const std::unordered_map<uint32_t, uint16_t>& m, uint32_t k) {
            auto it = m.find(k); return it == m.end() ? -1 : static_cast<int>(it->second);
        };
        if (font->cid_font) return cid_gid(code);
        int g = -1;
        bool names_first = !font->symbolic && tt.has31;
        if (names_first) {
            std::string n = glyph_name(code);
            uint32_t u = n.empty() ? 0 : glyph_name_to_unicode(n);
            if (!u) u = font->decode_char(code);
            if (u) g = look(tt.cmap31, u);
            if (g < 0 && !n.empty()) {
                auto pn = tt.post_names.find(n);
                if (pn != tt.post_names.end()) g = pn->second;
            }
        }
        if (g < 0 && tt.has30)
            for (uint32_t base : {0u, 0xF000u, 0xF100u, 0xF200u})
                if ((g = look(tt.cmap30, base + code)) >= 0) break;
        if (g < 0 && tt.has10) g = look(tt.cmap10, code);
        if (g < 0 && tt.has31) g = look(tt.cmap31, font->decode_char(code));
        if (g < 0 && !tt.has30 && !tt.has10 && !tt.has31) g = static_cast<int>(code);
        return g;
    }

    int cff_gid(uint32_t code) const {
        if (cff.cid_keyed) {
            if (!font->cid_font) return -1;
            auto it = cff.cid_to_gid.find(code);
            return it == cff.cid_to_gid.end() ? -1 : it->second;
        }
        if (font->cid_font) return cid_gid(code);
        if (tt_cmap) return truetype_gid(code);
        bool pdf_names = font->differences.count(static_cast<int>(code)) || font->named_base_encoding;
        if (!pdf_names) {
            auto e = cff.enc_code_to_gid.find(code);
            if (e != cff.enc_code_to_gid.end()) return e->second;
        }
        std::string n = glyph_name(code);
        return n.empty() ? -1 : cff.gid_for_name(n);
    }
};

GlyphSource::GlyphSource(PdfDoc& doc, const PdfFont& font) : impl_(new Impl) {
    impl_->font = &font;
    if (font.program_ref < 0 || !font.program_kind) return;
    PdfObj st = doc.get_obj(font.program_ref);
    if (!st.is_stream()) return;
    std::vector<uint8_t> prog = doc.decode_stream(st, font.program_ref, font.program_gen);
    if (prog.empty()) return;
    if (font.cid_font && font.cid_to_gid_ref >= 0) {
        PdfObj m = doc.get_obj(font.cid_to_gid_ref);
        if (m.is_stream()) {
            auto raw = doc.decode_stream(m, font.cid_to_gid_ref, 0);
            impl_->cid_to_gid.resize(raw.size() / 2);
            for (size_t i = 0; i + 1 < raw.size(); i += 2)
                impl_->cid_to_gid[i / 2] = static_cast<uint16_t>((raw[i] << 8) | raw[i + 1]);
        }
    }
    switch (font.program_kind) {
        case 1: {
            size_t len1 = static_cast<size_t>(std::max(0, doc.resolve(st.get("Length1")).as_int()));
            if (impl_->t1.parse(std::move(prog), len1)) impl_->kind = 1;
            break;
        }
        case 2: case 4: {
            impl_->tt.data = std::move(prog);
            if (!impl_->tt.parse()) break;
            if (impl_->tt.glyf && impl_->tt.loca) impl_->kind = 2;
            else if (impl_->tt.cff &&
                     impl_->cff.parse(impl_->tt.data.data() + impl_->tt.cff, impl_->tt.cff_len)) {
                impl_->kind = 3;
                impl_->tt_cmap = true;
            }
            break;
        }
        case 3: {
            impl_->cff.own = std::move(prog);
            if (impl_->cff.parse(impl_->cff.own.data(), impl_->cff.own.size())) impl_->kind = 3;
            break;
        }
    }
}

GlyphSource::~GlyphSource() = default;

bool GlyphSource::ok() const { return impl_ && impl_->kind != 0; }

bool GlyphSource::outline(uint32_t code, std::vector<PathPoint>& out) const {
    if (!ok()) return false;
    out.clear();
    PathBuilder pb;
    pb.out = &out;
    const Impl& m = *impl_;
    bool drew = false;
    if (m.kind == 2) {
        double s = 1.0 / m.tt.upem;
        pb.m[0] = s; pb.m[3] = s;
        drew = m.tt.outline(m.truetype_gid(code), pb);
    } else if (m.kind == 3) {
        pb.m[0] = m.cff.scale; pb.m[3] = m.cff.scale;
        drew = m.cff.outline(m.cff_gid(code), pb);
    } else if (m.kind == 1) {
        pb.m[0] = m.t1.scale; pb.m[3] = m.t1.scale;
        std::string n = m.glyph_name(code);
        drew = !n.empty() && m.t1.outline(n, pb);
    }
    pb.close();
    return drew && !out.empty();
}

}} // namespace jdoc::pdf_detail
