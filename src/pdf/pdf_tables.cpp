#include "pdf_extract.h"
#include "common/string_utils.h"
#include <algorithm>
#include <cassert>
#include <cmath>
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

    while (col_xs.size() > 9) {
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

void trim_table(TableData& table) {
    auto row_empty = [](const std::vector<std::string>& row) {
        for (auto& c : row) if (!c.empty()) return false;
        return true;
    };
    while (!table.rows.empty() && row_empty(table.rows.back()))
        table.rows.pop_back();
    while (!table.rows.empty() && row_empty(table.rows.front()))
        table.rows.erase(table.rows.begin());

    while (!table.rows.empty() && !table.rows[0].empty()) {
        int last = (int)table.rows[0].size() - 1;
        bool empty = true;
        for (auto& row : table.rows)
            if (last < (int)row.size() && !row[last].empty()) { empty = false; break; }
        if (empty) { for (auto& row : table.rows) if (!row.empty()) row.pop_back(); }
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
                       const PageCharCache& cache) {
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

    if (col_xs.size() < 3) {
        table.rows.clear();
        return table;
    }

    std::vector<double> actual_ys;
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
        auto text_row_ys = cluster_values(cy_vals, row_h * 0.4);

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
        double tol = row_h * 0.4;
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
        int rows_in_grid = 0;
        for (double tc : table_row_centers)
            if (tc >= tb - row_h * 0.5 && tc <= tt + row_h * 0.5)
                rows_in_grid++;

        int n_rows_expected = (int)row_ys.size() - 1;
        bool use_text_rows = (rows_in_grid < n_rows_expected * 0.9) &&
                             ((int)table_row_centers.size() >= n_rows_expected * 0.8);

        if (use_text_rows && !table_row_centers.empty()) {
            std::sort(table_row_centers.begin(), table_row_centers.end());
            double half = row_h / 2.0;
            // Clamp to h-line grid boundaries — don't extend beyond the table
            double grid_top = row_ys.back() + half;
            double grid_bot = row_ys.front() - half;
            actual_ys.push_back(std::max(table_row_centers.front() - half, grid_bot));
            for (size_t i = 0; i < table_row_centers.size() - 1; i++)
                actual_ys.push_back((table_row_centers[i] + table_row_centers[i + 1]) / 2.0);
            actual_ys.push_back(std::min(table_row_centers.back() + half, grid_top));
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

    table.rows.resize(n_rows);
    for (int r = 0; r < n_rows; r++) {
        table.rows[r].resize(total_cols);
        int c = 0;
        while (c < n_cols) {
            // Determine span: extend while no v-line at next boundary
            int span = 1;
            while (c + span < n_cols && !has_vline[r][c + span])
                span++;

            double left   = col_xs[c];
            double right  = col_xs[c + span];
            double bottom = actual_ys[r];
            double top    = actual_ys[r + 1];

            if (c + span - 1 == last_col_idx && n_sub > 1 && span == 1) {
                if (is_scale_row(cache, left, right, bottom, top, sub_boundaries)) {
                    for (int sc = 0; sc < n_sub; sc++) {
                        table.rows[r][n_cols - 1 + sc] = cache.get_text_in_rect(
                            sub_boundaries[sc], top, sub_boundaries[sc+1], bottom);
                    }
                } else {
                    table.rows[r][c] = cache.get_text_in_rect(left, top, right, bottom);
                }
            } else {
                table.rows[r][c] = cache.get_text_in_rect(left, top, right, bottom);
            }
            c += span;
        }
    }

    std::reverse(table.rows.begin(), table.rows.end());
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
        }
    }

    int meaningful_rows = 0;
    for (auto& row : table.rows) {
        int filled_cols = 0;
        for (auto& cell : row) if (!cell.empty()) filled_cols++;
        if (filled_cols >= 2) meaningful_rows++;
    }
    if (meaningful_rows < 3) table.rows.clear();

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
    // SKIP when merged cells detected: v-line grid confirms real table structure.
    if (!table.rows.empty() && !has_merged_cells) {
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
            if (checked >= 3 && cont_rows >= checked * 0.15)
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
    if (!table.rows.empty() && (int)table.rows.size() >= 3) {
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
    double gap_start = 0;
    for (int b = 0; b < n_bins; b++) {
        double bx = left + b * bin_w + bin_w / 2.0;
        if (gap_counts[b] >= threshold) {
            if (!in_gap) { gap_start = bx; in_gap = true; }
        } else {
            if (in_gap) {
                double gap_center = (gap_start + bx) / 2.0;
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

std::vector<TableData> detect_tables(const std::vector<PdfLineSegment>& lines,
                                      const PageCharCache& cache,
                                      double page_width, double page_height) {
    if (lines.size() < 4) return {};

    std::vector<PdfLineSegment> h_lines, v_lines;
    for (auto& l : lines) {
        if (l.is_horizontal()) {
            double y = (l.y0 + l.y1) / 2.0;
            if (y < 0 || y > page_height) continue;
            double lx = std::min((double)l.x0, (double)l.x1);
            double rx = std::max((double)l.x0, (double)l.x1);
            if (lx < -10 || rx > page_width + 10) continue;
            if (rx - lx < 50.0) continue;
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

    std::vector<double> h_ys;
    for (auto& hl : h_lines) h_ys.push_back((hl.y0 + hl.y1) / 2.0);
    auto row_ys = cluster_values(h_ys, 3.0);
    if (row_ys.size() < 3) return {};

    int n_levels = (int)row_ys.size();

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

    std::vector<std::vector<double>> table_groups;
    std::vector<double> current_group;
    int group_vline_connections = 0;
    current_group.push_back(row_ys[0]);
    for (int i = 0; i < n_levels - 1; i++) {
        double gap = row_ys[i + 1] - row_ys[i];
        bool close_enough = gap < 200.0;
        // Include row if v-line connected, h-lines share span,
        // or h-lines share span with a small gap (merged-cell rows in forms)
        bool h_span_ok = x_overlap[i] && close_enough;
        if (connected[i] || h_span_ok) {
            current_group.push_back(row_ys[i + 1]);
            if (connected[i]) group_vline_connections++;
        } else if (close_enough && group_vline_connections > 0) {
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
            if (extent > 50 && overlap >= extent * 0.7) {
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
            if (extent > 50 && overlap >= extent * 0.7 && y_gap < 100) {
                for (auto& y : table_groups[g])
                    merged.back().push_back(y);
            } else {
                merged.push_back(table_groups[g]);
            }
        }
        table_groups = std::move(merged);
    }

    if (table_groups.empty()) return {};

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

    std::vector<TableData> result;
    for (auto& group : final_groups) {
        TableData t = build_table(group, h_lines, v_lines, cache);
        if (t.rows.empty()) continue;
        // Reject grids that swallowed page prose (stacked separate tables
        // bridged across body text): no real cell holds a whole paragraph.
        // Rejecting lets the band's lines flow back as normal text.
        size_t max_cell = 0;
        for (auto& row : t.rows)
            for (auto& c : row)
                if (c.size() > max_cell) max_cell = c.size();
        if (max_cell > 300) continue;
        result.push_back(std::move(t));
    }

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
        }
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

struct CharInfo { double x, y, left, right, top, bot; unsigned int unicode; };

struct TextRow {
    double y_center;
    double y_top, y_bot;
    std::vector<std::pair<double,double>> char_ranges;   // per-char [left,right]
    std::vector<size_t> char_indices;                    // indices into chars
    double x_min, x_max;
    bool is_multi_cell;
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

// S1: find y-bands of consecutive multi-cell rows (with bounded 1-cell rows)
static std::vector<YBand> find_y_bands(const std::vector<TextRow>& rows) {
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
static std::vector<double> infer_columns_in_band(
        const std::vector<TextRow>& rows, const YBand& band,
        double median_fs) {
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

    std::vector<double> col_edges;
    for (auto& run : empty_runs) {
        double width = run.second - run.first;
        if (width < col_gap_min) continue;
        double mid = (run.first + run.second) / 2.0;
        // skip runs hugging the band edges (those are just margins)
        if (mid <= x_lo + 2.0) continue;
        if (mid >= x_hi - 2.0) continue;
        col_edges.push_back(mid);
    }
    if (col_edges.empty()) return {};

    // Validate: for each candidate boundary, ≥70% of multi-cell rows that
    // overlap a neighborhood of the boundary must "straddle" it (chars on
    // both sides). Rows that have no chars near the boundary are ignored — a
    // row of body text on the opposite side of the page does not invalidate
    // a column boundary inside a data table.
    std::vector<double> kept;
    double neigh = std::max(median_fs * 6.0, 60.0);
    for (double e : col_edges) {
        int agree = 0;
        int relevant = 0;
        for (size_t ri : mc) {
            bool has_left = false, has_right = false;
            bool near = false;
            for (auto& cr : rows[ri].char_ranges) {
                if (cr.second <= e) has_left = true;
                else if (cr.first >= e) has_right = true;
                if (cr.first <= e + neigh && cr.second >= e - neigh) near = true;
                if (has_left && has_right && near) break;
            }
            if (!near) continue;
            relevant++;
            if (has_left && has_right) agree++;
        }
        int needed = std::max(2, (int)std::ceil(relevant * 0.70));
        if (relevant >= 2 && agree >= needed) kept.push_back(e);
    }
    if (kept.empty()) return {};

    std::vector<double> bounds;
    bounds.push_back(x_lo);
    for (double e : kept) bounds.push_back(e);
    bounds.push_back(x_hi);
    return bounds;
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
        // Ensure monotonic
        for (int c = 1; c < (int)row_bounds.size(); c++) {
            if (row_bounds[c] < row_bounds[c-1] + 0.1)
                row_bounds[c] = row_bounds[c-1] + 0.1;
        }

        std::vector<std::string> cells(n_cols);
        std::vector<double> last_right(n_cols, -1e9);

        for (size_t idx : ci) {
            const auto& ch = chars[idx];
            double cmid = (ch.left + ch.right) / 2.0;
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
            if (!cells[col].empty() && (ch.left - last_right[col]) >= word_gap)
                cells[col] += ' ';
            util::append_utf8(cells[col], ch.unicode);
            last_right[col] = ch.right;
        }

        for (auto& c : cells) c = util::trim(c);
        table.rows.push_back(std::move(cells));
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
// Returns true if the table is acceptable (kept).
static bool accept_table(TableData& table) {
    if (table.rows.empty()) return false;
    // pre-step: strip body-text columns adjacent to the table
    strip_prose_columns(table);
    if (table.rows.empty()) return false;
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
    // have a short token in a column-like position. (2-col tables stay at 4
    // because key-value lists are easy to mistake otherwise.)
    int min_rows = (n_cols == 2) ? 4 : 3;
    if (meaningful < min_rows) return false;

    // Merge continuation rows: row with a single filled cell in column c, after
    // a row whose column c was already filled → join with " " in same cell.
    for (size_t r = 1; r < table.rows.size(); r++) {
        int filled = 0;
        int filled_col = -1;
        for (int c = 0; c < n_cols; c++) {
            if (!table.rows[r][c].empty()) { filled++; filled_col = c; }
        }
        if (filled == 1 && filled_col >= 0 && r > 0 &&
            !table.rows[r-1][filled_col].empty()) {
            table.rows[r-1][filled_col] += " ";
            table.rows[r-1][filled_col] += table.rows[r][filled_col];
            table.rows.erase(table.rows.begin() + r);
            r--;
        }
    }
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

    // Reject tables where most cells consist only of non-ASCII junk characters.
    {
        int total = 0, junk = 0;
        for (auto& row : table.rows) {
            for (auto& c : row) {
                if (c.empty()) continue;
                total++;
                int letters = 0, digits = 0;
                for (char ch : c) {
                    if ((ch >= 'a' && ch <= 'z') ||
                        (ch >= 'A' && ch <= 'Z')) letters++;
                    else if (ch >= '0' && ch <= '9') digits++;
                }
                if (letters + digits == 0) junk++;
            }
        }
        if (total >= 4 && junk >= total * 0.4) return false;
    }

    // --- list / prose rejection heuristics (mirrored from v1, tightened) ---
    auto is_lower_or_multibyte = [](unsigned char c) -> bool {
        return (c >= 'a' && c <= 'z') || c >= 0x80;
    };
    auto is_filler_only = [](const std::string& s) -> bool {
        for (char c : s)
            if (c != '.' && c != ',' && c != ' ' && c != '\t' &&
                c != ';' && c != ':' && c != '-')
                return false;
        return !s.empty();
    };

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
                if (is_lower_or_multibyte((unsigned char)row[c].back()) &&
                    is_lower_or_multibyte((unsigned char)row[c+1][0]))
                    pairs_continued++;
            }
            if (pairs_checked > 0 && pairs_continued > pairs_checked / 2)
                continuation_rows++;
            checked_rows++;
        }
        double ct = (n_cols == 2) ? 0.15 : 0.30;
        if (checked_rows >= 2 && continuation_rows >= checked_rows * ct)
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

    // 2-column body-text heuristic
    if (n_cols <= 3) {
        int total_rows = 0;
        double sum_first = 0, sum_second = 0;
        int unbalanced = 0;
        for (auto& row : table.rows) {
            if (row.empty() || row[0].empty()) continue;
            total_rows++;
            sum_first += row[0].size();
            if (n_cols >= 2 && row.size() >= 2) sum_second += row[1].size();
            if (n_cols == 2 && row.size() >= 2 && !row[1].empty() &&
                row[0].size() > row[1].size() * 5) unbalanced++;
        }
        if (total_rows >= 3) {
            double avg_first = sum_first / total_rows;
            double avg_second = (n_cols >= 2) ? sum_second / total_rows : 0;
            if (avg_first > 30 && avg_second > 0 && avg_first > avg_second * 2.5)
                return false;
            if (n_cols == 2 && avg_first > 30 && avg_second > 30)
                return false;
            if (unbalanced >= total_rows * 0.4) return false;
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

    trim_table(table);
    if (table.rows.empty()) return false;
    return true;
}

} // namespace text_tables

std::vector<TableData> detect_text_tables(const PageCharCache& cache,
                                           const std::vector<TableData>& existing_tables,
                                           double page_width, double page_height) {
    using namespace text_tables;
    if (cache.chars.size() < 10) return {};

    std::vector<CharInfo> chars;
    chars.reserve(cache.chars.size());
    for (auto& ch : cache.chars) {
        if (ch.unicode == ' ' || ch.unicode == '\t' || ch.unicode == 0xA0) continue;
        if (ch.x < 0 || ch.x > page_width || ch.y < 0 || ch.y > page_height) continue;
        chars.push_back({ch.x, ch.y, ch.left, ch.right, ch.top, ch.bot,
                         ch.unicode});
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
        TextRow cur;
        cur.y_center = chars[0].y;
        cur.y_top = chars[0].top;
        cur.y_bot = chars[0].bot;
        cur.char_ranges.push_back({chars[0].left, chars[0].right});
        cur.char_indices.push_back(0);
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
                cur.char_ranges.push_back({chars[i].left, chars[i].right});
                cur.char_indices.push_back(i);
                cur.y_top = std::max(cur.y_top, chars[i].top);
                cur.y_bot = std::min(cur.y_bot, chars[i].bot);
                cur.y_center = (cur.y_center * (cur.char_ranges.size() - 1) +
                                chars[i].y) / cur.char_ranges.size();
            } else {
                if (!cur.char_ranges.empty()) {
                    finalize_row(cur);
                    rows.push_back(std::move(cur));
                }
                cur = TextRow();
                cur.y_center = chars[i].y;
                cur.y_top = chars[i].top;
                cur.y_bot = chars[i].bot;
                cur.char_ranges.push_back({chars[i].left, chars[i].right});
                cur.char_indices.push_back(i);
            }
        }
        if (!cur.char_ranges.empty()) {
            finalize_row(cur);
            rows.push_back(std::move(cur));
        }
    }
    if (rows.size() < 3) return {};

    // multi-cell: at least one gap ≥ cell_merge_gap
    double cell_merge_gap = std::max(median_fs * 0.8, 8.0);
    for (auto& r : rows) {
        r.is_multi_cell = row_is_multi_cell(r, cell_merge_gap);
    }

    // drop rows entirely inside an existing line-based table
    auto row_in_existing = [&](const TextRow& r) {
        for (auto& t : existing_tables) {
            double tb = std::min(t.y0, t.y1) - 5.0;
            double tt = std::max(t.y0, t.y1) + 5.0;
            if (r.y_center >= tb && r.y_center <= tt) return true;
        }
        return false;
    };
    for (auto& r : rows) {
        if (row_in_existing(r)) {
            r.is_multi_cell = false;
        }
    }

    // S1: find y-bands
    auto bands = find_y_bands(rows);
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

        // S2: infer columns
        auto bounds = infer_columns_in_band(rows, band, median_fs);
        if (bounds.size() < 3) continue;        // need ≥1 inner boundary

        // S3: build cells
        TableData table = build_table_from_band(rows, band, bounds, chars,
                                                median_fs);

        // S4-S5: rejection
        if (!accept_table(table)) continue;

        result.push_back(std::move(table));
    }
    return result;
}

std::string format_table(const TableData& table) {
    if (table.rows.empty()) return "";

    std::vector<std::vector<std::string>> filtered;
    for (size_t r = 0; r < table.rows.size(); r++) {
        bool all_empty = true;
        for (auto& cell : table.rows[r])
            if (!cell.empty()) { all_empty = false; break; }
        if (!all_empty || r == 0)
            filtered.push_back(table.rows[r]);
    }
    if (filtered.empty()) return "";

    int n_cols = filtered[0].size();
    if (n_cols == 0) return "";

    std::vector<size_t> widths(n_cols, 3);
    for (auto& row : filtered)
        for (int c = 0; c < n_cols && c < (int)row.size(); c++)
            widths[c] = std::max(widths[c], row[c].size());

    std::string md;
    for (size_t r = 0; r < filtered.size(); r++) {
        md += "|";
        for (int c = 0; c < n_cols; c++) {
            std::string cell = (c < (int)filtered[r].size()) ? filtered[r][c] : "";
            md += " " + cell;
            for (size_t p = cell.size(); p < widths[c]; p++) md += ' ';
            md += " |";
        }
        md += '\n';

        if (r == 0) {
            md += "|";
            for (int c = 0; c < n_cols; c++) {
                md += " ";
                for (size_t p = 0; p < widths[c]; p++) md += '-';
                md += " |";
            }
            md += '\n';
        }
    }
    return md;
}

// ── CCITTFax Decoder (lookup-table based, algorithm from ITU-T T.4/T.6) ──
// Huffman lookup tables and algorithms derived from the ITU-T T.4/T.6 standards.


}} // namespace jdoc::pdf_detail
