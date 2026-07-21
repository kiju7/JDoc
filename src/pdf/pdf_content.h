#pragma once
// pdf_content.h — internal: content-stream parse vocabulary and line layout.
#include "pdf_core.h"
#include <cstring>
#include <unordered_map>
#include <vector>

namespace jdoc { namespace pdf_detail {

struct TextChar {
    double x, y;
    double left, right, top, bot;
    double font_size;
    uint32_t unicode;
    bool is_bold;
    bool is_italic;
};

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

struct PathPoint {
    double x, y;
    enum Type { MOVE, LINE, CURVE, CLOSE } type;
    double cx1, cy1, cx2, cy2; // for CURVE
};

struct PdfLineSegment {
    float x0, y0, x1, y1;
    bool is_horizontal() const { return std::abs(y1 - y0) < 2.0f; }
    bool is_vertical()   const { return std::abs(x1 - x0) < 2.0f; }
};

struct ImagePlacement {
    int xobj_ref = -1;
    std::string xobj_name;
    double ctm[6];
    double fill_r = 0, fill_g = 0, fill_b = 0; // fill color for ImageMask
};

struct RenderPath {
    std::vector<PathPoint> points;
    double fill_r, fill_g, fill_b;
    double stroke_r, stroke_g, stroke_b;
    double line_width;
    bool do_fill, do_stroke;
};

struct ContentParseResult {
    std::vector<TextChar> chars;
    std::vector<PdfLineSegment> segments;
    std::vector<ImagePlacement> images;
    std::vector<RenderPath> paths; // for vector rendering
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
};

struct PageCharCache {
    struct CharInfo {
        double x, y;
        double left, right, top, bot;
        double font_size;
        unsigned int unicode;
    };
    std::vector<CharInfo> chars;
    std::vector<size_t> y_sorted;

    void build(const std::vector<TextChar>& text_chars) {
        chars.reserve(text_chars.size());
        for (auto& tc : text_chars) {
            if (tc.unicode == 0 || tc.unicode == '\r' || tc.unicode == '\n' || tc.unicode == 0xFFFD) continue;
            chars.push_back({tc.x, tc.y, tc.left, tc.right, tc.top, tc.bot, tc.font_size, tc.unicode});
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
        // Sort by reading order: top-to-bottom, then left-to-right.
        // Single-row cells will fall through to a stable left-to-right order;
        // multi-row cells (merged) read top-to-bottom.
        std::sort(matches.begin(), matches.end(), [this](size_t a, size_t b) {
            const auto& ca = chars[a];
            const auto& cb = chars[b];
            double y_tol = std::max(ca.font_size, cb.font_size) * 0.4;
            if (y_tol < 2.0) y_tol = 2.0;
            if (std::abs(ca.y - cb.y) > y_tol) return ca.y > cb.y;
            return ca.left < cb.left;
        });
        std::string text;
        double prev_right = -1e9;
        double prev_y = 0.0;
        double prev_fs = 12.0;
        bool first = true;
        for (size_t idx : matches) {
            auto& ch = chars[idx];
            double fs = ch.font_size > 1.0 ? ch.font_size : 12.0;
            if (!first) {
                double y_tol = std::max(prev_fs, fs) * 0.4;
                if (y_tol < 2.0) y_tol = 2.0;
                bool new_row = std::abs(ch.y - prev_y) > y_tol;
                if (new_row) {
                    if (!text.empty() && text.back() != ' ') text += ' ';
                } else {
                    // Insert a space when the positional gap exceeds the
                    // word-spacing threshold used by chars_to_lines.
                    double gap = ch.left - prev_right;
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
            prev_right = ch.right;
            prev_y = ch.y;
            prev_fs = fs;
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

// Cross-translation-unit declarations.
ContentParseResult parse_content_stream(PdfDoc& doc, const std::vector<uint8_t>& stream,
                                         const PdfObj& resources, double page_height,
                                         std::unordered_map<int, PdfFont>* font_cache = nullptr,
                                         bool skip_graphics = false,
                                         const double* initial_ctm = nullptr,
                                         int depth = 0);
std::vector<TextLine> chars_to_lines(const std::vector<TextChar>& chars,
                                     double* out_col_boundary = nullptr);

}} // namespace jdoc::pdf_detail
