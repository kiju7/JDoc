#pragma once
// pdf_content.h — internal: content-stream parse vocabulary and line layout.
#include "pdf_core.h"
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace jdoc { namespace pdf_detail {

struct TextChar {
    double x, y;                     // glyph origin, page space
    double left, right, top, bot;    // page-space AABB of the glyph box
    double font_size;
    uint32_t unicode;
    bool is_bold;
    bool is_italic;
    // Writing direction quantized to 15° steps (0 = upright, 6 = 90°, 12 =
    // 180°, 18 = 270°). CAD drawings label vertical dimensions with rotated
    // text; chars_to_lines groups each direction separately so a rotated run
    // reads along its own baseline instead of being scattered across the
    // page's rows. Fits the padding after the flags — TextChar stays 64 bytes.
    int16_t rot;
};

struct PathPoint {
    double x, y;
    enum Type { MOVE, LINE, CURVE, CLOSE } type;
    double cx1, cy1, cx2, cy2; // for CURVE
};

// Semantic of the colorspace selected by cs/CS, driving sc/scn interpretation.
struct CsInfo {
    enum Kind { NUMERIC, CMYK4, TINT, INDEXED } kind = NUMERIC;
    // TINT: 256-entry tint→RGB ramp; INDEXED: palette RGB entries
    std::vector<std::array<uint8_t, 3>> lut;
};

// "No clip" sentinel for the axis-aligned clip rect carried through the
// graphics state and onto recorded paths/placements. Real page coordinates
// never approach it.
inline constexpr double kClipUnbounded = 1e30;

struct GfxState {
    double ctm[6] = {1, 0, 0, 1, 0, 0};  // a b c d e f
    double text_mat[6] = {1, 0, 0, 1, 0, 0};
    double line_mat[6] = {1, 0, 0, 1, 0, 0};
    double font_size = 12;
    double word_spacing = 0;
    double char_spacing = 0;
    double h_scaling = 100;
    double text_rise = 0;
    double text_leading = 0;
    int render_mode = 0;   // Tr: 2/6 = fill+stroke (faux bold in HWP exports)
    PdfFont* font = nullptr;

    // Graphics state for paths
    double stroke_r = 0, stroke_g = 0, stroke_b = 0;
    double fill_r = 0, fill_g = 0, fill_b = 0;
    double fill_alpha = 1, stroke_alpha = 1; // ExtGState /ca and /CA
    std::shared_ptr<const CsInfo> fill_cs, stroke_cs; // active cs/CS for sc/scn
    // Last clip path set by W/W*. Fills whose bbox fully covers the clip are
    // replaced by it (the "clip to shape, paint a rect" idiom); shape-exact
    // clipping is not modeled, but the running axis-aligned intersection of
    // every committed clip's bbox is (clip_* below) — exact for rectangular
    // clips, a conservative superset for shaped ones. q/Q scope it by copy.
    std::shared_ptr<const std::vector<PathPoint>> clip_path;
    double clip_x0 = -kClipUnbounded, clip_y0 = -kClipUnbounded;
    double clip_x1 = kClipUnbounded, clip_y1 = kClipUnbounded;
    // Shading dict of the active fill pattern (scn /P with PatternType 2);
    // fills decompose into gradient strips while it is set.
    std::shared_ptr<const PdfObj> fill_shading;
    double line_width = 1;
    int line_cap = 0, line_join = 0;
    double miter_limit = 10;
    bool in_text = false;
};

// Hot per-glyph helpers: kept inline so the TU split preserves inlining.
inline void mat_multiply(double* out, const double* a, const double* b) {
    double r[6];
    r[0] = a[0]*b[0] + a[1]*b[2];
    r[1] = a[0]*b[1] + a[1]*b[3];
    r[2] = a[2]*b[0] + a[3]*b[2];
    r[3] = a[2]*b[1] + a[3]*b[3];
    r[4] = a[4]*b[0] + a[5]*b[2] + b[4];
    r[5] = a[4]*b[1] + a[5]*b[3] + b[5];
    std::memcpy(out, r, sizeof(r));
}

inline void transform_point(const double* m, double x, double y, double& ox, double& oy) {
    ox = m[0]*x + m[2]*y + m[4];
    oy = m[1]*x + m[3]*y + m[5];
}

// How much a matrix scales a stroke pen. Line width is a user-space quantity,
// so it has to travel through the same transform as the geometry. A non-uniform
// matrix turns the round pen elliptical; averaging the two axis lengths is the
// usual stand-in for the width a viewer draws.
inline double ctm_pen_scale(const double* m) {
    return (std::hypot(m[0], m[1]) + std::hypot(m[2], m[3])) / 2.0;
}

struct PdfLineSegment {
    float x0, y0, x1, y1;
    bool is_horizontal() const { return std::abs(y1 - y0) < 2.0f; }
    bool is_vertical()   const { return std::abs(x1 - x0) < 2.0f; }
};

// Axis-aligned filled rectangle (cell shading, highlight bands). Kept apart
// from PdfLineSegment: shading edges are weaker evidence than drawn rules and
// must not enter the line pools directly.
struct PdfFillRect {
    float x0, y0, x1, y1;      // normalized: x0<x1, y0<y1
    float r, g, b;
};

struct ImagePlacement {
    int xobj_ref = -1;
    std::string xobj_name;
    // Inline image (BI…ID…EI): a synthesized stream object carrying the
    // normalized dict and payload; null for XObject placements.
    std::shared_ptr<const PdfObj> inline_img;
    double ctm[6];
    double fill_r = 0, fill_g = 0, fill_b = 0; // fill color for ImageMask
    double alpha = 1; // ExtGState /ca in force at the Do (watermark fades)
    // Clip rect in force at the Do (page space, x0 y0 x1 y1); the compositor
    // narrows its destination ranges to it.
    float clip[4] = {-1e30f, -1e30f, 1e30f, 1e30f};
    int seq = 0; // draw order shared with RenderPath (z-order for compositing)
};

struct RenderPath {
    std::vector<PathPoint> points;
    double fill_r, fill_g, fill_b;
    double stroke_r, stroke_g, stroke_b;
    double fill_alpha = 1, stroke_alpha = 1; // ExtGState /ca, /CA
    double line_width;
    bool do_fill, do_stroke;
    bool even_odd = false; // f*/B*/b*: even-odd fill rule (default nonzero)
    float clip[4] = {-1e30f, -1e30f, 1e30f, 1e30f}; // page-space clip rect
    int seq = 0; // draw order shared with ImagePlacement
};

struct ContentParseResult {
    std::vector<TextChar> chars;
    std::vector<PdfLineSegment> segments;
    std::vector<PdfFillRect> fill_rects; // sizable pure-fill rects (cell shading)
    std::vector<ImagePlacement> images;
    std::vector<RenderPath> paths; // for vector rendering
    int draw_ops = 0; // total paths+images recorded (seq offset for nested forms)
    int visible_text_chars = 0; // chars emitted outside Tr 3/7 (invisible text)
    int inline_images = 0;        // BI…ID…EI images consumed
    int inline_scan_bailouts = 0; // EI never found; rest of stream skipped
    int shading_unsupported = 0;  // sh/pattern types beyond axial+radial
    int shading_paths = 0;        // gradient strips in `paths` (not drawings)
};

struct TextLine {
    std::string text;
    double font_size = 0;
    bool is_bold = false;
    bool is_italic = false;
    bool is_column_split = false;
    double y_center = 0;
    double x_left = 1e9;
    double x_right = 0;
    // Writing direction of the run this line came from, as TextChar::rot.
    // The geometry above is page-space whatever the direction, so without
    // this a vertical caption whose page-space midpoint lands on a body
    // line's baseline reads as part of that line.
    int16_t rot = 0;
};

// ── Reading order for rotated runs ──────────────────────
// Glyphs are ordered along their own writing direction rather than along the
// page: a 180° run advances toward -x, so comparing left-to-right spells it
// backwards. CAD title blocks repeat the drawing number upside down along the
// sheet's top edge, and that is the shape this exists for. Cell assembly lives
// in two places (ruled tables in PageCharCache, borderless ones in
// pdf_tables.cpp), so the projection is shared rather than written twice.
//
// `rot` is TextChar::rot: 15° steps, 0 = upright. The four axis-aligned cases
// are spelled out because sin(180°) through <cmath> is 1.2e-16, not 0, and
// that residue is enough to reorder glyphs that share a baseline.
inline void writing_axes(int16_t rot, double& c, double& s) {
    int q = ((rot % 24) + 24) % 24;
    switch (q) {
        case 0:  c =  1; s =  0; return;
        case 6:  c =  0; s =  1; return;
        case 12: c = -1; s =  0; return;
        case 18: c =  0; s = -1; return;
    }
    double th = q * (15.0 * 3.14159265358979323846 / 180.0);
    c = std::cos(th);
    s = std::sin(th);
}

// Baseline index: larger reads earlier, the way page-space y does for upright
// text (where this returns y unchanged).
//
// Known limitation: `rot` is quantized from the advance direction alone, so a
// mirrored matrix (negative determinant, e.g. `-1 0 0 1 Tm`) is indistinguish-
// able from a half turn even though its up-vector still points at +y. Such a
// run reads its characters in the right order but stacks its lines bottom-up.
// Telling the two apart needs a flag TextChar has no room for — it is exactly
// 64 bytes with no padding left — and no corpus or CAD sample exercises it.
inline double text_across(int16_t rot, double x, double y) {
    double c, s;
    writing_axes(rot, c, s);
    return y * c - x * s;
}

// Extent along the glyph advance. The glyph box is an axis-aligned page
// rectangle, so project all four corners; upright text gets [left, right].
inline void text_along_span(int16_t rot, double left, double right,
                            double top, double bot, double& lo, double& hi) {
    double c, s;
    writing_axes(rot, c, s);
    double v0 = left  * c + bot * s;
    double v1 = left  * c + top * s;
    double v2 = right * c + bot * s;
    double v3 = right * c + top * s;
    lo = std::min(std::min(v0, v1), std::min(v2, v3));
    hi = std::max(std::max(v0, v1), std::max(v2, v3));
}

struct PageCharCache {
    struct CharInfo {
        double x, y;
        double left, right, top, bot;
        double font_size;
        unsigned int unicode;
        int16_t rot;   // writing direction, as TextChar::rot
    };
    std::vector<CharInfo> chars;
    std::vector<size_t> y_sorted;

    void build(const std::vector<TextChar>& text_chars) {
        chars.reserve(text_chars.size());
        for (auto& tc : text_chars) {
            if (tc.unicode == 0 || tc.unicode == '\r' || tc.unicode == '\n' || tc.unicode == 0xFFFD) continue;
            chars.push_back({tc.x, tc.y, tc.left, tc.right, tc.top, tc.bot, tc.font_size, tc.unicode, tc.rot});
        }
        y_sorted.resize(chars.size());
        for (size_t i = 0; i < chars.size(); i++) y_sorted[i] = i;
        std::stable_sort(y_sorted.begin(), y_sorted.end(),
            [this](size_t a, size_t b) { return chars[a].y < chars[b].y; });
    }

    std::string get_text_in_rect(double left, double top, double right, double bottom) const {
        double rect_top = std::max(top, bottom);
        double rect_bot = std::min(top, bottom);
        double y_lo = rect_bot + 0.5, y_hi = rect_top - 0.5;
        auto lo_it = std::lower_bound(y_sorted.begin(), y_sorted.end(), y_lo,
            [this](size_t idx, double val) { return chars[idx].y < val; });
        auto hi_it = std::upper_bound(lo_it, y_sorted.end(), y_hi,
            [this](double val, size_t idx) { return val < chars[idx].y; });
        // Include a char if its horizontal center falls inside [left, right).
        // Outer cell edges get a small extra tolerance so glyphs that touch
        // the column boundary line are not dropped.
        std::vector<size_t> matches;
        for (auto it = lo_it; it != hi_it; ++it) {
            auto& ch = chars[*it];
            double cx = (ch.left + ch.right) * 0.5;
            if (cx >= left - 1.0 && cx < right + 1.0)
                matches.push_back(*it);
        }
        // Sort by reading order: top-to-bottom, then along the advance.
        // Single-row cells will fall through to a stable advance-order sort;
        // multi-row cells (merged) read top-to-bottom.
        //
        // Direction is the primary key: projecting each glyph with its own
        // angle would not be a strict weak ordering in a cell that mixes them.
        std::sort(matches.begin(), matches.end(), [this](size_t a, size_t b) {
            const auto& ca = chars[a];
            const auto& cb = chars[b];
            if (ca.rot != cb.rot) return ca.rot < cb.rot;
            double y_tol = std::max(ca.font_size, cb.font_size) * 0.4;
            if (y_tol < 2.0) y_tol = 2.0;
            double aa = text_across(ca.rot, ca.x, ca.y);
            double ab = text_across(cb.rot, cb.x, cb.y);
            if (std::abs(aa - ab) > y_tol) return aa > ab;
            double a_lo, a_hi, b_lo, b_hi;
            text_along_span(ca.rot, ca.left, ca.right, ca.top, ca.bot, a_lo, a_hi);
            text_along_span(cb.rot, cb.left, cb.right, cb.top, cb.bot, b_lo, b_hi);
            return a_lo < b_lo;
        });
        std::string text;
        double prev_hi = -1e9;
        double prev_across = 0.0;
        double prev_fs = 12.0;
        int16_t prev_rot = 0;
        bool first = true;
        for (size_t idx : matches) {
            auto& ch = chars[idx];
            double fs = ch.font_size > 1.0 ? ch.font_size : 12.0;
            // Same frame the sort used, so row breaks and word gaps are
            // measured along the run's baseline rather than the page's.
            double lo, hi;
            text_along_span(ch.rot, ch.left, ch.right, ch.top, ch.bot, lo, hi);
            double acr = text_across(ch.rot, ch.x, ch.y);
            if (!first) {
                double y_tol = std::max(prev_fs, fs) * 0.4;
                if (y_tol < 2.0) y_tol = 2.0;
                // A direction change always breaks the run: the two frames
                // share no baseline to compare against.
                bool new_row = ch.rot != prev_rot ||
                               std::abs(acr - prev_across) > y_tol;
                if (new_row) {
                    // A number wrapped across cell lines ("991225-" /
                    // "1234567") continues without a space; only the
                    // digit-hyphen-digit shape is joined, so hyphenated
                    // words keep their space.
                    bool digit_wrap = text.size() >= 2 && text.back() == '-' &&
                                      text[text.size() - 2] >= '0' &&
                                      text[text.size() - 2] <= '9' &&
                                      ch.unicode >= '0' && ch.unicode <= '9';
                    if (!digit_wrap && !text.empty() && text.back() != ' ')
                        text += ' ';
                } else {
                    // Insert a space when the positional gap exceeds the
                    // word-spacing threshold used by chars_to_lines.
                    double gap = lo - prev_hi;
                    double word_gap = fs * 0.15;
                    if (word_gap < 1.0) word_gap = 1.0;
                    if (ch.unicode == ' ' || ch.unicode == 0xA0) {
                        if (!text.empty() && text.back() != ' ') text += ' ';
                    } else if (gap > word_gap && !text.empty() && text.back() != ' ') {
                        text += ' ';
                    }
                }
            }
            if (ch.unicode != ' ' && ch.unicode != 0xA0)
                util::append_utf8(text, ch.unicode);
            prev_hi = hi;
            prev_across = acr;
            prev_fs = fs;
            prev_rot = ch.rot;
            first = false;
        }
        size_t s = text.find_first_not_of(" ");
        size_t e = text.find_last_not_of(" ");
        if (s != std::string::npos) return text.substr(s, e - s + 1);
        return "";
    }

    std::vector<std::pair<double,double>> get_char_ranges_in_row(
            double y_center, double y_tol, double x_min, double x_max) const {
        std::vector<std::pair<double,double>> ranges;
        for (auto& ch : chars) {
            if (ch.unicode == ' ' || ch.unicode == '\t' || ch.unicode == 0xA0) continue;
            if (std::abs(ch.y - y_center) > y_tol) continue;
            if (ch.x < x_min - 5 || ch.x > x_max + 5) continue;
            ranges.push_back({ch.left, ch.right});
        }
        return ranges;
    }
};

// Cross-page font cache keyed by font object number. Shared by the parallel
// page workers, so lookups/inserts go through the lock. load_font is
// deterministic, so two workers racing on a miss both compute the same value
// and the duplicate insert is harmless.
struct FontCache {
    std::unordered_map<int, PdfFont> map;
    std::mutex mu;
};

enum class GraphicsCollection {
    None,
    TableGeometry,
    RenderPaths,
};

struct ContentParseOptions {
    GraphicsCollection graphics = GraphicsCollection::RenderPaths;
    // Page width in viewing coordinates; bounds unclipped shading regions.
    // 0 = unknown (a page-height-based bound is used instead).
    double page_width = 0;
};

// Cross-translation-unit declarations.
// inherit_gs: graphics state a Form XObject inherits from its caller (colors,
// /ca alpha, clip). initial_ctm still overrides the CTM after the copy.
ContentParseResult parse_content_stream(PdfDoc& doc, const std::vector<uint8_t>& stream,
                                         const PdfObj& resources, double page_height,
                                         FontCache* font_cache = nullptr,
                                         const ContentParseOptions& options = {},
                                         const double* initial_ctm = nullptr,
                                         int depth = 0,
                                         const GfxState* inherit_gs = nullptr);
std::vector<TextLine> chars_to_lines(const std::vector<TextChar>& chars,
                                     double* out_col_boundary = nullptr);

}} // namespace jdoc::pdf_detail
