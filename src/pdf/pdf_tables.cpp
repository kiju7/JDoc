#include "pdf_extract.h"
#include "common/file_utils.h"
#include "common/string_utils.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jdoc { namespace pdf_detail {

std::vector<std::pair<double,double>> merge_char_ranges(
        std::vector<std::pair<double,double>>& ranges, double merge_gap = 8.0) {
    std::vector<std::pair<double,double>> spans;
    if (ranges.empty()) return spans;
    std::sort(ranges.begin(), ranges.end());
    auto cur = ranges[0];
    for (size_t i = 1; i < ranges.size(); i++) {
        if (ranges[i].first - cur.second < merge_gap) {
            cur.second = std::max(cur.second, ranges[i].second);
        } else {
            spans.push_back(cur);
            cur = ranges[i];
        }
    }
    spans.push_back(cur);
    return spans;
}

std::vector<double> cluster_values(std::vector<double>& vals, double tol) {
    if (vals.empty()) return {};
    std::sort(vals.begin(), vals.end());
    std::vector<double> clusters;
    clusters.push_back(vals[0]);
    for (size_t i = 1; i < vals.size(); i++) {
        if (vals[i] - clusters.back() > tol)
            clusters.push_back(vals[i]);
    }
    return clusters;
}

std::vector<double> detect_response_boundaries(const PageCharCache& cache,
                                                double left, double right,
                                                const std::vector<double>& row_ys) {
    double width = right - left;
    if (width < 150) return {};

    int n_rows = (int)row_ys.size() - 1;
    if (n_rows < 2) return {};

    struct RowChars { std::vector<double> xs; };
    std::vector<RowChars> per_row(n_rows);

    for (auto& ch : cache.chars) {
        if (ch.unicode == ' ' || ch.unicode == 0xA0 || ch.unicode == '\t') continue;
        if (ch.x < left + 1 || ch.x > right - 1) continue;

        for (int r = 0; r < n_rows; r++) {
            double bot = std::min(row_ys[r], row_ys[r+1]);
            double top = std::max(row_ys[r], row_ys[r+1]);
            if (ch.y >= bot + 1 && ch.y <= top - 1) {
                per_row[r].xs.push_back(ch.x);
                break;
            }
        }
    }

    std::vector<double> all_centers;
    int rows_with_clusters = 0;

    for (int r = 0; r < n_rows; r++) {
        auto& xs = per_row[r].xs;
        if (xs.size() < 3) continue;
        std::sort(xs.begin(), xs.end());

        std::vector<double> centers;
        double sum = xs[0];
        int cnt = 1;
        for (size_t i = 1; i < xs.size(); i++) {
            if (xs[i] - xs[i-1] > 20.0) {
                centers.push_back(sum / cnt);
                sum = xs[i];
                cnt = 1;
            } else {
                sum += xs[i];
                cnt++;
            }
        }
        centers.push_back(sum / cnt);

        if ((int)centers.size() >= 3) {
            rows_with_clusters++;
            for (double c : centers)
                all_centers.push_back(c);
        }
    }

    if (rows_with_clusters < 2) return {};

    auto stable_xs = cluster_values(all_centers, 15.0);

    if ((int)stable_xs.size() > 7) {
        int n = (int)stable_xs.size();
        std::vector<int> hits(n, 0);
        for (int i = 0; i < n; i++)
            for (double c : all_centers)
                if (std::abs(c - stable_xs[i]) < 15.0) hits[i]++;

        std::vector<int> order(n);
        for (int i = 0; i < n; i++) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return hits[a] > hits[b]; });

        int top = std::min(n, 8);
        std::vector<double> cands;
        for (int i = 0; i < top; i++) cands.push_back(stable_xs[order[i]]);
        std::sort(cands.begin(), cands.end());

        if ((int)cands.size() >= 5) {
            double best_var = 1e9;
            std::vector<double> best_set;
            int cn = (int)cands.size();
            for (int a = 0; a < cn-4; a++)
            for (int b = a+1; b < cn-3; b++)
            for (int c = b+1; c < cn-2; c++)
            for (int d = c+1; d < cn-1; d++)
            for (int e = d+1; e < cn; e++) {
                double xs[5] = {cands[a], cands[b], cands[c], cands[d], cands[e]};
                double avg = (xs[4] - xs[0]) / 4.0;
                double var = 0;
                for (int i = 0; i < 4; i++) {
                    double g = xs[i+1] - xs[i] - avg;
                    var += g * g;
                }
                if (var < best_var) { best_var = var; best_set = {xs[0], xs[1], xs[2], xs[3], xs[4]}; }
            }
            if (!best_set.empty() && best_var < 200.0) {
                stable_xs = best_set;
            } else {
                return {};
            }
        } else {
            return {};
        }
    }

    if ((int)stable_xs.size() < 3 || (int)stable_xs.size() > 7) return {};

    int n_sub = (int)stable_xs.size();
    std::vector<double> boundaries;
    boundaries.push_back(left);
    for (int i = 0; i < n_sub - 1; i++) {
        boundaries.push_back((stable_xs[i] + stable_xs[i+1]) / 2.0);
    }
    boundaries.push_back(right);

    return boundaries;
}

bool is_scale_row(const PageCharCache& cache, double left, double right,
                   double bot, double top, const std::vector<double>& boundaries) {
    int n_sub = (int)boundaries.size() - 1;
    if (n_sub < 3) return false;

    int filled = 0, total_len = 0, max_len = 0;
    for (int sc = 0; sc < n_sub; sc++) {
        std::string t = cache.get_text_in_rect(boundaries[sc], top, boundaries[sc+1], bot);
        int len = (int)t.size();
        if (len > 0) { filled++; total_len += len; }
        if (len > max_len) max_len = len;
    }

    return filled >= 3 && max_len <= 40 && total_len <= 80;
}

std::vector<double> find_column_boundaries(
        const std::vector<PdfLineSegment>& v_lines,
        const std::vector<PdfLineSegment>& h_lines,
        double table_left, double table_right,
        double table_bot, double table_top,
        const std::vector<double>& row_ys) {
    double table_height = table_top - table_bot;

    // Compute average row height for gap tolerance
    double avg_row_h = table_height;
    if (row_ys.size() >= 2) {
        avg_row_h = (row_ys.back() - row_ys.front()) / (double)(row_ys.size() - 1);
    }
    double gap_tol = std::max(3.0, avg_row_h * 0.6);

    std::vector<double> vx_vals;
    for (auto& vl : v_lines) {
        double vx = (vl.x0 + vl.x1) / 2.0;
        double vy_lo = std::min((double)vl.y0, (double)vl.y1);
        double vy_hi = std::max((double)vl.y0, (double)vl.y1);
        if (vy_hi < table_bot - 5 || vy_lo > table_top + 5) continue;
        if (vx < table_left - 5 || vx > table_right + 5) continue;
        vx_vals.push_back(vx);
    }
    auto vx_clusters = cluster_values(vx_vals, 5.0);

    struct VLineInfo { double x, coverage; int seg_count; };
    std::vector<VLineInfo> vline_infos;
    for (double cx : vx_clusters) {
        std::vector<std::pair<double,double>> intervals;
        for (auto& vl : v_lines) {
            double vx = (vl.x0 + vl.x1) / 2.0;
            if (std::abs(vx - cx) > 6.0) continue;
            double vy_lo = std::min((double)vl.y0, (double)vl.y1);
            double vy_hi = std::max((double)vl.y0, (double)vl.y1);
            if (vy_hi < table_bot - 5 || vy_lo > table_top + 5) continue;
            intervals.push_back({vy_lo, vy_hi});
        }
        if (intervals.empty()) continue;
        int seg_count = (int)intervals.size();
        std::sort(intervals.begin(), intervals.end());
        double total = 0, cur_lo = intervals[0].first, cur_hi = intervals[0].second;
        for (size_t i = 1; i < intervals.size(); i++) {
            if (intervals[i].first <= cur_hi + gap_tol)
                cur_hi = std::max(cur_hi, intervals[i].second);
            else { total += cur_hi - cur_lo; cur_lo = intervals[i].first; cur_hi = intervals[i].second; }
        }
        total += cur_hi - cur_lo;
        vline_infos.push_back({cx, total, seg_count});
    }

    // Accept column boundary:
    // - High coverage (≥ 50%): strong continuous v-line
    // - Many segments (Word→PDF cell-unit borders, ≥ 1/3 of rows)
    // - Moderate coverage (≥ 15%) with h-line endpoint evidence at ≥ half of row levels
    int min_segs = std::max(2, static_cast<int>(row_ys.size() / 3));
    std::vector<double> col_xs;
    col_xs.push_back(table_left);
    for (auto& vi : vline_infos) {
        if (vi.x <= table_left + 5 || vi.x >= table_right - 5) continue;
        // Count row levels where h-lines terminate at this v-line x (true grid evidence)
        int rows_with_ep = 0;
        for (double ry : row_ys) {
            for (auto& hl : h_lines) {
                double hy = (hl.y0 + hl.y1) / 2.0;
                if (std::abs(hy - ry) > 4.0) continue;
                double hx_lo = std::min((double)hl.x0, (double)hl.x1);
                double hx_hi = std::max((double)hl.x0, (double)hl.x1);
                if (std::abs(hx_lo - vi.x) < 8.0 || std::abs(hx_hi - vi.x) < 8.0) {
                    rows_with_ep++;
                    break;
                }
            }
        }
        bool accept = vi.coverage >= table_height * 0.5 ||
                      vi.seg_count >= min_segs ||
                      (vi.coverage >= table_height * 0.15 &&
                       rows_with_ep >= (int)row_ys.size() / 2);
        if (accept)
            col_xs.push_back(vi.x);
    }
    col_xs.push_back(table_right);

    std::sort(col_xs.begin(), col_xs.end());
    col_xs.erase(std::unique(col_xs.begin(), col_xs.end(),
        [](double a, double b) { return std::abs(a - b) < 5.0; }), col_xs.end());

    if (col_xs.size() > 3) {
        std::vector<double> merged;
        merged.push_back(col_xs[0]);
        for (size_t i = 1; i < col_xs.size() - 1; i++) {
            if (col_xs[i] - merged.back() < 25.0) {
                double cov = 0;
                int segs = 0;
                for (auto& vi : vline_infos)
                    if (std::abs(vi.x - col_xs[i]) < 6.0) { cov = vi.coverage; segs = vi.seg_count; break; }
                if (cov >= table_height * 0.5 || segs >= min_segs)
                    merged.push_back(col_xs[i]);
            } else {
                merged.push_back(col_xs[i]);
            }
        }
        merged.push_back(col_xs.back());
        col_xs = std::move(merged);
    }

    // Accepted boundaries already have repeated rule evidence.  Keep wide
    // grids intact (survey/result tables commonly exceed eight columns) and
    // retain only a generous safety cap for pathological vector drawings.
    constexpr size_t kMaxStrongGridColumns = 64;
    while (col_xs.size() > kMaxStrongGridColumns + 1) {
        double min_gap = 1e9;
        size_t min_idx = 1;
        for (size_t i = 1; i < col_xs.size() - 1; i++) {
            double gap = col_xs[i + 1] - col_xs[i];
            if (gap < min_gap) { min_gap = gap; min_idx = i; }
        }
        col_xs.erase(col_xs.begin() + min_idx);
    }
    return col_xs;
}

// cell_bold rides along with rows: every site that reshapes rows moves the
// mask the same way (no-ops when the table carries no mask).
static void mask_erase_row(TableData& t, size_t r) {
    if (r < t.cell_bold.size()) t.cell_bold.erase(t.cell_bold.begin() + r);
}
static void mask_erase_col(TableData& t, size_t c) {
    for (auto& m : t.cell_bold)
        if (c < m.size()) m.erase(m.begin() + c);
}
static void mask_pop_col(TableData& t) {
    for (auto& m : t.cell_bold)
        if (!m.empty()) m.pop_back();
}

void trim_table(TableData& table) {
    auto row_empty = [](const std::vector<std::string>& row) {
        for (auto& c : row) if (!c.empty()) return false;
        return true;
    };
    while (!table.rows.empty() && row_empty(table.rows.back())) {
        table.rows.pop_back();
        mask_erase_row(table, table.rows.size());
    }
    while (!table.rows.empty() && row_empty(table.rows.front())) {
        table.rows.erase(table.rows.begin());
        mask_erase_row(table, 0);
    }

    while (!table.rows.empty() && !table.rows[0].empty()) {
        int last = (int)table.rows[0].size() - 1;
        bool empty = true;
        for (auto& row : table.rows)
            if (last < (int)row.size() && !row[last].empty()) { empty = false; break; }
        if (empty) {
            for (auto& row : table.rows) if (!row.empty()) row.pop_back();
            mask_pop_col(table);
        }
        else break;
    }
}

// Forward declaration
std::vector<double> infer_columns_from_text(const PageCharCache& cache,
                                             double left, double right,
                                             const std::vector<double>& row_ys);

