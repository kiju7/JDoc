#include "pdf_content.h"
#include "common/string_utils.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
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
//  - Multi-column pages keep reading order. With a column boundary detected
//    (chars_to_lines, the same one the markdown path uses), the page is cut
//    into zones top to bottom: a full-width zone (title, abstract, a table
//    or caption across the gutter) is laid out on the page grid; a column
//    zone emits its whole left column, then its whole right column, each on
//    a grid measured from that column's own left edge. A table inside one
//    column stays aligned within it. See column_blocks().
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
// Widest a single glyph's ink is taken to be, in ems (see ink_right).
constexpr double kMaxGlyphEm = 1.5;
// Blank lines a vertical gap may produce.
constexpr int kMaxBlankLines = 2;

// Right edge of the glyph's ink, for gap and overlap decisions. TextChar's
// right edge is the next glyph's origin, so it carries character spacing
// (Tc) and whatever advance the producer wrote. Hangul word processors place
// table cells by spreading one run with a huge Tc: two "F"s 36 pt apart in
// an 8 pt font each report a 40 pt box and read as touching. The box is
// capped at kMaxGlyphEm: wide enough for a real wide letter (an italic math
// "W" or "M" runs past one em, and a tight cap splits "W ikipedia"), far
// below the spread of a Tc-positioned cell.
double ink_right(const TextChar* g) {
    double w = g->right - g->left;
    if (g->font_size > 1.0) w = std::min(w, g->font_size * kMaxGlyphEm);
    return g->left + w;
}

struct LayoutLine {
    std::vector<const TextChar*> glyphs;
    double base_y = 0;   // baseline of the line's largest glyph
    double font_size = 0;
};

// Lines laid out together on one grid: a full-width zone, or one column of a
// column zone. `margin` is the x of grid column 0.
struct LayoutBlock {
    std::vector<LayoutLine> rows;
    double margin = 0;
    // The right column of a column zone: it starts back at the zone's top,
    // so there is no vertical gap to measure and a blank line separates it.
    bool restarts = false;
};

// A line over a subset of its glyphs (already in left-to-right order), with
// its baseline and size taken again from the glyphs it keeps.
LayoutLine slice_line(std::vector<const TextChar*>::const_iterator b,
                      std::vector<const TextChar*>::const_iterator e,
                      double default_fs) {
    LayoutLine ln;
    ln.glyphs.assign(b, e);
    for (auto* g : ln.glyphs) {
        double fs = g->font_size > 1.0 ? g->font_size : default_fs;
        if (fs > ln.font_size) {
            ln.font_size = fs;
            ln.base_y = g->y;
        }
    }
    return ln;
}

