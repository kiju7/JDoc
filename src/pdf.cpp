// pdf2md.cpp — PDF→Markdown using PDFium (BSD-3 license)
// Features: text, headings, bold/italic, images, table detection (path + text clustering)

#include "jdoc/pdf.h"
#include "common/string_utils.h"
#include "common/file_utils.h"

#include <fpdfview.h>
#include <fpdf_text.h>
#include <fpdf_edit.h>

#include <iostream>
#include <fstream>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>

namespace jdoc {
namespace {

// ── PDFium library lifecycle ─────────────────────────────

struct PdfiumLibrary {
    PdfiumLibrary() { FPDF_InitLibrary(); }
    ~PdfiumLibrary() { FPDF_DestroyLibrary(); }
};

void ensure_pdfium_initialized() {
    static PdfiumLibrary instance;
}

// ── PDF-specific types (only used in this file) ─────────

struct TableData {
    std::vector<std::vector<std::string>> rows;
    double x0, y0, x1, y1;
    int page = 0;
};

// ── Internal data structures ─────────────────────────────

struct TextLine {
    std::string text;
    double font_size = 0;
    bool is_bold = false;
    bool is_italic = false;
    double y_center = 0;
    double x_left = 1e9;
    double x_right = 0;
};

struct PdfLineSegment {
    float x0, y0, x1, y1;
    bool is_horizontal() const { return std::abs(y1 - y0) < 2.0f; }
    bool is_vertical()   const { return std::abs(x1 - x0) < 2.0f; }
};

struct FontStats {
    double body_size = 12.0;

    void compute(const std::vector<std::vector<TextLine>>& all_lines) {
        std::map<int, int> counts;
        for (auto& pl : all_lines)
            for (auto& l : pl)
                if (l.font_size > 1.0)
                    counts[(int)(l.font_size * 10)]++;

        int max_c = 0, max_k = 120;
        for (auto& [k, c] : counts)
            if (c > max_c) { max_c = c; max_k = k; }
        body_size = max_k / 10.0;
        if (body_size < 4.0) body_size = 12.0;
    }

    int heading_level(double fs) const {
        if (fs <= 0) return 0;
        double r = fs / body_size;
        if (r >= 1.8) return 1;
        if (r >= 1.5) return 2;
        if (r >= 1.3) return 3;
        if (r >= 1.15) return 4;
        return 0;
    }
};

// ── Font helpers ─────────────────────────────────────────

bool check_bold(const std::string& name, int flags) {
    if (flags & 0x40000) return true;
    std::string lower;
    for (char c : name) lower += std::tolower((unsigned char)c);
    return lower.find("bold") != std::string::npos ||
           lower.find("heavy") != std::string::npos ||
           lower.find("black") != std::string::npos;
}

bool check_italic(const std::string& name, int flags) {
    if (flags & 0x40) return true;
    std::string lower;
    for (char c : name) lower += std::tolower((unsigned char)c);
    return lower.find("italic") != std::string::npos ||
           lower.find("oblique") != std::string::npos;
}

// ── Extract text lines ───────────────────────────────────

std::vector<TextLine> extract_lines(FPDF_TEXTPAGE text_page) {
    int count = FPDFText_CountChars(text_page);
    if (count <= 0) return {};

    std::vector<TextLine> lines;
    TextLine current;
    std::vector<int> line_char_indices;
    int first_text_char = -1;

    auto flush_line = [&]() {
        if (current.text.empty()) return;

        if (!line_char_indices.empty()) {
            double total_h = 0, total_y = 0;
            int valid = 0;
            for (int ci : line_char_indices) {
                double l, r, b, t;
                if (FPDFText_GetCharBox(text_page, ci, &l, &r, &b, &t)) {
                    double h = std::abs(t - b);
                    if (h > 1.0) {
                        total_h += h;
                        total_y += (t + b) / 2.0;
                        valid++;
                    }
                    if (l < current.x_left) current.x_left = l;
                    if (r > current.x_right) current.x_right = r;
                }
            }
            if (valid > 0) {
                current.font_size = total_h / valid;
                current.y_center = total_y / valid;
            }
        }

        if (first_text_char >= 0) {
            char buf[256];
            int flags = 0;
            unsigned long len = FPDFText_GetFontInfo(text_page, first_text_char,
                                                     buf, sizeof(buf), &flags);
            std::string fname;
            if (len > 0) fname = std::string(buf, len - 1);
            current.is_bold = check_bold(fname, flags);
            current.is_italic = check_italic(fname, flags);
        }

        size_t end = current.text.find_last_not_of(" \t");
        if (end != std::string::npos) current.text.resize(end + 1);

        if (!current.text.empty())
            lines.push_back(std::move(current));

        current = TextLine{};
        line_char_indices.clear();
        first_text_char = -1;
    };

    for (int i = 0; i < count; i++) {
        unsigned int u = FPDFText_GetUnicode(text_page, i);
        if (u == 0) continue;

        if (u == '\r' || u == '\n') {
            if (u == '\r' && i + 1 < count && FPDFText_GetUnicode(text_page, i + 1) == '\n')
                i++;
            flush_line();
            continue;
        }

        if (u != 0xFFFD) {
            if (u != ' ' && u != '\t' && u != 0xA0) {
                if (first_text_char < 0) first_text_char = i;
                line_char_indices.push_back(i);
            }
            util::append_utf8(current.text, u);
        }
    }

    flush_line();
    return lines;
}

// ── Path line segment extraction ─────────────────────────

std::vector<PdfLineSegment> extract_line_segments(FPDF_PAGE page) {
    std::vector<PdfLineSegment> lines;
    int obj_count = FPDFPage_CountObjects(page);

    for (int i = 0; i < obj_count; i++) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH) continue;