TableData build_table(const std::vector<double>& row_ys,
                       const std::vector<PdfLineSegment>& h_lines,
                       const std::vector<PdfLineSegment>& v_lines,
                       const PageCharCache& cache,
                       bool drawn_rules) {
    TableData table;
    double table_top = row_ys.back();
    double table_bot = row_ys.front();

    double table_left = 1e9, table_right = 0;
    for (auto& hl : h_lines) {
        double hy = (hl.y0 + hl.y1) / 2.0;
        bool in_table = false;
        for (auto& ry : row_ys)
            if (std::abs(hy - ry) < 4.0) { in_table = true; break; }
        if (!in_table) continue;
        double lx = std::min((double)hl.x0, (double)hl.x1);
        double rx = std::max((double)hl.x0, (double)hl.x1);
        if (lx < table_left) table_left = lx;
        if (rx > table_right) table_right = rx;
    }

    if (table_left > table_right) {
        // No h-lines at these row levels (v-line-only grid): span from the
        // v-lines, widened to text sitting within one column-width outside
        // (the outer columns of such tables have no bounding rules).
        std::vector<double> vx;
        for (auto& vl : v_lines) {
            double vy_lo = std::min((double)vl.y0, (double)vl.y1);
            double vy_hi = std::max((double)vl.y0, (double)vl.y1);
            if (vy_hi < table_bot - 5 || vy_lo > table_top + 5) continue;
            vx.push_back((vl.x0 + vl.x1) / 2.0);
        }
        auto vxs = cluster_values(vx, 5.0);
        if (vxs.size() < 2) { table.rows.clear(); return table; }
        double max_gap = 0;
        for (size_t i = 1; i < vxs.size(); i++)
            max_gap = std::max(max_gap, vxs[i] - vxs[i-1]);
        if (max_gap < 40.0) max_gap = 100.0;
        double ext_l = vxs.front(), ext_r = vxs.back();
        for (auto& ch : cache.chars) {
            if (ch.unicode == ' ' || ch.unicode == 0xA0 || ch.unicode == '\t') continue;
            if (ch.y < table_bot + 1 || ch.y > table_top - 1) continue;
            if (ch.left < ext_l && ch.left > vxs.front() - max_gap * 1.2)
                ext_l = ch.left;
            if (ch.right > ext_r && ch.right < vxs.back() + max_gap * 1.2)
                ext_r = ch.right;
        }
        table_left = ext_l - 2;
        table_right = ext_r + 2;
    }

    auto col_xs = find_column_boundaries(v_lines, h_lines, table_left, table_right,
                                          table_bot, table_top, row_ys);

    int internal_vline_count = 0;
    for (auto& vl : v_lines) {
        double vx = (vl.x0 + vl.x1) / 2.0;
        if (vx > table_left + 10 && vx < table_right - 10) {
            double vy_lo = std::min((double)vl.y0, (double)vl.y1);
            double vy_hi = std::max((double)vl.y0, (double)vl.y1);
            if (vy_hi > table_bot - 5 && vy_lo < table_top + 5)
                internal_vline_count++;
        }
    }

    if (col_xs.size() < 3 && internal_vline_count == 0) {
        col_xs = infer_columns_from_text(cache, table_left, table_right, row_ys);
    }

    // A drawn grid may omit single rules (a subdivided header cell whose
    // data columns are separated by whitespace only).  When text alignment
    // independently reproduces most drawn boundaries, its extra boundaries
    // inside unusually wide columns are trusted as the omitted rules.  The
    // added boundaries carry no v-lines, so they are exempted from the
    // v-line colspan logic below.
    std::vector<double> text_boundaries;
    if (col_xs.size() >= 4 && internal_vline_count > 0) {
        auto inferred = infer_columns_from_text(cache, table_left,
                                                table_right, row_ys);
        std::vector<double> widths;
        for (size_t i = 1; i < col_xs.size(); i++)
            widths.push_back(col_xs[i] - col_xs[i - 1]);
        std::nth_element(widths.begin(), widths.begin() + widths.size() / 2,
                         widths.end());
        double median_w = widths[widths.size() / 2];
        // Right-aligned values shift a whitespace gap's center away from
        // the drawn rule, so alignment is judged at column scale.
        double align_tol = std::max(12.0, median_w * 0.4);
        size_t internal_n = col_xs.size() - 2;
        int aligned = 0;
        for (size_t i = 1; i + 1 < col_xs.size(); i++)
            for (double tx : inferred)
                if (std::abs(tx - col_xs[i]) < align_tol) { aligned++; break; }
        if (internal_n > 0 && aligned * 10 >= (int)internal_n * 7) {
            for (size_t j = 1; j + 1 < inferred.size(); j++) {
                double tx = inferred[j];
                // The offset twin of a drawn rule is not a new boundary.
                bool near_drawn = false;
                for (double cx : col_xs)
                    if (std::abs(cx - tx) < align_tol) { near_drawn = true; break; }
                if (near_drawn) continue;
                auto it = std::upper_bound(col_xs.begin(), col_xs.end(), tx);
                if (it == col_xs.begin() || it == col_xs.end()) continue;
                double lo = *(it - 1), hi = *it;
                if (hi - lo < median_w * 1.5) continue;    // not a wide column
                if (tx - lo < 15.0 || hi - tx < 15.0) continue;
                // An omitted rule separates text on both of its sides;
                // whitespace with no text at all on one side is the padding
                // of a wide left- or right-aligned column, not a column.
                int left_rows = 0, right_rows = 0;
                for (size_t r = 0; r + 1 < row_ys.size(); r++) {
                    double rb = std::min(row_ys[r], row_ys[r + 1]);
                    double rt = std::max(row_ys[r], row_ys[r + 1]);
                    bool has_l = false, has_r = false;
                    for (auto& ch : cache.chars) {
                        if (ch.unicode == ' ' || ch.unicode == 0xA0 ||
                            ch.unicode == '\t') continue;
                        if (ch.y <= rb || ch.y >= rt) continue;
                        if (ch.x > lo && ch.x < tx) has_l = true;
                        else if (ch.x > tx && ch.x < hi) has_r = true;
                    }
                    left_rows += has_l;
                    right_rows += has_r;
                }
                if (left_rows == 0 || right_rows == 0) continue;
                text_boundaries.push_back(tx);
            }
            if (!text_boundaries.empty()) {
                col_xs.insert(col_xs.end(), text_boundaries.begin(),
                              text_boundaries.end());
                std::sort(col_xs.begin(), col_xs.end());
            }
        }
    }

    if (col_xs.size() < 3) {
        table.rows.clear();
        return table;
    }

    // Per-level h-rule coverage (fraction of the grid width that carries a
    // rule) feeds the edge tolerance below and both closed-grid tests
    // further down.  Use the drawn row levels here, not text-derived row
    // splits: a ruled section may contain several wrapped logical rows
    // between two horizontal borders while remaining a closed grid.
    const double grid_width = table_right - table_left;
    std::vector<double> level_coverage;
    for (double ry : row_ys) {
        std::vector<std::pair<double,double>> intervals;
        for (auto& hl : h_lines) {
            double hy = (hl.y0 + hl.y1) / 2.0;
            if (std::abs(hy - ry) > 4.0) continue;
            double lo = std::max(table_left,
                                 std::min((double)hl.x0, (double)hl.x1));
            double hi = std::min(table_right,
                                 std::max((double)hl.x0, (double)hl.x1));
            if (hi > lo) intervals.push_back({lo, hi});
        }
        std::sort(intervals.begin(), intervals.end());
        double coverage = 0;
        if (!intervals.empty()) {
            double lo = intervals[0].first, hi = intervals[0].second;
            for (size_t i = 1; i < intervals.size(); i++) {
                if (intervals[i].first <= hi + 2.0)
                    hi = std::max(hi, intervals[i].second);
                else {
                    coverage += hi - lo;
                    lo = intervals[i].first;
                    hi = intervals[i].second;
                }
            }
            coverage += hi - lo;
        }
        level_coverage.push_back(grid_width > 0 ? coverage / grid_width : 0.0);
    }

    std::vector<double> actual_ys;
    bool merge_wrap_rows = false;
    bool rows_are_drawn = true;   // rows are the drawn rule intervals
    {
        double tl = col_xs.front(), tr = col_xs.back();
        double row_h = (row_ys.size() >= 2) ? (row_ys[1] - row_ys[0]) : 18.0;
        int n_cols_found = (int)col_xs.size() - 1;

        struct CharPos { double x, y; };
        std::vector<CharPos> cchars;
        for (auto& ch : cache.chars) {
            if (ch.unicode == ' ' || ch.unicode == 0xA0 || ch.unicode == '\t') continue;
            if (ch.x < tl - 5 || ch.x > tr + 5) continue;
            cchars.push_back({ch.x, ch.y});
        }

        std::vector<double> cy_vals;
        for (auto& cp : cchars) cy_vals.push_back(cp.y);
        // Row-height-relative tolerances break down when the grid has one
        // huge interval (sparsely ruled tables): clamp to text-line scale.
        double line_tol = std::min(row_h * 0.4, 8.0);
        auto text_row_ys = cluster_values(cy_vals, line_tol);

        std::vector<std::vector<double>> col_char_ys(n_cols_found);
        for (auto& cp : cchars) {
            for (int c = 0; c < n_cols_found; c++) {
                if (cp.x >= col_xs[c] && cp.x <= col_xs[c + 1]) {
                    col_char_ys[c].push_back(cp.y);
                    break;
                }
            }
        }

        std::vector<double> table_row_centers;
        double tol = line_tol;
        for (double ry : text_row_ys) {
            int cols_hit = 0;
            for (int c = 0; c < n_cols_found; c++) {
                for (double cy : col_char_ys[c]) {
                    if (std::abs(cy - ry) < tol) { cols_hit++; break; }
                }
            }
            if (cols_hit >= 2)
                table_row_centers.push_back(ry);
        }

        double tb = row_ys.front(), tt = row_ys.back();
        // Only centers inside the grid count: a page heading sharing the
        // table's x-band would otherwise inject a phantom row boundary and
        // break the monotonic row order. A fully drawn outer rule closes
        // its edge — a caption or unit note hugging the border stays out —
        // while an open edge keeps the half-row margin for rule-less rows.
        double top_tol = (!level_coverage.empty() &&
                          level_coverage.back() >= 0.7) ? 2.0 : row_h * 0.5;
        double bot_tol = (!level_coverage.empty() &&
                          level_coverage.front() >= 0.7) ? 2.0 : row_h * 0.5;
        std::vector<double> grid_centers;
        for (double tc : table_row_centers)
            if (tc >= tb - bot_tol && tc <= tt + top_tol)
                grid_centers.push_back(tc);
        int rows_in_grid = (int)grid_centers.size();

        int n_rows_expected = (int)row_ys.size() - 1;
        // Text rows replace the grid rows in two cases: slightly fewer text
        // rows than intervals (empty rows in the grid), or far more (a
        // sparsely ruled table whose data rows have no rules at all — the
        // grid would jam whole row groups into single cells).
        bool under_segmented = rows_in_grid >= n_rows_expected * 2.5 &&
                               rows_in_grid >= n_rows_expected + 3;
        bool use_text_rows = ((rows_in_grid < n_rows_expected * 0.9) &&
                              (rows_in_grid >= n_rows_expected * 0.8)) ||
                             under_segmented;
        merge_wrap_rows = under_segmented;

        if (use_text_rows && !grid_centers.empty()) {
            rows_are_drawn = false;
            std::sort(grid_centers.begin(), grid_centers.end());
            double half = std::min(row_h / 2.0, 10.0);
            // Clamp to h-line grid boundaries — don't extend beyond the table
            double grid_top = row_ys.back() + half;
            double grid_bot = row_ys.front() - half;
            actual_ys.push_back(std::max(grid_centers.front() - half, grid_bot));
            for (size_t i = 0; i < grid_centers.size() - 1; i++)
                actual_ys.push_back((grid_centers[i] + grid_centers[i + 1]) / 2.0);
            actual_ys.push_back(std::min(grid_centers.back() + half, grid_top));
        } else {
            actual_ys = row_ys;
        }
    }

    int n_rows = (int)actual_ys.size() - 1;
    int n_cols = (int)col_xs.size() - 1;

    table.x0 = col_xs.front();
    table.y0 = actual_ys.front();
    table.x1 = col_xs.back();
    table.y1 = actual_ys.back();

    int last_col_idx = n_cols - 1;
    auto sub_boundaries = detect_response_boundaries(cache,
        col_xs[last_col_idx], col_xs[last_col_idx + 1], actual_ys);
    int n_sub = sub_boundaries.empty() ? 0 : (int)sub_boundaries.size() - 1;
    int total_cols = (n_sub > 1) ? (n_cols - 1 + n_sub) : n_cols;

    // Detect merged cells: check v-line presence at each internal boundary per row
    // has_vline[r][b] = true if a v-line exists near col_xs[b] spanning row r
    std::vector<std::vector<bool>> has_vline(n_rows, std::vector<bool>(n_cols + 1, true));
    for (int r = 0; r < n_rows; r++) {
        double row_bot = std::min(actual_ys[r], actual_ys[r + 1]);
        double row_top = std::max(actual_ys[r], actual_ys[r + 1]);
        double row_h = row_top - row_bot;
        for (int b = 1; b < n_cols; b++) {
            double cx = col_xs[b];
            bool found = false;
            for (auto& vl : v_lines) {
                double vx = (vl.x0 + vl.x1) / 2.0;
                if (std::abs(vx - cx) > 6.0) continue;
                double vy_lo = std::min((double)vl.y0, (double)vl.y1);
                double vy_hi = std::max((double)vl.y0, (double)vl.y1);
                double overlap = std::min(vy_hi, row_top) - std::max(vy_lo, row_bot);
                if (overlap >= row_h * 0.3) {
                    found = true;
                    break;
                }
            }
            has_vline[r][b] = found;
        }
    }
    // Boundaries taken from text alignment have no rule to find: they
    // separate columns by definition, in every row.
    for (double tx : text_boundaries)
        for (int b = 1; b < n_cols; b++)
            if (std::abs(col_xs[b] - tx) < 1.0)
                for (int r = 0; r < n_rows; r++)
                    has_vline[r][b] = true;

    // A dense closed grid is stronger table evidence than small glyph/rule
    // overlaps.  Some producers place long labels almost flush with borders,
    // which makes the crossing heuristic below reject otherwise complete
    // rectangular tables.  Require both horizontal and vertical coverage so
    // diagram boxes do not receive the exemption.
    bool dense_closed_grid = n_rows >= 3 && n_cols >= 2;
    if (dense_closed_grid) {
        int ruled_levels = 0;
        for (double c : level_coverage)
            if (c >= 0.8) ruled_levels++;
        int min_ruled_levels = std::max(3, static_cast<int>(
            std::ceil(row_ys.size() * 0.8)));
        dense_closed_grid = ruled_levels >= min_ruled_levels;
        for (int b = 1; dense_closed_grid && b < n_cols; b++) {
            int covered_rows = 0;
            for (int r = 0; r < n_rows; r++)
                if (has_vline[r][b]) covered_rows++;
            if (covered_rows * 5 < n_rows * 4)
                dense_closed_grid = false;
        }
    }

    // Check v-line grid density: a real table has v-lines in most row/boundary positions.
    // Stray v-lines from body text have sparse coverage.
    // Only skip text continuation rejection for dense grids (real tables with merged cells).
    int vline_present = 0, vline_total = n_rows * (n_cols - 1);
    bool has_merged_cells = false;
    for (int r = 0; r < n_rows; r++)
        for (int b = 1; b < n_cols; b++)
            if (has_vline[r][b]) vline_present++;
    // Dense grid: >= 40% of positions have v-lines AND some are missing (merged cells)
    if (vline_total > 0 && vline_present < vline_total && vline_present >= vline_total * 0.4)
        has_merged_cells = true;

    // Merged-cell spans are keyed on missing v-lines, but a grid whose
    // columns came from text alignment has no v-lines anywhere — every span
    // would swallow the whole row. Such grids keep one cell per column.
    const bool spans_from_vlines = internal_vline_count > 0;

    table.rows.resize(n_rows);
    table.cell_bold.assign(n_rows, {});
    for (int r = 0; r < n_rows; r++) {
        table.rows[r].resize(total_cols);
        table.cell_bold[r].assign(total_cols, 0);
        int c = 0;
        while (c < n_cols) {
            // Determine span: extend while no v-line at next boundary
            int span = 1;
            while (spans_from_vlines && c + span < n_cols &&
                   !has_vline[r][c + span])
                span++;

            double left   = col_xs[c];
            double right  = col_xs[c + span];
            double bottom = actual_ys[r];
            double top    = actual_ys[r + 1];

            // A cell set entirely in a bold face keeps the emphasis (mask
            // only; the text stays bare for the shape heuristics below).
            auto fill_cell = [&](int col, double l, double t, double rt, double b) {
                bool bold = false;
                table.rows[r][col] = cache.get_text_in_rect(l, t, rt, b, &bold);
                table.cell_bold[r][col] = bold && !table.rows[r][col].empty();
            };
            if (c + span - 1 == last_col_idx && n_sub > 1 && span == 1) {
                if (is_scale_row(cache, left, right, bottom, top, sub_boundaries)) {
                    for (int sc = 0; sc < n_sub; sc++) {
                        fill_cell(n_cols - 1 + sc,
                            sub_boundaries[sc], top, sub_boundaries[sc+1], bottom);
                    }
                } else {
                    fill_cell(c, left, top, right, bottom);
                }
            } else {
                fill_cell(c, left, top, right, bottom);
            }
            // A cell the author ruled off: its row is a drawn interval
            // (rules across the grid above and below) and a column rule
            // runs beside it.
            bool ruled_side = (c > 0 && has_vline[r][c]) ||
                              (c + span < n_cols && has_vline[r][c + span]);
            bool drawn_row = rows_are_drawn &&
                             level_coverage[r] >= 0.8 &&
                             level_coverage[r + 1] >= 0.8;
            if (!(ruled_side && drawn_row))
                table.open_cell_max = std::max(table.open_cell_max,
                                               table.rows[r][c].size());
            c += span;
        }
    }

    std::reverse(table.rows.begin(), table.rows.end());
    std::reverse(table.cell_bold.begin(), table.cell_bold.end());

    // Text-row splitting of an under-segmented grid separates the wrap
    // lines of multi-line cells into their own rows; a row with a single
    // filled cell under a filled cell is such a fragment.
    if (merge_wrap_rows) {
        for (size_t r = 1; r < table.rows.size(); ) {
            auto& row = table.rows[r];
            int filled = 0, fc = -1;
            for (int c = 0; c < (int)row.size(); c++)
                if (!row[c].empty()) { filled++; fc = c; }
            auto& prev = table.rows[r - 1];
            if (filled == 1 && fc < (int)prev.size() && !prev[fc].empty()) {
                bool digit_wrap = prev[fc].size() >= 2 &&
                                  prev[fc].back() == '-' &&
                                  prev[fc][prev[fc].size() - 2] >= '0' &&
                                  prev[fc][prev[fc].size() - 2] <= '9' &&
                                  row[fc][0] >= '0' && row[fc][0] <= '9';
                if (!digit_wrap) prev[fc] += " ";
                prev[fc] += row[fc];
                table.rows.erase(table.rows.begin() + r);
                mask_erase_row(table, r);
            } else {
                r++;
            }
        }
    }

    // Text-split rows are no drawn cells; every cell counts (and wrap
    // merging above may have grown them).
    if (!rows_are_drawn)
        for (auto& row : table.rows)
            for (auto& cell : row)
                table.open_cell_max = std::max(table.open_cell_max, cell.size());

    trim_table(table);

    // Extract title rows: rows at top where only one cell has content,
    // the table has 3+ columns, and the text is long (form titles inside table borders).
    // Skip for 2-column tables where single-fill rows are normal (key-value pairs).
    if (n_cols >= 3) {
        while (table.rows.size() >= 3) {
            auto& row = table.rows.front();
            int filled = 0;
            for (int c = 0; c < (int)row.size(); c++)
                if (!row[c].empty()) filled++;
            if (filled != 1 || row[0].empty()) break;
            // Must be long text (>30 bytes) to look like a title, not a short label
            if (row[0].size() <= 30) break;
            if (!table.title.empty()) table.title += " ";
            table.title += row[0];
            table.rows.erase(table.rows.begin());
            mask_erase_row(table, 0);
        }
    }

    int meaningful_rows = 0;
    for (auto& row : table.rows) {
        int filled_cols = 0;
        for (auto& cell : row) if (!cell.empty()) filled_cols++;
        if (filled_cols >= 2) meaningful_rows++;
    }
    // A fully boxed small grid (header plus one data row, common for compact
    // result tables, or a two-column key/value box) is real even with only
    // two content rows. The evidence is the drawn structure: rules at every
    // level and column rules beside most cells, and then either several
    // column rules (a ruled grid of three or more columns) or, for a single
    // column rule, that rule running through every row with every content
    // row filling both its cells. A pair of stacked diagram boxes, or a
    // label box beside a paragraph, has no such consistent occupancy. The
    // single-rule case needs drawn strokes: a two-band patch of shading is
    // as often a highlighted column inside a larger table.
    bool small_closed_grid = false;
    if ((int)row_ys.size() >= 3 && !table.rows.empty()) {
        bool all_ruled = !level_coverage.empty();
        for (double c : level_coverage)
            if (c < 0.7) all_ruled = false;
        bool ruled_cells = all_ruled && vline_total > 0 &&
                           vline_present * 3 >= vline_total * 2;
        bool evidence = n_cols >= 3;
        if (ruled_cells && !evidence && drawn_rules &&
            vline_present == vline_total) {
            evidence = true;
            for (auto& row : table.rows) {
                int f = 0;
                for (auto& cell : row) if (!cell.empty()) f++;
                if (f > 0 && f < (int)row.size()) { evidence = false; break; }
            }
        }
        small_closed_grid = ruled_cells && evidence;
    }
    if (meaningful_rows < (small_closed_grid ? 2 : 3)) {
        table.rows.clear();
        table.too_sparse = true;
    }

    // Text running THROUGH a drawn v-line marks a drawing, not a table: a
    // table author never lays text across a rule, while diagram boxes (ERD
    // entities, flowchart nodes) cut through the labels around them. Count,
    // where a v-line exists at a row, cases whose flanking glyphs almost
    // touch across the boundary; widespread crossing rejects the grid.
    if (!table.rows.empty()) {
        int crossed = 0, near_n = 0;
        for (int r = 0; r < n_rows; r++) {
            double row_bot = std::min(actual_ys[r], actual_ys[r + 1]);
            double row_top = std::max(actual_ys[r], actual_ys[r + 1]);
            for (int b = 1; b < n_cols; b++) {
                if (!has_vline[r][b]) continue;
                double cx = col_xs[b];
                const PageCharCache::CharInfo* prev = nullptr;
                const PageCharCache::CharInfo* next = nullptr;
                for (auto& ch : cache.chars) {
                    if (ch.unicode == ' ' || ch.unicode == 0xA0 ||
                        ch.unicode == '\t') continue;
                    if (ch.y < row_bot + 1 || ch.y > row_top - 1) continue;
                    if ((ch.left + ch.right) / 2.0 < cx) {
                        if (!prev || ch.right > prev->right) prev = &ch;
                    } else {
                        if (!next || ch.left < next->left) next = &ch;
                    }
                }
                if (!prev || !next) continue;
                double fs = std::max(prev->font_size, next->font_size);
                if (fs < 4) fs = 10;
                if (cx - prev->right > fs * 3.0 || next->left - cx > fs * 3.0)
                    continue;
                near_n++;
                if (next->left - prev->right < std::max(fs * 0.2, 1.2))
                    crossed++;
            }
        }
        if (!dense_closed_grid && crossed >= 2 && crossed * 10 >= near_n * 3) {
            table.rows.clear();
            return table;
        }
    }

    if (!table.rows.empty()) {
        int n_cols_t = (int)table.rows[0].size();
        int total_cells = 0;
        int empty_cells = 0;
        for (auto& row : table.rows) {
            for (int c = 0; c < n_cols_t && c < (int)row.size(); c++) {
                total_cells++;
                if (row[c].empty()) empty_cells++;
            }
        }
        if (total_cells > 0 && empty_cells > total_cells * 0.75) {
            table.rows.clear();
            return table;
        }
    }

    // Reject list-like tables
    if (!table.rows.empty() && (int)table.rows[0].size() >= 2) {
        int n_cols_t = (int)table.rows[0].size();
        int rows_with_list_marker = 0;
        int valid_rows_t = 0;
        for (auto& row : table.rows) {
            bool has_content = false;
            for (auto& c : row) if (!c.empty()) { has_content = true; break; }
            if (!has_content) continue;
            valid_rows_t++;
            std::string first_cell;
            for (auto& c : row) if (!c.empty()) { first_cell = c; break; }
            if (first_cell.empty()) continue;
            bool is_marker = false;
            if (first_cell[0] >= '0' && first_cell[0] <= '9') {
                for (size_t k = 1; k < first_cell.size() && k < 4; k++) {
                    if (first_cell[k] == ')') { is_marker = true; break; }
                    if (first_cell[k] < '0' || first_cell[k] > '9') break;
                }
            }
            if (is_marker) rows_with_list_marker++;
        }
        if (valid_rows_t >= 2 && rows_with_list_marker >= valid_rows_t * 0.6) {
            table.rows.clear();
            return table;
        }
    }

    // Reject tables where text continues across column boundaries
    // (body text split by vertical lines — not real tabular data)
    // SKIP when merged cells detected: v-line grid confirms real table
    // structure. Grids without any v-line cannot have split body text this
    // way — their columns came from whitespace alignment instead.
    if (!table.rows.empty() && !has_merged_cells && !dense_closed_grid &&
        spans_from_vlines) {
        int n_cols_t = (int)table.rows[0].size();
        // Detect word continuation: Latin alphanumeric at both boundaries.
        // CJK characters are self-contained units (not word fragments),
        // so only check Latin for cross-boundary continuation.
        // Real CJK tables have independent data in each cell.
        // Body text split in CJK is caught by the empty cell ratio and
        // the long_first checks instead.
        auto is_content = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };

        if (n_cols_t <= 3) {
            int cont_rows = 0, checked = 0;
            for (auto& row : table.rows) {
                if ((int)row.size() < 2 || row[0].empty() || row[1].empty()) continue;
                checked++;
                if (is_content((unsigned char)row[0].back()) && is_content((unsigned char)row[1][0]))
                    cont_rows++;
            }
            // In a two-column grid of words, text runs on across the rule only where
            // the glyphs either side of it sit a word space apart or closer.
            // Cells keep their padding between them, however their words
            // begin and end ("heroin | anxiety, euphoria"). A grid of three
            // columns is left to this test alone: its rules are often the
            // coarse outline of a table whose columns the text alignment
            // detector finds in full.
            // Only for a grid of words on both sides (a term beside its
            // description): a chart frame's two cells hold axis figures.
            int worded = 0;
            for (auto& row : table.rows) {
                if ((int)row.size() < 2) continue;
                int letters[2] = {0, 0};
                for (int c = 0; c < 2; c++)
                    for (unsigned char ch : row[c])
                        if ((ch | 0x20) - 'a' < 26u || ch >= 0x80) letters[c]++;
                if (letters[0] >= 2 && letters[1] >= 2) worded++;
            }
            const bool word_grid = n_cols_t == 2 && worded * 2 >= checked;
            int tight_rows = word_grid ? 0 : checked;
            for (int r = 0; word_grid && r < n_rows; r++) {
                double row_bot = std::min(actual_ys[r], actual_ys[r + 1]);
                double row_top = std::max(actual_ys[r], actual_ys[r + 1]);
                bool tight = false;
                for (int b = 1; b < n_cols && !tight; b++) {
                    double cx = col_xs[b];
                    const PageCharCache::CharInfo* prev = nullptr;
                    const PageCharCache::CharInfo* next = nullptr;
                    for (auto& ch : cache.chars) {
                        if (ch.unicode == ' ' || ch.unicode == 0xA0 ||
                            ch.unicode == '\t') continue;
                        if (ch.y < row_bot + 1 || ch.y > row_top - 1) continue;
                        if ((ch.left + ch.right) / 2.0 < cx) {
                            if (!prev || ch.right > prev->right) prev = &ch;
                        } else {
                            if (!next || ch.left < next->left) next = &ch;
                        }
                    }
                    if (!prev || !next) continue;
                    double fs = std::max(prev->font_size, next->font_size);
                    if (fs < 4) fs = 10;
                    tight = next->left - prev->right <= fs * 0.6;
                }
                tight_rows += tight;
            }
            if (checked >= 3 && cont_rows >= checked * 0.15 &&
                tight_rows * 20 >= checked * 3)
                table.rows.clear();
            if (!table.rows.empty() && n_cols_t == 2) {
                int long_first = 0;
                for (auto& row : table.rows) {
                    if (row.size() >= 2 && row[0].size() > 100) long_first++;
                }
                if (long_first >= 2) table.rows.clear();
            }
        } else if (n_cols_t >= 4) {
            // 4+ columns: Latin-only continuation check + empty column check
            int cont_rows = 0, checked = 0;
            for (auto& row : table.rows) {
                if ((int)row.size() < n_cols_t) continue;
                int pairs_ok = 0, pairs_cont = 0;
                for (int c = 0; c + 1 < n_cols_t; c++) {
                    if (row[c].empty() || row[c+1].empty()) continue;
                    pairs_ok++;
                    unsigned char lc = (unsigned char)row[c].back();
                    unsigned char rc = (unsigned char)row[c+1][0];
                    // Latin-only: CJK chars are independent units, not word fragments
                    bool latin_cont = is_content(lc) && is_content(rc) && lc < 0x80 && rc < 0x80;
                    if (latin_cont) pairs_cont++;
                }
                checked++;
                // Require ALL non-empty pairs to show Latin continuation.
                // Data tables often have alphanumeric chars at cell boundaries
                // (e.g. phone → email) but not ALL pairs will continue.
                if (pairs_ok >= 2 && pairs_cont == pairs_ok) cont_rows++;
            }
            if (checked >= 3 && cont_rows >= checked * 0.5)
                table.rows.clear();

            // Reject if second column is mostly empty (body text + stray v-lines)
            if (!table.rows.empty() && (int)table.rows.size() >= 15) {
                int col1_trivial = 0;
                for (auto& row : table.rows)
                    if ((int)row.size() >= 2 && row[1].size() <= 2) col1_trivial++;
                if (col1_trivial >= (int)table.rows.size() * 0.7)
                    table.rows.clear();
            }
        }
    }

    // Reject tables where most content concentrates in one column
    // while others are mostly empty — body text split by stray vertical lines.
    if (!table.rows.empty() && !dense_closed_grid &&
        (int)table.rows.size() >= 3) {
        int n_cols_t = (int)table.rows[0].size();
        int nr = (int)table.rows.size();
        // Find the column with most content
        int best_col = 0;
        size_t best_len = 0;
        for (int c = 0; c < n_cols_t; c++) {
            size_t total_len = 0;
            for (auto& row : table.rows)
                if (c < (int)row.size()) total_len += row[c].size();
            if (total_len > best_len) { best_len = total_len; best_col = c; }
        }
        // Check if all other columns are mostly empty
        int empty_other_cols = 0;
        for (int c = 0; c < n_cols_t; c++) {
            if (c == best_col) continue;
            int empty = 0;
            for (auto& row : table.rows)
                if (c < (int)row.size() && row[c].empty()) empty++;
            if (empty >= nr / 2) empty_other_cols++;
        }
        // If ALL other columns are mostly empty and the main column has long text
        int long_rows = 0;
        for (auto& row : table.rows)
            if (best_col < (int)row.size() && row[best_col].size() > 30) long_rows++;
        // When all non-primary columns are mostly empty AND their total
        // content is < 5% of the primary column, it's body text split
        // by stray v-lines, not real tabular data.
        if (empty_other_cols == n_cols_t - 1 && best_len > 0) {
            size_t other_len = 0;
            for (int c = 0; c < n_cols_t; c++) {
                if (c == best_col) continue;
                for (auto& row : table.rows)
                    if (c < (int)row.size()) other_len += row[c].size();
            }
            if (other_len * 10 < best_len)
                table.rows.clear();
        }
    }

    return table;
}

double h_line_coverage_at_y(const std::vector<PdfLineSegment>& h_lines,
                             double y, double tol) {
    std::vector<std::pair<double,double>> intervals;
    for (auto& hl : h_lines) {
        double hy = (hl.y0 + hl.y1) / 2.0;
        if (std::abs(hy - y) > tol) continue;
        double lx = std::min((double)hl.x0, (double)hl.x1);
        double rx = std::max((double)hl.x0, (double)hl.x1);
        intervals.push_back({lx, rx});
    }
    if (intervals.empty()) return 0;
    std::sort(intervals.begin(), intervals.end());
    double total = 0, cur_l = intervals[0].first, cur_r = intervals[0].second;
    for (size_t i = 1; i < intervals.size(); i++) {
        if (intervals[i].first <= cur_r + 2.0)
            cur_r = std::max(cur_r, intervals[i].second);
        else { total += cur_r - cur_l; cur_l = intervals[i].first; cur_r = intervals[i].second; }
    }
    total += cur_r - cur_l;
    return total;
}

bool h_lines_share_full_span(const std::vector<PdfLineSegment>& h_lines,
                              double y1, double y2, double tol) {
    double min1 = 1e9, max1 = 0, min2 = 1e9, max2 = 0;
    for (auto& hl : h_lines) {
        double hy = (hl.y0 + hl.y1) / 2.0;
        double lx = std::min((double)hl.x0, (double)hl.x1);
        double rx = std::max((double)hl.x0, (double)hl.x1);
        if (std::abs(hy - y1) < tol) { if (lx < min1) min1 = lx; if (rx > max1) max1 = rx; }
        if (std::abs(hy - y2) < tol) { if (lx < min2) min2 = lx; if (rx > max2) max2 = rx; }
    }
    if (max1 == 0 || max2 == 0) return false;

    double extent1 = max1 - min1;
    double extent2 = max2 - min2;
    if (extent1 < 50 || extent2 < 50) return false;

    double cov1 = h_line_coverage_at_y(h_lines, y1, tol);
    double cov2 = h_line_coverage_at_y(h_lines, y2, tol);
    if (cov1 < extent1 * 0.4 || cov2 < extent2 * 0.4) return false;

    double overlap_l = std::max(min1, min2);
    double overlap_r = std::min(max1, max2);
    if (overlap_r <= overlap_l) return false;
    double overlap = overlap_r - overlap_l;
    double extent = std::max(extent1, extent2);
    double ratio = std::min(extent1, extent2) / extent;
    return overlap >= extent * 0.7 && ratio >= 0.5;
}

