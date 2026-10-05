#include "pdf_content.h"
#include "common/string_utils.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <utility>
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
//  - Rotated runs (TextChar::rot != 0) have no place on the page's grid.
//    Each writing direction gets a grid of its own, built the same way in
//    the run's own frame (to_writing_frame) and appended after the page
//    body: a sideways table keeps its columns, sideways prose its spaces.

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
        // A title, a line as wide as column text set well above the running
        // size, between a running head and a short heading rather than
        // lines of column text, stays across the page. (A large symbol
        // among a figure's labels is no title.)
        auto text_line = [&](const Piece& pc) {
            return pc.x1 - pc.x0 >= content_w * 0.25;
        };
        if (pieces[i].line.font_size > 1.3 * median_fs && text_line(pieces[i]) &&
            !text_line(pieces[i - 1]) && !text_line(pieces[i + 1]))
            continue;
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

// The page's character grid pitch (see the header comment): the trimmed
// mean advance per display column, with the grid's left edge and the median
// font size alongside.
struct GridPitch {
    double margin = 0;    // leftmost glyph: grid column 0 of the page grid
    double unit = 5.0;    // points per grid column
    double median_fs = 10.0;
};

GridPitch measure_pitch(const std::vector<const TextChar*>& glyphs) {
    GridPitch gp;
    double margin = 1e30, extent = -1e30;
    std::vector<double> pitches;
    std::vector<double> sizes;
    pitches.reserve(glyphs.size());
    sizes.reserve(glyphs.size());
    for (auto* g : glyphs) {
        margin = std::min(margin, g->left);
        extent = std::max(extent, ink_right(g));
        if (g->font_size > 1.0) sizes.push_back(g->font_size);
        int dw = util::display_width(g->unicode);
        double w = ink_right(g) - g->left;
        if (dw > 0 && w > 0.1) pitches.push_back(w / dw);
    }
    if (!sizes.empty()) {
        std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2,
                         sizes.end());
        gp.median_fs = sizes[sizes.size() / 2];
    }
    // Mean advance per column, not the median: proportional Latin type
    // has a median glyph (a, e, n, o) wider than its average, and a pitch
    // above the average makes words overrun their grid columns and shove
    // the next cell right. Advances far from the median (broken widths,
    // spread-out letters) are left out of the mean, and the result is
    // taken a tenth narrower so adjacent numeric columns keep a gap.
    // No measurable advances (every glyph zero-width): half an em is the
    // typical pitch of body text.
    double unit = gp.median_fs * 0.5;
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
    gp.margin = margin;
    gp.unit = unit;
    return gp;
}

// Physical lines, top of page first, each in left-to-right order. Sorts
// `glyphs` (by baseline) as a side effect.
std::vector<LayoutLine> group_lines(std::vector<const TextChar*>& glyphs,
                                    double median_fs) {
    // Top-first by baseline; a glyph joins the line whose first (highest)
    // glyph sits within half the smaller font size above it, which keeps
    // superscripts with their line without chaining into the next one.
    std::sort(glyphs.begin(), glyphs.end(),
              [](const TextChar* a, const TextChar* b) {
                  if (a->y != b->y) return a->y > b->y;
                  return a->left < b->left;
              });
    std::vector<LayoutLine> lines;
    double anchor_y = 0, anchor_fs = 0;
    for (auto* g : glyphs) {
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
    return lines;
}

// Word spaces the producer wrote (explicit_word_spaces), indexed from `base`;
// glyphs outside [base, base + sep.size()) — rotated-frame copies — have
// none. between(prev, g): a space glyph separates g from prev.
struct SpaceMarks {
    const TextChar* base = nullptr;
    std::vector<int> sep;
    bool between(const TextChar* prev, const TextChar* g) const {
        if (!base || g < base || prev < base) return false;
        size_t i = static_cast<size_t>(g - base);
        return i < sep.size() && sep[i] >= 0 && base + sep[i] == prev;
    }
};

// One grid row: a line's glyphs placed from `margin` on a grid of `unit`
// points per column.
std::string render_row(const LayoutLine& ln, double margin, double unit,
                       double median_fs, const SpaceMarks* spaces = nullptr) {
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
        long target = std::lround((g->left - margin) / unit);
        size_t col = target > 0 ? static_cast<size_t>(target) : 0;
        if (prev) {
            // Touching glyphs are one word (same word spacing rule
            // as chars_to_lines); an ordinary word space is one
            // space. Only a wider gap — a table gutter, a tab stop,
            // a second column — snaps to the grid, so prose does not
            // pick up stray double spaces wherever its proportional
            // advances drift off the pitch.
            // A space glyph the producer wrote between them is a word
            // space even where kerning or justification closed the gap,
            // unless it closed entirely (see chars_to_lines).
            double gap = g->left - ink_right(prev);
            double word_gap = std::max(1.0, fs * 0.15);
            if (gap < word_gap)
                col = (spaces && gap > kWrittenSpaceMinEm * fs &&
                       spaces->between(prev, g)) ? cursor + 1 : cursor;
            else if (gap < fs * kSnapGapEm)
                col = cursor + 1;
            else
                col = std::max(col, cursor + 1);
        }
        row.append(col - cursor, ' ');
        const size_t before = row.size();
        append_glyph_text(row, g->unicode);
        // A spelt-out ligature takes a column per letter.
        cursor = col + (g->unicode >= 0xFB00 && g->unicode <= 0xFB06
                            ? row.size() - before
                            : static_cast<size_t>(util::display_width(g->unicode)));
        prev = g;
    }
    return row;
}

