#include "pdf_content.h"
#include "common/string_utils.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace jdoc { namespace pdf_detail {

// ── Layout-preserving plain text ────────────────────────
// Plain-text output reproduces the page as a character grid, the way
// `pdftotext -layout` does, instead of flattening reading-order lines. Tables
// are the reason: a reader (human or LLM) recovers a table's columns from the
// alignment of its values, and that alignment is lost once the cells of a row
// are glued into one line and its empty cells dropped.
//
// The grid:
//  - Rows are physical lines: glyphs grouped by baseline, top of page first.
//  - A glyph's column is (left - page's leftmost glyph) / unit, where `unit`
//    is the page's character pitch: the mean advance per display column of
//    its upright glyphs (a Hangul syllable's advance is split over its two
//    columns), less a tenth of slack. Measured per glyph rather than taken from the font size, it
//    follows the actual fonts — condensed table type, wide CJK body text.
//  - East Asian wide glyphs occupy two columns (util::display_width), so a
//    Korean line stays aligned with the Latin and digit lines around it in a
//    monospace view.
//  - Glyphs never overwrite: a glyph whose column is already taken goes one
//    space after the cursor, and glyphs that touch (gap under a fraction of a
//    space) are written back to back so a word is never split by padding.
//    Only gaps wider than a word space snap to the grid column (kSnapGapEm).
//  - A vertical gap of extra line heights becomes blank lines (at most two).
//  - Side-by-side columns stay side by side; that is what layout mode means.
//  - Rotated runs (TextChar::rot != 0) have no place on a horizontal grid.
//    They are read along their own baselines (chars_to_lines) and appended
//    after the page body, one run per line.

namespace {

// Glyphs that only separate words: the grid derives spacing from geometry.
bool is_layout_space(uint32_t cp) {
    return cp == ' ' || cp == 0xA0 || cp == '\t' || cp == 0x3000;
}

// Widest grid a page may produce. A stray glyph far off the text block (a
// running header in the bleed, tiny CAD labels) must not stretch every line
// to thousands of columns; past this the pitch widens instead.
constexpr double kMaxLayoutColumns = 500.0;
// Pitch as a fraction of the mean glyph advance. Slightly under 1 so a run
// of text takes a little more grid than page width and never less: cells
// that overrun push their neighbours out of line, extra slack only pads.
constexpr double kPitchSlack = 0.9;
// Horizontal gap, in ems, from which a glyph is placed at its grid column
// rather than one space after the previous word. Justified word spaces stay
// under it; table gutters and tab stops clear it.
constexpr double kSnapGapEm = 0.8;
// Blank lines a vertical gap may produce.
constexpr int kMaxBlankLines = 2;

struct LayoutLine {
    std::vector<const TextChar*> glyphs;
    double base_y = 0;   // baseline of the line's largest glyph
    double font_size = 0;
};

} // namespace