std::vector<double> infer_columns_from_text(const PageCharCache& cache,
                                             double left, double right,
                                             const std::vector<double>& row_ys) {
    int n_rows = (int)row_ys.size() - 1;
    if (n_rows < 1) return {};

    std::vector<std::vector<std::pair<double,double>>> per_row(n_rows);

    for (auto& ch : cache.chars) {
        if (ch.unicode == ' ' || ch.unicode == 0xA0 || ch.unicode == '\t') continue;
        double cx = ch.x;
        double cy = ch.y;
        if (cx < left - 5 || cx > right + 5) continue;
        for (int r = 0; r < n_rows; r++) {
            double bot = std::min(row_ys[r], row_ys[r+1]);
            double top = std::max(row_ys[r], row_ys[r+1]);
            if (cy >= bot + 1 && cy <= top - 1) {
                per_row[r].push_back({ch.left, ch.right});
                break;
            }
        }
    }
    for (int r = 0; r < n_rows; r++) {
        per_row[r] = merge_char_ranges(per_row[r]);
    }

    double width = right - left;
    int n_bins = std::max(20, static_cast<int>(width / 5.0));
    double bin_w = width / n_bins;
    std::vector<int> gap_counts(n_bins, 0);

    for (int r = 0; r < n_rows; r++) {
        if (per_row[r].empty()) continue;
        for (int b = 0; b < n_bins; b++) {
            double bx = left + b * bin_w + bin_w / 2.0;
            bool in_text = false;
            for (auto& sp : per_row[r]) {
                if (bx >= sp.first - 2 && bx <= sp.second + 2) {
                    in_text = true;
                    break;
                }
            }
            if (!in_text) gap_counts[b]++;
        }
    }

    int non_empty_rows = 0;
    for (int r = 0; r < n_rows; r++)
        if (!per_row[r].empty()) non_empty_rows++;
    int threshold = std::max(1, static_cast<int>(non_empty_rows * 0.4));

    std::vector<double> boundaries;
    boundaries.push_back(left);
    bool in_gap = false;
    int gap_b0 = 0;
    for (int b = 0; b < n_bins; b++) {
        if (gap_counts[b] >= threshold) {
            if (!in_gap) { gap_b0 = b; in_gap = true; }
        } else {
            if (in_gap) {
                // Inside a gap run, the boundary goes to the stretch blank in
                // the most rows: a right-aligned header wider than its numbers
                // reaches into the run, and the run's plain center would cut
                // the header's first letters off into the left column.
                int best = 0;
                for (int k = gap_b0; k < b; k++) best = std::max(best, gap_counts[k]);
                int s0 = -1, s1 = -1, k = gap_b0;
                while (k < b) {
                    if (gap_counts[k] != best) { k++; continue; }
                    int e = k;
                    while (e + 1 < b && gap_counts[e + 1] == best) e++;
                    if (s0 < 0 || e - k > s1 - s0) { s0 = k; s1 = e; }
                    k = e + 1;
                }
                // (same center as before when the whole run is equally blank)
                double gap_center = left + (s0 + s1 + 2) * bin_w / 2.0;
                if (gap_center > left + 15 && gap_center < right - 15)
                    boundaries.push_back(gap_center);
                in_gap = false;
            }
        }
    }
    boundaries.push_back(right);

    if (boundaries.size() < 3) return {};

    int n_cols_inferred = (int)boundaries.size() - 1;
    int rows_with_multi_cols = 0;
    for (int r = 0; r < n_rows; r++) {
        if (per_row[r].empty()) continue;
        int cols_hit = 0;
        for (int c = 0; c < n_cols_inferred; c++) {
            double col_l = boundaries[c];
            double col_r = boundaries[c + 1];
            for (auto& sp : per_row[r]) {
                double sp_mid = (sp.first + sp.second) / 2.0;
                if (sp_mid >= col_l && sp_mid <= col_r) { cols_hit++; break; }
            }
        }
        if (cols_hit >= 2) rows_with_multi_cols++;
    }
    if (rows_with_multi_cols < non_empty_rows * 0.5) return {};

    return boundaries;
}

// True when the codepoints (one text line, x-sorted, spaces removed) start
// like a table/figure caption: "Table 3", "TABLE 2.6", "Fig. 5", "표 4.2",
// "그림 3", optionally wrapped in <>, 〈〉 or []. A caption between two rule
// levels marks the boundary between stacked tables, never a data row.
static bool is_caption_start(const std::vector<uint32_t>& cps) {
    size_t i = 0;
    if (i < cps.size() && (cps[i] == '<' || cps[i] == '[' ||
                           cps[i] == 0x3008 || cps[i] == 0xFF1C))
        i++;
    auto lower = [](uint32_t c) -> uint32_t {
        return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    };
    auto match = [&](const char* w) {
        size_t j = i, k = 0;
        while (w[k] && j < cps.size() && lower(cps[j]) == (uint32_t)w[k]) {
            j++; k++;
        }
        if (w[k]) return false;
        i = j;
        return true;
    };
    bool head = match("tables") || match("table") ||
                match("figures") || match("figure") || match("fig");
    if (!head && i < cps.size() && cps[i] == 0xD45C) { i++; head = true; }
    if (!head && i + 1 < cps.size() &&
        cps[i] == 0xADF8 && cps[i + 1] == 0xB9BC) { i += 2; head = true; }
    if (!head) return false;
    // The caption number follows immediately, past at most light
    // punctuation ("Fig. 5", "표: 3"). Roman numerals cover IEEE style;
    // a letter here means an ordinary word ("Tablet") — not a caption.
    for (int steps = 0; i < cps.size() && steps < 4; i++, steps++) {
        uint32_t c = cps[i];
        if (c >= '0' && c <= '9') return true;
        if (c == 'I' || c == 'V' || c == 'X') return true;
        if (c != '.' && c != ':') return false;
    }
    return false;
}

// One y-level group can hold two side-by-side independent tables (the left
// and right columns of a two-column page): grouping keys on the bounding
// x-span per level, so a level holding "left segment + right segment" looks
// like one wide rule. When every rule of the group stays clear of a tall
// vertical band that no v-line or text crosses either, that band is a page
// gutter, not a wide cell: split the group's lines there and build each
// side as its own table.
struct TableLineSet {
    std::vector<double> levels;
    std::vector<PdfLineSegment> h_lines, v_lines;
};

static std::vector<TableLineSet> split_group_at_gutter(
        const std::vector<double>& group,
        const std::vector<PdfLineSegment>& h_lines,
        const std::vector<PdfLineSegment>& v_lines,
        const PageCharCache& cache) {
    std::vector<TableLineSet> whole;
    whole.push_back({group, h_lines, v_lines});

    std::vector<std::pair<double, double>> iv;
    for (auto& hl : h_lines) {
        double hy = (hl.y0 + hl.y1) / 2.0;
        for (auto& ry : group)
            if (std::abs(hy - ry) < 4.0) {
                iv.push_back({std::min((double)hl.x0, (double)hl.x1),
                              std::max((double)hl.x0, (double)hl.x1)});
                break;
            }
    }
    if (iv.size() < 2) return whole;
    std::sort(iv.begin(), iv.end());
    // Dashed rules and per-cell borders arrive as fragments: join across
    // small stroke gaps so only genuine voids remain.
    constexpr double kStrokeJoinTol = 12.0;
    std::vector<std::pair<double, double>> runs{iv[0]};
    for (size_t i = 1; i < iv.size(); i++) {
        if (iv[i].first <= runs.back().second + kStrokeJoinTol)
            runs.back().second = std::max(runs.back().second, iv[i].second);
        else
            runs.push_back(iv[i]);
    }
    if (runs.size() < 2) return whole;

    double y_lo = group.front() - 2.0, y_hi = group.back() + 2.0;
    constexpr double kMinGutterWidth = 18.0;
    std::vector<double> cuts;
    for (size_t g = 1; g < runs.size(); g++) {
        double gap_l = runs[g - 1].second, gap_r = runs[g].first;
        if (gap_r - gap_l < kMinGutterWidth) continue;
        bool blocked = false;
        for (auto& vl : v_lines) {
            double vx = (vl.x0 + vl.x1) / 2.0;
            if (vx <= gap_l + 2.0 || vx >= gap_r - 2.0) continue;
            double vy_lo = std::min((double)vl.y0, (double)vl.y1);
            double vy_hi = std::max((double)vl.y0, (double)vl.y1);
            if (std::min(vy_hi, y_hi) - std::max(vy_lo, y_lo) > 2.0) {
                blocked = true;
                break;
            }
        }
        for (auto& ch : cache.chars) {
            if (blocked) break;
            if (ch.unicode == ' ' || ch.unicode == 0xA0 ||
                ch.unicode == '\t')
                continue;
            if (ch.y <= y_lo || ch.y >= y_hi) continue;
            if (ch.x > gap_l + 1.0 && ch.x < gap_r - 1.0) blocked = true;
        }
        if (!blocked) cuts.push_back((gap_l + gap_r) / 2.0);
    }
    if (cuts.empty()) return whole;

    cuts.insert(cuts.begin(), -1e9);
    cuts.push_back(1e9);
    std::vector<TableLineSet> parts;
    for (size_t s = 1; s < cuts.size(); s++) {
        TableLineSet part;
        double w_lo = cuts[s - 1], w_hi = cuts[s];
        for (auto& hl : h_lines) {
            double mx = (hl.x0 + hl.x1) / 2.0;
            if (mx > w_lo && mx < w_hi) part.h_lines.push_back(hl);
        }
        for (auto& vl : v_lines) {
            double vx = (vl.x0 + vl.x1) / 2.0;
            if (vx > w_lo && vx < w_hi) part.v_lines.push_back(vl);
        }
        // Keep only the levels this side actually rules: the other side's
        // row boundaries would otherwise split rows that have no rule here.
        for (double ry : group) {
            bool present = false;
            for (auto& hl : part.h_lines) {
                double hy = (hl.y0 + hl.y1) / 2.0;
                if (std::abs(hy - ry) < 4.0) { present = true; break; }
            }
            for (auto& vl : part.v_lines) {
                if (present) break;
                double vy_lo = std::min((double)vl.y0, (double)vl.y1);
                double vy_hi = std::max((double)vl.y0, (double)vl.y1);
                if (vy_hi - vy_lo < 6.0) continue;
                if (std::abs(vy_lo - ry) < 3.5 || std::abs(vy_hi - ry) < 3.5)
                    present = true;
            }
            if (present) part.levels.push_back(ry);
        }
        if (part.levels.size() >= 3) parts.push_back(std::move(part));
    }
    // Even a single surviving side beats the welded whole; the dropped
    // side's text stays in the prose flow.
    if (parts.empty()) return whole;
    return parts;
}

// Rule grouping over one zone's lines: row levels from h-rules (and v-rule
// ends), groups of levels joined by v-rules or shared spans, then one table
// per group (or per side of a gutter inside a group). keep_part, when set,
// vetoes groups before they are built; kept_ranges receives the y-range of
// every table built.
static void detect_ruled_in_zone(
        const std::vector<PdfLineSegment>& h_lines,
        const std::vector<PdfLineSegment>& v_lines,
        const PageCharCache& cache,
        std::vector<SparseGrid>* sparse_grids,
        const std::function<bool(const TableLineSet&)>& keep_part,
        std::vector<TableData>& out,
        std::vector<std::pair<double, double>>* kept_ranges) {
    std::vector<double> h_ys;
    for (auto& hl : h_lines) h_ys.push_back((hl.y0 + hl.y1) / 2.0);
    auto row_ys = cluster_values(h_ys, 3.0);

    // Synthesize row levels from v-line endpoints: when v-lines at >=2
    // distinct x positions start or end at the same y, that y is a row
    // boundary even without a drawn h-rule (tables whose outer rows have no
    // h-lines, and v-line-only grids).
    {
        // Only endpoints of v-lines at least a row tall count: dashed table
        // borders arrive as thousands of sub-point stroke fragments whose
        // endpoints would flood the row grid.
        std::vector<double> ep_ys;
        for (auto& vl : v_lines) {
            double lo = std::min((double)vl.y0, (double)vl.y1);
            double hi = std::max((double)vl.y0, (double)vl.y1);
            if (hi - lo < 6.0) continue;
            ep_ys.push_back(lo);
            ep_ys.push_back(hi);
        }
        auto ep_levels = cluster_values(ep_ys, 3.0);
        for (double ey : ep_levels) {
            std::vector<double> xs;
            for (auto& vl : v_lines) {
                double lo = std::min((double)vl.y0, (double)vl.y1);
                double hi = std::max((double)vl.y0, (double)vl.y1);
                if (hi - lo < 6.0) continue;
                if (std::abs(lo - ey) < 3.5 || std::abs(hi - ey) < 3.5)
                    xs.push_back((vl.x0 + vl.x1) / 2.0);
            }
            if ((int)cluster_values(xs, 5.0).size() < 2) continue;
            bool exists = false;
            for (double ry : row_ys)
                if (std::abs(ry - ey) < 4.0) { exists = true; break; }
            if (!exists) row_ys.push_back(ey);
        }
        std::sort(row_ys.begin(), row_ys.end());
    }
    if (row_ys.size() < 3) return;

    int n_levels = (int)row_ys.size();

    // Text between two h-levels decides whether an x-span-only bridge is
    // plausible: a real table row's text hugs both rules with even line
    // spacing, while a separate block in the gap (another table, prose)
    // floats clear of a rule or leaves an internal blank band.
    auto bridge_gap_ok = [&](double y_lo, double y_hi,
                             bool allow_empty) -> bool {
        double gap = y_hi - y_lo;
        if (gap > 250.0) return false;
        double lo_l = 1e9, lo_r = 0, hi_l = 1e9, hi_r = 0;
        for (auto& hl : h_lines) {
            double hy = (hl.y0 + hl.y1) / 2.0;
            double lx = std::min((double)hl.x0, (double)hl.x1);
            double rx = std::max((double)hl.x0, (double)hl.x1);
            if (std::abs(hy - y_lo) < 4.0) { lo_l = std::min(lo_l, lx); lo_r = std::max(lo_r, rx); }
            if (std::abs(hy - y_hi) < 4.0) { hi_l = std::min(hi_l, lx); hi_r = std::max(hi_r, rx); }
        }
        double x_lo = std::max(lo_l, hi_l), x_hi = std::min(lo_r, hi_r);
        if (x_hi <= x_lo) { x_lo = std::min(lo_l, hi_l); x_hi = std::max(lo_r, hi_r); }
        if (x_hi <= x_lo) return allow_empty && gap < 60.0;
        struct GapChar { double x, y; uint32_t cp; };
        std::vector<GapChar> gap_chars;
        std::vector<double> ys;
        for (auto& ch : cache.chars) {
            if (ch.unicode == ' ' || ch.unicode == 0xA0 || ch.unicode == '\t') continue;
            if (ch.x < x_lo + 1 || ch.x > x_hi - 1) continue;
            if (ch.y <= y_lo + 2 || ch.y >= y_hi - 2) continue;
            gap_chars.push_back({ch.x, ch.y, ch.unicode});
            ys.push_back(ch.y);
        }
        if (ys.empty()) return allow_empty && gap < 60.0;
        auto centers = cluster_values(ys, 3.0);
        // A caption line anywhere in the gap separates stacked tables.
        for (double cy : centers) {
            std::vector<GapChar> line;
            for (auto& gc : gap_chars)
                if (std::abs(gc.y - cy) < 3.0) line.push_back(gc);
            std::sort(line.begin(), line.end(),
                      [](const GapChar& a, const GapChar& b) { return a.x < b.x; });
            std::vector<uint32_t> cps;
            for (auto& gc : line) cps.push_back(gc.cp);
            if (is_caption_start(cps)) return false;
        }
        double pitch = 14.0;
        if (centers.size() >= 2) {
            std::vector<double> diffs;
            for (size_t k = 1; k < centers.size(); k++)
                diffs.push_back(centers[k] - centers[k-1]);
            std::nth_element(diffs.begin(), diffs.begin() + diffs.size()/2,
                             diffs.end());
            pitch = diffs[diffs.size()/2];
        }
        double attach_tol = std::max(pitch * 1.4, 20.0);
        if (y_hi - centers.back() > attach_tol) return false;
        if (centers.front() - y_lo > attach_tol) return false;
        double blank_tol = std::max(pitch * 1.8, 26.0);
        for (size_t k = 1; k < centers.size(); k++)
            if (centers[k] - centers[k-1] > blank_tol) return false;
        return true;
    };

    std::vector<bool> connected(n_levels - 1, false);
    for (int i = 0; i < n_levels - 1; i++) {
        double y_lo = row_ys[i];
        double y_hi = row_ys[i + 1];
        double row_gap = y_hi - y_lo;
        for (auto& vl : v_lines) {
            double vy_lo = std::min((double)vl.y0, (double)vl.y1);
            double vy_hi = std::max((double)vl.y0, (double)vl.y1);
            // Full span: v-line covers the entire row gap
            if (vy_lo <= y_lo + 5.0 && vy_hi >= y_hi - 5.0) {
                connected[i] = true;
                break;
            }
            // Partial span: v-line overlaps >= 50% of row gap (cell-unit v-lines)
            double overlap = std::min(vy_hi, y_hi) - std::max(vy_lo, y_lo);
            if (overlap >= row_gap * 0.5) {
                connected[i] = true;
                break;
            }
        }
    }

    std::vector<bool> x_overlap(n_levels - 1, false);
    for (int i = 0; i < n_levels - 1; i++) {
        x_overlap[i] = h_lines_share_full_span(h_lines, row_ys[i], row_ys[i+1], 3.0);
    }

    // A closed frame that stops is a table's end. An interval whose both
    // neighbours are boxed in by v-rules at the rules' left AND right ends,
    // while no v-rule crosses the interval itself, lies between two framed
    // grids: a merged-cell row of one form keeps its outer border running
    // through it. Such an interval is never bridged on span or text alone.
    std::vector<bool> framed(n_levels - 1, false);
    for (int i = 0; i < n_levels - 1; i++) {
        double y_lo = row_ys[i], y_hi = row_ys[i + 1];
        double lo = 1e9, hi = -1e9;
        for (auto& hl : h_lines) {
            double hy = (hl.y0 + hl.y1) / 2.0;
            if (std::abs(hy - y_lo) >= 4.0 && std::abs(hy - y_hi) >= 4.0) continue;
            lo = std::min(lo, (double)std::min(hl.x0, hl.x1));
            hi = std::max(hi, (double)std::max(hl.x0, hl.x1));
        }
        if (hi <= lo) continue;
        bool at_lo = false, at_hi = false;
        for (auto& vl : v_lines) {
            double vx = (vl.x0 + vl.x1) / 2.0;
            double vy_lo = std::min((double)vl.y0, (double)vl.y1);
            double vy_hi = std::max((double)vl.y0, (double)vl.y1);
            double overlap = std::min(vy_hi, y_hi) - std::max(vy_lo, y_lo);
            if (overlap < (y_hi - y_lo) * 0.5) continue;
            if (std::abs(vx - lo) < 6.0) at_lo = true;
            if (std::abs(vx - hi) < 6.0) at_hi = true;
        }
        framed[i] = at_lo && at_hi;
    }
    auto frame_break = [&](int i) {
        return !connected[i] && i > 0 && i + 1 < n_levels - 1 &&
               framed[i - 1] && framed[i + 1];
    };

    std::vector<std::vector<double>> table_groups;
    std::vector<double> current_group;
    int group_vline_connections = 0;
    current_group.push_back(row_ys[0]);
    for (int i = 0; i < n_levels - 1; i++) {
        double gap = row_ys[i + 1] - row_ys[i];
        bool close_enough = gap < 200.0;
        // Include row if v-line connected, or h-lines share span AND the gap
        // holds nothing but that row's own text (merged-cell rows in forms).
        // Without the text check, stacked tables whose rules share the same
        // x-span get bridged across whole blocks of unrelated content.
        bool h_span_ok = x_overlap[i] && close_enough && !frame_break(i) &&
                         bridge_gap_ok(row_ys[i], row_ys[i + 1], true);
        if (connected[i] || h_span_ok) {
            current_group.push_back(row_ys[i + 1]);
            if (connected[i]) group_vline_connections++;
        } else if (close_enough && group_vline_connections > 0 &&
                   !frame_break(i)) {
            // No v-line and no x-overlap, but we're already in a connected group.
            // Check if the next row's h-lines share x-range with the group's h-lines.
            double g_left = 1e9, g_right = 0;
            for (auto& hl : h_lines) {
                double hy = (hl.y0 + hl.y1) / 2.0;
                for (auto& gy : current_group) {
                    if (std::abs(hy - gy) < 4.0) {
                        double lx = std::min((double)hl.x0, (double)hl.x1);
                        double rx = std::max((double)hl.x0, (double)hl.x1);
                        if (lx < g_left) g_left = lx;
                        if (rx > g_right) g_right = rx;
                        break;
                    }
                }
            }
            double n_left = 1e9, n_right = 0;
            for (auto& hl : h_lines) {
                double hy = (hl.y0 + hl.y1) / 2.0;
                if (std::abs(hy - row_ys[i + 1]) < 4.0) {
                    double lx = std::min((double)hl.x0, (double)hl.x1);
                    double rx = std::max((double)hl.x0, (double)hl.x1);
                    if (lx < n_left) n_left = lx;
                    if (rx > n_right) n_right = rx;
                }
            }
            double extent = std::max(g_right - g_left, n_right - n_left);
            double overlap = std::min(g_right, n_right) - std::max(g_left, n_left);
            if (extent > 50 && overlap >= extent * 0.7 &&
                bridge_gap_ok(row_ys[i], row_ys[i + 1], true)) {
                current_group.push_back(row_ys[i + 1]);
            } else {
                if (current_group.size() >= 3 &&
                    (group_vline_connections > 0 || current_group.size() >= 7))
                    table_groups.push_back(current_group);
                current_group.clear();
                group_vline_connections = 0;
                current_group.push_back(row_ys[i + 1]);
            }
        } else {
            if (current_group.size() >= 3 &&
                (group_vline_connections > 0 || current_group.size() >= 7))
                table_groups.push_back(current_group);
            current_group.clear();
            group_vline_connections = 0;
            current_group.push_back(row_ys[i + 1]);
        }
    }
    if (current_group.size() >= 3 &&
        (group_vline_connections > 0 || current_group.size() >= 7))
        table_groups.push_back(current_group);

    // Merge adjacent groups that share the same h-line x-range.
    // Handles forms where some row intervals lack v-lines (merged cells)
    // but the h-lines clearly continue the same table.
    if (table_groups.size() >= 2) {
        auto h_x_range = [&](const std::vector<double>& group) -> std::pair<double,double> {
            double lo = 1e9, hi = 0;
            for (auto& hl : h_lines) {
                double hy = (hl.y0 + hl.y1) / 2.0;
                for (auto& ry : group) {
                    if (std::abs(hy - ry) < 4.0) {
                        double lx = std::min((double)hl.x0, (double)hl.x1);
                        double rx = std::max((double)hl.x0, (double)hl.x1);
                        if (lx < lo) lo = lx;
                        if (rx > hi) hi = rx;
                        break;
                    }
                }
            }
            return {lo, hi};
        };

        std::vector<std::vector<double>> merged;
        merged.push_back(table_groups[0]);
        for (size_t g = 1; g < table_groups.size(); g++) {
            auto [lo1, hi1] = h_x_range(merged.back());
            auto [lo2, hi2] = h_x_range(table_groups[g]);
            double extent = std::max(hi1 - lo1, hi2 - lo2);
            double overlap = std::min(hi1, hi2) - std::max(lo1, lo2);
            double y_gap = table_groups[g].front() - merged.back().back();
            // Merging split form pieces requires a swallowed text row in the
            // gap; an empty gap between two groups is inter-table spacing.
            // The interval between the groups (when it is one interval)
            // must not be the gap between two closed frames.
            bool frames_apart = false;
            for (int i = 0; i + 1 < n_levels; i++)
                if (std::abs(row_ys[i] - merged.back().back()) < 0.01 &&
                    std::abs(row_ys[i + 1] - table_groups[g].front()) < 0.01)
                    frames_apart = frame_break(i);
            if (extent > 50 && overlap >= extent * 0.7 && y_gap < 100 &&
                !frames_apart &&
                bridge_gap_ok(merged.back().back(), table_groups[g].front(),
                              false)) {
                for (auto& y : table_groups[g])
                    merged.back().push_back(y);
            } else {
                merged.push_back(table_groups[g]);
            }
        }
        table_groups = std::move(merged);
    }

    if (table_groups.empty()) return;

    // Split groups where v-line column structure changes significantly
    std::vector<std::vector<double>> final_groups;
    for (auto& group : table_groups) {
        if (group.size() < 5) { final_groups.push_back(group); continue; }
        int n = (int)group.size() - 1;

        // Count internal v-lines for each row interval
        std::vector<int> row_vcount(n, 0);
        double gl = 1e9, gr = 0;
        for (auto& hl : h_lines) {
            double hy = (hl.y0 + hl.y1) / 2.0;
            for (auto& ry : group)
                if (std::abs(hy - ry) < 4.0) {
                    double lx = std::min((double)hl.x0, (double)hl.x1);
                    double rx = std::max((double)hl.x0, (double)hl.x1);
                    if (lx < gl) gl = lx;
                    if (rx > gr) gr = rx;
                    break;
                }
        }
        for (int i = 0; i < n; i++) {
            double y_lo = group[i], y_hi = group[i + 1];
            double row_h = y_hi - y_lo;
            for (auto& vl : v_lines) {
                double vx = (vl.x0 + vl.x1) / 2.0;
                if (vx <= gl + 10 || vx >= gr - 10) continue;
                double vy_lo = std::min((double)vl.y0, (double)vl.y1);
                double vy_hi = std::max((double)vl.y0, (double)vl.y1);
                double overlap = std::min(vy_hi, y_hi) - std::max(vy_lo, y_lo);
                if (overlap >= row_h * 0.3) row_vcount[i]++;
            }
        }

        // Find split points: a row with 0-1 internal v-lines is a split
        // only if BOTH neighbors have >= 3 v-lines (true section boundary).
        // A single transition (many → few) is just a merged-cell area.
        std::vector<int> splits;
        for (int i = 1; i < n - 1; i++) {
            if (row_vcount[i] <= 1 &&
                row_vcount[i - 1] >= 3 && row_vcount[i + 1] >= 3)
                splits.push_back(i);
        }

        if (splits.empty()) { final_groups.push_back(group); continue; }

        int start = 0;
        for (int sp : splits) {
            std::vector<double> sub(group.begin() + start, group.begin() + sp + 1);
            if (sub.size() >= 3) final_groups.push_back(sub);
            start = sp;
        }
        std::vector<double> last(group.begin() + start, group.end());
        if (last.size() >= 3) final_groups.push_back(last);
    }

    for (auto& group : final_groups) {
        for (auto& part : split_group_at_gutter(group, h_lines, v_lines,
                                                cache)) {
            if (keep_part && !keep_part(part)) continue;
            TableData t = build_table(part.levels, part.h_lines,
                                      part.v_lines, cache);
            if (std::getenv("JDOC_TABLE_DEBUG")) {
                fprintf(stderr, "[rule-group]");
                for (double y : part.levels) fprintf(stderr, " %.1f", y);
                fprintf(stderr, " -> rows %zu%s\n", t.rows.size(),
                        t.too_sparse ? " (sparse)" : "");
            }
            if (t.rows.empty()) {
                // A grid of full-width row rules with a vertical rule inside
                // it is laid out in columns even when too few of its rows
                // fill two cells (a label column beside wrapped text):
                // report it so the markdown path can keep its layout
                // instead of gluing it.
                if (sparse_grids && t.too_sparse) {
                    double gx0 = 1e9, gx1 = -1e9;
                    double gy0 = part.levels.front(), gy1 = part.levels.back();
                    for (auto& hl : part.h_lines) {
                        double hy = (hl.y0 + hl.y1) / 2.0;
                        if (hy < gy0 - 4.0 || hy > gy1 + 4.0) continue;
                        gx0 = std::min(gx0, (double)std::min(hl.x0, hl.x1));
                        gx1 = std::max(gx1, (double)std::max(hl.x0, hl.x1));
                    }
                    // Every level a drawn rule across the grid: levels
                    // synthesized from v-line ends (chart axes) or rules
                    // over part of the width (a title box inside a page
                    // frame) are not a table's row rules.
                    bool full_rules = gx1 > gx0;
                    for (double lv : part.levels) {
                        double lo = 1e9, hi = -1e9;
                        for (auto& hl : part.h_lines) {
                            if (std::abs((hl.y0 + hl.y1) / 2.0 - lv) >= 4.0) continue;
                            lo = std::min(lo, (double)std::min(hl.x0, hl.x1));
                            hi = std::max(hi, (double)std::max(hl.x0, hl.x1));
                        }
                        if (hi - lo < 0.8 * (gx1 - gx0)) full_rules = false;
                    }
                    SparseGrid sg;
                    sg.box = {gx0, gy0, gx1, gy1};
                    sg.levels = part.levels;
                    for (auto& vl : part.v_lines) {
                        double vx = (vl.x0 + vl.x1) / 2.0;
                        double vlo = std::min(vl.y0, vl.y1);
                        double vhi = std::max(vl.y0, vl.y1);
                        if (vx > gx0 + 5.0 && vx < gx1 - 5.0 &&
                            std::min(vhi, gy1) - std::max(vlo, gy0) >= 10.0)
                            sg.rules.push_back({vx, vlo, vhi});
                    }
                    if (full_rules && !sg.rules.empty())
                        sparse_grids->push_back(std::move(sg));
                }
                continue;
            }
            // Reject grids that swallowed page prose (stacked separate
            // tables bridged across body text): no real cell holds a whole
            // paragraph. Rejecting lets the band's lines flow back as text.
            // Bridging only joins levels no column rule connects, so a long
            // cell set off by a drawn column rule is the author's own cell.
            if (t.open_cell_max > 300) continue;
            if (kept_ranges)
                kept_ranges->push_back({part.levels.front(), part.levels.back()});
            out.push_back(std::move(t));
        }
    }

}