struct PlacedRow {
    const LayoutLine* line;
    double margin;      // x of grid column 0 for this row's block
    bool restart;       // first row of a right column
};

// Rows on their grids, one text line each, with blank lines for vertical
// gaps of extra line heights.
std::string render_rows(const std::vector<PlacedRow>& rows, double unit,
                        double median_fs, const SpaceMarks* spaces = nullptr) {
    std::string out;
    bool first_line = true;
    double prev_y = 0, prev_fs = 0;
    for (auto& placed : rows) {
        const LayoutLine& ln = *placed.line;
        std::string row = render_row(ln, placed.margin, unit, median_fs, spaces);
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
    return out;
}

// Upright, non-space glyphs: what the grid places.
bool is_grid_glyph(const TextChar& ch) {
    return ch.unicode >= 0x20 && ch.unicode != 0xFFFD && ch.rot == 0 &&
           !is_layout_space(ch.unicode);
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
        const GridPitch gp = measure_pitch(upright);
        std::vector<LayoutLine> lines = group_lines(upright, gp.median_fs);

        // ── Reading order ──
        std::vector<LayoutBlock> blocks;
        if (col_boundary > 0) {
            blocks = column_blocks(lines, col_boundary, gp.margin, gp.median_fs);
        } else {
            blocks.emplace_back();
            blocks.back().rows = std::move(lines);
            blocks.back().margin = gp.margin;
        }

        // ── Grid rows ──
        std::vector<PlacedRow> rows;
        for (auto& blk : blocks)
            for (size_t ri = 0; ri < blk.rows.size(); ri++)
                rows.push_back({&blk.rows[ri], blk.margin,
                                blk.restarts && ri == 0});
        SpaceMarks spaces;
        spaces.base = chars.data();
        spaces.sep = explicit_word_spaces(chars);
        out = render_rows(rows, gp.unit, gp.median_fs, &spaces);
    }

    // ── Rotated runs, after the body ──
    // Each writing direction is laid out on its own grid in its own frame
    // (to_writing_frame): lines along its baselines, word gaps and columns
    // along its advance. Measured on the page axes instead, a 90° run's
    // advance is page height, so every gap reads as zero and the cells of a
    // sideways table glue together.
    for (int r = 1; r < 24 && !rotated.empty(); r++) {
        std::vector<TextChar> frame;
        for (auto& ch : rotated)
            if (ch.rot == r && !is_layout_space(ch.unicode))
                frame.push_back(to_writing_frame(ch));
        if (frame.empty()) continue;
        std::vector<const TextChar*> glyphs;
        glyphs.reserve(frame.size());
        for (auto& t : frame) glyphs.push_back(&t);
        const GridPitch gp = measure_pitch(glyphs);
        std::vector<LayoutLine> lines = group_lines(glyphs, gp.median_fs);
        std::vector<PlacedRow> rows;
        for (auto& ln : lines) rows.push_back({&ln, gp.margin, false});
        if (!out.empty()) out += '\n';
        out += render_rows(rows, gp.unit, gp.median_fs);
    }
    return out;
}

// ── Layout blocks for undetected tables (markdown) ─────
// The markdown path turns detected tables into pipe tables and everything
// else into prose lines. A region laid out in columns that no detector
// accepted then reads as glued text ("ItemQ1 Q2 Q3 Row0111 21 31"), and the
// column a value sat in is gone. Such a region is instead shown as a fenced
// block of grid lines, the same grid the plain-text output uses, measured
// from the region's own left edge. Two kinds of evidence qualify:
//  - A ruled grid the ruled detector rejected only for having too few rows
//    with two filled cells, provided its row rules run across it, a
//    vertical rule runs inside it (a label column beside wrapped cells),
//    and its rows hold text on both sides of that rule (chart gridlines and
//    axes enclose a plot, not cells). Adjacent such grids that line up are one
//    region: a merged section row between them breaks the rule grid, not
//    the table.
//  - Whitespace alignment that reads as a data table: at least three lines
//    of one column zone (see column_blocks) cut into cells by gaps of a cell
//    gap or more, most of the region's lines being such multi-cell lines;
//    at least two column gaps (three columns), each an x-interval where 60%
//    of them have a gap; no more than a fifth of them opening with a number
//    other than a year (a chart's y-axis ticks beside its plot);
//    at least three of them, and half, "data rows" (a first cell holding a
//    letter and no wider than 40% of the region), whose other cells hold a
//    digit in at least 40% of cases; a column of figures (mostly-digit
//    cells overlapping in x in three rows); and no more than half with a
//    value cell wider than 40% of the region (prose beside a short block of
//    figures). Lines within the region may be
//    single-cell (a wrapped cell, a row with empty cells) but never two in
//    a row. The column and digit demands are what keep out the aligned text
//    that is not a table: two prose columns side by side (one shared gap,
//    long cells), chart axis ticks (numeric first cells) and legends,
//    author and affiliation blocks.
// Regions are also rejected when the cells read as something other than a
// table: list or reference markers in the first cell, equation numbers
// closing rows (any at all in a short run: a display equation set over
// several lines) or math operators in 30% of rows (any in a short run), dot
// leaders (a table of contents), e-mail addresses, and chart axes — evenly
// stepped numbers along a row (five, x ticks) or stacked down a column
// (three, falling: y ticks), the labels of a chart drawn in vectors being
// aligned text with figures in it too.
// Rotated runs get the whitespace pass in their own frame, so a table set
// sideways on the page is laid out along its baselines.