std::string layout_page_text(const std::vector<TextChar>& chars) {
    std::vector<const TextChar*> upright;
    std::vector<TextChar> rotated;
    upright.reserve(chars.size());
    for (auto& ch : chars) {
        if (ch.unicode < 0x20 || ch.unicode == 0xFFFD) continue;
        if (ch.rot != 0) {
            rotated.push_back(ch);
            continue;
        }
        if (is_layout_space(ch.unicode)) continue;
        upright.push_back(&ch);
    }

    std::string out;
    if (!upright.empty()) {
        // ── Character pitch ──
        double margin = 1e30, extent = -1e30;
        std::vector<double> pitches;
        std::vector<double> sizes;
        pitches.reserve(upright.size());
        sizes.reserve(upright.size());
        for (auto* g : upright) {
            margin = std::min(margin, g->left);
            extent = std::max(extent, g->right);
            if (g->font_size > 1.0) sizes.push_back(g->font_size);
            int dw = util::display_width(g->unicode);
            double w = g->right - g->left;
            if (dw > 0 && w > 0.1) pitches.push_back(w / dw);
        }
        double median_fs = 10.0;
        if (!sizes.empty()) {
            std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2,
                             sizes.end());
            median_fs = sizes[sizes.size() / 2];
        }
        // Mean advance per column, not the median: proportional Latin type
        // has a median glyph (a, e, n, o) wider than its average, and a pitch
        // above the average makes words overrun their grid columns and shove
        // the next cell right. Advances far from the median (broken widths,
        // spread-out letters) are left out of the mean, and the result is
        // taken a tenth narrower so adjacent numeric columns keep a gap.
        // No measurable advances (every glyph zero-width): half an em is the
        // typical pitch of body text.
        double unit = median_fs * 0.5;
        if (!pitches.empty()) {
            std::nth_element(pitches.begin(),
                             pitches.begin() + pitches.size() / 2, pitches.end());
            double median = pitches[pitches.size() / 2];
            double sum = 0;
            size_t n = 0;
            for (double w : pitches) {
                if (w < median * 0.25 || w > median * 4.0) continue;
                sum += w;
                n++;
            }
            if (n > 0) unit = sum / n * kPitchSlack;
        }
        unit = std::max(unit, 1.0);
        unit = std::max(unit, (extent - margin) / kMaxLayoutColumns);

        // ── Physical lines ──
        // Top-first by baseline; a glyph joins the line whose first (highest)
        // glyph sits within half the smaller font size above it, which keeps
        // superscripts with their line without chaining into the next one.
        std::sort(upright.begin(), upright.end(),
                  [](const TextChar* a, const TextChar* b) {
                      if (a->y != b->y) return a->y > b->y;
                      return a->left < b->left;
                  });
        std::vector<LayoutLine> lines;
        double anchor_y = 0, anchor_fs = 0;
        for (auto* g : upright) {
            double fs = g->font_size > 1.0 ? g->font_size : median_fs;
            bool joins = false;
            if (!lines.empty()) {
                double tol = std::max(2.0, 0.55 * std::min(anchor_fs, fs));
                joins = anchor_y - g->y <= tol;
            }
            if (!joins) {
                lines.emplace_back();
                anchor_y = g->y;
                anchor_fs = fs;
            }
            LayoutLine& ln = lines.back();
            ln.glyphs.push_back(g);
            if (fs > ln.font_size) {
                ln.font_size = fs;
                ln.base_y = g->y;
            }
        }

        // A line of small raised glyphs (footnote marks, exponents set
        // higher than the join tolerance) belongs to the body line under it
        // when it fits between that line's glyphs. Left alone it becomes a
        // row of its own holding a stray "†" or "2".
        std::vector<LayoutLine> merged;
        merged.reserve(lines.size());
        for (size_t i = 0; i < lines.size(); i++) {
            LayoutLine& a = lines[i];
            if (i + 1 < lines.size()) {
                LayoutLine& b = lines[i + 1];
                bool raised = a.font_size <= 0.8 * b.font_size &&
                              a.base_y - b.base_y <= 0.6 * b.font_size;
                bool clear = raised;
                for (size_t ai = 0; clear && ai < a.glyphs.size(); ai++) {
                    auto* ga = a.glyphs[ai];
                    for (auto* gb : b.glyphs) {
                        double overlap = std::min(ga->right, gb->right) -
                                         std::max(ga->left, gb->left);
                        if (overlap > 0.5 * (ga->right - ga->left)) {
                            clear = false;
                            break;
                        }
                    }
                }
                if (clear) {
                    b.glyphs.insert(b.glyphs.end(), a.glyphs.begin(),
                                    a.glyphs.end());
                    continue;
                }
            }
            merged.push_back(std::move(a));
        }
        lines = std::move(merged);

        // ── Grid rows ──
        bool first_line = true;
        double prev_y = 0, prev_fs = 0;
        for (auto& ln : lines) {
            std::sort(ln.glyphs.begin(), ln.glyphs.end(),
                      [](const TextChar* a, const TextChar* b) {
                          return a->left < b->left;
                      });
            std::string row;
            size_t cursor = 0;
            const TextChar* prev = nullptr;
            for (auto* g : ln.glyphs) {
                // Overprinted duplicates (faux bold, drop shadows) land on
                // the same spot with the same code; draw them once.
                if (prev && prev->unicode == g->unicode &&
                    std::abs(g->left - prev->left) <
                        std::max(1.0, 0.3 * (prev->right - prev->left)))
                    continue;
                double fs = g->font_size > 1.0 ? g->font_size : median_fs;
                long target = std::lround((g->left - margin) / unit);
                size_t col = target > 0 ? static_cast<size_t>(target) : 0;
                if (prev) {
                    // Touching glyphs are one word (same word spacing rule
                    // as chars_to_lines); an ordinary word space is one
                    // space. Only a wider gap — a table gutter, a tab stop,
                    // a second column — snaps to the grid, so prose does not
                    // pick up stray double spaces wherever its proportional
                    // advances drift off the pitch.
                    double gap = g->left - prev->right;
                    double word_gap = std::max(1.0, fs * 0.15);
                    if (gap < word_gap)
                        col = cursor;
                    else if (gap < fs * kSnapGapEm)
                        col = cursor + 1;
                    else
                        col = std::max(col, cursor + 1);
                }
                row.append(col - cursor, ' ');
                util::append_utf8(row, g->unicode);
                cursor = col + static_cast<size_t>(util::display_width(g->unicode));
                prev = g;
            }

            if (!first_line) {
                double line_h = 1.2 * std::max(prev_fs, ln.font_size);
                double extra = (prev_y - ln.base_y) / line_h - 1.0;
                long blanks = std::lround(extra);
                if (blanks > kMaxBlankLines) blanks = kMaxBlankLines;
                for (long b = 0; b < blanks; b++) out += '\n';
            }
            out += row;
            out += '\n';
            first_line = false;
            prev_y = ln.base_y;
            prev_fs = ln.font_size;
        }
    }

    // ── Rotated runs, after the body ──
    if (!rotated.empty()) {
        bool first = true;
        for (auto& tl : chars_to_lines(rotated)) {
            if (tl.text.empty()) continue;
            if (first && !out.empty()) out += '\n';
            first = false;
            out += tl.text;
            out += '\n';
        }
    }
    return out;
}

}} // namespace jdoc::pdf_detail