// Rules of side-by-side structures (the tables of two page columns, a table
// beside a framed figure) must be grouped apart: grouping keys on page-wide
// y-levels, so rules of different columns at nearly the same height weld
// into one level whose x-extent spans both columns, and a frame's vertical
// rule in one column "connects" levels of a table in the other. A rule
// gutter is a vertical strip at least kMinGutter wide that every h-rule
// either avoids or crosses entirely, with two or more rules on each side
// whose heights overlap (the sides stand next to each other). Inside the
// strip only rules of the crossing (page-wide) structures may appear, and
// no text may sit in its middle where both sides stand.
struct RuleGutter { double a = 0, b = 0; };

static bool find_rule_gutter(const std::vector<PdfLineSegment>& h_lines,
                             const std::vector<PdfLineSegment>& v_lines,
                             const PageCharCache& cache, RuleGutter& out) {
    constexpr double kMinGutter = 18.0;
    constexpr double kEdgeTol = 0.5;
    // Vector-heavy pages (charts, drawings) carry thousands of rules; the
    // candidate scan is quadratic, and such pages are not table layouts.
    if (h_lines.size() < 4 || h_lines.size() > 1500) return false;
    struct Iv { double lo, hi, y; };
    std::vector<Iv> iv;
    iv.reserve(h_lines.size());
    for (auto& hl : h_lines)
        iv.push_back({std::min((double)hl.x0, (double)hl.x1),
                      std::max((double)hl.x0, (double)hl.x1),
                      (hl.y0 + hl.y1) / 2.0});
    std::vector<double> rights, lefts;
    for (auto& i : iv) { rights.push_back(i.hi); lefts.push_back(i.lo); }
    rights = cluster_values(rights, kEdgeTol);
    std::sort(lefts.begin(), lefts.end());

    bool found = false;
    int best_min = 0;
    double best_w = 0;
    for (double a : rights) {
        auto it = std::lower_bound(lefts.begin(), lefts.end(), a + kMinGutter);
        if (it == lefts.end()) continue;
        double b = *it;
        int n_left = 0, n_right = 0;
        double l_lo = 1e18, l_hi = -1e18, r_lo = 1e18, r_hi = -1e18;
        std::vector<const Iv*> crossing;
        bool clean = true;
        for (auto& i : iv) {
            if (i.hi <= a + kEdgeTol) {
                n_left++; l_lo = std::min(l_lo, i.y); l_hi = std::max(l_hi, i.y);
            } else if (i.lo >= b - kEdgeTol) {
                n_right++; r_lo = std::min(r_lo, i.y); r_hi = std::max(r_hi, i.y);
            } else if (i.lo <= a + kEdgeTol && i.hi >= b - kEdgeTol) {
                crossing.push_back(&i);
            } else {
                clean = false;   // a rule ends inside the strip
                break;
            }
        }
        if (!clean || n_left < 2 || n_right < 2) continue;
        double ov_lo = std::max(l_lo, r_lo), ov_hi = std::min(l_hi, r_hi);
        if (ov_hi <= ov_lo) continue;    // stacked, not side by side
        // A v-rule inside the strip belongs to a crossing structure (a
        // page-wide grid's column rule) or the strip is no gutter.
        for (auto& vl : v_lines) {
            if (!clean) break;
            double vx = (vl.x0 + vl.x1) / 2.0;
            if (vx <= a + kEdgeTol || vx >= b - kEdgeTol) continue;
            double vlo = std::min((double)vl.y0, (double)vl.y1);
            double vhi = std::max((double)vl.y0, (double)vl.y1);
            bool attached = false;
            for (auto* c : crossing)
                if (c->y >= vlo - 4.0 && c->y <= vhi + 4.0) { attached = true; break; }
            if (!attached) clean = false;
        }
        if (!clean) continue;
        // Text in the middle of the strip, at heights where both sides
        // stand, is content running across it (a wide cell, prose).
        double w = b - a;
        double m_lo = a + w * 0.25, m_hi = b - w * 0.25;
        for (auto& ch : cache.chars) {
            if (ch.unicode == ' ' || ch.unicode == 0xA0 || ch.unicode == '\t') continue;
            if (ch.y <= ov_lo || ch.y >= ov_hi) continue;
            if (ch.x > m_lo && ch.x < m_hi) { clean = false; break; }
        }
        if (!clean) continue;
        int m = std::min(n_left, n_right);
        if (!found || m > best_min || (m == best_min && w > best_w)) {
            found = true;
            best_min = m;
            best_w = w;
            out = {a, b};
        }
    }
    return found;
}

// Rules split at a rule gutter are grouped per side. Rules crossing the
// gutter (a page-wide table) are grouped together with everything, as
// before, and keep only groups standing on two or more crossing levels; the
// side groups inside such a group's height are part of it and dropped.
static void detect_ruled_zoned(const std::vector<PdfLineSegment>& h_lines,
                               const std::vector<PdfLineSegment>& v_lines,
                               const PageCharCache& cache,
                               std::vector<SparseGrid>* sparse_grids,
                               std::vector<TableData>& out, int depth) {
    RuleGutter g;
    if (depth >= 3 || !find_rule_gutter(h_lines, v_lines, cache, g)) {
        detect_ruled_in_zone(h_lines, v_lines, cache, sparse_grids, nullptr,
                             out, nullptr);
        return;
    }
    if (std::getenv("JDOC_TABLE_DEBUG"))
        fprintf(stderr, "[rule-gutter] depth %d x %.1f..%.1f\n", depth, g.a, g.b);
    constexpr double kEdgeTol = 0.5;
    std::vector<PdfLineSegment> hl, hr, hc, vl, vr;
    for (auto& l : h_lines) {
        double lo = std::min(l.x0, l.x1), hi = std::max(l.x0, l.x1);
        if (hi <= g.a + kEdgeTol) hl.push_back(l);
        else if (lo >= g.b - kEdgeTol) hr.push_back(l);
        else hc.push_back(l);
    }
    for (auto& l : v_lines) {
        double x = (l.x0 + l.x1) / 2.0;
        if (x <= g.a + kEdgeTol) vl.push_back(l);
        else if (x >= g.b - kEdgeTol) vr.push_back(l);
    }

    std::vector<std::pair<double, double>> wide_ranges;
    if (!hc.empty()) {
        auto crossing_levels = [&](const TableLineSet& part) {
            int n = 0;
            for (double ry : part.levels)
                for (auto& c : hc)
                    if (std::abs((c.y0 + c.y1) / 2.0 - ry) < 4.0) { n++; break; }
            return n >= 2;
        };
        detect_ruled_in_zone(h_lines, v_lines, cache, sparse_grids,
                             crossing_levels, out, &wide_ranges);
    }
    auto inside_wide = [&](double y0, double y1) {
        double lo = std::min(y0, y1), hi = std::max(y0, y1);
        for (auto& r : wide_ranges)
            if (std::min(hi, r.second) - std::max(lo, r.first) > 2.0) return true;
        return false;
    };
    for (int side = 0; side < 2; side++) {
        std::vector<TableData> part_out;
        std::vector<SparseGrid> part_sparse;
        detect_ruled_zoned(side == 0 ? hl : hr, side == 0 ? vl : vr, cache,
                           sparse_grids ? &part_sparse : nullptr, part_out,
                           depth + 1);
        for (auto& t : part_out)
            if (!inside_wide(t.y0, t.y1)) out.push_back(std::move(t));
        if (sparse_grids)
            for (auto& sg : part_sparse)
                if (!inside_wide(sg.box[1], sg.box[3]))
                    sparse_grids->push_back(std::move(sg));
    }
}

std::vector<TableData> detect_tables(const std::vector<PdfLineSegment>& lines,
                                      const PageCharCache& cache,
                                      double page_width, double page_height,
                                      std::vector<SparseGrid>* sparse_grids) {
    if (lines.size() < 4) return {};

    std::vector<PdfLineSegment> h_lines, v_lines, h_frags;
    for (auto& l : lines) {
        if (l.is_horizontal()) {
            double y = (l.y0 + l.y1) / 2.0;
            if (y < 0 || y > page_height) continue;
            double lx = std::min((double)l.x0, (double)l.x1);
            double rx = std::max((double)l.x0, (double)l.x1);
            if (lx < -10 || rx > page_width + 10) continue;
            // Too short to be a rule on its own, but per-cell borders and
            // dashed rules arrive as exactly such fragments: hold them for
            // the collinear merge below instead of dropping them outright.
            if (rx - lx < 50.0) { h_frags.push_back(l); continue; }
            h_lines.push_back(l);
        } else if (l.is_vertical()) {
            double x = (l.x0 + l.x1) / 2.0;
            if (x < 0 || x > page_width) continue;
            double ly = std::min((double)l.y0, (double)l.y1);
            double ry = std::max((double)l.y0, (double)l.y1);
            if (ly < -10 || ry > page_height + 10) continue;
            v_lines.push_back(l);
        }
    }

    // Rules drawn per cell (each border segment one column wide) or dashed
    // fall under the length cut fragment by fragment. Touching fragments on
    // one y level merge into a run, and a run of rule length is a rule.
    if (!h_frags.empty()) {
        std::vector<double> frag_ys;
        for (auto& l : h_frags) frag_ys.push_back((l.y0 + l.y1) / 2.0);
        constexpr double kFragJoinTol = 3.0;
        for (double ly : cluster_values(frag_ys, 3.0)) {
            std::vector<std::pair<double, double>> iv;
            for (auto& l : h_frags) {
                if (std::abs((l.y0 + l.y1) / 2.0 - ly) > 3.0) continue;
                iv.push_back({std::min((double)l.x0, (double)l.x1),
                              std::max((double)l.x0, (double)l.x1)});
            }
            std::sort(iv.begin(), iv.end());
            double lo = iv[0].first, hi = iv[0].second;
            auto flush = [&]() {
                if (hi - lo >= 50.0)
                    h_lines.push_back({static_cast<float>(lo),
                                       static_cast<float>(ly),
                                       static_cast<float>(hi),
                                       static_cast<float>(ly)});
            };
            for (size_t i = 1; i < iv.size(); i++) {
                if (iv[i].first <= hi + kFragJoinTol) {
                    hi = std::max(hi, iv[i].second);
                } else {
                    flush();
                    lo = iv[i].first;
                    hi = iv[i].second;
                }
            }
            flush();
        }
    }

    std::vector<TableData> result;
    detect_ruled_zoned(h_lines, v_lines, cache, sparse_grids, result, 0);
    // Detach a trailing caption row ("표 4.2 ...", "그림 ...") that was
    // absorbed when stacked tables were bridged across the caption line;
    // it belongs to the table directly below as its title.
    for (auto& t : result) {
        if (t.rows.size() < 2) continue;
        auto& last = t.rows.back();
        int filled = 0;
        std::string text;
        for (auto& c : last)
            if (!c.empty()) { filled++; text = c; }
        if (filled != 1 || text.size() < 8) continue;
        bool is_caption = text.rfind("\xED\x91\x9C ", 0) == 0 ||          // "표 "
                          text.rfind("\xEA\xB7\xB8\xEB\xA6\xBC ", 0) == 0; // "그림 "
        if (!is_caption) continue;
        double bottom = std::min(t.y0, t.y1);
        TableData* below = nullptr;
        for (auto& u : result) {
            if (&u == &t) continue;
            double utop = std::max(u.y0, u.y1);
            if (utop <= bottom + 5.0 &&
                (!below || utop > std::max(below->y0, below->y1)))
                below = &u;
        }
        if (below && below->title.empty()) {
            below->title = text;
            t.rows.pop_back();
            mask_erase_row(t, t.rows.size());
        }
    }
    return result;
}

// Banded (zebra) tables commonly leave the first or last body row
// unshaded: stripes start on the second body row, or the row count is odd
// and the last stripe falls on a white row. The shading run then stops at
// the last painted rect and the unshaded row is cut off. Extend the run
// level by level over adjacent row slots (one row pitch each) whose text
// is a single line that stays inside the table's width and whose word
// spans each fall inside one column of the run's own text-inferred grid,
// covering at least half the columns, and that keeps the run's row rhythm:
// its line sits one pitch from the text of the run's edge row, and no other
// line follows within most of a pitch (a header-only tint above a tighter
// body is not a band; absorbing one body row of it would split the body).
// A caption, a paragraph or a note fails: it crosses a column boundary,
// spills past the table or wraps.
static void extend_shading_run_to_aligned_rows(
        std::vector<double>& run, double left, double right,
        const PageCharCache& cache,
        const std::vector<TableData>& existing_tables) {
    if (run.size() < 3 || right - left < 30.0) return;
    std::vector<double> gaps;
    for (size_t i = 1; i < run.size(); i++) gaps.push_back(run[i] - run[i - 1]);
    std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
    const double pitch = gaps[gaps.size() / 2];
    if (pitch < 6.0) return;
    const auto cols = infer_columns_from_text(cache, left, right, run);
    if (cols.size() < 3) return;
    const int n_cols = (int)cols.size() - 1;
    double min_col_w = 1e9;
    for (int c = 0; c < n_cols; c++)
        min_col_w = std::min(min_col_w, cols[c + 1] - cols[c]);

    auto in_x = [&](const PageCharCache::CharInfo& ch) {
        return ch.unicode != ' ' && ch.unicode != 0xA0 && ch.unicode != '\t' &&
               ch.x >= left - 2.0 && ch.x <= right + 2.0;
    };
    // Text line center of a run row (median glyph center), NaN when empty.
    auto row_text_y = [&](double lo, double hi) {
        std::vector<double> ys;
        for (auto& ch : cache.chars)
            if (in_x(ch) && ch.y > lo + 1.0 && ch.y < hi - 1.0) ys.push_back(ch.y);
        if (ys.empty()) return std::nan("");
        std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
        return ys[ys.size() / 2];
    };

    // dir < 0: slot below the run (lower y); dir > 0: above it.
    auto slot_is_row = [&](double lo, double hi, double edge_text_y, int dir) {
        if (std::isnan(edge_text_y)) return false;
        for (auto& t : existing_tables) {
            double tb = std::min(t.y0, t.y1), tt = std::max(t.y0, t.y1);
            double tl = std::min(t.x0, t.x1), tr = std::max(t.x0, t.x1);
            if (std::min(hi, tt) - std::max(lo, tb) > 2.0 &&
                std::min(right, tr) - std::max(left, tl) > 2.0)
                return false;
        }
        std::vector<std::pair<double,double>> ranges;
        double y_lo = 1e9, y_hi = -1e9, h_max = 0;
        for (auto& ch : cache.chars) {
            if (ch.unicode == ' ' || ch.unicode == 0xA0 || ch.unicode == '\t')
                continue;
            if (ch.y <= lo + 1.0 || ch.y >= hi - 1.0) continue;
            if (ch.x < left - 2.0 || ch.x > right + 2.0) {
                // Text beside the table within a column width is the same
                // line running past it (prose); farther off is another
                // page column and says nothing about this slot.
                if (ch.right > left - min_col_w && ch.left < right + min_col_w)
                    return false;
                continue;
            }
            // The glyph must sit inside the slot, not straddle its edge.
            if (ch.top > hi + 1.0 || ch.bot < lo - 1.0) return false;
            ranges.push_back({ch.left, ch.right});
            y_lo = std::min(y_lo, ch.y);
            y_hi = std::max(y_hi, ch.y);
            h_max = std::max(h_max, ch.top - ch.bot);
        }
        if (ranges.size() < 2) return false;
        if (y_hi - y_lo > std::max(2.0, h_max * 0.5)) return false;  // wraps
        auto spans = merge_char_ranges(ranges);
        if (spans.size() < 2) return false;
        std::vector<bool> hit(n_cols, false);
        for (auto& sp : spans) {
            int col = -1;
            for (int c = 0; c < n_cols; c++)
                if (sp.first >= cols[c] - 2.0 && sp.second <= cols[c + 1] + 2.0) {
                    col = c;
                    break;
                }
            if (col < 0) return false;   // crosses a column boundary
            hit[col] = true;
        }
        int n_hit = 0;
        for (bool h : hit) n_hit += h ? 1 : 0;
        if (n_hit < 2 || n_hit * 2 < n_cols) return false;
        // Row rhythm: one pitch from the edge row's line, and no line
        // crowding it from outside.
        const double cand_y = (y_lo + y_hi) / 2.0;
        if (std::abs(std::abs(edge_text_y - cand_y) - pitch) > pitch * 0.25)
            return false;
        for (auto& ch : cache.chars) {
            if (!in_x(ch)) continue;
            double d = (ch.y - cand_y) * dir;   // > 0: outward
            if (d > std::max(2.0, h_max * 0.5) && d < pitch * 0.75) return false;
        }
        return true;
    };

    // Lower edge (smaller y) first, then the upper edge.
    while (slot_is_row(run.front() - pitch, run.front(),
                       row_text_y(run[0], run[1]), -1))
        run.insert(run.begin(), run.front() - pitch);
    while (slot_is_row(run.back(), run.back() + pitch,
                       row_text_y(run[run.size() - 2], run.back()), +1))
        run.push_back(run.back() + pitch);
}