// Cut a two-column page into blocks in reading order. The rules mirror the
// markdown path so both outputs agree on what spans the page:
//  - A physical line splits at the gutter when the gap crossing the column
//    boundary is wider than a word space and either wider than a gutter or
//    followed by a single run on the right (lines_from_upright_chars). A row
//    with several cells right of the gutter is a table row and stays whole.
//  - A piece that straddles the boundary and is wide (> 60% of the content
//    width) or centred on the page spans both columns; any other piece
//    belongs to the column holding its centre (reorder_column_lines).
//  - A centred piece that is not wide, with column text directly above and
//    below it (an equation, a short label sitting on the gutter), is not
//    allowed to cut the column flow in two; it joins the column of its
//    centre.
// Spanning pieces form full-width zones on the page grid; the pieces between
// them form a column zone, emitted left column first, then right, each on a
// grid starting at that column's leftmost glyph on the page.
std::vector<LayoutBlock> column_blocks(const std::vector<LayoutLine>& lines,
                                       double boundary, double page_margin,
                                       double median_fs) {
    enum Side { LEFT, RIGHT, SPAN };
    struct Piece {
        LayoutLine line;
        double x0, x1;
        Side side = LEFT;
        size_t line_idx;    // physical line the piece was cut from
        bool fills = false; // column text: fills its column in one run
    };
    const double col_gap = std::max(median_fs * 1.2, 8.0);
    const double gutter_gap = std::max(median_fs * 2.0, 18.0);

    std::vector<Piece> pieces;
    for (size_t li = 0; li < lines.size(); li++) {
        const auto& gl = lines[li].glyphs;
        size_t cut = gl.size();
        for (size_t i = 0; i + 1 < gl.size(); i++) {
            double gap = gl[i + 1]->left - ink_right(gl[i]);
            if (ink_right(gl[i]) < boundary && gl[i + 1]->left > boundary &&
                gap > col_gap) {
                int right_runs = 1;
                for (size_t k = i + 2; k < gl.size() && right_runs < 2; k++)
                    if (gl[k]->left - ink_right(gl[k - 1]) > col_gap)
                        right_runs++;
                if (gap > gutter_gap || right_runs < 2) cut = i + 1;
                break;
            }
        }
        auto add = [&](size_t b, size_t e) {
            Piece pc;
            pc.line_idx = li;
            pc.line = slice_line(gl.begin() + b, gl.begin() + e, median_fs);
            pc.x0 = gl[b]->left;
            pc.x1 = 0;
            for (size_t k = b; k < e; k++) pc.x1 = std::max(pc.x1, ink_right(gl[k]));
            pieces.push_back(std::move(pc));
        };
        if (cut < gl.size()) {
            add(0, cut);
            add(cut, gl.size());
        } else if (!gl.empty()) {
            add(0, gl.size());
        }
    }

    double min_x = 1e30, max_x = -1e30;
    for (auto& pc : pieces) {
        min_x = std::min(min_x, pc.x0);
        max_x = std::max(max_x, pc.x1);
    }
    const double content_w = max_x - min_x;
    const double page_center = (min_x + max_x) / 2.0;
    std::vector<bool> wide(pieces.size(), false);
    for (size_t i = 0; i < pieces.size(); i++) {
        auto& pc = pieces[i];
        bool straddles = pc.x0 < boundary - 5 && pc.x1 > boundary + 5;
        wide[i] = pc.x1 - pc.x0 > content_w * 0.6;
        double off_center = std::abs((pc.x0 + pc.x1) / 2.0 - page_center);
        bool centered = straddles && off_center < content_w * 0.15;
        // A short piece on the page's centre line (a page number, a centred
        // heading) need not reach across the boundary to be page furniture
        // rather than one column's text.
        bool on_axis = pc.x1 - pc.x0 < content_w * 0.2 &&
                       off_center < content_w * 0.05;
        if ((straddles && (wide[i] || centered)) || on_axis)
            pc.side = SPAN;
        else
            pc.side = (pc.x0 + pc.x1) / 2.0 < boundary ? LEFT : RIGHT;
    }
    for (size_t i = 1; i + 1 < pieces.size(); i++) {
        if (pieces[i].side != SPAN || wide[i]) continue;
        if (pieces[i - 1].side != SPAN && pieces[i + 1].side != SPAN)
            pieces[i].side = (pieces[i].x0 + pieces[i].x1) / 2.0 < boundary
                                 ? LEFT : RIGHT;
    }

    // A physical line is full-width or columnar as a whole: if one of its
    // pieces spans (a centred header cell over a table), so does the rest.
    {
        std::vector<bool> line_spans(lines.size(), false);
        for (auto& pc : pieces)
            if (pc.side == SPAN) line_spans[pc.line_idx] = true;
        for (auto& pc : pieces)
            if (line_spans[pc.line_idx]) pc.side = SPAN;
    }

    // Each column's edge is the leftmost start of its text lines: pieces at
    // least a tenth of the content wide, and for the right column starting
    // right of the boundary. A short label, or a piece assigned to a column
    // by its centre, must not indent the whole column; pieces left of the
    // edge clamp to grid column 0.
    double edge[2];
    auto column_edges = [&]() {
        double any[2] = {1e30, 1e30};
        edge[LEFT] = edge[RIGHT] = 1e30;
        for (auto& pc : pieces) {
            if (pc.side == SPAN) continue;
            any[pc.side] = std::min(any[pc.side], pc.x0);
            if (pc.x1 - pc.x0 < content_w * 0.1) continue;
            if (pc.side == RIGHT && pc.x0 < boundary) continue;
            edge[pc.side] = std::min(edge[pc.side], pc.x0);
        }
        for (int sd = 0; sd < 2; sd++)
            if (edge[sd] > 1e29) edge[sd] = any[sd];
    };
    column_edges();

    // A table across the gutter whose cell gaps exceed a gutter splits like
    // two columns, line by line. Column text is told apart by its lines
    // filling their column in one run (no gap wider than a column gap: a
    // table row spreads its width over cells); a run of lines with content
    // on both sides and no line of column text is a table (or a figure)
    // spanning the page, and goes full-width so its cells keep their
    // positions. Three such lines are required, so a heading beside a short
    // last line of a paragraph does not qualify. One-sided short lines inside
    // the run (a row with its right cells empty) do not break it.
    {
        double right_end = 0;
        for (auto& pc : pieces)
            if (pc.side == RIGHT) right_end = std::max(right_end, pc.x1);
        const double col_w[2] = {boundary - edge[LEFT], right_end - edge[RIGHT]};
        enum LineKind { NEUTRAL, TABULAR, PROSE };
        std::vector<LineKind> kind(lines.size(), NEUTRAL);
        std::vector<int> sides(lines.size(), 0);
        std::vector<bool> fills(lines.size(), false);
        for (auto& pc : pieces) {
            if (pc.side == SPAN) {
                fills[pc.line_idx] = true;  // already full-width
                continue;
            }
            sides[pc.line_idx] |= 1 << pc.side;
            if (pc.x1 - pc.x0 < 0.7 * col_w[pc.side]) continue;
            const auto& gl = pc.line.glyphs;
            bool one_run = true;
            for (size_t k = 1; k < gl.size() && one_run; k++)
                one_run = gl[k]->left - ink_right(gl[k - 1]) <= col_gap;
            if (one_run) fills[pc.line_idx] = pc.fills = true;
        }
        for (size_t li = 0; li < lines.size(); li++)
            kind[li] = fills[li] ? PROSE : sides[li] == 3 ? TABULAR : NEUTRAL;

        std::vector<bool> spans(lines.size(), false);
        size_t li = 0;
        while (li < lines.size()) {
            if (kind[li] != TABULAR) { li++; continue; }
            size_t end = li, tabular = 0, last_tab = li;
            while (end < lines.size() && kind[end] != PROSE) {
                if (kind[end] == TABULAR) { tabular++; last_tab = end; }
                end++;
            }
            if (tabular >= 3)
                for (size_t k = li; k <= last_tab; k++) spans[k] = true;
            li = end;
        }
        bool changed = false;
        for (auto& pc : pieces)
            if (spans[pc.line_idx] && pc.side != SPAN) {
                pc.side = SPAN;
                changed = true;
            }
        if (changed) column_edges();
    }
    const double left_margin = edge[LEFT], right_margin = edge[RIGHT];

    // Pieces [b, e) on the page grid, in physical line order; pieces of one
    // line rejoin as one row.
    std::vector<LayoutBlock> blocks;
    auto page_block = [&](size_t b, size_t e) {
        LayoutBlock full;
        full.margin = page_margin;
        size_t prev_line = SIZE_MAX;
        for (size_t k = b; k < e; k++) {
            auto& pc = pieces[k];
            if (pc.line_idx == prev_line) {
                auto& row = full.rows.back();
                row.glyphs.insert(row.glyphs.end(), pc.line.glyphs.begin(),
                                  pc.line.glyphs.end());
                if (pc.line.font_size > row.font_size) {
                    row.font_size = pc.line.font_size;
                    row.base_y = pc.line.base_y;
                }
            } else {
                full.rows.push_back(std::move(pc.line));
            }
            prev_line = pc.line_idx;
        }
        blocks.push_back(std::move(full));
    };

    // A band of column pieces is a column zone only when it shows two
    // columns of text side by side: at least two physical lines whose left
    // and right pieces both fill their column. A spurious boundary on a
    // one-column page (cut through a table, or between short and long
    // lines) never produces that, and its band keeps the page grid.
    size_t i = 0;
    while (i < pieces.size()) {
        size_t j = i;
        if (pieces[i].side == SPAN) {
            while (j < pieces.size() && pieces[j].side == SPAN) j++;
            page_block(i, j);
            i = j;
            continue;
        }
        int side_by_side = 0;
        while (j < pieces.size() && pieces[j].side != SPAN) {
            if (pieces[j].side == RIGHT && pieces[j].fills && j > i &&
                pieces[j - 1].line_idx == pieces[j].line_idx &&
                pieces[j - 1].side == LEFT && pieces[j - 1].fills)
                side_by_side++;
            j++;
        }
        if (side_by_side < 2) {
            page_block(i, j);
            i = j;
            continue;
        }
        LayoutBlock left, right;
        left.margin = left_margin;
        right.margin = right_margin;
        right.restarts = true;
        for (size_t k = i; k < j; k++)
            (pieces[k].side == LEFT ? left : right)
                .rows.push_back(std::move(pieces[k].line));
        if (!left.rows.empty()) blocks.push_back(std::move(left));
        if (!right.rows.empty()) blocks.push_back(std::move(right));
        i = j;
    }
    return blocks;
}

} // namespace