        int seg_count = FPDFPath_CountSegments(obj);
        if (seg_count < 2) continue;

        float prev_x = 0, prev_y = 0;
        for (int s = 0; s < seg_count; s++) {
            FPDF_PATHSEGMENT seg = FPDFPath_GetPathSegment(obj, s);
            if (!seg) continue;

            float x, y;
            FPDFPathSegment_GetPoint(seg, &x, &y);
            int type = FPDFPathSegment_GetType(seg);

            if (type == FPDF_SEGMENT_LINETO && s > 0) {
                PdfLineSegment ls;
                ls.x0 = prev_x; ls.y0 = prev_y;
                ls.x1 = x;      ls.y1 = y;
                if (ls.is_horizontal() || ls.is_vertical()) {
                    lines.push_back(ls);
                }
            }
            prev_x = x; prev_y = y;
        }
    }
    return lines;
}

// ── Table detection helpers ──────────────────────────────

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

std::string get_bounded_text(FPDF_TEXTPAGE text_page,
                              double left, double top, double right, double bottom) {
    double cell_top = std::max(top, bottom);
    double cell_bottom = std::min(top, bottom);
    int buf_len = FPDFText_GetBoundedText(text_page, left + 1, cell_top - 1,
                                           right - 1, cell_bottom + 1, nullptr, 0);
    if (buf_len <= 0) return "";

    std::vector<unsigned short> buf(buf_len + 1, 0);
    FPDFText_GetBoundedText(text_page, left + 1, cell_top - 1,
                            right - 1, cell_bottom + 1, buf.data(), buf_len + 1);
    std::string text;
    for (int k = 0; k < buf_len; k++) {
        unsigned int cp = buf[k];
        if (cp == 0) break;
        if (cp == '\r' || cp == '\n') { text += ' '; continue; }
        util::append_utf8(text, cp);
    }
    size_t s = text.find_first_not_of(" ");
    size_t e = text.find_last_not_of(" ");
    if (s != std::string::npos) return text.substr(s, e - s + 1);
    return "";
}