// ── Shading-grid table detection ────────────────────────────────────
//
// Tables drawn with cell background fills and no rules at all (zebra
// striping, shaded headers). Cell shading rects form a grid: their x-edges
// align across bands. Edges are synthesized into rules and build_table is
// reused. Kept separate from the drawn-line pass so shading evidence can
// never bridge into real-rule groups.
std::vector<TableData> detect_shading_tables(
        const std::vector<PdfFillRect>& fill_rects,
        const PageCharCache& cache,
        const std::vector<TableData>& existing_tables,
        double page_width, double page_height) {
    if (fill_rects.size() < 2) return {};

    std::vector<PdfFillRect> rects;
    for (auto& fr : fill_rects) {
        if (fr.x1 - fr.x0 < 15 || fr.y1 - fr.y0 < 8) continue;
        if (fr.x0 < -10 || fr.x1 > page_width + 10) continue;
        if (fr.y0 < -10 || fr.y1 > page_height + 10) continue;
        // Near-white fills are page background, not shading (OCR tools back
        // every text line with a white-ish rect; an explicitly white-shaded
        // cell is visually indistinguishable from no table anyway). Same
        // threshold as filter_white_stroke.
        if (fr.r >= 0.94f && fr.g >= 0.94f && fr.b >= 0.94f) continue;
        // Page-background sized fills are never cell shading
        if ((fr.x1 - fr.x0) * (fr.y1 - fr.y0) > page_width * page_height * 0.5)
            continue;
        rects.push_back(fr);
    }

    // Drop rects contained in a larger rect: Word draws paragraph shading
    // inset inside the cell shading rect — the inset edges are not cell
    // boundaries and would fabricate columns.
    std::vector<PdfFillRect> outer;
    for (size_t i = 0; i < rects.size(); i++) {
        bool contained = false;
        double area_i = (rects[i].x1 - rects[i].x0) * (rects[i].y1 - rects[i].y0);
        for (size_t j = 0; j < rects.size(); j++) {
            if (i == j) continue;
            double area_j = (rects[j].x1 - rects[j].x0) * (rects[j].y1 - rects[j].y0);
            if (area_j <= area_i + 1.0) continue;
            if (rects[i].x0 >= rects[j].x0 - 1.5 && rects[i].x1 <= rects[j].x1 + 1.5 &&
                rects[i].y0 >= rects[j].y0 - 1.5 && rects[i].y1 <= rects[j].y1 + 1.5) {
                contained = true;
                break;
            }
        }
        if (!contained) outer.push_back(rects[i]);
    }
    if (outer.size() < 2) return {};

    // Grid-coherent rects only: both x-edges must be shared with another
    // rect (adjacency in a band, or alignment across bands). Lone highlight
    // rects and callout boxes fail this.
    auto x_shared = [&](double x, size_t self) {
        for (size_t j = 0; j < outer.size(); j++) {
            if (j == self) continue;
            if (std::abs(outer[j].x0 - x) < 3.0 || std::abs(outer[j].x1 - x) < 3.0)
                return true;
        }
        return false;
    };
    std::vector<PdfFillRect> shared;
    for (size_t i = 0; i < outer.size(); i++)
        if (x_shared(outer[i].x0, i) && x_shared(outer[i].x1, i))
            shared.push_back(outer[i]);
    if (shared.size() < 2) return {};

    // Band structure: cluster rects by vertical extent. A shading band is
    // either several cells side by side, or a single full-row stripe whose
    // x-extent recurs in another band (zebra). Ragged same-left rects
    // (line-backing fills) form neither.
    std::sort(shared.begin(), shared.end(),
              [](const PdfFillRect& a, const PdfFillRect& b) {
                  return a.y0 < b.y0;
              });
    struct ShadeBand { double y0, y1; std::vector<size_t> idx; };
    std::vector<ShadeBand> bands;
    for (size_t i = 0; i < shared.size(); i++) {
        if (!bands.empty() && std::abs(shared[i].y0 - bands.back().y0) < 3.0 &&
            std::abs(shared[i].y1 - bands.back().y1) < 3.0) {
            bands.back().idx.push_back(i);
        } else {
            bands.push_back({shared[i].y0, shared[i].y1, {i}});
        }
    }
    // Cell shading is taller than its text (cell padding); a rect that hugs
    // the text height is a line-backing fill, not a cell.
    auto band_padded = [&](const ShadeBand& bd) {
        std::vector<double> hs;
        for (auto& ch : cache.chars) {
            if (ch.y < bd.y0 || ch.y > bd.y1) continue;
            double h = ch.top - ch.bot;
            if (h > 1) hs.push_back(h);
        }
        if (hs.empty()) return true;
        std::nth_element(hs.begin(), hs.begin() + hs.size() / 2, hs.end());
        return (bd.y1 - bd.y0) >= hs[hs.size() / 2] * 1.35;
    };

    std::vector<PdfFillRect> grid;
    for (size_t b = 0; b < bands.size(); b++) {
        if (!band_padded(bands[b])) continue;
        bool ok = bands[b].idx.size() >= 2;
        if (!ok) {
            auto& r = shared[bands[b].idx[0]];
            for (size_t o = 0; o < bands.size() && !ok; o++) {
                if (o == b) continue;
                for (size_t oi : bands[o].idx) {
                    if (std::abs(shared[oi].x0 - r.x0) < 3.0 &&
                        std::abs(shared[oi].x1 - r.x1) < 3.0) {
                        ok = true;
                        break;
                    }
                }
            }
        }
        if (ok)
            for (size_t oi : bands[b].idx) grid.push_back(shared[oi]);
    }
    if (grid.size() < 2) return {};

    // Row levels from rect y-edges, column levels from x-edges.
    std::vector<double> ys, xs;
    for (auto& r : grid) {
        ys.push_back(r.y0);
        ys.push_back(r.y1);
        xs.push_back(r.x0);
        xs.push_back(r.x1);
    }
    auto y_levels = cluster_values(ys, 3.0);
    auto x_levels = cluster_values(xs, 3.0);
    if (y_levels.size() < 3 || x_levels.size() < 2) return {};

    // Split y-levels into contiguous runs; a gap much larger than a row is
    // a boundary between separate shaded regions. Unshaded rows BETWEEN
    // shaded bands (zebra striping) stay inside a run.
    std::vector<std::vector<double>> runs;
    std::vector<double> cur;
    cur.push_back(y_levels[0]);
    for (size_t i = 1; i < y_levels.size(); i++) {
        if (y_levels[i] - y_levels[i-1] > 90.0) {
            if (cur.size() >= 3) runs.push_back(cur);
            cur.clear();
        }
        cur.push_back(y_levels[i]);
    }
    if (cur.size() >= 3) runs.push_back(cur);

    std::vector<TableData> result;
    for (auto run : runs) {
        double y_min = run.front(), y_max = run.back();

        bool overlaps = false;
        for (auto& t : existing_tables) {
            double tb = std::min(t.y0, t.y1), tt = std::max(t.y0, t.y1);
            if (std::min(y_max, tt) - std::max(y_min, tb) > 5.0) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) continue;

        // Synthesize rules from the actual rect edges and reuse the line
        // builder. Each rule spans only where rects testify to it: a column
        // edge covers first-to-last band having that edge (bridging unshaded
        // zebra rows in between) but never rows without it — build_table's
        // merged-cell logic then keeps cells whole where no edge exists.
        std::vector<const PdfFillRect*> rrects;
        for (auto& r : grid)
            if (r.y0 >= y_min - 3.0 && r.y1 <= y_max + 3.0) rrects.push_back(&r);

        // Unshaded first/last rows that continue the column alignment.
        const double painted_min = y_min, painted_max = y_max;
        {
            double rl = 1e9, rr = -1e9;
            for (auto* r : rrects) {
                rl = std::min(rl, (double)r->x0);
                rr = std::max(rr, (double)r->x1);
            }
            if (rr > rl) {
                std::vector<TableData> others = existing_tables;
                others.insert(others.end(), result.begin(), result.end());
                extend_shading_run_to_aligned_rows(run, rl, rr, cache, others);
                y_min = run.front();
                y_max = run.back();
            }
        }

        // Shading often starts several columns into a table (for example,
        // forecast columns are tinted while row labels and historical values
        // stay white).  Extend a long, coherent shading run to text-inferred
        // boundaries outside its painted span.  When the inferred grid agrees
        // with most painted edges, use it as one coherent grid so an
        // unpainted historical column is not merged into its shaded neighbor.
        std::vector<double> run_x_levels = x_levels;
        double synth_left = x_levels.front(), synth_right = x_levels.back();
        bool text_extended_grid = false;
        if (run.size() >= 8) {
            double text_left = 1e9, text_right = -1e9;
            for (auto& ch : cache.chars) {
                if (ch.unicode == ' ' || ch.unicode == 0xA0 ||
                    ch.unicode == '\t') continue;
                if (ch.y <= y_min + 1.0 || ch.y >= y_max - 1.0) continue;
                text_left = std::min(text_left, ch.left);
                text_right = std::max(text_right, ch.right);
            }
            if (text_right > text_left) {
                auto inferred = infer_columns_from_text(
                    cache, text_left - 2.0, text_right + 2.0, run);
                int aligned = 0;
                for (double sx : x_levels) {
                    for (double tx : inferred) {
                        if (std::abs(sx - tx) < 12.0) {
                            aligned++;
                            break;
                        }
                    }
                }
                bool expands = !inferred.empty() &&
                    (inferred.front() < x_levels.front() - 8.0 ||
                     inferred.back() > x_levels.back() + 8.0);
                if (inferred.size() >= 4 && expands &&
                    aligned * 10 >= (int)x_levels.size() * 7) {
                    run_x_levels = std::move(inferred);
                    synth_left = run_x_levels.front();
                    synth_right = run_x_levels.back();
                    text_extended_grid = true;
                }
            }
        }

        std::vector<PdfLineSegment> h_synth, v_synth;
        for (double ry : run) {
            double lo = 1e9, hi = -1e9;
            for (auto* r : rrects) {
                // An extended (unshaded) level is bounded like the run.
                bool extended = ry < painted_min - 1.0 || ry > painted_max + 1.0;
                if (extended || std::abs(r->y0 - ry) < 3.0 ||
                    std::abs(r->y1 - ry) < 3.0) {
                    lo = std::min(lo, (double)r->x0);
                    hi = std::max(hi, (double)r->x1);
                }
            }
            if (hi > lo) {
                lo = std::min(lo, synth_left);
                hi = std::max(hi, synth_right);
                h_synth.push_back({static_cast<float>(lo), static_cast<float>(ry),
                                   static_cast<float>(hi), static_cast<float>(ry)});
            }
        }
        for (double cx : run_x_levels) {
            double lo = 1e9, hi = -1e9;
            for (auto* r : rrects) {
                if (std::abs(r->x0 - cx) < 3.0 || std::abs(r->x1 - cx) < 3.0) {
                    lo = std::min(lo, (double)r->y0);
                    hi = std::max(hi, (double)r->y1);
                }
            }
            if (text_extended_grid) {
                lo = y_min;
                hi = y_max;
            }
            // A rule reaching the painted edge continues over the
            // extended unshaded rows.
            if (hi > lo) {
                if (y_min < painted_min - 1.0 && lo < painted_min + 3.0) lo = y_min;
                if (y_max > painted_max + 1.0 && hi > painted_max - 3.0) hi = y_max;
            }
            if (hi > lo)
                v_synth.push_back({static_cast<float>(cx), static_cast<float>(lo),
                                   static_cast<float>(cx), static_cast<float>(hi)});
        }

        TableData t = build_table(run, h_synth, v_synth, cache, false);
        if (t.rows.empty()) continue;
        size_t max_cell = 0;
        for (auto& row : t.rows)
            for (auto& c : row)
                if (c.size() > max_cell) max_cell = c.size();
        if (max_cell > 300) continue;
        t.kind = TableData::SHADING;
        result.push_back(std::move(t));
    }
    return result;
}

// ── Pure text-based table detection (column-first) ──────────────────
//
// Algorithm overview (see bench/TABLE_DETECTION_REDESIGN.md):
//  S1. group chars into rows; find "y-bands" = runs of multi-cell rows
//      (allow 1-cell rows interleaved; split when ≥N consecutive 1-cell rows)
//  S2. inside each band, build an x histogram of char ranges from multi-cell
//      rows; locate consecutive bins that are empty in ≥70% of those rows
//      and wider than max(median_fs*0.7, 7.0) → column boundary
//  S3. assign chars of each row to columns; word-gap based space recovery
//  S4. rejection: list-like, caption, continuation, numeric-cell ratio
namespace text_tables {

struct CharInfo { double x, y, left, right, top, bot; unsigned int unicode;
                  int16_t rot; bool is_bold; };

struct TextRow {
    double y_center;
    double y_top, y_bot;
    std::vector<std::pair<double,double>> char_ranges;   // per-char [left,right]
    std::vector<size_t> char_indices;                    // indices into chars
    double x_min, x_max;
    bool is_multi_cell;
    // Dot leaders on the row ([x0, x1], left to right) and the label each
    // one leads from ([first glyph left, last glyph right]). A leader joins
    // one label to one value, so its label is a single cell however wide
    // its word gaps. labels[i] belongs to leaders[i]; it is empty
    // (first > second) when nothing precedes the leader.
    std::vector<std::pair<double,double>> leaders;
    std::vector<std::pair<double,double>> labels;
};

struct YBand {
    size_t first_row;     // inclusive
    size_t last_row;      // inclusive
    double y_top, y_bot;
    double x_min, x_max;
};

// helper: is a row "multi-cell" given a cell-merge gap (≥ gap → multi-cell)
static bool row_is_multi_cell(const TextRow& tr, double cell_merge_gap) {
    if (tr.char_ranges.size() < 2) return false;
    auto sorted = tr.char_ranges;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); i++) {
        if (sorted[i].first - sorted[i-1].second >= cell_merge_gap)
            return true;
    }
    return false;
}

// A row that opens a table caption ("Table 2.", "표 3") ends any band above
// it: the rows below belong to the next table. Figure captions do not split:
// what follows one is a figure's own labels (or, captions set below, the
// next block), and a band of chart labels is no table either way. Not
// applied to
// the full-width pass of a two-column page, where rows weld both page
// columns and a caption in one column says nothing about the other; the
// per-column passes split there.
static bool row_is_table_caption(const TextRow& tr, const std::vector<CharInfo>& chars) {
    std::vector<size_t> ci = tr.char_indices;
    std::sort(ci.begin(), ci.end(), [&](size_t a, size_t b) {
        return chars[a].x < chars[b].x;
    });
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < ci.size() && cps.size() < 16; i++)
        cps.push_back(chars[ci[i]].unicode);
    if (!is_caption_start(cps)) return false;
    size_t i = 0;
    if (cps[i] == '<' || cps[i] == '[' || cps[i] == 0x3008 || cps[i] == 0xFF1C) i++;
    return i < cps.size() && (cps[i] == 'T' || cps[i] == 't' || cps[i] == 0xD45C);
}

// S1: find y-bands of consecutive multi-cell rows (with bounded 1-cell rows)
static std::vector<YBand> find_y_bands(const std::vector<TextRow>& rows,
                                       const std::vector<CharInfo>& chars,
                                       bool split_at_captions) {
    std::vector<YBand> bands;
    const int kMaxSingleRunInside = 2;   // ≥3 consecutive 1-cell rows splits a band

    size_t i = 0;
    while (i < rows.size()) {
        // find next multi-cell row
        while (i < rows.size() && !rows[i].is_multi_cell) i++;
        if (i >= rows.size()) break;

        size_t band_start = i;
        size_t band_end = i;          // last multi-cell row in band
        int multi = 1;
        int single_run = 0;
        size_t j = i + 1;
        while (j < rows.size()) {
            // y-gap sanity: if the row is too far below the previous tracked row, stop
            const auto& prev = rows[(band_end > i) ? band_end : i];
            double vgap = std::abs(prev.y_bot - rows[j].y_top);
            // Use the typical line spacing as a guard; allow up to 3x font-size
            double fs_guard = std::max(prev.y_top - prev.y_bot, 8.0) * 3.5;
            if (vgap > fs_guard) break;

            if (rows[j].is_multi_cell) {
                band_end = j;
                multi++;
                single_run = 0;
            } else {
                single_run++;
                if (single_run > kMaxSingleRunInside) break;
                if (split_at_captions && row_is_table_caption(rows[j], chars)) break;
            }
            j++;
        }

        if (multi >= 2) {
            YBand b;
            b.first_row = band_start;
            b.last_row  = band_end;
            b.y_top = rows[band_start].y_top;
            b.y_bot = rows[band_end].y_bot;
            // include any 1-cell rows that sit between band_start and band_end
            for (size_t k = band_start; k <= band_end; k++) {
                b.y_top = std::max(b.y_top, rows[k].y_top);
                b.y_bot = std::min(b.y_bot, rows[k].y_bot);
            }

            // x extent from multi-cell rows in the band
            double xl = 1e18, xr = -1e18;
            for (size_t k = band_start; k <= band_end; k++) {
                if (!rows[k].is_multi_cell) continue;
                xl = std::min(xl, rows[k].x_min);
                xr = std::max(xr, rows[k].x_max);
            }
            b.x_min = xl;
            b.x_max = xr;
            bands.push_back(b);
        }
        i = (band_end >= i) ? (band_end + 1) : (i + 1);
    }
    return bands;
}

// S2: column boundaries inside a band by x-bin histogram of multi-cell rows.
//   Returns boundaries (left, inner col-edges, right) — empty on failure.
// column_evidence: also place boundaries from cell-stack evidence (sparse
// columns, see below); false gives the gap-and-straddle columns alone.
static std::vector<double> infer_columns_in_band(
        const std::vector<TextRow>& rows, const YBand& band,
        double median_fs, bool column_evidence,
        const std::vector<CharInfo>& chars) {
    // Collect multi-cell rows in the band
    std::vector<size_t> mc;
    for (size_t k = band.first_row; k <= band.last_row; k++)
        if (rows[k].is_multi_cell) mc.push_back(k);
    if (mc.size() < 2) return {};

    double x_lo = band.x_min, x_hi = band.x_max;
    if (x_hi - x_lo < 30) return {};
    double bin_w = std::max(median_fs * 0.15, 1.0);
    int n_bins = std::max(8, (int)std::ceil((x_hi - x_lo) / bin_w));
    bin_w = (x_hi - x_lo) / n_bins;

    // hit_count[b] = number of multi-cell rows that have a char overlapping bin b
    std::vector<int> hit_count(n_bins, 0);

    auto bin_idx = [&](double x) -> int {
        int b = (int)std::floor((x - x_lo) / bin_w);
        if (b < 0) b = 0;
        if (b >= n_bins) b = n_bins - 1;
        return b;
    };

    for (size_t ri : mc) {
        // mark bins covered by any char range in this row
        std::vector<bool> row_hit(n_bins, false);
        for (auto& cr : rows[ri].char_ranges) {
            int b0 = bin_idx(cr.first);
            int b1 = bin_idx(cr.second);
            for (int b = b0; b <= b1; b++) row_hit[b] = true;
        }
        // A leader's label fills its word gaps: no column runs through it.
        for (auto& lb : rows[ri].labels) {
            int b0 = bin_idx(lb.first);
            int b1 = bin_idx(lb.second);
            for (int b = b0; b <= b1; b++) row_hit[b] = true;
        }
        for (int b = 0; b < n_bins; b++) if (row_hit[b]) hit_count[b]++;
    }

    int total_mc = (int)mc.size();
    // empty bin: ≤ 40% of multi-cell rows have a char there. A bin still has
    // to be a *gap* — adjacent occupied bins on both sides — to be a column
    // boundary, so a slightly looser empty threshold helps catch tables where
    // not every row has every column populated.
    double empty_thresh_frac = 0.40;
    int empty_max = (int)std::floor(total_mc * empty_thresh_frac);

    double col_gap_min = std::max(median_fs * 0.4, 3.0);

    // sweep for runs of empty bins inside [x_lo+, x_hi-]
    std::vector<std::pair<double,double>> empty_runs;   // (start_x, end_x)
    int run_start = -1;
    for (int b = 0; b < n_bins; b++) {
        bool is_empty = hit_count[b] <= empty_max;
        if (is_empty) {
            if (run_start < 0) run_start = b;
        } else {
            if (run_start >= 0) {
                double sx = x_lo + run_start * bin_w;
                double ex = x_lo + b * bin_w;
                empty_runs.push_back({sx, ex});
                run_start = -1;
            }
        }
    }
    if (run_start >= 0) {
        double sx = x_lo + run_start * bin_w;
        double ex = x_lo + n_bins * bin_w;
        empty_runs.push_back({sx, ex});
    }

    // Glyph extent of row ri inside (a, b) — glyph centres decide.
    auto row_extent = [&](size_t ri, double a, double b,
                          double& l, double& r) {
        l = 1e18; r = -1e18;
        for (auto& cr : rows[ri].char_ranges) {
            double c = (cr.first + cr.second) / 2.0;
            if (c <= a || c >= b) continue;
            l = std::min(l, cr.first);
            r = std::max(r, cr.second);
        }
        return r > l;
    };
    // A column of a table is a stack of cells sharing an alignment: their
    // left edges, right edges or centres line up (one header cell may set
    // its own alignment once the stack has four or more cells). Lines of
    // running text line up too (a paragraph beside a figure's labels), so
    // a stack whose typical cell is a phrase of five or more words is
    // prose, not a column. n receives the number of rows with text in
    // (a, b).
    // Whether every glyph centred in (a, b) is a currency sign.
    auto only_currency = [&](double a, double b) {
        bool any = false;
        for (size_t ri : mc)
            for (size_t ci : rows[ri].char_indices) {
                const auto& c = chars[ci];
                double m = (c.left + c.right) / 2.0;
                if (m <= a || m >= b) continue;
                uint32_t u = c.unicode;
                if (u != '$' && u != 0x20AC && u != 0xA3 && u != 0xA5 && u != 0x20A9) return false;
                any = true;
            }
        return any;
    };
    const double align_tol = std::max(median_fs * 0.5, 2.0);
    const double word_gap = std::max(median_fs * 0.2, 1.5);
    auto stack_aligned = [&](double a, double b, int& n) {
        std::vector<double> L, R, C;
        std::vector<int> words;
        for (size_t ri : mc) {
            double l, r;
            if (!row_extent(ri, a, b, l, r)) continue;
            L.push_back(l); R.push_back(r); C.push_back((l + r) / 2.0);
            std::vector<std::pair<double, double>> in;
            for (auto& cr : rows[ri].char_ranges) {
                double c = (cr.first + cr.second) / 2.0;
                if (c > a && c < b) in.push_back(cr);
            }
            std::sort(in.begin(), in.end());
            int w = 1;
            for (size_t k = 1; k < in.size(); k++)
                if (in[k].first - in[k - 1].second > word_gap) w++;
            words.push_back(w);
        }
        n = (int)L.size();
        if (n < 2) return false;
        std::nth_element(words.begin(), words.begin() + words.size() / 2, words.end());
        if (words[words.size() / 2] >= 5) return false;
        const int keep = n >= 4 ? n - 1 : n;
        auto tight = [&](std::vector<double>& v) {
            std::sort(v.begin(), v.end());
            for (int st = 0; st + keep <= n; st++)
                if (v[st + keep - 1] - v[st] <= align_tol) return true;
            return false;
        };
        return tight(L) || tight(R) || tight(C);
    };

    // Candidate boundaries. A run of bins empty in most rows normally holds
    // one boundary at its middle. A sparse column (values in fewer than
    // 40% of the rows) is itself "mostly empty", so its run reaches from the
    // previous column to the next one (or the band edge) with the sparse
    // column inside as a hump of hits between bins no row touches at all.
    // When the hump's cells form an aligned stack, the boundaries are the
    // untouched sub-runs around it, not the middle of the whole run.
    // z_*: untouched sub-run; lo/hi: the empty run around the boundary
    struct Cand { double e; double z_lo, z_hi; double lo, hi; };
    std::vector<Cand> cands;
    auto zero_runs_in = [&](int b0, int b1) {
        std::vector<std::pair<int, int>> z;   // [start, end) bins, hit == 0
        int st = -1;
        for (int b = b0; b < b1; b++) {
            if (hit_count[b] == 0) { if (st < 0) st = b; }
            else if (st >= 0) { z.push_back({st, b}); st = -1; }
        }
        if (st >= 0) z.push_back({st, b1});
        std::vector<std::pair<int, int>> wide;
        for (auto& r : z)
            if ((r.second - r.first) * bin_w >= col_gap_min) wide.push_back(r);
        return wide;
    };
    for (auto& run : empty_runs) {
        double width = run.second - run.first;
        if (width < col_gap_min) continue;
        int b0 = (int)std::lround((run.first - x_lo) / bin_w);
        int b1 = std::min(n_bins, (int)std::lround((run.second - x_lo) / bin_w));
        auto zr = zero_runs_in(b0, b1);
        bool hump = false;
        for (int b = b0; b < b1; b++) if (hit_count[b] > 0) { hump = true; break; }
        if (column_evidence && hump && !zr.empty()) {
            // Boundaries at the untouched sub-runs that are not margins;
            // the stretches between them (and the run's ends) are columns.
            std::vector<Cand> inner;
            for (auto& z : zr) {
                double zl = x_lo + z.first * bin_w, zh = x_lo + z.second * bin_w;
                double mid = (zl + zh) / 2.0;
                if (mid <= x_lo + 2.0 || mid >= x_hi - 2.0) continue;
                inner.push_back({mid, zl, zh, zl, zh});
            }
            bool stacks_ok = !inner.empty();
            for (size_t k = 0; stacks_ok && k <= inner.size(); k++) {
                double a = (k == 0) ? run.first : inner[k - 1].z_hi;
                double b = (k == inner.size()) ? run.second : inner[k].z_lo;
                // A stretch holding no glyph centre is the frayed edge of
                // the neighbouring column; one holding cells must stack.
                // Currency signs set apart from their amounts ("$  1,111")
                // stack too, but they are the amounts' prefix, not a column.
                int n = 0;
                bool aligned = stack_aligned(a, b, n);
                if (n > 0 && (!aligned || only_currency(a, b))) stacks_ok = false;
            }
            if (stacks_ok) {
                for (auto& c : inner) cands.push_back(c);
                continue;
            }
        }
        double mid = (run.first + run.second) / 2.0;
        // skip runs hugging the band edges (those are just margins)
        if (mid <= x_lo + 2.0) continue;
        if (mid >= x_hi - 2.0) continue;
        // The untouched sub-run nearest the middle, if any, for the
        // alignment test below.
        double zl = NAN, zh = NAN, best = 1e18;
        for (auto& z : zr) {
            double l = x_lo + z.first * bin_w, h = x_lo + z.second * bin_w;
            double d = std::abs((l + h) / 2.0 - mid);
            if (d < best) { best = d; zl = l; zh = h; }
        }
        cands.push_back({mid, zl, zh, run.first, run.second});
    }
    if (cands.empty()) return {};
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.e < b.e; });
    if (std::getenv("JDOC_TABLE_DEBUG")) {
        fprintf(stderr, "[band-cols] x %.1f..%.1f runs", x_lo, x_hi);
        for (auto& r : empty_runs) fprintf(stderr, " [%.1f,%.1f]", r.first, r.second);
        fprintf(stderr, " cands");
        for (auto& c : cands) fprintf(stderr, " %.1f(%.1f-%.1f)", c.e, c.z_lo, c.z_hi);
        fprintf(stderr, "\n");
    }

    // Validate: for each candidate boundary, ≥70% of multi-cell rows that
    // overlap a neighborhood of the boundary must "straddle" it (chars on
    // both sides). Rows that have no chars near the boundary are ignored — a
    // row of body text on the opposite side of the page does not invalidate
    // a column boundary inside a data table.
    //
    // A sparse first or last column fails that test by construction: rows
    // with its cell empty have nothing on one side. Such a boundary still
    // stands on column evidence: no row's glyphs touch a gap at least a
    // column gap wide around it, and the cells on each side form an aligned
    // stack — two or more cells on the sparse side, three or more on the
    // other. Prose torn there leaves no such stacks.
    std::vector<double> kept;
    double neigh = std::max(median_fs * 6.0, 60.0);
    // A wide gutter whose right side opens with figures on most rows is the
    // label/value split of a financial statement ("Revenues ...... 100.0%").
    // No glyph lies within reach of its midpoint, so nearness to it is taken
    // from the edges of the empty run; otherwise the boundary was dropped for
    // lack of evidence however many rows straddled it. Other wide gutters
    // keep the midpoint test: the gap between the two text columns of a page
    // is wide too, and must not split prose into a table.
    auto figure_first = [&](const TextRow& r, double x) {
        double best = 1e18; unsigned int u = 0;
        for (size_t idx : r.char_indices) {
            const auto& c = chars[idx];
            if (c.left >= x && c.left < best) { best = c.left; u = c.unicode; }
        }
        return (u >= '0' && u <= '9') || u == '$' || u == '(' || u == '-' ||
               u == 0x2014 || u == 0x2013 || u == '%' || u == 0x20AC || u == 0xA3;
    };
    // The figure that opens a row's text from x on, as a number ("2,500",
    // "(3.4)", "–15"); NaN when the row's first token there is not one.
    auto first_number = [&](const TextRow& r, double x) {
        std::vector<size_t> idx;
        for (size_t i : r.char_indices) if (chars[i].left >= x) idx.push_back(i);
        std::sort(idx.begin(), idx.end(),
                  [&](size_t p, size_t q) { return chars[p].left < chars[q].left; });
        std::string tok;
        double prev_right = 0;
        for (size_t k = 0; k < idx.size(); k++) {
            const auto& c = chars[idx[k]];
            double h = std::max(c.top - c.bot, 1.0);
            if (k > 0 && c.left - prev_right > 0.2 * h) break;   // word space
            unsigned int u = c.unicode;
            if (u >= '0' && u <= '9') tok += (char)u;
            else if (u == '.') tok += '.';
            else if (u == '-' || u == 0x2212 || u == 0x2013 || u == '(') { if (tok.empty()) tok += '-'; }
            else if (u != ',' && u != ')' && u != '%') break;
            prev_right = c.right;
        }
        if (tok.empty() || tok == "-") return std::nan("");
        return std::strtod(tok.c_str(), nullptr);
    };
    // Figures falling by even steps down the rows (30, 20, 10, 0) are the
    // tick labels of a chart's value axis, which rises up the page; amounts
    // in a statement never step evenly, and page references in a contents
    // list rise down it. Other chart text (data labels, legend entries) may
    // sit on rows between the ticks or open a tick's row, so the ladder is
    // sought in row order with rows skipped and a single rung missing at a
    // time. True when such a ladder of three or more covers at least half
    // of the values.
    auto ticks = [](const std::vector<double>& v) {
        size_t n = v.size(), best = 0;
        for (size_t i = 0; i < n; i++) {
            if (std::isnan(v[i])) continue;
            for (size_t j = i + 1; j < n; j++) {
                double d = v[j] - v[i];
                if (std::isnan(d) || d >= 0) continue;
                size_t len = 2;
                long rung = 1;
                for (size_t k = j + 1; k < n; k++) {
                    if (std::isnan(v[k])) continue;
                    double q = (v[k] - v[i]) / d;
                    long r = std::lround(q);
                    if (std::abs(q - r) > 1e-6 || r <= rung || r > rung + 2) continue;
                    len++;
                    rung = r;
                }
                best = std::max(best, len);
            }
        }
        return best >= 3 && best * 2 >= n;
    };
    for (size_t ci = 0; ci < cands.size(); ci++) {
        const Cand& ed = cands[ci];
        double e = ed.e;
        bool wide = ed.hi - ed.lo > median_fs * 6.0;
        bool figures = false;
        if (wide) {
            int straddle = 0, fig = 0;
            std::vector<double> right_vals;
            for (size_t ri : mc) {
                bool l = false, rr = false;
                for (auto& cr : rows[ri].char_ranges) {
                    if (cr.second <= ed.lo + 0.5) l = true;
                    if (cr.first >= ed.hi - 0.5) rr = true;
                }
                if (l && rr) {
                    straddle++;
                    if (figure_first(rows[ri], ed.hi - 0.5)) {
                        fig++;
                        right_vals.push_back(first_number(rows[ri], e));
                    }
                }
            }
            figures = straddle >= 3 && fig * 10 >= straddle * 7 && !ticks(right_vals);
            if (std::getenv("JDOC_TABLE_DEBUG")) {
                fprintf(stderr, "[fig-gutter] %.1f-%.1f straddle %d fig %d ticks %d:",
                        ed.lo, ed.hi, straddle, fig, (int)ticks(right_vals));
                for (double v : right_vals) fprintf(stderr, " %g", v);
                fprintf(stderr, "\n");
            }
        }
        int agree = 0;
        int relevant = 0;
        for (size_t ri : mc) {
            bool has_left = false, has_right = false;
            bool near = false;
            bool splits_label = false;
            for (auto& lb : rows[ri].labels)
                if (lb.first < e - 0.5 && lb.second > e + 0.5) splits_label = true;
            if (splits_label) { relevant++; continue; }
            for (auto& cr : rows[ri].char_ranges) {
                if (cr.second <= e) has_left = true;
                else if (cr.first >= e) has_right = true;
                if (figures ? (cr.first <= ed.hi + neigh && cr.second >= ed.lo - neigh)
                            : (cr.first <= e + neigh && cr.second >= e - neigh))
                    near = true;
                if (has_left && has_right && near) break;
            }
            if (!near) continue;
            relevant++;
            if (has_left && has_right) agree++;
        }
        int needed = std::max(2, (int)std::ceil(relevant * 0.70));
        if (relevant >= 2 && agree >= needed) { kept.push_back(e); continue; }
        if (!column_evidence || std::isnan(cands[ci].z_lo)) continue;
        double a = ci > 0 ? cands[ci - 1].e : x_lo - 1.0;
        double b = ci + 1 < cands.size() ? cands[ci + 1].e : x_hi + 1.0;
        int nl = 0, nr = 0;
        bool al = stack_aligned(a, cands[ci].z_lo, nl);
        bool ar = stack_aligned(cands[ci].z_hi, b, nr);
        if (al && ar && std::min(nl, nr) >= 2 && std::max(nl, nr) >= 3)
            kept.push_back((cands[ci].z_lo + cands[ci].z_hi) / 2.0);
    }
    if (kept.empty()) return {};

    std::vector<double> bounds;
    bounds.push_back(x_lo);
    for (double e : kept) bounds.push_back(e);
    bounds.push_back(x_hi);
    return bounds;
}

