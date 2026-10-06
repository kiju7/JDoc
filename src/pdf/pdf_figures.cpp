// Drawing regions: where on the page a figure is drawn, from the marks the
// content stream paints (see pdf_content.h, DrawnShape). The table
// detectors read text by its alignment alone, and a chart's axis ticks,
// legends and panel titles align as well as any table's cells. What tells
// them apart is what is drawn around them: a chart holds marks no table
// draws, so its text is a figure's and never a table's.
#include "pdf_content.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace jdoc { namespace pdf_detail {

namespace {

struct Mark {
    double x0, y0, x1, y1;
    bool nonortho;  // a curve or a diagonal line: never ruling
    bool bar;       // a dark or coloured fill taller than a line with no text in it
    bool image;     // a raster placement
};

struct DisjointSet {
    std::vector<size_t> parent;
    explicit DisjointSet(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    size_t find(size_t a) {
        while (parent[a] != a) a = parent[a] = parent[parent[a]];
        return a;
    }
    void unite(size_t a, size_t b) { parent[find(a)] = find(b); }
};

} // namespace

std::vector<PageBox> drawing_regions(const ContentParseResult& pr,
                                     const std::vector<TextChar>& chars,
                                     double page_w, double page_h) {
    std::vector<PageBox> out;
    std::vector<double> fs;
    for (const auto& c : chars)
        if (c.font_size > 1.0 && c.unicode != ' ') fs.push_back(c.font_size);
    if (fs.size() < 10) return out;
    std::nth_element(fs.begin(), fs.begin() + fs.size() / 2, fs.end());
    const double em = fs[fs.size() / 2];

    // A glyph centre inside a box: the fill is a cell's shading, not a bar.
    auto holds_text = [&](double x0, double y0, double x1, double y1) {
        for (const auto& c : chars) {
            if (c.unicode == ' ' || c.unicode == 0xA0) continue;
            double cx = (c.left + c.right) / 2, cy = (c.top + c.bot) / 2;
            if (cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1) return true;
        }
        return false;
    };
    // Page frames and backgrounds join everything and mean nothing.
    auto page_sized = [&](double w, double h) { return w > 0.7 * page_w && h > 0.7 * page_h; };

    std::vector<Mark> marks;
    for (const auto& sh : pr.shapes) {
        double w = sh.x1 - sh.x0, h = sh.y1 - sh.y0;
        if (!(w >= 0) || !(h >= 0) || page_sized(w, h)) continue;
        if (sh.x1 < 0 || sh.x0 > page_w || sh.y1 < 0 || sh.y0 > page_h) continue;
        bool nonortho = sh.curve || !sh.ortho;
        // Bars: dark or coloured fills with two real dimensions, taller than
        // a text line and taller than wide, holding no text. Dot leaders
        // drawn as specks and the shading of a table's cells (text inside,
        // a line tall, wider than tall) are not bars.
        bool bar = sh.filled && !sh.rule && (sh.dark || sh.colored) && std::min(w, h) >= 3.0 &&
                   h >= 1.5 * em && h > w && !holds_text(sh.x0, sh.y0, sh.x1, sh.y1);
        marks.push_back({sh.x0, sh.y0, sh.x1, sh.y1, nonortho, bar, false});
    }
    for (const auto& b : pr.image_boxes) {
        double w = b[2] - b[0], h = b[3] - b[1];
        if (page_sized(w, h) || w < 1.0 || h < 1.0) continue;
        marks.push_back({b[0], b[1], b[2], b[3], false, false, true});
    }
    if (marks.empty()) return out;

    // Marks that touch (within eps) belong to one drawing. Sorted by x0, a
    // mark only needs comparing with those starting before its right edge.
    const double eps = 2.0;
    std::sort(marks.begin(), marks.end(), [](const Mark& a, const Mark& b) { return a.x0 < b.x0; });
    DisjointSet sets(marks.size());
    for (size_t i = 0; i < marks.size(); i++)
        for (size_t j = i + 1; j < marks.size() && marks[j].x0 <= marks[i].x1 + eps; j++)
            if (marks[j].y0 <= marks[i].y1 + eps && marks[i].y0 <= marks[j].y1 + eps)
                sets.unite(i, j);

    // Each cluster's hull takes in every mark it holds, axes, frame and
    // gridlines included: the labels stand beside those, not beside the
    // bars and lines alone. Whether the cluster is a drawing is decided on
    // its evidence marks only.
    struct Cluster {
        size_t nonortho = 0, bars = 0, images = 0;
        double hx0 = 1e300, hy0 = 1e300, hx1 = -1e300, hy1 = -1e300;
        double image_area = 0;
        std::vector<long> bar_heights;  // quantised to 2pt: bars encode data by their length
    };
    std::vector<Cluster> clusters(marks.size());
    for (size_t i = 0; i < marks.size(); i++) {
        const Mark& m = marks[i];
        Cluster& c = clusters[sets.find(i)];
        c.hx0 = std::min(c.hx0, m.x0); c.hy0 = std::min(c.hy0, m.y0);
        c.hx1 = std::max(c.hx1, m.x1); c.hy1 = std::max(c.hy1, m.y1);
        if (!m.nonortho && !m.bar && !m.image) continue;
        if (m.nonortho) c.nonortho++;
        if (m.bar) {
            c.bars++;
            c.bar_heights.push_back(std::lround((m.y1 - m.y0) / 2.0));
        }
        if (m.image) {
            c.images++;
            c.image_area += (m.x1 - m.x0) * (m.y1 - m.y0);
        }
    }
    // Every cluster, with evidence or without: a legend's colour swatches
    // and the tick marks of an axis are small marks standing a little apart
    // from the plot, and they belong to it.
    std::vector<Cluster> live;
    std::vector<bool> is_root(marks.size(), false);
    for (size_t i = 0; i < marks.size(); i++) is_root[sets.find(i)] = true;
    for (size_t r = 0; r < clusters.size(); r++)
        if (is_root[r]) live.push_back(clusters[r]);
    // A chart's marks seldom touch: bars stand apart, a line series floats
    // over its gridlines, and the panels of one figure sit side by side a
    // gutter apart. Hulls within three ems of one another are one drawing.
    const double near = 3.0 * em;
    // A cluster of rules alone that frames a block of text is a table or a
    // text box set near the drawing, not a part of it: legend swatches and
    // axis ticks hold no text. Keep it out of the proximity merge, or the
    // drawing's region swallows the table's text.
    auto framed_text = [&](const Cluster& c) {
        if (c.nonortho || c.bars || c.images) return false;
        size_t glyphs = 0;
        for (const auto& ch : chars) {
            if (ch.unicode == ' ' || ch.unicode == 0xA0) continue;
            double cx = (ch.left + ch.right) / 2, cy = (ch.top + ch.bot) / 2;
            if (cx >= c.hx0 && cx <= c.hx1 && cy >= c.hy0 && cy <= c.hy1 && ++glyphs >= 20) return true;
        }
        return false;
    };
    std::vector<char> framed(live.size());
    for (size_t i = 0; i < live.size(); i++) framed[i] = framed_text(live[i]);
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t a = 0; a < live.size() && !merged; a++)
            for (size_t b = a + 1; b < live.size(); b++) {
                if (framed[a] || framed[b]) continue;
                if (std::max(live[a].hx0, live[b].hx0) - std::min(live[a].hx1, live[b].hx1) > near) continue;
                if (std::max(live[a].hy0, live[b].hy0) - std::min(live[a].hy1, live[b].hy1) > near) continue;
                live[a].nonortho += live[b].nonortho;
                live[a].bars += live[b].bars;
                live[a].images += live[b].images;
                live[a].image_area += live[b].image_area;
                live[a].bar_heights.insert(live[a].bar_heights.end(), live[b].bar_heights.begin(), live[b].bar_heights.end());
                live[a].hx0 = std::min(live[a].hx0, live[b].hx0); live[a].hy0 = std::min(live[a].hy0, live[b].hy0);
                live[a].hx1 = std::max(live[a].hx1, live[b].hx1); live[a].hy1 = std::max(live[a].hy1, live[b].hy1);
                live.erase(live.begin() + b);
                framed.erase(framed.begin() + b);
                merged = true;
                break;
            }
    }
    for (const Cluster& c : live) {
        double hw = c.hx1 - c.hx0, hh = c.hy1 - c.hy0;
        if (hw <= 0 || hh <= 0 || page_sized(hw, hh)) continue;
        // A raster drawing: rasters covering half the hull, the hull at least
        // a few lines each way (an inline icon is no figure).
        bool raster = c.images > 0 && c.image_area >= 0.5 * hw * hh && hw >= 3.0 * em && hh >= 3.0 * em;
        // Bars encode data by their length, so a series of them stands at
        // two heights at least; the shaded cells of a table share one.
        std::vector<long> hs = c.bar_heights;
        std::sort(hs.begin(), hs.end());
        size_t heights = std::unique(hs.begin(), hs.end()) - hs.begin();
        bool bars = c.bars >= 3 && heights >= 2;
        if (c.nonortho < 2 && !bars && !raster) continue;
        // Labels sit beside the drawing: tick labels left of the plot, as
        // wide as a figure with its sign ("–100"), tick labels and a legend
        // under it, a line or two deep. The region takes them in.
        const double grow_x = 3.0 * em, grow_y = 2.5 * em;
        out.push_back({std::max(0.0, c.hx0 - grow_x), std::max(0.0, c.hy0 - grow_y),
                       std::min(page_w, c.hx1 + grow_x), std::min(page_h, c.hy1 + grow_y)});
    }
    return out;
}

}} // namespace jdoc::pdf_detail