namespace {

// A row cut into cells at gaps of at least `cell_gap`.
struct RowCells {
    std::vector<std::pair<size_t, size_t>> cells;  // glyph index ranges
    std::vector<std::pair<double, double>> gaps;   // x extent of each gap
};

RowCells cut_cells(const LayoutLine& ln, double cell_gap) {
    RowCells rc;
    const auto& gl = ln.glyphs;
    size_t start = 0;
    double reach = gl.empty() ? 0 : ink_right(gl[0]);
    for (size_t k = 1; k < gl.size(); k++) {
        if (gl[k]->left - reach >= cell_gap) {
            rc.cells.push_back({start, k});
            rc.gaps.push_back({reach, gl[k]->left});
            start = k;
        }
        reach = std::max(reach, ink_right(gl[k]));
    }
    if (!gl.empty()) rc.cells.push_back({start, gl.size()});
    return rc;
}

std::string cell_text(const LayoutLine& ln, std::pair<size_t, size_t> r) {
    std::string t;
    for (size_t k = r.first; k < r.second; k++)
        append_glyph_text(t, ln.glyphs[k]->unicode);
    return t;
}

// "[12]", "12.", "12)", "(12)", "a)", bullets: what opens a list item or a
// reference entry, never a table's first cell on most of its rows.
bool is_list_marker(const std::string& t) {
    if (t.empty()) return false;
    static const char* const bullets[] = {
        "\xE2\x80\xA2", "\xC2\xB7", "-", "*", "\xE2\x97\x8B", "\xE2\x97\x8F",
        "\xE2\x96\xA0", "\xE2\x96\xA1", "\xE2\x96\xAA", "\xE2\x97\xA6",
        "\xE2\x80\x93", "\xE2\x80\x94"};
    for (const char* b : bullets)
        if (t == b) return true;
    size_t i = 0, n = t.size();
    bool open = t[0] == '[' || t[0] == '(';
    if (open) i++;
    size_t d0 = i;
    while (i < n && ((t[i] >= '0' && t[i] <= '9') ||
                     (i == d0 && i + 1 < n && t[i] >= 'a' && t[i] <= 'z')))
        i++;
    if (i == d0 || i - d0 > 3) return false;
    if (i == n) return false;
    char c = t[i];
    if (open) return (c == ']' || c == ')') && i + 1 == n;
    return (c == '.' || c == ')') && i + 1 == n;
}

// "(3)", "(3.2)", "(12a)": a display equation's number.
bool is_equation_number(const std::string& t) {
    if (t.size() < 3 || t.front() != '(' || t.back() != ')') return false;
    bool digit = false;
    for (size_t i = 1; i + 1 < t.size(); i++) {
        char c = t[i];
        if (c >= '0' && c <= '9') digit = true;
        else if (c != '.' && !(c >= 'a' && c <= 'z')) return false;
    }
    return digit;
}

// Four or more leader dots in a row (".", "·", "…").
bool has_leader(const std::string& t) {
    int run = 0;
    for (size_t i = 0; i < t.size();) {
        unsigned char c = t[i];
        size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        bool dot = c == '.' || t.compare(i, n, "\xC2\xB7") == 0 ||
                   t.compare(i, n, "\xE2\x80\xA6") == 0;
        run = dot ? run + 1 : 0;
        if (run >= 4) return true;
        i += n;
    }
    return false;
}

// The value of a cell that is a plain number ("12", "–0.5", "1,200"); false
// for anything else. En dash and minus sign count as a minus.
bool cell_number(const std::string& t, double& v) {
    std::string a;
    for (size_t i = 0; i < t.size();) {
        if (t.compare(i, 3, "\xE2\x80\x93") == 0 ||
            t.compare(i, 3, "\xE2\x88\x92") == 0) {
            a += '-';
            i += 3;
        } else if (t[i] == ',') {
            i++;
        } else {
            a += t[i++];
        }
    }
    if (a.empty() || a.size() > 12) return false;
    char* end = nullptr;
    v = std::strtod(a.c_str(), &end);
    return end && *end == '\0' && a.find_first_of("0123456789") != std::string::npos;
}

// Evenly stepped numbers: a chart axis's tick labels.
bool arithmetic(const std::vector<double>& v, bool decreasing) {
    if (v.size() < 2) return false;
    double step = v[1] - v[0];
    if (step == 0 || (step < 0) != decreasing) return false;
    for (size_t k = 2; k < v.size(); k++)
        if (std::abs((v[k] - v[k - 1]) - step) > std::abs(step) * 0.01)
            return false;
    return true;
}

// A math operator or relation (U+2200 block, minus sign aside): set
// mathematics, not table data.
bool is_math_glyph(uint32_t cp) {
    return cp >= 0x2200 && cp <= 0x22FF && cp != 0x2212;
}

// Row index ranges [first, last] of one block's rows that pass the
// whitespace rule (see the section comment).
std::vector<std::pair<size_t, size_t>> tabular_runs(
        const std::vector<LayoutLine>& rows, double cell_gap) {
    std::vector<std::pair<size_t, size_t>> out;
    const size_t n = rows.size();
    std::vector<RowCells> cells(n);
    std::vector<bool> multi(n);
    for (size_t i = 0; i < n; i++) {
        cells[i] = cut_cells(rows[i], cell_gap);
        multi[i] = !cells[i].gaps.empty();
    }
    // Rows further apart than three line heights belong to separate blocks.
    auto close = [&](size_t a, size_t b) {
        double fs = std::max(rows[a].font_size, rows[b].font_size);
        return rows[a].base_y - rows[b].base_y <= 3.0 * fs;
    };

    size_t i = 0;
    while (i < n) {
        if (!multi[i]) { i++; continue; }
        size_t j = i, last = i;
        while (j + 1 < n && close(j, j + 1)) {
            if (!multi[j + 1] &&
                !(j + 2 < n && multi[j + 2] && close(j + 1, j + 2)))
                break;
            j++;
            if (multi[j]) last = j;
        }

        std::vector<size_t> m;
        for (size_t k = i; k <= last; k++)
            if (multi[k]) m.push_back(k);
        bool ok = m.size() >= 3 && m.size() * 10 >= (last - i + 1) * 6;
        bool two_col = false;
        if (ok) {
            // Column gaps: maximal x-intervals where at least 60% of the
            // multi-cell rows (and three) have a gap. Two are needed (three
            // columns). Counted as intervals, a gap that rows of different
            // lengths open at different x (title, then a page number) is one
            // gap, not many.
            std::vector<std::pair<double, int>> ev;
            for (size_t a : m)
                for (auto& g : cells[a].gaps) {
                    ev.push_back({g.first - 1.0, +1});
                    ev.push_back({g.second + 1.0, -1});
                }
            std::sort(ev.begin(), ev.end(),
                      [](const std::pair<double, int>& x,
                         const std::pair<double, int>& y) {
                          if (x.first != y.first) return x.first < y.first;
                          return x.second > y.second;
                      });
            const size_t need = std::max<size_t>(3, (m.size() * 6 + 9) / 10);
            int rivers = 0, depth = 0;
            bool inside = false;
            for (auto& e : ev) {
                depth += e.second;
                bool now = depth >= (int)need;
                if (now && !inside) rivers++;
                inside = now;
            }
            // Two gaps make three columns; one (two columns) is held to the
            // narrow first cell below.
            ok = rivers >= 1;
            two_col = rivers == 1;
        }
        if (ok) {
            // Data rows: a label first cell, digits in the cells after it.
            double x0 = 1e30, x1 = -1e30;
            for (size_t k = i; k <= last; k++)
                for (auto* g : rows[k].glyphs) {
                    x0 = std::min(x0, g->left);
                    x1 = std::max(x1, ink_right(g));
                }
            const double width = std::max(x1 - x0, 1.0);
            size_t data_rows = 0, value_cells = 0, digit_cells = 0;
            size_t wide_rows = 0, number_first = 0;
            for (size_t a : m) {
                const auto& rc = cells[a];
                const auto& gl = rows[a].glyphs;
                for (size_t c = 1; c < rc.cells.size(); c++) {
                    auto cr = rc.cells[c];
                    if (ink_right(gl[cr.second - 1]) - gl[cr.first]->left >
                        0.4 * width) {
                        wide_rows++;
                        break;
                    }
                }
                auto first = rc.cells.front();
                // A number opening the row, other than a year: a chart's
                // y-axis tick beside its plot.
                double fv;
                std::string ft = cell_text(rows[a], first);
                bool year = ft.size() == 4 &&
                            ft.find_first_not_of("0123456789") == std::string::npos &&
                            ft >= "1900" && ft <= "2100";
                if (cell_number(ft, fv) && !year) number_first++;
                bool letter = false;
                for (size_t k = first.first; k < first.second; k++) {
                    uint32_t cp = gl[k]->unicode;
                    if ((cp | 0x20) - 'a' < 26u || cp >= 0x3040) letter = true;
                }
                double w = ink_right(gl[first.second - 1]) - gl[first.first]->left;
                if (!letter || w > 0.4 * width) continue;
                data_rows++;
                for (size_t c = 1; c < rc.cells.size(); c++) {
                    value_cells++;
                    for (size_t k = rc.cells[c].first; k < rc.cells[c].second; k++)
                        if (gl[k]->unicode >= '0' && gl[k]->unicode <= '9') {
                            digit_cells++;
                            break;
                        }
                }
            }
            ok = data_rows >= 3 && data_rows * 2 >= m.size() &&
                 digit_cells * 10 >= value_cells * 4 &&
                 wide_rows * 2 < m.size() && number_first * 5 <= m.size();
            if (two_col) {
                // Two columns: a narrow first cell on most rows (a term, a
                // year, a code) beside its value, figures or words. Two
                // columns of running text fill half the width each, so their
                // first cells are never narrow.
                // A short table may be narrow enough that its first column
                // takes more than 40% of it; then both ends are short.
                size_t narrow = 0;
                for (size_t a : m) {
                    auto first = cells[a].cells.front(), lastc = cells[a].cells.back();
                    const auto& gl = rows[a].glyphs;
                    double fs = gl[first.first]->font_size > 1.0 ? gl[first.first]->font_size : 10.0;
                    // Measured to its last word: dot leaders after it run
                    // to the value.
                    size_t end = first.second;
                    while (end > first.first + 1) {
                        uint32_t cp = gl[end - 1]->unicode;
                        if (cp != '.' && cp != 0x2026 && cp != 0xB7 && cp != '_') break;
                        end--;
                    }
                    double w = ink_right(gl[end - 1]) - gl[first.first]->left;
                    double wl = ink_right(gl[lastc.second - 1]) - gl[lastc.first]->left;
                    if (w <= 0.4 * width || (w <= 12 * fs && wl <= 12 * fs)) narrow++;
                }
                ok = narrow >= 3 && narrow * 10 >= m.size() * 8;
                // A column of running text beside a side column has lines of
                // its own at the second column's left edge: above the side
                // column, among its lines, or below its last. A table's
                // value column ends with the table.
                std::vector<double> xs;
                for (size_t a : m) {
                    const auto& gl = rows[a].glyphs;
                    xs.push_back(gl[cells[a].cells[1].first]->left);
                }
                std::nth_element(xs.begin(), xs.begin() + xs.size() / 2, xs.end());
                const double col2 = xs[xs.size() / 2];
                auto carries_on = [&](size_t k) {
                    if (multi[k] || rows[k].glyphs.empty()) return false;
                    const auto* g = rows[k].glyphs.front();
                    double fs = g->font_size > 1.0 ? g->font_size : 10.0;
                    return std::abs(g->left - col2) <= fs;
                };
                bool runs_on = i > 0 && close(i - 1, i) && carries_on(i - 1);
                for (size_t k = i; k <= last && !runs_on; k++) runs_on = carries_on(k);
                for (size_t k = last + 1; k < n && k <= last + 3 && !runs_on && close(k - 1, k); k++)
                    runs_on = carries_on(k);
                // Numbers opening most rows ("4:", "12.", "(3)"): the steps of
                // an algorithm or a numbered list, not a table.
                size_t numbered = 0;
                for (size_t a : m) {
                    std::string t = cell_text(rows[a], cells[a].cells.front());
                    size_t d = t.find_first_not_of("(");
                    size_t e = t.find_first_not_of("0123456789", d);
                    if (d != std::string::npos && e != d && e - d <= 3 &&
                        (e == t.size() || (e + 1 >= t.size() - (t.back() == ')' ? 1 : 0) &&
                                           std::string(".:)").find(t[e]) != std::string::npos)))
                        numbered++;
                }
                // The value column holds figures on most rows ("480",
                // "$ 672.8"); a second column of words beside a first is a
                // list, a form, two side columns or a chart's labels as
                // often as a table, and stays text. Rising whole numbers
                // closing the rows are the page numbers of a contents list.
                // Both cells the same on most rows: the axis labels of two
                // charts set side by side.
                size_t figures = 0, pages = 0, twins = 0;
                double prev_page = -1;
                for (size_t a : m) {
                    if (cell_text(rows[a], cells[a].cells.front()) ==
                        cell_text(rows[a], cells[a].cells.back()))
                        twins++;
                    const auto& gl = rows[a].glyphs;
                    auto lc = cells[a].cells.back();
                    size_t digits = 0, letters = 0;
                    for (size_t k = lc.first; k < lc.second; k++) {
                        uint32_t cp = gl[k]->unicode;
                        if (cp >= '0' && cp <= '9') digits++;
                        else if ((cp | 0x20) - 'a' < 26u || cp >= 0x3040) letters++;
                    }
                    if (digits > 0 && digits >= 2 * letters) figures++;
                    double v;
                    std::string lt = cell_text(rows[a], lc);
                    if (cell_number(lt, v) && v == std::floor(v) && v >= 1 && v < 10000 &&
                        lt.find_first_of(".,") == std::string::npos && v >= prev_page) {
                        pages++;
                        prev_page = v;
                    }
                }
                if (ok && (runs_on || numbered * 10 >= m.size() * 6 ||
                           figures * 10 < m.size() * 6 || pages * 10 >= m.size() * 7 ||
                           twins * 10 >= m.size() * 6))
                    ok = false;
            }
        }
        if (ok) {
            // A column of figures: cells that are mostly digits ("5.1",
            // "1,200", "12%"), stacked over one another in three rows.
            // Legends and timelines carry digits scattered or among words
            // ("Jan-20", "35억").
            struct Fig { double x0, x1; size_t row; };
            std::vector<Fig> figs;
            for (size_t a : m)
                for (auto& c : cells[a].cells) {
                    const auto& gl = rows[a].glyphs;
                    size_t digits = 0, letters = 0;
                    for (size_t k = c.first; k < c.second; k++) {
                        uint32_t cp = gl[k]->unicode;
                        if (cp >= '0' && cp <= '9') digits++;
                        else if ((cp | 0x20) - 'a' < 26u || cp >= 0x3040) letters++;
                    }
                    if (digits > 0 && digits >= 2 * letters)
                        figs.push_back({gl[c.first]->left,
                                        ink_right(gl[c.second - 1]), a});
                }
            bool column = false;
            for (auto& f : figs) {
                std::vector<size_t> seen{f.row};
                for (auto& o : figs)
                    if (std::min(f.x1, o.x1) > std::max(f.x0, o.x0) &&
                        std::find(seen.begin(), seen.end(), o.row) == seen.end())
                        seen.push_back(o.row);
                if (seen.size() >= 3) column = true;
            }
            ok = column;
        }
        if (ok) {
            // Chart axes: a run of five or more numeric cells in a row
            // stepping up evenly (x ticks), or three or more numbers stacked
            // at one x stepping down evenly (y ticks; a row-number column
            // counts up).
            for (size_t a : m) {
                std::vector<double> seq;
                for (auto& c : cells[a].cells) {
                    double v;
                    if (cell_number(cell_text(rows[a], c), v)) {
                        seq.push_back(v);
                        if (seq.size() >= 5 && arithmetic(seq, false)) ok = false;
                    } else {
                        seq.clear();
                    }
                    if (seq.size() >= 2 && !arithmetic(seq, false))
                        seq.erase(seq.begin(), seq.end() - 1);
                }
            }
            struct Stack { double x; std::vector<double> v; };
            std::vector<Stack> stacks;
            for (size_t k = i; k <= last && ok; k++)
                for (auto& c : cells[k].cells) {
                    double v;
                    if (!cell_number(cell_text(rows[k], c), v)) continue;
                    double x = ink_right(rows[k].glyphs[c.second - 1]);
                    Stack* st = nullptr;
                    for (auto& s2 : stacks)
                        if (std::abs(s2.x - x) <= 2.0) st = &s2;
                    if (!st) {
                        stacks.push_back({x, {}});
                        st = &stacks.back();
                    }
                    st->v.push_back(v);
                }
            for (auto& st : stacks)
                if (st.v.size() >= 3 && arithmetic(st.v, true)) ok = false;
        }
        if (ok) {
            size_t lists = 0, eqs = 0, leaders = 0, mails = 0, maths = 0;
            for (size_t a : m) {
                const auto& rc = cells[a];
                const auto& gl = rows[a].glyphs;
                if (is_list_marker(cell_text(rows[a], rc.cells.front()))) lists++;
                // An equation number closes the row, set off by a space.
                bool eq = is_equation_number(cell_text(rows[a], rc.cells.back()));
                for (size_t k = gl.size(); !eq && k-- > 1;) {
                    if (gl[k]->unicode != '(') continue;
                    std::string tail = cell_text(rows[a], {k, gl.size()});
                    double fs = gl[k]->font_size > 1.0 ? gl[k]->font_size : 10.0;
                    eq = is_equation_number(tail) &&
                         gl[k]->left - ink_right(gl[k - 1]) >= 0.5 * fs;
                    break;
                }
                if (eq) eqs++;
                for (auto* g : gl)
                    if (is_math_glyph(g->unicode)) {
                        maths++;
                        break;
                    }
                std::string all = cell_text(rows[a], {0, rows[a].glyphs.size()});
                if (has_leader(all)) leaders++;
                if (all.find('@') != std::string::npos) mails++;
            }
            // Dot leaders run to page numbers in a table of contents (a
            // list) and to amounts in a financial statement (a table): only
            // the first, rising whole numbers closing the rows, is dropped.
            size_t pages = 0;
            double prev_page = -1;
            for (size_t a : m) {
                double v;
                std::string lt = cell_text(rows[a], cells[a].cells.back());
                if (cell_number(lt, v) && v == std::floor(v) && v >= 1 && v < 10000 &&
                    lt.find_first_of(".,") == std::string::npos && v >= prev_page) {
                    pages++;
                    prev_page = v;
                }
            }
            bool contents = leaders * 2 >= m.size() && pages * 10 >= m.size() * 7;
            if (lists * 10 >= m.size() * 6 || eqs * 2 >= m.size() ||
                (eqs > 0 && last - i + 1 <= 5) ||
                maths * 10 >= m.size() * 3 || (maths > 0 && last - i + 1 <= 8) ||
                contents || (two_col && leaders * 2 >= m.size() && pages > 0) || mails >= 2)
                ok = false;
        }
        if (ok) out.push_back({i, last});
        i = last + 1;
    }
    return out;
}

bool box_holds(const PageBox& b, double x, double y, double tol) {
    return x >= b[0] - tol && x <= b[2] + tol && y >= b[1] - tol &&
           y <= b[3] + tol;
}

double glyph_cx(const TextChar* g) { return (g->left + ink_right(g)) / 2.0; }
double glyph_cy(const TextChar* g) { return (g->top + g->bot) / 2.0; }

// A fallback over `rows`, laid out from the rows' leftmost glyph. `origin`
// maps a (rotated-frame) glyph back to the page glyph whose box the region
// reports; identity for upright text.
template <typename Origin>
LayoutFallback make_fallback(const std::vector<const LayoutLine*>& rows,
                             const GridPitch& gp, int16_t rot,
                             Origin origin,
                             const SpaceMarks* spaces = nullptr) {
    LayoutFallback fb;
    fb.rot = rot;
    fb.x0 = fb.y0 = 1e30;
    fb.x1 = fb.y1 = -1e30;
    double margin = 1e30;
    for (auto* ln : rows)
        for (auto* g : ln->glyphs) {
            margin = std::min(margin, g->left);
            const TextChar* pg = origin(g);
            fb.x0 = std::min(fb.x0, pg->left);
            // TextChar's own right edge, as the text lines measure it, so a
            // line made of these glyphs is inside the box.
            fb.x1 = std::max(fb.x1, pg->right);
            fb.y0 = std::min(fb.y0, pg->bot);
            fb.y1 = std::max(fb.y1, pg->top);
        }
    std::vector<PlacedRow> placed;
    for (auto* ln : rows) placed.push_back({ln, margin, false});
    fb.text = render_rows(placed, gp.unit, gp.median_fs, spaces);
    return fb;
}

} // namespace