// A torn top row may be a spanning header ("BLEU" centred over EN-DE and
// EN-FR) rather than drifted prose: each of its glyph runs sits centred over
// a run of whole columns instead of being cut mid-word by a boundary the
// rows below agree on. Fills the header cells (each run in its leftmost
// column) and the span per column; false when the row is not that shape.
static bool spanning_header_cells(const std::vector<size_t>& ci,
                                  const std::vector<CharInfo>& chars,
                                  const std::vector<double>& row_bounds,
                                  double min_split_gap, double word_gap,
                                  std::vector<std::string>& cells,
                                  std::vector<int>& spans) {
    int n_cols = (int)row_bounds.size() - 1;
    struct Run { std::string text; double left, right; };
    std::vector<Run> runs;
    double prev_right = -1e9;
    for (size_t idx : ci) {
        const auto& ch = chars[idx];
        if (ch.rot != 0) return false;
        if (runs.empty() || ch.left - prev_right >= min_split_gap)
            runs.push_back({"", ch.left, ch.right});
        else if (ch.left - prev_right >= word_gap)
            runs.back().text += ' ';
        append_glyph_text(runs.back().text, ch.unicode);
        runs.back().right = std::max(runs.back().right, ch.right);
        prev_right = std::max(prev_right, ch.right);
    }
    if (runs.empty() || (int)runs.size() > n_cols) return false;
    auto col_of = [&](double x) {
        for (int c = 0; c < n_cols; c++)
            if (x < row_bounds[c + 1]) return c;
        return n_cols - 1;
    };
    cells.assign(n_cols, "");
    spans.assign(n_cols, 0);
    bool any_span = false, any_letter = false;
    for (auto& r : runs) {
        // A header names something; a row of affiliation marks ("1 2* 1")
        // over an author block has no letter in it.
        for (unsigned char ch : r.text)
            if ((ch | 0x20) - 'a' < 26u || ch >= 0x80) { any_letter = true; break; }
        int lo = col_of(r.left), hi = col_of(r.right);
        int span = hi - lo + 1;
        // A run across every column is a caption line, and a long one is
        // prose; a second run landing in a taken column is ordinary text.
        if (span >= n_cols || r.text.size() > 60 || !cells[lo].empty()) return false;
        if (span >= 2) {
            double span_l = row_bounds[lo], span_r = row_bounds[hi + 1];
            double off = std::fabs((r.left + r.right) / 2.0 - (span_l + span_r) / 2.0);
            if (off > (span_r - span_l) * 0.3) return false;
            any_span = true;
        }
        cells[lo] = r.text;
        spans[lo] = span;
    }
    return any_span && any_letter;
}

// S3: build the table from a band + columns.
// For each row, snap each inner column boundary to the nearest natural gap
// in that row (so we don't split words). Falls back to the global boundary if
// no usable gap is nearby.
static TableData build_table_from_band(
        const std::vector<TextRow>& rows, const YBand& band,
        const std::vector<double>& col_bounds,
        const std::vector<CharInfo>& chars,
        double median_fs) {
    TableData table;
    int n_cols = (int)col_bounds.size() - 1;
    if (n_cols < 2) return table;

    table.x0 = col_bounds.front();
    table.x1 = col_bounds.back();
    table.y0 = band.y_bot;
    table.y1 = band.y_top;

    double word_gap   = std::max(median_fs * 0.15, 1.2);
    double snap_tol   = std::max(median_fs * 2.0, 15.0);
    double min_split_gap = std::max(median_fs * 0.5, 4.0);

    // A real column boundary falls into whitespace on every row; a phantom
    // one (coincidentally aligned gaps in prose) is forced to cut between
    // glyphs that have no gap at all on some rows. Count, per boundary, the
    // rows where glyphs sit close on both sides ("near") and among those the
    // rows where the flanking glyphs almost touch ("torn").
    std::vector<int> near_cnt(n_cols + 1, 0), torn_cnt(n_cols + 1, 0);
    double torn_gap = std::max(median_fs * 0.2, 1.2);
    double near_win = median_fs * 3.0;
    std::vector<bool> row_has_tear;   // parallel to table.rows
    // A spanning header found at the top waits until the body is built: it
    // only joins a table that stands on three data rows of its own, so an
    // author block (marker row over two name rows) cannot reach the row
    // minimum by counting its markers as a header.
    bool have_pending_header = false;
    std::vector<std::string> pending_header;
    std::vector<int> pending_spans, pending_near, pending_torn;

    for (size_t k = band.first_row; k <= band.last_row; k++) {
        const auto& tr = rows[k];
        // gather chars sorted by x (use char_indices into the per-page chars[])
        std::vector<size_t> ci = tr.char_indices;
        std::sort(ci.begin(), ci.end(), [&](size_t a, size_t b) {
            return chars[a].x < chars[b].x;
        });

        // Find natural gap midpoints in this row (gaps between consecutive chars
        // ≥ min_split_gap), used for snapping column boundaries.
        std::vector<std::pair<double,double>> gap_runs;   // (gap_start, gap_end)
        for (size_t i = 1; i < ci.size(); i++) {
            double prev_r = chars[ci[i-1]].right;
            double cur_l  = chars[ci[i]].left;
            if (cur_l - prev_r >= min_split_gap) {
                gap_runs.push_back({prev_r, cur_l});
            }
        }

        // For each inner boundary, snap to nearest gap midpoint within snap_tol.
        std::vector<double> row_bounds(col_bounds.size());
        row_bounds.front() = col_bounds.front();
        row_bounds.back()  = col_bounds.back();
        for (int c = 1; c < (int)col_bounds.size() - 1; c++) {
            double e = col_bounds[c];
            double best = e;
            double best_d = 1e9;
            for (auto& g : gap_runs) {
                double gm = (g.first + g.second) / 2.0;
                double d = std::abs(gm - e);
                if (d < best_d && d <= snap_tol) {
                    best_d = d;
                    best = gm;
                }
            }
            row_bounds[c] = best;
        }
        // A boundary that would cut a leader's label moves to that leader:
        // the label is one cell and the leader is where it ends (a long
        // entry runs past the point where shorter entries' leaders start).
        for (int c = 1; c < (int)col_bounds.size() - 1; c++)
            for (size_t li = 0; li < tr.labels.size(); li++) {
                const auto& lb = tr.labels[li];
                if (lb.first < row_bounds[c] && row_bounds[c] < lb.second)
                    row_bounds[c] = (tr.leaders[li].first + tr.leaders[li].second) / 2.0;
            }
        // Ensure monotonic
        for (int c = 1; c < (int)row_bounds.size(); c++) {
            if (row_bounds[c] < row_bounds[c-1] + 0.1)
                row_bounds[c] = row_bounds[c-1] + 0.1;
        }

        int row_torn = 0, row_overflow = 0;
        std::vector<int> row_near(n_cols + 1, 0), row_torn_at(n_cols + 1, 0);
        // The row's own type size: a display title's word spaces scale with
        // it, so the gap that separates an overflowing label from the next
        // cell is measured against the row, not the page median.
        double row_fs = 0;
        for (size_t idx : ci) row_fs = std::max(row_fs, chars[idx].top - chars[idx].bot);
        for (int c = 1; c < (int)row_bounds.size() - 1; c++) {
            double b = row_bounds[c];
            const CharInfo* prev = nullptr;
            const CharInfo* next = nullptr;
            for (size_t idx : ci) {
                const auto& ch = chars[idx];
                if ((ch.left + ch.right) / 2.0 < b) prev = &ch;
                else { next = &ch; break; }
            }
            if (!prev || !next) continue;
            if (b - prev->right > near_win || next->left - b > near_win)
                continue;
            near_cnt[c]++;
            row_near[c]++;
            if (next->left - prev->right < torn_gap) {
                // A label longer than its column overflows into the
                // neighbour's whitespace but still ends in a real gap before
                // the neighbour's own text ("GoogLeNet [44] (ILSVRC'14)"
                // beside "-"); a prose line torn by a phantom boundary runs
                // on with word spaces only. Overflow is not a tear.
                // The gap that ends an overflowing label is cell padding, a
                // font size or more; a word space in a large title (which
                // can exceed the split gap) is not.
                const double overflow_gap = std::max({median_fs, row_fs, 8.0});
                bool overflow = false;
                double run_end = next->right;
                for (size_t idx : ci) {
                    const auto& ch = chars[idx];
                    if (ch.left < next->left) continue;
                    if (ch.left - run_end >= overflow_gap) {
                        overflow = run_end <= row_bounds[c + 1];
                        break;
                    }
                    run_end = std::max(run_end, ch.right);
                }
                if (!overflow) {
                    torn_cnt[c]++;
                    row_torn_at[c]++;
                    row_torn++;
                } else {
                    row_overflow++;
                }
            }
        }
        // A row whose glyphs are cut mid-word by two or more boundaries is
        // prose that drifted into the band (a paragraph line right below a
        // table): evict it. The capture check in the markdown pass returns
        // the line to the prose flow. Above the first data row the same
        // shape is a spanning header, which tears by construction; it is
        // kept, and its tears are not evidence against the boundaries.
        if (std::getenv("JDOC_TABLE_DEBUG")) {
            std::string dbg;
            for (size_t idx : ci) util::append_utf8(dbg, chars[idx].unicode);
            fprintf(stderr, "[text-row] y %.1f torn %d n %zu: %s\n",
                    tr.y_center, row_torn, ci.size(), dbg.c_str());
        }
        // A header needs a body: three or more rows below it, or the band
        // is a display equation whose aligned pieces line up. Any glyph run
        // across a boundary (torn or overflowing) qualifies the top row.
        if (table.rows.empty() && band.last_row - k >= 3 &&
            row_torn + row_overflow >= 1) {
            std::vector<std::string> hcells;
            std::vector<int> hspans;
            if (!have_pending_header &&
                spanning_header_cells(ci, chars, row_bounds, min_split_gap,
                                      word_gap, hcells, hspans)) {
                for (int c = 1; c < n_cols; c++) {
                    near_cnt[c] -= row_near[c];
                    torn_cnt[c] -= row_torn_at[c];
                }
                have_pending_header = true;
                pending_header = std::move(hcells);
                pending_spans = std::move(hspans);
                pending_near = row_near;
                pending_torn = row_torn_at;
                continue;
            }
        }
        if (row_torn >= 2) continue;

        std::vector<std::string> cells(n_cols);
        std::vector<double> last_right(n_cols, -1e9);
        std::vector<int16_t> last_rot(n_cols, 0);
        std::vector<int> glyphs(n_cols, 0), bold_glyphs(n_cols, 0);
        int run_col = 0;
        double run_right = -1e9;

        // Column assignment above works in page space, but the text of a cell
        // has to be emitted in the run's own reading order — a 180° run
        // advances toward -x, so ci's left-to-right order spells it backwards.
        // An upright row already is that order, which is nearly every row, so
        // it keeps ci and skips the resort. Direction is the primary key so a
        // row mixing them stays a strict weak ordering.
        std::vector<size_t> ri;
        const std::vector<size_t>* order = &ci;
        bool rotated = false;
        for (size_t idx : ci)
            if (chars[idx].rot != 0) { rotated = true; break; }
        if (rotated) {
            ri = ci;
            std::sort(ri.begin(), ri.end(), [&](size_t a, size_t b) {
                const auto& ca = chars[a];
                const auto& cb = chars[b];
                if (ca.rot != cb.rot) return ca.rot < cb.rot;
                double a_lo, a_hi, b_lo, b_hi;
                text_along_span(ca.rot, ca.left, ca.right, ca.top, ca.bot, a_lo, a_hi);
                text_along_span(cb.rot, cb.left, cb.right, cb.top, cb.bot, b_lo, b_hi);
                return a_lo < b_lo;
            });
            order = &ri;
        }

        for (size_t idx : *order) {
            const auto& ch = chars[idx];
            double cmid = (ch.left + ch.right) / 2.0;
            // A char well outside the inferred column span never belongs to a
            // cell (margin stamp grouped into the row by y); leave it to the
            // prose flow instead of prepending it to an edge cell.
            if (cmid < table.x0 - median_fs * 2.0 ||
                cmid > table.x1 + median_fs * 2.0)
                continue;
            int col = -1;
            for (int c = 0; c < n_cols; c++) {
                // Use strict less-than-or-equal on the right edge so chars that
                // sit exactly on a column boundary fall into the LEFT column —
                // this avoids the first letter of a word being pushed across
                // the boundary in some rows when boundaries snap tightly.
                double lo = row_bounds[c] - 0.5;
                double hi = (c == n_cols - 1) ? row_bounds[c+1] + 1.0
                                              : row_bounds[c+1];
                if (cmid >= lo && cmid < hi) {
                    col = c;
                    break;
                }
            }
            if (col < 0) {
                // fall back: leftmost or rightmost
                if (cmid < row_bounds.front()) col = 0;
                else col = n_cols - 1;
            }
            // A run of glyphs with no cell-sized gap inside it is one cell's
            // text even where a boundary (placed by the other rows) cuts it:
            // a label wider than its column keeps its tail rather than
            // dropping "’14)" into the neighbour. Upright rows only — a
            // rotated run's page-space gaps do not measure its advance.
            if (!rotated) {
                if (run_right > -1e8 && ch.left - run_right < min_split_gap)
                    col = run_col;
                else
                    run_col = col;
                run_right = std::max(run_right, ch.right);
            }
            // Word gaps are measured along the advance too, so a rotated run
            // does not read as one gapless word (its page-space gaps run
            // backwards and never clear the threshold). Across a direction
            // change the two frames share no axis, so the difference is
            // meaningless — the runs are separated unconditionally instead,
            // which is what get_text_in_rect does by breaking the row there.
            double lo, hi;
            text_along_span(ch.rot, ch.left, ch.right, ch.top, ch.bot, lo, hi);
            if (!cells[col].empty() &&
                (ch.rot != last_rot[col] || (lo - last_right[col]) >= word_gap))
                cells[col] += ' ';
            append_glyph_text(cells[col], ch.unicode);
            last_right[col] = hi;
            last_rot[col] = ch.rot;
            glyphs[col]++;
            if (ch.is_bold) bold_glyphs[col]++;
        }

        std::vector<uint8_t> bold_mask(n_cols, 0);
        for (int c = 0; c < n_cols; c++) {
            cells[c] = util::trim(cells[c]);
            // A cell set entirely in a bold face keeps the emphasis (the
            // best value in a results table); a mixed cell stays plain.
            bold_mask[c] = !cells[c].empty() && glyphs[c] > 0 && bold_glyphs[c] == glyphs[c];
        }
        table.rows.push_back(std::move(cells));
        table.cell_bold.push_back(std::move(bold_mask));
        row_has_tear.push_back(row_torn > 0);
    }

    if (have_pending_header) {
        // The header stands only on a body of three rows that each fill two
        // or more cells; an author block (a marker row, a name row, a lone
        // keyword) does not reach that by having a title above it.
        int body_rows = 0;
        for (auto& row : table.rows) {
            int filled = 0;
            for (auto& c : row) if (!c.empty()) filled++;
            if (filled >= 2) body_rows++;
        }
        if (body_rows >= 3) {
            table.rows.insert(table.rows.begin(), std::move(pending_header));
            table.cell_bold.insert(table.cell_bold.begin(), std::vector<uint8_t>(n_cols, 0));
            row_has_tear.insert(row_has_tear.begin(), false);
            table.header_spans = std::move(pending_spans);
        } else {
            // Not a header after all: its tears count against the boundaries
            // like any other row's.
            for (int c = 1; c < n_cols; c++) {
                near_cnt[c] += pending_near[c];
                torn_cnt[c] += pending_torn[c];
            }
        }
    }

    // A boundary that cuts through touching glyphs on 30%+ of its populated
    // rows is a phantom column — the whole band is prose, not a table. Small
    // bands (diagram labels) rarely repeat a tear on one boundary, so tears
    // are also summed across boundaries: real cells have padding, and any
    // substantial overall tear rate marks a non-table.
    bool tear_rate_bad = false;
    int total_torn = 0, total_near = 0;
    for (int c = 1; c < n_cols; c++) {
        total_torn += torn_cnt[c];
        total_near += near_cnt[c];
        if (torn_cnt[c] >= 2 && torn_cnt[c] * 10 >= near_cnt[c] * 3)
            tear_rate_bad = true;
    }
    if (total_torn >= 1 && total_torn * 20 >= total_near * 7)
        tear_rate_bad = true;
    if (tear_rate_bad) {
        // The tears often come from a wrapped caption or a stray prose line
        // lying inside the band, not from a phantom boundary: several visual
        // lines of one caption tear the same boundary and dominate the rate.
        // When an untorn majority still fills the band, evict the torn rows
        // to the prose flow and keep the table; the band is prose only when
        // torn rows rival the clean ones.
        int clean = 0, torn_rows = 0;
        for (bool t : row_has_tear) t ? torn_rows++ : clean++;
        if (clean >= 3 && torn_rows * 2 <= clean) {
            size_t w = 0;
            for (size_t r = 0; r < table.rows.size(); r++) {
                if (row_has_tear[r]) continue;
                if (w != r) {
                    table.rows[w] = std::move(table.rows[r]);
                    table.cell_bold[w] = std::move(table.cell_bold[r]);
                }
                w++;
            }
            table.rows.resize(w);
            table.cell_bold.resize(w);
        } else {
            table.rows.clear();
            return table;
        }
    }
    return table;
}

// Strip leading/trailing prose columns: if the leftmost or rightmost column
// has very long cells (avg > 40, many > 30 chars) while ≥2 other columns are
// short numeric-style (avg < 12), drop the prose column. Body text alongside
// a real table.
static void strip_prose_columns(TableData& table) {
    size_t stripped_bytes = 0;
    while (!table.rows.empty() && !table.rows[0].empty()) {
        int n_cols = (int)table.rows[0].size();
        if (n_cols < 3) return;
        auto col_stats = [&](int c) -> std::tuple<double,int,int> {
            double sum = 0;
            int cnt = 0;
            int long_n = 0;
            int total_rows = (int)table.rows.size();
            for (auto& row : table.rows) {
                if (c >= (int)row.size()) continue;
                if (row[c].empty()) continue;
                sum += row[c].size();
                cnt++;
                if (row[c].size() > 30) long_n++;
            }
            double avg = cnt > 0 ? sum / cnt : 0.0;
            return {avg, long_n, cnt > 0 ? (cnt * 100 / total_rows) : 0};
        };

        auto [avg_first, long_first, fill_first] = col_stats(0);
        auto [avg_last, long_last, fill_last]    = col_stats(n_cols - 1);

        // Strip a prose edge column iff it is significantly longer (avg > 2x)
        // than any non-edge column, and contains many cells > 30 chars.
        double max_other_avg = 0;
        for (int c = 1; c < n_cols - 1; c++) {
            auto [a, ln, fil] = col_stats(c);
            if (a > max_other_avg) max_other_avg = a;
        }

        bool stripped = false;
        // Left edge prose
        if (avg_first > 35 && long_first >= 3 && fill_first >= 50 &&
            max_other_avg > 0 && avg_first > max_other_avg * 1.8) {
            for (auto& row : table.rows)
                if (!row.empty()) {
                    stripped_bytes += row.front().size();
                    row.erase(row.begin());
                }
            if (!table.header_spans.empty())
                table.header_spans.erase(table.header_spans.begin());
            mask_erase_col(table, 0);
            stripped = true;
        }
        // Right edge prose (recompute n_cols if changed)
        else if (avg_last > 35 && long_last >= 3 && fill_last >= 50 &&
                 max_other_avg > 0 && avg_last > max_other_avg * 1.8) {
            for (auto& row : table.rows)
                if (!row.empty()) {
                    stripped_bytes += row.back().size();
                    row.pop_back();
                }
            if (!table.header_spans.empty()) table.header_spans.pop_back();
            mask_pop_col(table);
            stripped = true;
        }
        if (!stripped) break;
    }

    // If the stripped prose held the majority of the band's text, this was
    // never a table with body text beside it — it was prose with a column-ish
    // fringe. Reject outright so the text flows back as normal lines.
    if (stripped_bytes > 0) {
        size_t kept_bytes = 0;
        for (auto& row : table.rows)
            for (auto& c : row) kept_bytes += c.size();
        if (stripped_bytes > kept_bytes) table.rows.clear();
    }
}