std::string layout_page_text(const std::vector<TextChar>& chars,
                             double col_boundary) {
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
            extent = std::max(extent, ink_right(g));
            if (g->font_size > 1.0) sizes.push_back(g->font_size);
            int dw = util::display_width(g->unicode);
            double w = ink_right(g) - g->left;
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
                        double overlap = std::min(ink_right(ga), ink_right(gb)) -
                                         std::max(ga->left, gb->left);
                        if (overlap > 0.5 * (ink_right(ga) - ga->left)) {
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

        for (auto& ln : lines)
            std::sort(ln.glyphs.begin(), ln.glyphs.end(),
                      [](const TextChar* a, const TextChar* b) {
                          return a->left < b->left;
                      });

        // ── Reading order ──
        std::vector<LayoutBlock> blocks;
        if (col_boundary > 0) {
            blocks = column_blocks(lines, col_boundary, margin, median_fs);
        } else {
            blocks.emplace_back();
            blocks.back().rows = std::move(lines);
            blocks.back().margin = margin;
        }

        // ── Grid rows ──
        struct PlacedRow {
            const LayoutLine* line;
            double margin;      // x of grid column 0 for this row's block
            bool restart;       // first row of a right column
        };
        std::vector<PlacedRow> rows;
        for (auto& blk : blocks)
            for (size_t ri = 0; ri < blk.rows.size(); ri++)
                rows.push_back({&blk.rows[ri], blk.margin,
                                blk.restarts && ri == 0});

        bool first_line = true;
        double prev_y = 0, prev_fs = 0;
        for (auto& placed : rows) {
            const LayoutLine& ln = *placed.line;
            std::string row;
            size_t cursor = 0;
            const TextChar* prev = nullptr;
            for (auto* g : ln.glyphs) {
                // Overprinted duplicates (faux bold, drop shadows) land on
                // the same spot with the same code; draw them once.
                if (prev && prev->unicode == g->unicode &&
                    std::abs(g->left - prev->left) <
                        std::max(1.0, 0.3 * (ink_right(prev) - prev->left)))
                    continue;
                double fs = g->font_size > 1.0 ? g->font_size : median_fs;
                long target = std::lround((g->left - placed.margin) / unit);
                size_t col = target > 0 ? static_cast<size_t>(target) : 0;
                if (prev) {
                    // Touching glyphs are one word (same word spacing rule
                    // as chars_to_lines); an ordinary word space is one
                    // space. Only a wider gap — a table gutter, a tab stop,
                    // a second column — snaps to the grid, so prose does not
                    // pick up stray double spaces wherever its proportional
                    // advances drift off the pitch.
                    double gap = g->left - ink_right(prev);
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
                if (placed.restart && blanks < 1) blanks = 1;
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