std::vector<double> detect_response_boundaries(FPDF_TEXTPAGE text_page,
                                                double left, double right,
                                                const std::vector<double>& row_ys) {
    double width = right - left;
    if (width < 150) return {};

    int n_rows = (int)row_ys.size() - 1;
    if (n_rows < 2) return {};

    int total_chars = FPDFText_CountChars(text_page);

    struct RowChars { std::vector<double> xs; };
    std::vector<RowChars> per_row(n_rows);

    for (int ci = 0; ci < total_chars; ci++) {
        unsigned int u = FPDFText_GetUnicode(text_page, ci);
        if (u == 0 || u == '\r' || u == '\n' || u == 0xFFFD || u == ' ' || u == 0xA0)
            continue;

        double cl, cr, cb, ct;
        if (!FPDFText_GetCharBox(text_page, ci, &cl, &cr, &cb, &ct)) continue;
        double cx = (cl + cr) / 2.0;
        double cy = (ct + cb) / 2.0;

        if (cx < left + 1 || cx > right - 1) continue;

        for (int r = 0; r < n_rows; r++) {
            double bot = std::min(row_ys[r], row_ys[r+1]);
            double top = std::max(row_ys[r], row_ys[r+1]);
            if (cy >= bot + 1 && cy <= top - 1) {
                per_row[r].xs.push_back(cx);
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
        double best_var = 1e9;
        std::vector<double> best_set;
        int n = (int)stable_xs.size();

        for (int a = 0; a < n-4; a++)
        for (int b = a+1; b < n-3; b++)
        for (int c = b+1; c < n-2; c++)
        for (int d = c+1; d < n-1; d++)
        for (int e = d+1; e < n; e++) {
            double xs[5] = {stable_xs[a], stable_xs[b], stable_xs[c],
                            stable_xs[d], stable_xs[e]};
            double gaps[4];
            double avg = 0;
            for (int i = 0; i < 4; i++) { gaps[i] = xs[i+1] - xs[i]; avg += gaps[i]; }
            avg /= 4;
            double var = 0;
            for (int i = 0; i < 4; i++) var += (gaps[i] - avg) * (gaps[i] - avg);

            int min_hits = 999;
            for (int i = 0; i < 5; i++) {
                int hits = 0;
                for (double c : all_centers)
                    if (std::abs(c - xs[i]) < 15.0) hits++;
                if (hits < min_hits) min_hits = hits;
            }

            if (min_hits >= 2 && var < best_var) {
                best_var = var;
                best_set = {xs[0], xs[1], xs[2], xs[3], xs[4]};
            }
        }

        if (!best_set.empty() && best_var < 200.0) {
            stable_xs = best_set;
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

bool is_scale_row(FPDF_TEXTPAGE text_page, double left, double right,
                   double bot, double top, const std::vector<double>& boundaries) {
    int n_sub = (int)boundaries.size() - 1;
    if (n_sub < 3) return false;

    int filled = 0, total_len = 0, max_len = 0;
    for (int sc = 0; sc < n_sub; sc++) {
        std::string t = get_bounded_text(text_page, boundaries[sc], top, boundaries[sc+1], bot);
        int len = (int)t.size();
        if (len > 0) { filled++; total_len += len; }
        if (len > max_len) max_len = len;
    }

    return filled >= 3 && max_len <= 40 && total_len <= 80;
}

std::vector<double> find_column_boundaries(
        const std::vector<PdfLineSegment>& v_lines,
        double table_left, double table_right,
        double table_bot, double table_top) {
    double table_height = table_top - table_bot;

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

    struct VLineInfo { double x, coverage; };
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
        std::sort(intervals.begin(), intervals.end());
        double total = 0, cur_lo = intervals[0].first, cur_hi = intervals[0].second;
        for (size_t i = 1; i < intervals.size(); i++) {
            if (intervals[i].first <= cur_hi + 3.0)
                cur_hi = std::max(cur_hi, intervals[i].second);
            else { total += cur_hi - cur_lo; cur_lo = intervals[i].first; cur_hi = intervals[i].second; }
        }
        total += cur_hi - cur_lo;
        vline_infos.push_back({cx, total});
    }

    std::vector<double> col_xs;
    col_xs.push_back(table_left);
    for (auto& vi : vline_infos)
        if (vi.coverage >= table_height * 0.4 &&
            vi.x > table_left + 5 && vi.x < table_right - 5)
            col_xs.push_back(vi.x);
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
                for (auto& vi : vline_infos)
                    if (std::abs(vi.x - col_xs[i]) < 6.0) { cov = vi.coverage; break; }
                if (cov >= table_height * 0.8)
                    merged.push_back(col_xs[i]);
            } else {
                merged.push_back(col_xs[i]);
            }
        }
        merged.push_back(col_xs.back());
        col_xs = std::move(merged);
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

TableData build_table(const std::vector<double>& row_ys,
                       const std::vector<PdfLineSegment>& h_lines,
                       const std::vector<PdfLineSegment>& v_lines,
                       FPDF_TEXTPAGE text_page) {
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

    auto col_xs = find_column_boundaries(v_lines, table_left, table_right,
                                          table_bot, table_top);

    if (col_xs.size() < 3) {
        table.rows.clear();
        return table;
    }

    int n_rows = (int)row_ys.size() - 1;
    int n_cols = (int)col_xs.size() - 1;

    table.x0 = col_xs.front();
    table.y0 = row_ys.front();
    table.x1 = col_xs.back();
    table.y1 = row_ys.back();

    int last_col_idx = n_cols - 1;
    auto sub_boundaries = detect_response_boundaries(text_page,
        col_xs[last_col_idx], col_xs[last_col_idx + 1], row_ys);
    int n_sub = sub_boundaries.empty() ? 0 : (int)sub_boundaries.size() - 1;
    int total_cols = (n_sub > 1) ? (n_cols - 1 + n_sub) : n_cols;

    table.rows.resize(n_rows);
    for (int r = 0; r < n_rows; r++) {
        table.rows[r].resize(total_cols);
        for (int c = 0; c < n_cols; c++) {
            double left   = col_xs[c];
            double right  = col_xs[c + 1];
            double bottom = row_ys[r];
            double top    = row_ys[r + 1];

            if (c == last_col_idx && n_sub > 1) {
                if (is_scale_row(text_page, left, right, bottom, top, sub_boundaries)) {
                    for (int sc = 0; sc < n_sub; sc++) {
                        table.rows[r][n_cols - 1 + sc] = get_bounded_text(
                            text_page, sub_boundaries[sc], top, sub_boundaries[sc+1], bottom);
                    }
                } else {
                    table.rows[r][n_cols - 1] = get_bounded_text(text_page, left, top, right, bottom);
                }
            } else {
                table.rows[r][c] = get_bounded_text(text_page, left, top, right, bottom);
            }
        }
    }

    std::reverse(table.rows.begin(), table.rows.end());
    trim_table(table);

    int non_empty = 0;
    for (auto& row : table.rows)
        for (auto& cell : row) if (!cell.empty()) non_empty++;
    if (non_empty < 3) table.rows.clear();

    return table;
}

std::vector<TableData> detect_tables(const std::vector<PdfLineSegment>& lines,
                                      FPDF_TEXTPAGE text_page,
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
            if (rx - lx < 15.0) continue;
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
        for (auto& vl : v_lines) {
            double vy_lo = std::min((double)vl.y0, (double)vl.y1);
            double vy_hi = std::max((double)vl.y0, (double)vl.y1);
            if (vy_lo <= y_lo + 5.0 && vy_hi >= y_hi - 5.0) {
                connected[i] = true;
                break;
            }
        }
    }

    std::vector<std::vector<double>> table_groups;
    std::vector<double> current_group;
    current_group.push_back(row_ys[0]);
    for (int i = 0; i < n_levels - 1; i++) {
        if (connected[i]) {
            current_group.push_back(row_ys[i + 1]);
        } else {
            if (current_group.size() >= 3)
                table_groups.push_back(current_group);
            current_group.clear();
            current_group.push_back(row_ys[i + 1]);
        }
    }
    if (current_group.size() >= 3)
        table_groups.push_back(current_group);

    if (table_groups.empty()) return {};

    std::vector<TableData> result;
    for (auto& group : table_groups) {
        TableData t = build_table(group, h_lines, v_lines, text_page);
        if (!t.rows.empty())
            result.push_back(std::move(t));
    }
    return result;
}

std::string format_table(const TableData& table) {
    if (table.rows.empty()) return "";

    int n_cols = table.rows[0].size();
    if (n_cols == 0) return "";

    std::vector<size_t> widths(n_cols, 3);
    for (auto& row : table.rows)
        for (int c = 0; c < n_cols && c < (int)row.size(); c++)
            widths[c] = std::max(widths[c], row[c].size());

    std::string md;
    for (size_t r = 0; r < table.rows.size(); r++) {
        md += "|";
        for (int c = 0; c < n_cols; c++) {
            std::string cell = (c < (int)table.rows[r].size()) ? table.rows[r][c] : "";
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

// ── Image extraction ─────────────────────────────────────

std::vector<ImageData> extract_images(FPDF_PAGE page, int page_num,
                                       const std::string& output_dir) {
    std::vector<ImageData> images;
    int obj_count = FPDFPage_CountObjects(page);
    int img_idx = 0;

    for (int i = 0; i < obj_count; i++) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) continue;

        ImageData img;
        img.page_number = page_num;
        img.name = "page" + std::to_string(page_num + 1) + "_img" + std::to_string(img_idx);

        unsigned int w = 0, h = 0;
        FPDFImageObj_GetImagePixelSize(obj, &w, &h);
        img.width = w;
        img.height = h;

        img.format = "raw";
        int filter_count = FPDFImageObj_GetImageFilterCount(obj);
        if (filter_count > 0) {
            char fbuf[64];
            unsigned long flen = FPDFImageObj_GetImageFilter(obj, 0, fbuf, sizeof(fbuf));
            if (flen > 0) {
                std::string filter(fbuf, flen - 1);
                if (filter == "DCTDecode") img.format = "jpeg";
                else if (filter == "FlateDecode") img.format = "png";
                else if (filter == "JPXDecode") img.format = "jp2";
                else img.format = filter;
            }
        }

        unsigned long raw_size = FPDFImageObj_GetImageDataRaw(obj, nullptr, 0);
        if (raw_size > 0) {
            img.data.resize(raw_size);
            FPDFImageObj_GetImageDataRaw(obj,
                reinterpret_cast<unsigned char*>(img.data.data()), raw_size);
        }

        if (!output_dir.empty() && !img.data.empty()) {
            std::string ext = (img.format == "jpeg") ? ".jpg" :
                              (img.format == "png") ? ".png" :
                              (img.format == "jp2") ? ".jp2" : ".bin";
            std::string path = output_dir + "/" + img.name + ext;

            std::ofstream ofs(path, std::ios::binary);
            if (ofs) {
                ofs.write(img.data.data(), img.data.size());
                img.saved_path = path;
            }
        }

        images.push_back(std::move(img));
        img_idx++;
    }
    return images;
}

// ── Markdown formatting ──────────────────────────────────

bool line_in_table(const TextLine& line, const std::vector<TableData>& tables) {
    for (auto& t : tables) {
        double t_bottom = std::min(t.y0, t.y1) - 10.0;
        double t_top = std::max(t.y0, t.y1) + 5.0;
        if (line.y_center >= t_bottom && line.y_center <= t_top &&
            line.x_left >= t.x0 - 10.0 && line.x_right <= t.x1 + 10.0) {
            return true;
        }
    }
    return false;
}

std::vector<TextLine> merge_colinear_lines(const std::vector<TextLine>& lines) {
    if (lines.size() < 2) return lines;

    std::vector<size_t> idx(lines.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        return lines[a].y_center > lines[b].y_center;
    });

    std::vector<TextLine> merged;
    size_t i = 0;
    while (i < idx.size()) {
        double y = lines[idx[i]].y_center;
        std::vector<size_t> group;
        group.push_back(idx[i]);
        size_t j = i + 1;
        while (j < idx.size() && std::abs(lines[idx[j]].y_center - y) < 5.0) {
            group.push_back(idx[j]);
            j++;
        }

        if (group.size() == 1) {
            merged.push_back(lines[group[0]]);
        } else {
            std::sort(group.begin(), group.end(), [&](size_t a, size_t b) {
                return lines[a].x_left < lines[b].x_left;
            });
            TextLine m;
            m.y_center = lines[group[0]].y_center;
            m.x_left = lines[group[0]].x_left;
            m.font_size = lines[group[0]].font_size;
            m.is_bold = lines[group[0]].is_bold;
            m.is_italic = lines[group[0]].is_italic;
            for (size_t k = 0; k < group.size(); k++) {
                if (k > 0) m.text += " | ";
                m.text += lines[group[k]].text;
                if (lines[group[k]].x_right > m.x_right)
                    m.x_right = lines[group[k]].x_right;
            }
            merged.push_back(std::move(m));
        }
        i = j;
    }
    return merged;
}

std::string page_to_markdown(const std::vector<TextLine>& raw_lines,
                              const FontStats& stats,
                              const std::vector<ImageData>& images,
                              const std::vector<TableData>& tables) {
    auto lines = merge_colinear_lines(raw_lines);

    std::string md;
    md.reserve(lines.size() * 80);

    struct TableInsert {
        double y_pos;
        size_t table_idx;
    };
    std::vector<TableInsert> table_inserts;
    for (size_t ti = 0; ti < tables.size(); ti++) {
        table_inserts.push_back({std::max(tables[ti].y0, tables[ti].y1), ti});
    }
    std::sort(table_inserts.begin(), table_inserts.end(),
              [](const TableInsert& a, const TableInsert& b) { return a.y_pos > b.y_pos; });

    size_t next_table = 0;

    for (size_t i = 0; i < lines.size(); i++) {
        const auto& l = lines[i];

        while (next_table < table_inserts.size() &&
               table_inserts[next_table].y_pos >= l.y_center) {
            md += "\n";
            md += format_table(tables[table_inserts[next_table].table_idx]);
            md += "\n";
            next_table++;
        }

        if (line_in_table(l, tables)) continue;

        int hlevel = stats.heading_level(l.font_size);

        if (hlevel > 0) {
            if (i > 0) md += '\n';
            for (int h = 0; h < hlevel; h++) md += '#';
            md += ' ';
            md += l.text;
            md += '\n';
        } else if (l.is_bold && l.is_italic) {
            md += "***" + l.text + "***\n";
        } else if (l.is_bold) {
            md += "**" + l.text + "**\n";
        } else if (l.is_italic) {
            md += "*" + l.text + "*\n";
        } else {
            md += l.text;
            md += '\n';
        }
    }

    while (next_table < table_inserts.size()) {
        md += "\n";
        md += format_table(tables[table_inserts[next_table].table_idx]);
        md += "\n";
        next_table++;
    }

    for (auto& img : images) {
        std::string ref = img.saved_path.empty() ? img.name : img.saved_path;
        md += "\n![" + img.name + "](" + ref + ")\n";
    }
    return md;
}

// ── Core extraction logic ────────────────────────────────

struct ExtractResult {
    std::vector<std::vector<TextLine>> all_lines;
    std::vector<std::vector<ImageData>> all_images;
    std::vector<std::vector<TableData>> all_tables;
    std::vector<double> page_widths;
    std::vector<double> page_heights;
    FontStats stats;
    int total_pages = 0;
};

ExtractResult extract_pdf(const std::string& pdf_path, const ConvertOptions& opts) {
    ensure_pdfium_initialized();

    FPDF_DOCUMENT doc = FPDF_LoadDocument(pdf_path.c_str(), nullptr);
    if (!doc)
        throw std::runtime_error("Cannot open PDF: " + pdf_path);

    ExtractResult result;
    result.total_pages = FPDF_GetPageCount(doc);
    int tp = result.total_pages;

    result.all_lines.resize(tp);
    result.all_images.resize(tp);
    result.all_tables.resize(tp);
    result.page_widths.resize(tp, 0);
    result.page_heights.resize(tp, 0);

    std::vector<int> page_indices;
    if (opts.pages.empty()) {
        for (int i = 0; i < tp; i++) page_indices.push_back(i);
    } else {
        page_indices = opts.pages;
    }

    std::string image_dir;
    if (opts.extract_images && !opts.image_output_dir.empty()) {
        image_dir = opts.image_output_dir;
        mkdir(image_dir.c_str(), 0755);
    }

    for (int p : page_indices) {
        if (p < 0 || p >= tp) continue;
        FPDF_PAGE page = FPDF_LoadPage(doc, p);
        if (!page) continue;

        result.page_widths[p] = FPDF_GetPageWidth(page);
        result.page_heights[p] = FPDF_GetPageHeight(page);

        FPDF_TEXTPAGE text_page = FPDFText_LoadPage(page);
        if (text_page) {
            result.all_lines[p] = extract_lines(text_page);
            if (opts.extract_tables) {
                auto segments = extract_line_segments(page);
                result.all_tables[p] = detect_tables(segments, text_page,
                    result.page_widths[p], result.page_heights[p]);
            }
            FPDFText_ClosePage(text_page);
        }

        if (opts.extract_images) {
            result.all_images[p] = extract_images(page, p, image_dir);
        }
        FPDF_ClosePage(page);
    }

    FPDF_CloseDocument(doc);

    result.stats.compute(result.all_lines);
    return result;
}

} // anonymous namespace

// ── Public API ───────────────────────────────────────────

std::string pdf_to_markdown(const std::string& pdf_path, ConvertOptions opts) {
    auto r = extract_pdf(pdf_path, opts);

    std::vector<int> page_indices;
    if (opts.pages.empty()) {
        for (int i = 0; i < r.total_pages; i++) page_indices.push_back(i);
    } else {
        page_indices = opts.pages;
    }

    bool plaintext = (opts.output_format == OutputFormat::PLAINTEXT);

    std::string full_md;
    full_md.reserve(64 * 1024);
    bool first = true;
    for (int p : page_indices) {
        if (p < 0 || p >= r.total_pages) continue;
        if (!first) {
            if (plaintext)
                full_md += "\n--- Page " + std::to_string(p + 1) + " ---\n\n";
            else
                full_md += "\n---\n\n";
        }
        first = false;
        std::string page_md = page_to_markdown(r.all_lines[p], r.stats,
                                                r.all_images[p], r.all_tables[p]);
        if (plaintext)
            full_md += util::strip_markdown(page_md);
        else
            full_md += page_md;
    }
    return full_md;
}

std::vector<PageChunk> pdf_to_markdown_chunks(const std::string& pdf_path,
                                           ConvertOptions opts) {
    auto r = extract_pdf(pdf_path, opts);

    std::vector<int> page_indices;
    if (opts.pages.empty()) {
        for (int i = 0; i < r.total_pages; i++) page_indices.push_back(i);
    } else {
        page_indices = opts.pages;
    }

    bool plaintext = (opts.output_format == OutputFormat::PLAINTEXT);

    std::vector<PageChunk> chunks;
    for (int p : page_indices) {
        if (p < 0 || p >= r.total_pages) continue;
        PageChunk chunk;
        chunk.page_number = p;
        chunk.page_width = r.page_widths[p];
        chunk.page_height = r.page_heights[p];
        chunk.body_font_size = r.stats.body_size;
        std::string page_md = page_to_markdown(r.all_lines[p], r.stats,
                                                r.all_images[p], r.all_tables[p]);
        chunk.text = plaintext ? util::strip_markdown(page_md) : page_md;

        for (auto& td : r.all_tables[p])
            chunk.tables.push_back(td.rows);

        chunk.images = std::move(r.all_images[p]);
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

} // namespace jdoc