// S4: rejection / cleanup heuristics.
// A figure cell: an amount, a ratio, a year or a short range ("$(55,218)",
// "2.1", "12.5%", "5 to 20", "F-2"). It holds a digit and at most a few
// letters; a cell of words is a label or prose.
static bool is_value_cell(const std::string& s) {
    if (s.empty() || s.size() > 20) return false;
    int digits = 0, letters = 0;
    for (unsigned char c : s) {
        if (c >= '0' && c <= '9') digits++;
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC2) letters++;
    }
    return digits > 0 && letters <= 4;
}

// Returns true if the table is acceptable (kept). gutter is the empty gap
// between the two columns of a 2-column candidate, in font sizes, when it
// is the widest gap of its rows (0 otherwise or for other widths). ragged:
// the lines of a 2-column candidate's second column end where their words
// end, well short of the column's right edge on a third of the rows at
// least, as a table's cells do and a column of prose does not.
static bool accept_table(TableData& table, double gutter = 0.0, int led_rows = 0,
                         bool ragged = false) {
    if (table.rows.empty()) return false;
    // pre-step: strip body-text columns adjacent to the table
    strip_prose_columns(table);
    if (table.rows.empty()) return false;

    // A display equation: the pieces of a fraction align into short rows,
    // with the equation number "(5)" alone in the last column. Rows of a
    // real table never end in a bare parenthesised number.
    if (table.rows.size() <= 5) {
        auto eq_label = [](const std::string& s) {
            if (s.size() < 3 || s.size() > 6 || s.front() != '(' || s.back() != ')') return false;
            for (size_t i = 1; i + 1 < s.size(); i++)
                if (s[i] < '0' || s[i] > '9') return false;
            return true;
        };
        for (auto& row : table.rows)
            if (!row.empty() && eq_label(row.back())) return false;
    }
    int n_cols = (int)table.rows[0].size();
    if (n_cols < 2) return false;

    // count rows with ≥2 filled cells
    int meaningful = 0;
    for (auto& row : table.rows) {
        int filled = 0;
        for (auto& c : row) if (!c.empty()) filled++;
        if (filled >= 2) meaningful++;
    }
    // Three rows is the minimum for a real multi-column table — anything
    // smaller is almost always a stray body paragraph that happened to
    // have a short token in a column-like position. 2-col tables need 4,
    // because key-value lists are easy to mistake otherwise — unless a stub
    // of labels is followed by columns of figures ("Balance as of January 1
    // | $208,840"). Prose torn by a phantom boundary leaves word fragments on
    // the right, never a column of values, so such a table is also exempt
    // from the long-label prose tests below. Two kinds of figure columns do
    // not qualify: one beside a stub that is not mostly labels (axis ticks
    // of a chart), and whole numbers that nearly always rise (the page
    // references of a contents list, which is a list rather than a table).
    // Whole numbers that nearly always rise down the rows, entries sharing
    // a page now and then: the page references of a contents list.
    auto page_refs = [&](int c) {
        int filled = 0, ints = 0, rises = 0;
        long last = -1;
        std::vector<long> seen;
        for (auto& row : table.rows) {
            if (c >= (int)row.size() || row[c].empty()) continue;
            filled++;
            char* end = nullptr;
            long v = std::strtol(row[c].c_str(), &end, 10);
            if (*end != '\0' || end == row[c].c_str()) continue;
            if (ints++ > 0 && v >= last) rises++;
            last = v;
            seen.push_back(v);
        }
        std::sort(seen.begin(), seen.end());
        size_t distinct = std::unique(seen.begin(), seen.end()) - seen.begin();
        return filled >= 3 && ints * 10 >= filled * 8 && rises * 10 >= (ints - 1) * 9 &&
               distinct * 10 >= seen.size() * 4;
    };
    // A contents list is a list, not a table: titles (with their numbers,
    // "표 I-1. | 주요국 경제성장률") beside the page references above. Data
    // tables with a rising count stay: their stub is short labels, not titles.
    if (n_cols <= 3 && page_refs(n_cols - 1)) {
        size_t title_chars = 0, titles = 0;
        for (auto& row : table.rows) {
            size_t len = 0;
            for (int c = 0; c + 1 < (int)row.size(); c++) len += row[c].size();
            if (len) { title_chars += len; titles++; }
        }
        if (titles > 0 && title_chars >= titles * 15) return false;
    }
    bool value_cols = n_cols >= 2;
    for (int c = 0; c < n_cols && value_cols; c++) {
        int filled = 0, values = 0;
        for (auto& row : table.rows) {
            if (c >= (int)row.size() || row[c].empty()) continue;
            filled++;
            if (is_value_cell(row[c])) values++;
        }
        bool figures = filled >= 2 && values * 10 >= filled * 8;
        if (c == 0 ? values * 5 > filled : (!figures || page_refs(c))) value_cols = false;
    }
    // Entries joined to their values by dot leaders (a contents list, a
    // statement's line items) are pairs by construction, not prose torn at
    // a phantom boundary: the leader is drawn to tie the two cells together.
    // When most rows are such entries the prose tests below do not apply.
    bool leader_pairs = led_rows >= 2 && led_rows * 10 >= meaningful * 6;
    int min_rows = (n_cols == 2 && !value_cols && !leader_pairs) ? 4 : 3;
    if (meaningful < min_rows) return false;

    // Merge continuation rows: row with a single filled cell in column c, after
    // a row whose column c was already filled → join with " " in same cell.
    // keep_group_headers leaves a stub label that heads the rows below it on
    // its own row (see below).
    auto merge_continuations = [n_cols](TableData& t, bool keep_group_headers) {
    for (size_t r = 1; r < t.rows.size(); r++) {
        int filled = 0;
        int filled_col = -1;
        for (int c = 0; c < n_cols; c++) {
            if (!t.rows[r][c].empty()) { filled++; filled_col = c; }
        }
        if (filled == 1 && filled_col >= 0 && r > 0 &&
            !t.rows[r-1][filled_col].empty()) {
            std::string& prev = t.rows[r-1][filled_col];
            const std::string& next = t.rows[r][filled_col];
            // A label alone in the stub column is a group header of the rows
            // below it ("Costs of revenues:", "Operating expenses:") rather
            // than the wrapped tail of the row above when it ends in a colon,
            // or when it opens with a capital after a row that is already
            // complete (its figures filled). Folding it in glued the group
            // name to the previous line item and the grouping was lost.
            if (keep_group_headers && filled_col == 0 && !next.empty()) {
                int prev_filled = 0;
                for (auto& c : t.rows[r-1]) if (!c.empty()) prev_filled++;
                bool colon = next.back() == ':';
                bool capital = next[0] >= 'A' && next[0] <= 'Z';
                if (colon || (capital && prev_filled >= 2)) continue;
            }
            // Wrapped numbers ("991225-" / "1234567") continue without a
            // space, matching get_text_in_rect.
            bool digit_wrap = prev.size() >= 2 && prev.back() == '-' &&
                              prev[prev.size() - 2] >= '0' &&
                              prev[prev.size() - 2] <= '9' &&
                              !next.empty() && next[0] >= '0' && next[0] <= '9';
            if (!digit_wrap) prev += " ";
            prev += next;
            t.rows.erase(t.rows.begin() + r);
            mask_erase_row(t, r);
            r--;
        }
    }
    };
    // Whether the band is a table is judged on its rows with every
    // continuation folded in: keeping group headers apart is a choice of
    // how an accepted table reads, and must not decide acceptance — split
    // into more, shorter rows, the lines of a chart (titles, legend entries,
    // tick labels) pass the prose tests below that they fail as wrapped
    // text. The accepted table is the one with its group headers kept.
    TableData grouped = table;
    merge_continuations(grouped, true);
    merge_continuations(table, false);
    if ((int)table.rows.size() < 2) return false;

    // Reject tables with too many empty cells (likely a degenerate band).
    {
        int total = 0, empty_n = 0;
        for (auto& row : table.rows) {
            for (auto& c : row) {
                total++;
                if (c.empty()) empty_n++;
            }
        }
        if (total > 0 && empty_n > total * 0.65) return false;
    }

    // Reject tables whose cells hold whole sentences — a band of prose that
    // was force-fit into a grid (real table cells are short values/labels).
    {
        size_t sum = 0, filled = 0, max_cell = 0;
        for (auto& row : table.rows) {
            for (auto& c : row) {
                if (c.empty()) continue;
                filled++;
                sum += c.size();
                if (c.size() > max_cell) max_cell = c.size();
            }
        }
        if (max_cell > 250) return false;
        if (filled >= 4 && sum > filled * 60) return false;
    }

    // Reject tables where most cells consist only of junk characters
    // (dashes, bullets, box-drawing). CJK/Hangul text counts as letters:
    // UTF-8 lead bytes 0xE3-0xED cover kana, CJK ideographs and Hangul,
    // while 0xE2 (general punctuation, geometric shapes, box drawing)
    // stays junk.
    {
        int total = 0, junk = 0;
        for (auto& row : table.rows) {
            for (auto& c : row) {
                if (c.empty()) continue;
                total++;
                int letters = 0, digits = 0;
                for (char ch : c) {
                    unsigned char u = (unsigned char)ch;
                    if ((u >= 'a' && u <= 'z') ||
                        (u >= 'A' && u <= 'Z')) letters++;
                    else if (u >= '0' && u <= '9') digits++;
                    else if ((u >= 0xC2 && u <= 0xDF) ||
                             (u >= 0xE3 && u <= 0xED)) letters++;
                }
                if (letters + digits == 0) junk++;
            }
        }
        if (total >= 4 && junk >= total * 0.4) return false;
    }

    // Author/affiliation blocks on title pages align into phantom columns:
    // superscript affiliation digits and daggers form their own visual row
    // ("1 | 2* | 1", "† | †† | †††") beside the author names. A row whose
    // cells are all such markers never occurs in a real data table, and
    // sinks the candidate.
    {
        int marker_rows = 0;
        for (auto& row : table.rows) {
            int filled = 0;
            bool all_marker = true, has_sym = false;
            for (auto& cell : row) {
                if (cell.empty()) continue;
                filled++;
                int units = 0;
                bool ok = true;
                for (size_t i = 0; i < cell.size(); ) {
                    unsigned char c = cell[i];
                    if (c == ' ') { i++; continue; }
                    if (c >= '0' && c <= '9') { units++; i++; continue; }
                    if (c == '*') { units++; has_sym = true; i++; continue; }
                    if (c == 0xE2 && i + 2 < cell.size() &&
                        (unsigned char)cell[i+1] == 0x80 &&
                        ((unsigned char)cell[i+2] == 0xA0 ||    // dagger
                         (unsigned char)cell[i+2] == 0xA1)) {   // double dagger
                        units++; has_sym = true; i += 3; continue;
                    }
                    // The interpunct that joins Korean author names ("권석재⋅
                    // 이정호") rides along in the marker row; it separates
                    // marks, it is not one.
                    if (c == 0xE2 && i + 2 < cell.size() &&
                        (unsigned char)cell[i+1] == 0x8B &&
                        (unsigned char)cell[i+2] == 0x85) {     // U+22C5
                        i += 3; continue;
                    }
                    if (c == 0xC2 && i + 1 < cell.size() &&
                        (unsigned char)cell[i+1] == 0xB7) {     // U+00B7
                        i += 2; continue;
                    }
                    ok = false;
                    break;
                }
                if (!ok || units == 0 || units > 4) all_marker = false;
            }
            if (filled >= 2 && all_marker && has_sym) marker_rows++;
        }
        if (marker_rows >= 1) return false;
    }

    // --- list / prose rejection heuristics (mirrored from v1, tightened) ---
    // Lowercase only: digit-to-digit boundaries are normal in data tables
    // (binary/hex value columns), not word fragments.
    auto is_latin_frag = [](unsigned char c) -> bool {
        return c >= 'a' && c <= 'z';
    };
    auto is_filler_only = [](const std::string& s) -> bool {
        for (char c : s)
            if (c != '.' && c != ',' && c != ' ' && c != '\t' &&
                c != ';' && c != ':' && c != '-' && c != '*' && c != '/')
                return false;
        return !s.empty();
    };

    // Mid-word CJK splits: a 1-char CJK cell whose left neighbor ends in
    // CJK, in a row that also holds prose-length CJK text, is a word torn
    // apart by a phantom column (fill-in-the-blank sheets, justified
    // prose). Two such cells sink the table. Legitimate 1-char columns
    // (남/여, ○/×) live in rows of short cells and are untouched.
    {
        int torn = 0;
        for (auto& row : table.rows) {
            bool has_long_cjk = false;
            for (auto& c : row)
                if (c.size() >= 20 && (unsigned char)c[0] >= 0x80)
                    has_long_cjk = true;
            if (!has_long_cjk) continue;
            for (int c = 1; c < (int)row.size(); c++) {
                if (row[c].empty() || row[c].size() > 3) continue;
                if ((unsigned char)row[c][0] < 0x80) continue;
                if (!row[c-1].empty() && (unsigned char)row[c-1].back() >= 0x80)
                    torn++;
            }
        }
        if (torn >= 2) return false;
    }

    // Two-column page layouts: the tail characters of one text column can
    // fall into a phantom 1-char column beside the other column's prose. A
    // column dominated by single CJK characters that leaves fewer than two
    // other columns is such an artifact, not a table. (Vertical label
    // columns in real tables survive: their tables keep 2+ other columns.)
    {
        int n_cols_t = (int)table.rows[0].size();
        int single_cjk_cols = 0;
        for (int c = 0; c < n_cols_t; c++) {
            int filled = 0, single_cjk = 0;
            for (auto& row : table.rows) {
                if (c >= (int)row.size() || row[c].empty()) continue;
                filled++;
                if (row[c].size() <= 3 && (unsigned char)row[c][0] >= 0x80)
                    single_cjk++;
            }
            if (filled >= 3 && single_cjk * 10 >= filled * 6)
                single_cjk_cols++;
        }
        if (single_cjk_cols > 0 && n_cols_t - single_cjk_cols < 2)
            return false;
    }

    // Exam-sheet choice rows: two or more cells in one row starting with an
    // enclosed number (①-⑳, UTF-8 E2 91/92 xx) mean answer options torn
    // into phantom columns, not tabular data.
    {
        int choice_rows = 0, content_rows = 0;
        for (auto& row : table.rows) {
            int choices = 0, filled = 0;
            for (auto& c : row) {
                if (c.empty()) continue;
                filled++;
                if (c.size() >= 3 && (unsigned char)c[0] == 0xE2 &&
                    ((unsigned char)c[1] == 0x91 || (unsigned char)c[1] == 0x92))
                    choices++;
            }
            if (filled > 0) content_rows++;
            if (choices >= 2) choice_rows++;
        }
        if (content_rows >= 2 && choice_rows * 4 >= content_rows) return false;
    }

    // Code listings: cells of C-style statements and comments (torn source
    // code with aligned inline comments).
    {
        int code_cells = 0, total = 0;
        for (auto& row : table.rows) {
            for (auto& c : row) {
                if (c.empty()) continue;
                total++;
                char last = c.back();
                bool code = last == ';' || last == '{' || last == '}' ||
                            c.rfind("//", 0) == 0 || c.rfind("*/", 0) == 0 ||
                            c.rfind("/*", 0) == 0 ||
                            (c.size() >= 2 && c.compare(c.size() - 2, 2, "*/") == 0);
                if (code) code_cells++;
            }
        }
        if (total >= 6 && code_cells * 10 >= total * 3) return false;
    }

    // Wide but ragged: real 6+ column grids are densely filled; torn prose
    // leaves scattered holes.
    {
        if (n_cols >= 6) {
            int total = 0, empty_n = 0;
            for (auto& row : table.rows)
                for (auto& c : row) {
                    total++;
                    if (c.empty()) empty_n++;
                }
            if (total > 0 && empty_n * 10 > total * 4) return false;
        }
    }

    // numbered-list detection (1) 2) 3) etc)
    {
        int marker_rows = 0, content_rows = 0;
        for (auto& row : table.rows) {
            bool has_content = false;
            for (auto& c : row) if (!c.empty()) { has_content = true; break; }
            if (!has_content) continue;
            content_rows++;
            std::string first;
            for (auto& c : row) if (!c.empty()) { first = c; break; }
            if (!first.empty() && first[0] >= '0' && first[0] <= '9') {
                for (size_t k = 1; k < first.size() && k < 4; k++) {
                    if (first[k] == ')') { marker_rows++; break; }
                    if (first[k] < '0' || first[k] > '9') break;
                }
            }
        }
        if (content_rows >= 2 && marker_rows >= content_rows * 0.6) return false;
    }
    // Numbered notes ("12 | See Arslan et al (2020).") are a list too: the
    // first column counts up by one row after row, and the second holds
    // sentences, not a value or a short label.
    if (n_cols == 2) {
        int rows_n = 0, counted = 0;
        size_t text_chars = 0;
        long prev = -1;
        for (auto& row : table.rows) {
            if (row[0].empty() || row[1].empty()) continue;
            rows_n++;
            text_chars += row[1].size();
            char* end = nullptr;
            long v = std::strtol(row[0].c_str(), &end, 10);
            if (*end == '\0' && end != row[0].c_str()) {
                if (prev >= 0 && v == prev + 1) counted++;
                prev = v;
            }
        }
        if (rows_n >= 3 && counted * 10 >= (rows_n - 1) * 8 && text_chars >= (size_t)rows_n * 25)
            return false;
    }

    // continuation rows (lower-letter ↔ lower-letter across column boundary)
    {
        int continuation_rows = 0;
        int checked_rows = 0;
        int filler_cells = 0;
        int total_cells = 0;
        int hyphen_end_cells = 0;
        for (auto& row : table.rows) {
            if ((int)row.size() < n_cols) continue;
            for (auto& cell : row) {
                if (cell.empty()) continue;
                total_cells++;
                if (is_filler_only(cell)) filler_cells++;
                // hyphen-wrap: cell ends with '-' preceded by a letter (text wrap)
                if (cell.size() >= 2 && cell.back() == '-') {
                    unsigned char prev = (unsigned char)cell[cell.size() - 2];
                    if ((prev >= 'a' && prev <= 'z') ||
                        (prev >= 'A' && prev <= 'Z') || prev >= 0x80)
                        hyphen_end_cells++;
                }
            }
            int pairs_checked = 0, pairs_continued = 0;
            for (int c = 0; c + 1 < n_cols; c++) {
                if (row[c].empty() || row[c+1].empty()) continue;
                pairs_checked++;
                unsigned char lcb = (unsigned char)row[c].back();
                unsigned char rcb = (unsigned char)row[c+1][0];
                // Latin word fragments continue across the boundary; for CJK
                // (self-contained units) only prose-length cells on BOTH
                // sides indicate split body text — short adjacent Hangul
                // cells (구분|성명 header rows) are labels, not fragments.
                bool cont = (is_latin_frag(lcb) && is_latin_frag(rcb)) ||
                            (lcb >= 0x80 && rcb >= 0x80 &&
                             row[c].size() >= 14 && row[c+1].size() >= 14);
                if (cont)
                    pairs_continued++;
            }
            if (pairs_checked > 0 && pairs_continued > pairs_checked / 2)
                continuation_rows++;
            checked_rows++;
        }
        // A glossary ("Drug | effects", "L | set of city locations") pairs
        // lowercase terms with lowercase definitions, so its rows read as
        // continued words too. Its terms are short and a wide empty gutter
        // (well beyond a stretched word space) parts them from the
        // definitions; a phantom boundary in justified prose has neither.
        // Every entry has both parts, and the definitions run longer than
        // the terms (side-by-side chart labels pair alike text) yet stay
        // shorter than a line of prose.
        bool glossary = n_cols == 2 && gutter >= 1.5;
        size_t term_chars = 0, def_chars = 0;
        for (auto& row : table.rows) {
            if (row.size() < 2 || row[0].empty() || row[1].empty() || row[0].size() > 30)
                glossary = false;
            else {
                term_chars += row[0].size();
                def_chars += row[1].size();
            }
        }
        // Definitions of a line or less each, or longer ones whose lines
        // end where their words end (ragged): a column of prose beside a
        // column of chart labels fills its lines to the right edge.
        bool short_defs = def_chars >= term_chars * 2 && def_chars <= table.rows.size() * 30;
        bool long_defs = ragged && def_chars * 2 >= term_chars * 3;
        if (!short_defs && !long_defs) glossary = false;
        double ct = (n_cols == 2) ? 0.15 : 0.30;
        if (!glossary && !leader_pairs && checked_rows >= 2 &&
            continuation_rows >= checked_rows * ct)
            return false;
        if (total_cells > 0 && filler_cells >= total_cells * 0.35)
            return false;
        // Hyphen-wrap rejection: prose tends to break words at line ends.
        // Skip this when the table is clearly numeric (>= 30% cells have digits)
        // so that real tables surrounded by prose aren't lost.
        {
            int has_digits = 0;
            for (auto& row : table.rows)
                for (auto& c : row) {
                    if (c.empty()) continue;
                    for (char ch : c) if (ch >= '0' && ch <= '9') { has_digits++; break; }
                }
            bool numeric_table = total_cells > 0 && has_digits >= total_cells * 0.30;
            if (!numeric_table && total_cells >= 4 &&
                hyphen_end_cells >= total_cells * 0.20)
                return false;
        }
    }

    // 2-column body-text heuristic. A populated third column exempts the
    // long-first-column test: prose with a stray fringe never fills three
    // columns row after row, while question/answer tables (long question,
    // short verdict columns) legitimately do.
    if (n_cols <= 3 && !leader_pairs) {
        int total_rows = 0;
        double sum_first = 0, sum_second = 0;
        int unbalanced = 0, third_filled = 0;
        for (auto& row : table.rows) {
            if (row.empty() || row[0].empty()) continue;
            total_rows++;
            sum_first += row[0].size();
            if (n_cols >= 2 && row.size() >= 2) sum_second += row[1].size();
            if (n_cols >= 3 && row.size() >= 3 && !row[2].empty()) third_filled++;
            if (n_cols == 2 && row.size() >= 2 && !row[1].empty() &&
                row[0].size() > row[1].size() * 5) unbalanced++;
        }
        if (total_rows >= 3) {
            double avg_first = sum_first / total_rows;
            double avg_second = (n_cols >= 2) ? sum_second / total_rows : 0;
            bool dense_third = n_cols >= 3 && third_filled * 10 >= total_rows * 7;
            if (!dense_third && !value_cols &&
                avg_first > 30 && avg_second > 0 && avg_first > avg_second * 2.5)
                return false;
            if (n_cols == 2 && avg_first > 30 && avg_second > 30)
                return false;
            if (!value_cols && unbalanced >= total_rows * 0.4) return false;
        }
    }

    // --- S5: numeric-cell ratio sanity check for narrow tables ---
    // If table has very few "tabular" cues (numbers, short tokens), reject —
    // *unless* col 0 is consistently a short label and the rest is description
    // (a definitions table).
    {
        int data_cells = 0;
        int numeric_cells = 0;
        int short_cells = 0;
        for (auto& row : table.rows) {
            for (auto& c : row) {
                if (c.empty()) continue;
                data_cells++;
                bool has_digit = false;
                for (char ch : c) {
                    if (ch >= '0' && ch <= '9') { has_digit = true; break; }
                }
                if (has_digit) numeric_cells++;
                if (c.size() <= 8) short_cells++;
            }
        }
        if (data_cells >= 6 && numeric_cells == 0 && n_cols >= 2) {
            int long_cells = 0;
            int short_col0 = 0, col0_filled = 0;
            for (auto& row : table.rows) {
                for (auto& c : row) if (c.size() > 30) long_cells++;
                if (!row.empty() && !row[0].empty()) {
                    col0_filled++;
                    if (row[0].size() <= 20) short_col0++;
                }
            }
            // definitions table: col 0 short labels in ≥70% of rows
            bool is_definitions = col0_filled >= 3 &&
                                  short_col0 >= col0_filled * 0.70;
            if (!is_definitions && long_cells >= data_cells * 0.3) return false;
        }
    }

    table = std::move(grouped);
    trim_table(table);
    if (table.rows.empty()) return false;
    return true;
}