std::vector<LayoutFallback> find_layout_fallbacks(
        const std::vector<TextChar>& chars, double col_boundary,
        const std::vector<PageBox>& table_boxes,
        const std::vector<SparseGrid>& sparse_grids) {
    std::vector<LayoutFallback> out;

    std::vector<const TextChar*> upright;
    std::vector<const TextChar*> rotated;
    for (auto& ch : chars) {
        if (ch.unicode < 0x20 || ch.unicode == 0xFFFD ||
            is_layout_space(ch.unicode))
            continue;
        bool in_table = false;
        for (auto& b : table_boxes)
            if (box_holds(b, glyph_cx(&ch), glyph_cy(&ch), 2.0)) {
                in_table = true;
                break;
            }
        if (in_table) continue;
        (ch.rot == 0 ? upright : rotated).push_back(&ch);
    }
    auto identity = [](const TextChar* g) { return g; };
    SpaceMarks spaces;
    spaces.base = chars.data();
    spaces.sep = explicit_word_spaces(chars);

    if (!upright.empty()) {
        const GridPitch gp = measure_pitch(upright);

        // Sparse ruled grids. One rejected by the ruled detector may still
        // have been taken by a later detector: an accepted table over most
        // of it wins. A table's rule grid holds text in its rows and on both
        // sides of its inner rules; a chart's gridlines and axis enclose a
        // plot drawn in vectors, with its labels outside or scattered. So a
        // grid needs text in 75% of its bands at least a line and a half
        // tall, and two text lines left of an inner rule with text right of
        // it at the same height.
        // Grids that touch vertically and share their x-span merge (a merged
        // section row splits one table's rule grid).
        auto grid_holds_table = [&](const SparseGrid& sg) {
            const PageBox& b = sg.box;
            int tall = 0, with_text = 0, both_sides = 0;
            for (size_t k = 0; k + 1 < sg.levels.size(); k++) {
                double lo = sg.levels[k], hi = sg.levels[k + 1];
                if (hi - lo < 1.5 * gp.median_fs) continue;
                tall++;
                bool any = false;
                for (auto* g : upright) {
                    double cx = glyph_cx(g), cy = glyph_cy(g);
                    if (cy > lo && cy < hi && cx >= b[0] && cx <= b[2]) {
                        any = true;
                        break;
                    }
                }
                if (any) with_text++;
                // Text lines left of a rule that sit beside text right of it:
                // within the band's right-side lines' extent, give or take
                // three quarters of an em (a label centred beside a wrapped
                // cell sits between two of its lines). Drawn per cell, a
                // rule may cover only part of a band.
                for (auto& r : sg.rules) {
                    if (std::min(r[2], hi) - std::max(r[1], lo) <= 0) continue;
                    std::vector<double> left, right;
                    for (auto* g : upright) {
                        double cx = glyph_cx(g), cy = glyph_cy(g);
                        if (cy <= lo || cy >= hi || cx < b[0] || cx > b[2]) continue;
                        if (cy < r[1] || cy > r[2]) continue;
                        (cx < r[0] ? left : right).push_back(g->y);
                    }
                    auto distinct = [](std::vector<double>& v) {
                        std::sort(v.begin(), v.end());
                        std::vector<double> d;
                        for (double y : v)
                            if (d.empty() || y - d.back() > 1.0) d.push_back(y);
                        v = std::move(d);
                    };
                    distinct(left);
                    distinct(right);
                    if (right.empty()) continue;
                    const double slack = 0.75 * gp.median_fs;
                    for (double ly : left)
                        if (ly >= right.front() - slack && ly <= right.back() + slack)
                            both_sides++;
                }
            }
            return tall > 0 && with_text * 4 >= tall * 3 && both_sides >= 2;
        };
        std::vector<PageBox> grids;
        for (auto& sg : sparse_grids) {
            if (!grid_holds_table(sg)) continue;
            const PageBox& g = sg.box;
            double area = (g[2] - g[0]) * (g[3] - g[1]);
            bool taken = false;
            for (auto& t : table_boxes) {
                double ox = std::min(g[2], t[2]) - std::max(g[0], t[0]);
                double oy = std::min(g[3], t[3]) - std::max(g[1], t[1]);
                if (ox > 0 && oy > 0 && ox * oy >= area * 0.5) taken = true;
            }
            if (!taken) grids.push_back(g);
        }
        std::sort(grids.begin(), grids.end(),
                  [](const PageBox& a, const PageBox& b) { return a[3] > b[3]; });
        std::vector<PageBox> merged;
        for (auto& g : grids) {
            if (!merged.empty()) {
                auto& m = merged.back();
                double span = std::max(m[2] - m[0], g[2] - g[0]);
                double ox = std::min(m[2], g[2]) - std::max(m[0], g[0]);
                if (m[1] - g[3] <= 3.0 * gp.median_fs && ox >= 0.8 * span) {
                    m = {std::min(m[0], g[0]), std::min(m[1], g[1]),
                         std::max(m[2], g[2]), std::max(m[3], g[3])};
                    continue;
                }
            }
            merged.push_back(g);
        }
        std::vector<const TextChar*> rest;
        std::vector<std::vector<const TextChar*>> in_grid(merged.size());
        for (auto* g : upright) {
            size_t hit = merged.size();
            for (size_t k = 0; k < merged.size() && hit == merged.size(); k++)
                if (box_holds(merged[k], glyph_cx(g), glyph_cy(g), 2.0)) hit = k;
            (hit < merged.size() ? in_grid[hit] : rest).push_back(g);
        }
        for (auto& gl : in_grid) {
            if (gl.empty()) continue;
            auto lines = group_lines(gl, gp.median_fs);
            if (lines.size() < 2) continue;
            std::vector<const LayoutLine*> rows;
            for (auto& ln : lines) rows.push_back(&ln);
            out.push_back(make_fallback(rows, gp, 0, identity, &spaces));
        }

        // Whitespace alignment, zone by zone.
        if (!rest.empty()) {
            const double cell_gap = std::max(0.8 * gp.median_fs, 8.0);
            auto lines = group_lines(rest, gp.median_fs);
            std::vector<LayoutBlock> blocks;
            if (col_boundary > 0) {
                blocks = column_blocks(lines, col_boundary, gp.margin,
                                       gp.median_fs);
            } else {
                blocks.emplace_back();
                blocks.back().rows = std::move(lines);
            }
            const size_t found = out.size();
            for (auto& blk : blocks)
                for (auto& run : tabular_runs(blk.rows, cell_gap)) {
                    std::vector<const LayoutLine*> rows;
                    for (size_t k = run.first; k <= run.second; k++)
                        rows.push_back(&blk.rows[k]);
                    out.push_back(make_fallback(rows, gp, 0, identity, &spaces));
                }
            // A page whose column boundary falls inside a tabular region (a
            // page holding little but that region) cuts its rows in two at
            // the boundary, and neither half is a region any more. The rows
            // are tried again whole; a region found that way is kept where
            // no column block found one at its height. Two columns of
            // running text do not pass: their first cells are not narrow.
            if (col_boundary > 0) {
                auto whole = group_lines(rest, gp.median_fs);
                for (auto& run : tabular_runs(whole, cell_gap)) {
                    double top = whole[run.first].base_y, bot = whole[run.second].base_y;
                    bool seen = false;
                    for (size_t k = found; k < out.size() && !seen; k++)
                        seen = out[k].y0 <= top + gp.median_fs && out[k].y1 >= bot - gp.median_fs;
                    if (seen) continue;
                    std::vector<const LayoutLine*> rows;
                    for (size_t k = run.first; k <= run.second; k++)
                        rows.push_back(&whole[k]);
                    out.push_back(make_fallback(rows, gp, 0, identity, &spaces));
                }
            }
        }
    }

    // Rotated runs, each direction in its own frame (as chars_to_lines).
    for (int r = 1; r < 24 && !rotated.empty(); r++) {
        std::vector<TextChar> frame;
        std::vector<const TextChar*> page_glyph;
        for (auto* ch : rotated) {
            if (ch->rot != r) continue;
            frame.push_back(to_writing_frame(*ch));
            page_glyph.push_back(ch);
        }
        if (frame.size() < 6) continue;
        std::vector<const TextChar*> glyphs;
        for (auto& t : frame) glyphs.push_back(&t);
        const GridPitch gp = measure_pitch(glyphs);
        const double cell_gap = std::max(0.8 * gp.median_fs, 8.0);
        auto lines = group_lines(glyphs, gp.median_fs);
        const TextChar* base = frame.data();
        auto origin = [&](const TextChar* g) {
            return page_glyph[static_cast<size_t>(g - base)];
        };
        for (auto& run : tabular_runs(lines, cell_gap)) {
            std::vector<const LayoutLine*> rows;
            for (size_t k = run.first; k <= run.second; k++)
                rows.push_back(&lines[k]);
            out.push_back(make_fallback(rows, gp, static_cast<int16_t>(r),
                                        origin));
        }
    }
    return out;
}

}} // namespace jdoc::pdf_detail