// Empty gap at boundary x, in the band's rows that have text on both sides
// of it, when it is the widest gap in nearly all of them (median, else 0).
// A row whose own text has a gap at least half as wide is a grid of more
// columns than the boundary splits (an author block), not two columns.
static double clean_gutter(const std::vector<TextRow>& rows, const YBand& band, double x) {
    std::vector<double> gaps;
    std::vector<std::pair<double, double>> cr;
    int rows_both = 0;
    for (size_t k = band.first_row; k <= band.last_row; k++) {
        cr = rows[k].char_ranges;
        std::sort(cr.begin(), cr.end());
        double gutter = 0, other = 0, reach = -1e18;
        bool left = false, right = false;
        for (auto& c : cr) {
            double g = c.first - reach;
            if (reach > -1e17 && reach < x && c.first + c.second >= 2 * x) gutter = std::max(gutter, g);
            else if (reach > -1e17) other = std::max(other, g);
            (c.first + c.second < 2 * x ? left : right) = true;
            reach = std::max(reach, c.second);
        }
        if (!left || !right) continue;
        rows_both++;
        if (other < gutter * 0.5) gaps.push_back(gutter);
    }
    if (gaps.empty() || gaps.size() * 10 < (size_t)rows_both * 8) return 0.0;
    std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
    return gaps[gaps.size() / 2];
}

} // namespace text_tables

// Dot leaders ("Revenues ........ 100.0%") fill the gap between a row
// label and its first value, so no empty vertical gutter separates the
// two columns and the row reads as one cell. A run of four or more
// periods on one baseline is a leader: it carries no text and is left
// out of both the column evidence and the cell text. Two ellipsis glyphs
// in a row are one too ("……" in Hangul documents); a single one after a
// period ("B. …together") is punctuation.
// Returns one flag per character of the cache, and each leader's extent.
struct DotLeaders {
    std::vector<bool> is_leader;
    struct Span { double x0, x1, y; };
    std::vector<Span> spans;
};
static DotLeaders dot_leader_mask(const PageCharCache& cache) {
    DotLeaders out;
    std::vector<bool>& is_leader = out.is_leader;
    is_leader.assign(cache.chars.size(), false);
    std::vector<size_t> dots;
    for (size_t i = 0; i < cache.chars.size(); i++) {
        uint32_t u = cache.chars[i].unicode;
        if (u == '.' || u == 0x2024 || u == 0x2026 || u == 0x00B7) dots.push_back(i);
    }
    // Top to bottom, then left to right within a baseline. Baselines jitter
    // by fractions of a point, so the dots are cut into lines first (a gap
    // over 1pt in y) and each line is ordered by x; a comparator that
    // treated near-equal y as equal would not be a strict weak ordering.
    std::sort(dots.begin(), dots.end(), [&](size_t a, size_t b) {
        return cache.chars[a].y > cache.chars[b].y;
    });
    for (size_t lo = 0, hi; lo < dots.size(); lo = hi) {
        for (hi = lo + 1; hi < dots.size() &&
             cache.chars[dots[hi - 1]].y - cache.chars[dots[hi]].y <= 1.0; hi++) {}
        std::sort(dots.begin() + lo, dots.begin() + hi, [&](size_t a, size_t b) {
            return cache.chars[a].x < cache.chars[b].x;
        });
    }
    size_t run_start = 0;
    auto flush = [&](size_t end) {
        size_t ellipses = 0;
        double y = 0;
        for (size_t k = run_start; k < end; k++) {
            if (cache.chars[dots[k]].unicode == 0x2026) ellipses++;
            y += cache.chars[dots[k]].y;
        }
        if (end - run_start < 4 && ellipses < 2) return;
        for (size_t k = run_start; k < end; k++) is_leader[dots[k]] = true;
        out.spans.push_back({cache.chars[dots[run_start]].left,
                             cache.chars[dots[end - 1]].right,
                             y / (double)(end - run_start)});
    };
    for (size_t k = 1; k <= dots.size(); k++) {
        bool cont = false;
        if (k < dots.size()) {
            const auto& a = cache.chars[dots[k - 1]];
            const auto& b = cache.chars[dots[k]];
            double h = std::max(a.top - a.bot, 1.0);
            cont = std::abs(a.y - b.y) <= 1.0 && b.left - a.right < 1.5 * h;
        }
        if (!cont) { flush(k); run_start = k; }
    }
    return out;
}

static std::vector<TableData> detect_text_tables_range(
        const PageCharCache& cache,
        const std::vector<TableData>& existing_tables,
        double page_width, double page_height,
        double x_lo, double x_hi,
        const DotLeaders& leaders,
        double gutter_x = 0.0) {
    const std::vector<bool>& is_leader = leaders.is_leader;
    using namespace text_tables;
    if (cache.chars.size() < 10) return {};

    std::vector<CharInfo> chars;
    chars.reserve(cache.chars.size());
    for (size_t ci = 0; ci < cache.chars.size(); ci++) {
        const auto& ch = cache.chars[ci];
        if (ch.unicode == ' ' || ch.unicode == '\t' || ch.unicode == 0xA0) continue;
        if (is_leader[ci]) continue;
        if (ch.x < 0 || ch.x > page_width || ch.y < 0 || ch.y > page_height) continue;
        if (ch.x < x_lo || ch.x >= x_hi) continue;
        chars.push_back({ch.x, ch.y, ch.left, ch.right, ch.top, ch.bot,
                         ch.unicode, ch.rot, ch.is_bold});
    }
    if (chars.size() < 10) return {};

    // sort top-to-bottom (y descending in PDF coords)
    std::sort(chars.begin(), chars.end(), [](const CharInfo& a, const CharInfo& b) {
        return a.y > b.y;
    });

    // median font size (top-bot)
    std::vector<double> fsizes;
    fsizes.reserve(chars.size());
    for (auto& ch : chars) {
        double h = ch.top - ch.bot;
        if (h > 0) fsizes.push_back(h);
    }
    double median_fs = 10.0;
    if (!fsizes.empty()) {
        std::nth_element(fsizes.begin(), fsizes.begin() + fsizes.size() / 2,
                         fsizes.end());
        median_fs = fsizes[fsizes.size() / 2];
    }

    // build TextRows
    std::vector<TextRow> rows;
    {
        // Rotated runs (a vertical arXiv stamp in the margin, CAD labels)
        // contribute cell TEXT if a table stands, but must not act as column
        // evidence: a margin stamp beside a prose block otherwise fabricates
        // a phantom first column and the "table" swallows the block. Only
        // upright chars enter char_ranges (multi-cell + histogram evidence);
        // rotated chars still ride along in char_indices.
        auto push_char = [&](TextRow& r, size_t idx) {
            if (chars[idx].rot == 0)
                r.char_ranges.push_back({chars[idx].left, chars[idx].right});
            r.char_indices.push_back(idx);
        };
        TextRow cur;
        cur.y_center = chars[0].y;
        cur.y_top = chars[0].top;
        cur.y_bot = chars[0].bot;
        push_char(cur, 0);
        auto finalize_row = [](TextRow& r) {
            r.x_min = 1e18;
            r.x_max = -1e18;
            for (auto& cr : r.char_ranges) {
                if (cr.first < r.x_min) r.x_min = cr.first;
                if (cr.second > r.x_max) r.x_max = cr.second;
            }
        };
        for (size_t i = 1; i < chars.size(); i++) {
            if (std::abs(chars[i].y - cur.y_center) < std::max(median_fs * 0.4, 3.0)) {
                push_char(cur, i);
                cur.y_top = std::max(cur.y_top, chars[i].top);
                cur.y_bot = std::min(cur.y_bot, chars[i].bot);
                cur.y_center = (cur.y_center * (cur.char_indices.size() - 1) +
                                chars[i].y) / cur.char_indices.size();
            } else {
                if (!cur.char_ranges.empty()) {
                    finalize_row(cur);
                    rows.push_back(std::move(cur));
                }
                cur = TextRow();
                cur.y_center = chars[i].y;
                cur.y_top = chars[i].top;
                cur.y_bot = chars[i].bot;
                push_char(cur, i);
            }
        }
        if (!cur.char_ranges.empty()) {
            finalize_row(cur);
            rows.push_back(std::move(cur));
        }
    }
    if (rows.size() < 3) return {};

    // Attach each dot leader to its row, with the label it leads from: the
    // row's glyphs left of it, back to the value of an earlier leader on the
    // same row (contents set in two columns) — that value is the first word
    // after the earlier leader.
    for (const auto& sp : leaders.spans) {
        if (sp.x0 < x_lo || sp.x1 > x_hi) continue;
        TextRow* row = nullptr;
        double best = std::max(median_fs * 0.4, 3.0);
        for (auto& r : rows) {
            double d = std::abs(r.y_center - sp.y);
            if (d < best) { best = d; row = &r; }
        }
        if (!row) continue;
        row->leaders.push_back({sp.x0, sp.x1});
    }
    for (auto& r : rows) {
        if (r.leaders.empty()) continue;
        std::sort(r.leaders.begin(), r.leaders.end());
        auto cr = r.char_ranges;
        std::sort(cr.begin(), cr.end());
        double word_gap = std::max(median_fs * 0.3, 1.5);
        double from = -1e18;
        for (auto& ld : r.leaders) {
            double lo = 1e18, hi = -1e18;
            for (auto& c : cr)
                if (c.first >= from && c.second <= ld.first + 0.5) {
                    lo = std::min(lo, c.first);
                    hi = std::max(hi, c.second);
                }
            r.labels.push_back({lo, hi});   // lo > hi: the leader has no label
            // skip the value this leader leads to
            double reach = ld.second;
            bool started = false;
            for (auto& c : cr) {
                if (c.first < ld.second - 0.5) continue;
                if (started && c.first - reach > word_gap) break;
                started = true;
                reach = std::max(reach, c.second);
            }
            from = reach + 0.01;
        }
    }

    // multi-cell: at least one gap ≥ cell_merge_gap
    double cell_merge_gap = std::max(median_fs * 0.8, 8.0);
    for (auto& r : rows) {
        r.is_multi_cell = row_is_multi_cell(r, cell_merge_gap);
    }

    // drop rows inside an existing line-based table; x-overlap is required
    // too, or a table in one page column would erase the rows of a table
    // beside it in the other column
    auto row_in_existing = [&](const TextRow& r) {
        for (auto& t : existing_tables) {
            double tb = std::min(t.y0, t.y1) - 5.0;
            double tt = std::max(t.y0, t.y1) + 5.0;
            if (r.y_center < tb || r.y_center > tt) continue;
            double tl = std::min(t.x0, t.x1) - 5.0;
            double tr = std::max(t.x0, t.x1) + 5.0;
            if (r.x_max >= tl && r.x_min <= tr) return true;
        }
        return false;
    };
    for (auto& r : rows) {
        if (row_in_existing(r)) {
            r.is_multi_cell = false;
        }
    }

    // S1: find y-bands
    auto bands = find_y_bands(rows, chars, gutter_x <= 0);
    if (bands.empty()) return {};

    std::vector<TableData> result;
    for (auto& band : bands) {
        // Bibliography bands ("[1] Author, ..." reference lists) are justified
        // prose whose stretched word gaps mimic columns — never tables.
        {
            bool has_biblio_row = false;
            for (size_t k = band.first_row; k <= band.last_row && !has_biblio_row; k++) {
                const auto& tr = rows[k];
                size_t first_idx = SIZE_MAX, second_idx = SIZE_MAX;
                for (size_t idx : tr.char_indices) {
                    if (first_idx == SIZE_MAX || chars[idx].x < chars[first_idx].x) {
                        second_idx = first_idx;
                        first_idx = idx;
                    } else if (second_idx == SIZE_MAX || chars[idx].x < chars[second_idx].x) {
                        second_idx = idx;
                    }
                }
                if (first_idx != SIZE_MAX && second_idx != SIZE_MAX &&
                    chars[first_idx].unicode == '[' &&
                    chars[second_idx].unicode >= '0' && chars[second_idx].unicode <= '9')
                    has_biblio_row = true;
            }
            if (has_biblio_row) continue;
        }

        // S2-S4 for one set of column bounds: the gutter check, wrap-line
        // absorption, cell building and rejection. False when no table.
        auto build_band = [&](const std::vector<double>& bounds,
                              TableData& table) -> bool {
        if (bounds.size() < 3) return false;    // need ≥1 inner boundary

        // On a two-column page a full-width band whose inferred columns
        // split right at the page gutter is usually the two columns'
        // unrelated content welded side by side; the per-column retries see
        // each half on its own. So is a band whose rows all leave the gutter
        // empty, wherever its inferred columns fall (a column of equations
        // or a figure's labels moves them off it), when the three lines above
        // and below it leave it empty too: a page gutter runs on past the
        // band, while a gap between a table's columns on a one-column page
        // ends at the prose around the table (past a heading or two). A
        // genuine page-wide table
        // also straddles the gutter, but then nearly every row holds cells on
        // both sides, while welded content pairs rows only where the sides
        // happen to overlap — so only sparse straddling skips the band.
        if (gutter_x > 0) {
            bool at_gutter = false;
            for (size_t b = 1; b + 1 < bounds.size(); b++)
                if (std::abs(bounds[b] - gutter_x) < 20.0) at_gutter = true;
            bool gutter_clear = true;
            int band_rows = 0, both_sides = 0;
            for (size_t k = band.first_row; k <= band.last_row; k++) {
                if (rows[k].char_ranges.empty()) continue;
                band_rows++;
                bool left = false, right = false;
                for (auto& cr : rows[k].char_ranges) {
                    if (cr.second < gutter_x - 10.0) left = true;
                    if (cr.first > gutter_x + 10.0) right = true;
                    if (cr.first < gutter_x && cr.second > gutter_x) gutter_clear = false;
                }
                if (left && right) both_sides++;
            }
            auto crosses = [&](size_t k) {
                for (auto& cr : rows[k].char_ranges)
                    if (cr.first < gutter_x && cr.second > gutter_x) return true;
                return false;
            };
            for (size_t d = 1; d <= 3 && gutter_clear; d++) {
                if (band.first_row >= d && crosses(band.first_row - d)) gutter_clear = false;
                if (band.last_row + d < rows.size() && crosses(band.last_row + d)) gutter_clear = false;
            }
            if ((at_gutter || gutter_clear) && both_sides * 10 < band_rows * 7) return false;
        }

        // S2.5: absorb trailing wrapped cell lines — single-cell rows just
        // below the band whose text lies entirely inside one inferred column
        // (the last row's wrap continuation). Captions and prose span wider
        // than a column or sit further away, so they stay out.
        YBand ext = band;
        {
            size_t k = ext.last_row + 1;
            int absorbed = 0;
            while (k < rows.size() && absorbed < 2) {
                const auto& tr = rows[k];
                if (tr.is_multi_cell || tr.char_ranges.empty()) break;
                if (row_in_existing(tr)) break;
                const auto& prev = rows[ext.last_row];
                double line_h = std::max(prev.y_top - prev.y_bot, 8.0);
                if (std::abs(prev.y_bot - tr.y_top) > line_h * 2.0) break;
                bool inside_one_col = false;
                for (size_t c = 0; c + 1 < bounds.size(); c++) {
                    if (tr.x_min >= bounds[c] - 2.0 &&
                        tr.x_max <= bounds[c + 1] + 2.0) {
                        inside_one_col = true;
                        break;
                    }
                }
                if (!inside_one_col) break;
                ext.last_row = k;
                ext.y_bot = std::min(ext.y_bot, tr.y_bot);
                absorbed++;
                k++;
            }
        }

        // S3: build cells
        table = build_table_from_band(rows, ext, bounds, chars, median_fs);

        // S4-S5: rejection
        double gutter = bounds.size() == 3
                            ? clean_gutter(rows, ext, bounds[1]) / median_fs : 0.0;
        // Rows whose dot leader leads to a column boundary — the boundary
        // lies on the leader or in the empty space beside it (a leader set
        // in several runs, or ending short of a right-aligned value): the
        // leader itself pairs the cells on its two sides.
        int led_rows = 0;
        for (size_t k = ext.first_row; k <= ext.last_row; k++) {
            bool led = false;
            for (auto& ld : rows[k].leaders)
                for (size_t b = 1; b + 1 < bounds.size() && !led; b++) {
                    if (ld.first <= bounds[b] + 0.5 && ld.second >= bounds[b] - 0.5) {
                        led = true;
                        break;
                    }
                    // the stretch between the leader and the boundary
                    double lo = bounds[b] < ld.first ? bounds[b] : ld.second;
                    double hi = bounds[b] < ld.first ? ld.first : bounds[b];
                    bool clear = true;
                    for (auto& cr : rows[k].char_ranges)
                        if (cr.second > lo && cr.first < hi) clear = false;
                    led = clear;
                }
            if (led) led_rows++;
        }
        // Second-column lines of a two-column candidate that stop well
        // short of the column's right edge (over a quarter of its width):
        // a third of the rows or more makes the column ragged.
        bool ragged = false;
        if (bounds.size() == 3) {
            double left = 1e18, right = -1e18;
            std::vector<double> ends;
            for (size_t k = ext.first_row; k <= ext.last_row; k++) {
                double e = -1e18;
                for (auto& cr : rows[k].char_ranges)
                    if (cr.first >= bounds[1]) {
                        left = std::min(left, cr.first);
                        e = std::max(e, cr.second);
                    }
                if (e > -1e18) ends.push_back(e);
            }
            for (double e : ends) right = std::max(right, e);
            size_t shortn = 0;
            for (double e : ends)
                if (e < right - 0.25 * (right - left)) shortn++;
            ragged = ends.size() >= 3 && shortn * 3 >= ends.size();
        }
        return accept_table(table, gutter, led_rows, ragged);
        };

        // Whether the band is a table at all, and which rows it holds, is
        // decided on the columns its gaps support. Cell-stack evidence (a
        // sparse column, see infer_columns_in_band) then only re-divides
        // those rows into columns: aligned stacks are what chart legends and
        // axis ticks are made of too, so they must neither turn a band into
        // a table nor change which lines a table keeps.
        TableData table;
        auto bounds = infer_columns_in_band(rows, band, median_fs, false, chars);
        if (!build_band(bounds, table)) continue;
        auto refined = infer_columns_in_band(rows, band, median_fs, true, chars);
        if (refined != bounds) {
            TableData t2;
            auto row_text = [](const std::vector<std::string>& row) {
                std::string t;
                for (auto& c : row)
                    for (char ch : c) if (ch != ' ') t += ch;
                return t;
            };
            bool same_rows = build_band(refined, t2) &&
                             t2.rows.size() == table.rows.size();
            for (size_t r = 0; same_rows && r < t2.rows.size(); r++)
                if (row_text(t2.rows[r]) != row_text(table.rows[r]))
                    same_rows = false;
            if (same_rows) table = std::move(t2);
        }

        table.kind = TableData::TEXT;
        result.push_back(std::move(table));
    }
    return result;
}

std::vector<TableData> detect_text_tables(const PageCharCache& cache,
                                           const std::vector<TableData>& existing_tables,
                                           double page_width, double page_height,
                                           double col_boundary) {
    // Leaders are found once for the page: the per-column passes below
    // would otherwise sort every period of the page again.
    const DotLeaders leaders = dot_leader_mask(cache);
    auto result = detect_text_tables_range(cache, existing_tables,
                                           page_width, page_height,
                                           0.0, page_width, leaders,
                                           col_boundary);

    // Two-column pages: rows built across the gutter glue a column's table
    // to the prose beside it, so the full-width pass misses column-local
    // tables. Retry per column, with the full-width finds suppressing
    // duplicates.
    if (col_boundary > 0) {
        std::vector<TableData> known = existing_tables;
        known.insert(known.end(), result.begin(), result.end());
        for (int side = 0; side < 2; side++) {
            double x_lo = side == 0 ? 0.0 : col_boundary;
            double x_hi = side == 0 ? col_boundary : page_width;
            auto part = detect_text_tables_range(cache, known,
                                                 page_width, page_height,
                                                 x_lo, x_hi, leaders);
            for (auto& t : part) {
                known.push_back(t);
                result.push_back(std::move(t));
            }
        }
    }
    return result;
}

// Whether a cell reads as a header label: short and holding a letter. A
// number, a reference mark ("[39]") or a dash is what a data row is made of.
static bool header_like_cell(const std::string& s, size_t max_len) {
    if (s.empty()) return true;
    if (s.size() > max_len) return false;
    for (unsigned char c : s)
        if ((c | 0x20) - 'a' < 26u || c >= 0x80) return true;
    return false;
}

static bool header_like_row(const std::vector<std::string>& row, size_t max_len) {
    int filled = 0;
    for (auto& c : row) {
        if (!header_like_cell(c, max_len)) return false;
        if (!c.empty()) filled++;
    }
    return filled > 0;
}

void merge_header_rows(TableData& table) {
    for (int pass = 0; pass < 2; pass++) {
        if (table.rows.size() < 3) break;
        auto& r0 = table.rows[0];
        auto& r1 = table.rows[1];
        size_t n = std::min(r0.size(), r1.size());
        if (n == 0 || !header_like_row(r0, 48) || !header_like_row(r1, 30)) break;
        // r1 completes r0 when it is a units row (every cell under a label:
        // "(Acc)" under "MNLI-m"), when its few cells fill columns r0 left
        // empty ("Model" set between the header lines), or when r0's labels
        // are known spans over it. A wide sub-label row under two sparse
        // labels with no span information ("Consonant … Vowel" over 35
        // jamo) stays a row of its own: folding it would lose both labels.
        int r0_filled = 0, r1_filled = 0, overlap = 0;
        for (size_t c = 0; c < n; c++) {
            if (!r0[c].empty()) r0_filled++;
            if (!r1[c].empty()) r1_filled++;
            if (!r0[c].empty() && !r1[c].empty()) overlap++;
        }
        const bool units_row = r1[0].empty() && overlap == r1_filled;
        const bool sparse_fill = overlap == 0 && r1_filled <= r0_filled;
        if (!units_row && !sparse_fill && table.header_spans.empty()) break;

        const auto& spans = table.header_spans;
        bool spans_used = false;
        std::vector<std::string> merged(r0.size());
        for (size_t c = 0; c < r0.size(); c++) {
            std::string top = r0[c];
            if (top.empty() && c < n && !r1[c].empty() && !spans.empty()) {
                // Inherit the spanning label whose columns cover c.
                for (size_t s = c + 1; s-- > 0;) {
                    if (s < spans.size() && spans[s] > 0) {
                        if (c < s + (size_t)spans[s]) { top = r0[s]; spans_used = true; }
                        break;
                    }
                }
            }
            std::string below = (c < n) ? r1[c] : "";
            if (top.empty()) merged[c] = below;
            else if (below.empty()) merged[c] = top;
            else merged[c] = top + " " + below;
        }
        table.rows[0] = std::move(merged);
        table.rows.erase(table.rows.begin() + 1);
        mask_erase_row(table, 1);
        if (!table.cell_bold.empty())
            std::fill(table.cell_bold[0].begin(), table.cell_bold[0].end(), 0);
        if (spans_used) table.header_spans.clear();
    }
}

std::string format_table(const TableData& table) {
    if (table.rows.empty()) return "";

    // The bold mask is applied here, at the end, and only while it still
    // matches the rows it was built for.
    const bool use_mask = table.cell_bold.size() == table.rows.size();
    std::vector<std::vector<std::string>> filtered;
    for (size_t r = 0; r < table.rows.size(); r++) {
        bool all_empty = true;
        for (auto& cell : table.rows[r])
            if (!cell.empty()) { all_empty = false; break; }
        if (!all_empty || r == 0) {
            filtered.push_back(table.rows[r]);
            if (use_mask) {
                auto& row = filtered.back();
                const auto& m = table.cell_bold[r];
                for (size_t c = 0; c < row.size() && c < m.size(); c++)
                    if (m[c] && !row[c].empty()) row[c] = "**" + row[c] + "**";
            }
        }
    }
    if (filtered.empty()) return "";

    int n_cols = filtered[0].size();
    if (n_cols == 0) return "";

    return util::format_padded_markdown_table(filtered, n_cols);
}

// ── CCITTFax Decoder (lookup-table based, algorithm from ITU-T T.4/T.6) ──
// Huffman lookup tables and algorithms derived from the ITU-T T.4/T.6 standards.


}} // namespace jdoc::pdf_detail
