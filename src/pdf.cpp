// pdf.cpp — PDF→Markdown using PDFium
// Features: text, headings, bold/italic, images, table detection (line-based + text-based)

#include "jdoc/pdf.h"
#include "common/string_utils.h"
#include "common/file_utils.h"

#include <fpdfview.h>
#include <fpdf_text.h>
#include <fpdf_edit.h>
#include <fpdf_doc.h>

#include <fstream>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>
#include <zlib.h>

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

// RAII guards for PDFium handles — prevent leaks on exception (e.g. bad_alloc)
struct DocGuard {
    FPDF_DOCUMENT h;
    explicit DocGuard(FPDF_DOCUMENT d) : h(d) {}
    ~DocGuard() { if (h) FPDF_CloseDocument(h); }
    DocGuard(const DocGuard&) = delete;
    DocGuard& operator=(const DocGuard&) = delete;
};

struct PageGuard {
    FPDF_PAGE h;
    explicit PageGuard(FPDF_PAGE p) : h(p) {}
    ~PageGuard() { if (h) FPDF_ClosePage(h); }
    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;
};

struct TextPageGuard {
    FPDF_TEXTPAGE h;
    explicit TextPageGuard(FPDF_TEXTPAGE t) : h(t) {}
    ~TextPageGuard() { if (h) FPDFText_ClosePage(h); }
    TextPageGuard(const TextPageGuard&) = delete;
    TextPageGuard& operator=(const TextPageGuard&) = delete;
};

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

// Merge sorted (left, right) ranges into spans where inter-span gap >= merge_gap
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

// Forward declaration for text-based column inference
std::vector<double> infer_columns_from_text(FPDF_TEXTPAGE text_page,
                                             double left, double right,
                                             const std::vector<double>& row_ys);

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

    // Count vertical lines within the table region (not just edge lines)
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

    // Fallback: use text-based column inference only when there are
    // no internal vertical lines (horizontal-line-only tables).
    // If internal v-lines exist but don't form columns, it's likely
    // a complex graphic (chart, diagram) rather than a table.
    if (col_xs.size() < 3 && internal_vline_count == 0) {
        col_xs = infer_columns_from_text(text_page, table_left, table_right, row_ys);
    }

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

    // Validate: need at least 2 rows where 2+ columns have content
    int meaningful_rows = 0;
    for (auto& row : table.rows) {
        int filled_cols = 0;
        for (auto& cell : row) if (!cell.empty()) filled_cols++;
        if (filled_cols >= 2) meaningful_rows++;
    }
    if (meaningful_rows < 2) table.rows.clear();

    // Reject tables where the column count does not match the visible
    // cell structure. When many internal vertical lines create more columns
    // than content fills (>50% cells empty), it's a bordered text block.
    if (!table.rows.empty()) {
        int n_cols_t = (int)table.rows[0].size();
        if (n_cols_t >= 4) {
            int total_cells = 0;
            int empty_cells = 0;
            for (auto& row : table.rows) {
                for (int c = 0; c < n_cols_t && c < (int)row.size(); c++) {
                    total_cells++;
                    if (row[c].empty()) empty_cells++;
                }
            }
            if (total_cells > 0 && empty_cells > total_cells * 0.5) {
                table.rows.clear();
                return table;
            }
        }
    }

    // Reject tables that look like numbered/bulleted lists.
    // Patterns detected:
    // 1. 2-column: short label (가., 1)) + long content
    // 2. Any column count: first row starts with a list marker (N) or N.)
    //    AND the same pattern repeats in subsequent rows
    if (!table.rows.empty() && (int)table.rows[0].size() >= 2) {
        int n_cols_t = (int)table.rows[0].size();
        int rows_with_list_marker = 0;
        int valid_rows_t = 0;
        for (auto& row : table.rows) {
            bool has_content = false;
            for (auto& c : row) if (!c.empty()) { has_content = true; break; }
            if (!has_content) continue;
            valid_rows_t++;
            // Find the first non-empty cell
            std::string first_cell;
            for (auto& c : row) if (!c.empty()) { first_cell = c; break; }
            if (first_cell.empty()) continue;
            // Check for numbered list markers: "N)" pattern only
            // (not "N." which could be a table cell like "3.5")
            bool is_marker = false;
            if (first_cell[0] >= '0' && first_cell[0] <= '9') {
                for (size_t k = 1; k < first_cell.size() && k < 4; k++) {
                    if (first_cell[k] == ')') {
                        is_marker = true; break;
                    }
                    if (first_cell[k] < '0' || first_cell[k] > '9') break;
                }
            }
            if (is_marker) rows_with_list_marker++;
        }
        // If >60% of rows start with list markers, reject as a list
        if (valid_rows_t >= 2 &&
            rows_with_list_marker >= valid_rows_t * 0.6) {
            table.rows.clear();
            return table;
        }
    }

    // Additional check for 2-column tables:
    // Short label (가., 나., 1), 2) etc.) + long content
    if (!table.rows.empty() && (int)table.rows[0].size() == 2) {
        int short_label_rows = 0;
        int long_content_rows = 0;
        for (auto& row : table.rows) {
            if (row.size() >= 2 && row[0].size() <= 6 && !row[0].empty())
                short_label_rows++;
            if (row.size() >= 2 && row[1].size() > 15)
                long_content_rows++;
        }
        int valid_rows = (int)table.rows.size();
        if (valid_rows >= 2 &&
            short_label_rows >= valid_rows * 0.5 &&
            long_content_rows >= valid_rows * 0.5) {
            table.rows.clear();
        }
    }

    return table;
}

// Compute total h-line coverage at a given Y level
// (sum of non-overlapping line lengths at that Y)
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

// Check if two row-level horizontal lines span a similar table-width region
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

    // Check total coverage (sum of h-line lengths) vs extent
    double cov1 = h_line_coverage_at_y(h_lines, y1, tol);
    double cov2 = h_line_coverage_at_y(h_lines, y2, tol);
    // At least 40% coverage of the extent (allows for gaps between cells)
    if (cov1 < extent1 * 0.4 || cov2 < extent2 * 0.4) return false;

    // Must share significant X overlap with similar extent
    double overlap_l = std::max(min1, min2);
    double overlap_r = std::min(max1, max2);
    if (overlap_r <= overlap_l) return false;
    double overlap = overlap_r - overlap_l;
    double extent = std::max(extent1, extent2);
    double ratio = std::min(extent1, extent2) / extent;
    return overlap >= extent * 0.7 && ratio >= 0.5;
}

// Infer column boundaries from text x-positions when vertical lines are absent
std::vector<double> infer_columns_from_text(FPDF_TEXTPAGE text_page,
                                             double left, double right,
                                             const std::vector<double>& row_ys) {
    int n_rows = (int)row_ys.size() - 1;
    if (n_rows < 1) return {};

    int total_chars = FPDFText_CountChars(text_page);

    // Collect text spans per row
    std::vector<std::vector<std::pair<double,double>>> per_row(n_rows);

    for (int r = 0; r < n_rows; r++) {
        double bot = std::min(row_ys[r], row_ys[r+1]);
        double top = std::max(row_ys[r], row_ys[r+1]);

        std::vector<std::pair<double,double>> char_xs;
        for (int ci = 0; ci < total_chars; ci++) {
            unsigned int u = FPDFText_GetUnicode(text_page, ci);
            if (u == 0 || u == '\r' || u == '\n' || u == 0xFFFD || u == ' ' || u == 0xA0)
                continue;
            double cl, cr, cb, ct;
            if (!FPDFText_GetCharBox(text_page, ci, &cl, &cr, &cb, &ct)) continue;
            double cx = (cl + cr) / 2.0;
            double cy = (ct + cb) / 2.0;
            if (cx < left - 5 || cx > right + 5) continue;
            if (cy >= bot + 1 && cy <= top - 1)
                char_xs.push_back({cl, cr});
        }
        per_row[r] = merge_char_ranges(char_xs);
    }

    // Find consistent gap positions across rows
    // A gap is a region where most rows have no text
    double width = right - left;
    int n_bins = std::max(20, (int)(width / 5.0));
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

    // Threshold: gap must appear in at least 40% of non-empty rows
    int non_empty_rows = 0;
    for (int r = 0; r < n_rows; r++)
        if (!per_row[r].empty()) non_empty_rows++;
    int threshold = std::max(1, (int)(non_empty_rows * 0.4));

    // Find gap regions (consecutive bins above threshold)
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
                // Only add if gap is not too close to edges
                if (gap_center > left + 15 && gap_center < right - 15)
                    boundaries.push_back(gap_center);
                in_gap = false;
            }
        }
    }
    boundaries.push_back(right);

    if (boundaries.size() < 3) return {}; // need at least 2 columns

    // Validate: check that most non-empty rows have text in multiple columns
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
    // At least 50% of non-empty rows must use 2+ columns
    if (rows_with_multi_cols < non_empty_rows * 0.5) return {};

    return boundaries;
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

    // Check vertical line connectivity between adjacent row levels
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

    // Also check if rows share similar X extent (heuristic for tables
    // with only horizontal lines or partial vertical lines)
    std::vector<bool> x_overlap(n_levels - 1, false);
    for (int i = 0; i < n_levels - 1; i++) {
        x_overlap[i] = h_lines_share_full_span(h_lines, row_ys[i], row_ys[i+1], 3.0);
    }

    // Group rows: connected by vertical lines OR by shared X extent
    // with reasonable vertical gap (not too far apart)
    std::vector<std::vector<double>> table_groups;
    std::vector<double> current_group;
    current_group.push_back(row_ys[0]);
    for (int i = 0; i < n_levels - 1; i++) {
        double gap = row_ys[i + 1] - row_ys[i];
        bool close_enough = gap < 200.0; // max gap between rows in a table
        if (connected[i] || (x_overlap[i] && close_enough)) {
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
        if (!t.rows.empty()) {
            result.push_back(std::move(t));
        }
    }
    return result;
}

// ── Pure text-based table detection (no lines required) ──────────
// Detects tables by analyzing text position patterns:
// 1. Cluster all chars by Y into text rows
// 2. Find consistent column gaps across multiple rows
// 3. Build table from rows that share the same column structure

std::vector<TableData> detect_text_tables(FPDF_TEXTPAGE text_page,
                                           const std::vector<TableData>& existing_tables,
                                           double page_width, double page_height) {
    int total_chars = FPDFText_CountChars(text_page);
    if (total_chars < 10) return {};

    // Collect all non-space chars with positions
    struct CharInfo { double x, y, left, right, top, bot; };
    std::vector<CharInfo> chars;
    for (int ci = 0; ci < total_chars; ci++) {
        unsigned int u = FPDFText_GetUnicode(text_page, ci);
        if (u == 0 || u == '\r' || u == '\n' || u == 0xFFFD || u == ' ' || u == '\t' || u == 0xA0)
            continue;
        double l, r, b, t;
        if (!FPDFText_GetCharBox(text_page, ci, &l, &r, &b, &t)) continue;
        double cx = (l + r) / 2.0;
        double cy = (t + b) / 2.0;
        if (cx < 0 || cx > page_width || cy < 0 || cy > page_height) continue;
        chars.push_back({cx, cy, l, r, t, b});
    }
    if (chars.empty()) return {};

    // Cluster chars by Y into text rows (tolerance ~3pt for same line)
    std::sort(chars.begin(), chars.end(), [](const CharInfo& a, const CharInfo& b) {
        return a.y > b.y; // top to bottom (PDF coords: higher Y = higher on page)
    });

    struct TextRow {
        double y_center;
        double y_top, y_bot;
        std::vector<std::pair<double,double>> char_ranges; // (left, right) of each char
    };
    std::vector<TextRow> text_rows;
    {
        TextRow cur;
        cur.y_center = chars[0].y;
        cur.y_top = chars[0].top;
        cur.y_bot = chars[0].bot;
        cur.char_ranges.push_back({chars[0].left, chars[0].right});

        for (size_t i = 1; i < chars.size(); i++) {
            if (std::abs(chars[i].y - cur.y_center) < 3.0) {
                cur.char_ranges.push_back({chars[i].left, chars[i].right});
                cur.y_top = std::max(cur.y_top, chars[i].top);
                cur.y_bot = std::min(cur.y_bot, chars[i].bot);
                // Update running average
                cur.y_center = (cur.y_center * (cur.char_ranges.size() - 1) + chars[i].y)
                               / cur.char_ranges.size();
            } else {
                if (!cur.char_ranges.empty()) text_rows.push_back(cur);
                cur = TextRow();
                cur.y_center = chars[i].y;
                cur.y_top = chars[i].top;
                cur.y_bot = chars[i].bot;
                cur.char_ranges.push_back({chars[i].left, chars[i].right});
            }
        }
        if (!cur.char_ranges.empty()) text_rows.push_back(cur);
    }

    if (text_rows.size() < 3) return {};

    // Skip rows already covered by existing tables
    auto row_in_existing_table = [&](const TextRow& row) -> bool {
        for (auto& t : existing_tables) {
            double t_bottom = std::min(t.y0, t.y1) - 5.0;
            double t_top = std::max(t.y0, t.y1) + 5.0;
            if (row.y_center >= t_bottom && row.y_center <= t_top)
                return true;
        }
        return false;
    };

    // For each text row, merge chars into spans and compute the X extent
    struct RowSpans {
        double y_center, y_top, y_bot;
        double x_min, x_max;
        std::vector<std::pair<double,double>> spans; // merged text spans
    };

    std::vector<RowSpans> row_spans;
    for (size_t ri = 0; ri < text_rows.size(); ri++) {
        auto& tr = text_rows[ri];
        if (row_in_existing_table(tr)) continue;

        RowSpans rs;
        rs.y_center = tr.y_center;
        rs.y_top = tr.y_top;
        rs.y_bot = tr.y_bot;

        // Merge chars into text spans (gap < 8pt = same word/phrase)
        auto ranges = tr.char_ranges;
        rs.spans = merge_char_ranges(ranges);

        rs.x_min = rs.spans.front().first;
        rs.x_max = rs.spans.back().second;
        row_spans.push_back(rs);
    }

    if (row_spans.size() < 3) return {};

    // Now find groups of consecutive rows that look tabular:
    // - Similar X extent (overlap > 70%)
    // - Multiple text spans (columns) with consistent gap positions
    // - At least 3 rows

    // For each row, note gap positions (between spans)
    // A "gap" is the midpoint between consecutive spans
    auto get_gaps = [](const RowSpans& rs) -> std::vector<double> {
        std::vector<double> gaps;
        for (size_t i = 1; i < rs.spans.size(); i++) {
            double gap_mid = (rs.spans[i-1].second + rs.spans[i].first) / 2.0;
            gaps.push_back(gap_mid);
        }
        return gaps;
    };

    // Try to find tabular groups
    std::vector<TableData> result;

    size_t start = 0;
    while (start < row_spans.size()) {
        // Skip rows with only 1 span (single column text)
        if (row_spans[start].spans.size() < 2) { start++; continue; }

        // Try to extend a group from 'start'
        std::vector<size_t> group_indices;
        group_indices.push_back(start);

        auto ref_gaps = get_gaps(row_spans[start]);
        double ref_xmin = row_spans[start].x_min;
        double ref_xmax = row_spans[start].x_max;

        for (size_t j = start + 1; j < row_spans.size(); j++) {
            auto& rs = row_spans[j];

            // Check Y proximity: rows should be close (< 40pt gap)
            double y_gap = std::abs(row_spans[group_indices.back()].y_center - rs.y_center);
            if (y_gap > 40.0) break;

            // Single-span row: continuation line or merged cell
            // Check this BEFORE X extent overlap (which would fail for narrow continuations)
            if (rs.spans.size() == 1) {
                double sp_mid = (rs.spans[0].first + rs.spans[0].second) / 2.0;
                // Allow if the span falls within the reference table X extent
                if (sp_mid >= ref_xmin - 5 && sp_mid <= ref_xmax + 5) {
                    group_indices.push_back(j);
                    continue;
                }
                break;
            }

            // Check X extent overlap (only for multi-span rows)
            double overlap_l = std::max(ref_xmin, rs.x_min);
            double overlap_r = std::min(ref_xmax, rs.x_max);
            double extent = std::max(ref_xmax - ref_xmin, rs.x_max - rs.x_min);
            if (extent < 50) break;
            if (overlap_r - overlap_l < extent * 0.5) break;

            // Check gap alignment: at least some gaps should match
            auto cur_gaps = get_gaps(rs);
            if (cur_gaps.size() > 0) {
                int matching = 0;
                for (auto& cg : cur_gaps) {
                    for (auto& rg : ref_gaps) {
                        if (std::abs(cg - rg) < 15.0) { matching++; break; }
                    }
                }
                // At least half the gaps should align
                int min_gaps = (int)std::min(cur_gaps.size(), ref_gaps.size());
                if (min_gaps > 0 && matching >= (min_gaps + 1) / 2) {
                    group_indices.push_back(j);
                    // Update reference extent
                    ref_xmin = std::min(ref_xmin, rs.x_min);
                    ref_xmax = std::max(ref_xmax, rs.x_max);
                    continue;
                }
            }

            break;
        }

        if (group_indices.size() < 3) {
            start++;
            continue;
        }

        // Build the table from this group
        // Collect gap positions only from multi-span rows
        // Also track the actual gap width to filter narrow false gaps
        std::vector<double> all_gaps;
        int multi_span_rows = 0;
        for (auto idx : group_indices) {
            auto& rs = row_spans[idx];
            if (rs.spans.size() < 2) continue;
            multi_span_rows++;
            for (size_t s = 1; s < rs.spans.size(); s++) {
                double gap_width = rs.spans[s].first - rs.spans[s-1].second;
                if (gap_width >= 10.0) { // minimum gap width to be a column separator
                    double gap_mid = (rs.spans[s-1].second + rs.spans[s].first) / 2.0;
                    all_gaps.push_back(gap_mid);
                }
            }
        }

        if (all_gaps.empty() || multi_span_rows < 2) {
            start = group_indices.back() + 1; continue;
        }

        // Cluster gaps by position
        std::sort(all_gaps.begin(), all_gaps.end());
        std::vector<double> col_boundaries;
        col_boundaries.push_back(ref_xmin);

        double cluster_sum = all_gaps[0];
        int cluster_count = 1;
        for (size_t i = 1; i < all_gaps.size(); i++) {
            if (all_gaps[i] - (cluster_sum / cluster_count) < 15.0) {
                cluster_sum += all_gaps[i];
                cluster_count++;
            } else {
                // Gap must appear in majority of multi-span rows
                if (cluster_count >= std::max(2, (int)(multi_span_rows * 0.5))) {
                    col_boundaries.push_back(cluster_sum / cluster_count);
                }
                cluster_sum = all_gaps[i];
                cluster_count = 1;
            }
        }
        if (cluster_count >= std::max(2, (int)(multi_span_rows * 0.5))) {
            col_boundaries.push_back(cluster_sum / cluster_count);
        }
        col_boundaries.push_back(ref_xmax);

        int n_cols = (int)col_boundaries.size() - 1;
        if (n_cols < 2) { start = group_indices.back() + 1; continue; }

        // Build TableData
        TableData table;
        table.y0 = row_spans[group_indices.back()].y_bot;
        table.y1 = row_spans[group_indices.front()].y_top;
        table.x0 = ref_xmin;
        table.x1 = ref_xmax;

        for (auto idx : group_indices) {
            auto& rs = row_spans[idx];
            std::vector<std::string> row(n_cols);

            // Assign each span to a column
            for (auto& sp : rs.spans) {
                double sp_mid = (sp.first + sp.second) / 2.0;
                int best_col = 0;
                for (int c = 0; c < n_cols; c++) {
                    if (sp_mid >= col_boundaries[c] && sp_mid <= col_boundaries[c+1]) {
                        best_col = c;
                        break;
                    }
                }

                // Extract text for this span from text_page
                std::string cell_text;
                for (int ci = 0; ci < total_chars; ci++) {
                    double l, r, b, t;
                    if (!FPDFText_GetCharBox(text_page, ci, &l, &r, &b, &t)) continue;
                    double cx = (l + r) / 2.0;
                    double cy = (t + b) / 2.0;
                    if (std::abs(cy - rs.y_center) > 3.0) continue;
                    if (cx >= sp.first - 2 && cx <= sp.second + 2) {
                        unsigned int u = FPDFText_GetUnicode(text_page, ci);
                        if (u == 0 || u == '\r' || u == '\n' || u == 0xFFFD) continue;
                        util::append_utf8(cell_text, u);
                    }
                }

                // Trim
                size_t s = cell_text.find_first_not_of(" \t");
                size_t e = cell_text.find_last_not_of(" \t");
                if (s != std::string::npos)
                    cell_text = cell_text.substr(s, e - s + 1);
                else
                    cell_text.clear();

                if (!cell_text.empty()) {
                    if (!row[best_col].empty()) row[best_col] += " ";
                    row[best_col] += cell_text;
                }
            }
            table.rows.push_back(std::move(row));
        }

        // Merge continuation rows (single-cell rows) into previous row
        for (size_t r = 1; r < table.rows.size(); r++) {
            int filled = 0;
            int filled_col = -1;
            for (int c = 0; c < n_cols; c++) {
                if (!table.rows[r][c].empty()) { filled++; filled_col = c; }
            }
            if (filled == 1 && filled_col >= 0 && r > 0) {
                // Merge into previous row's same column
                if (!table.rows[r-1][filled_col].empty())
                    table.rows[r-1][filled_col] += " ";
                table.rows[r-1][filled_col] += table.rows[r][filled_col];
                table.rows.erase(table.rows.begin() + r);
                r--;
            }
        }

        // Validate: at least 2 rows with 2+ filled columns
        int meaningful_rows = 0;
        for (auto& row : table.rows) {
            int filled = 0;
            for (auto& c : row) if (!c.empty()) filled++;
            if (filled >= 2) meaningful_rows++;
        }

        if (meaningful_rows >= 2) {
            // Reject tables that look like numbered/bulleted lists.
            bool looks_like_list = false;

            // Check for N) pattern in first cell of each row (any column count)
            {
                int marker_rows = 0;
                int content_rows = 0;
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
                if (content_rows >= 2 && marker_rows >= content_rows * 0.6)
                    looks_like_list = true;
            }

            // Additional 2-column check: short labels + long content
            if (!looks_like_list && n_cols == 2) {
                int short_label_rows = 0;
                int long_content_rows = 0;
                for (auto& row : table.rows) {
                    // Column 0 is a short label (bullet, number, letter+period)
                    if (row.size() >= 2 && row[0].size() <= 6 && !row[0].empty())
                        short_label_rows++;
                    // Column 1 has substantial content
                    if (row.size() >= 2 && row[1].size() > 15)
                        long_content_rows++;
                }
                int valid_rows = (int)table.rows.size();
                if (valid_rows >= 2 &&
                    short_label_rows >= valid_rows * 0.5 &&
                    long_content_rows >= valid_rows * 0.5) {
                    looks_like_list = true;
                }
            }

            if (!looks_like_list) {
                trim_table(table);
                result.push_back(std::move(table));
            }
        }

        start = group_indices.back() + 1;
    }

    return result;
}

std::string format_table(const TableData& table) {
    if (table.rows.empty()) return "";

    // Filter out all-empty rows (except header)
    std::vector<std::vector<std::string>> filtered;
    for (size_t r = 0; r < table.rows.size(); r++) {
        bool all_empty = true;
        for (auto& cell : table.rows[r])
            if (!cell.empty()) { all_empty = false; break; }
        if (!all_empty || r == 0) // keep header even if empty
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

// ── BMP writer helpers ───────────────────────────────────

static std::vector<char> bitmap_to_bmp(FPDF_BITMAP bitmap) {
    int w = FPDFBitmap_GetWidth(bitmap);
    int h = FPDFBitmap_GetHeight(bitmap);
    int stride = FPDFBitmap_GetStride(bitmap);
    int fmt = FPDFBitmap_GetFormat(bitmap);
    void* buf = FPDFBitmap_GetBuffer(bitmap);
    if (!buf || w <= 0 || h <= 0) return {};

    int bpp;
    switch (fmt) {
        case FPDFBitmap_Gray: bpp = 1; break;
        case FPDFBitmap_BGR:  bpp = 3; break;
        case FPDFBitmap_BGRx:
        case FPDFBitmap_BGRA: bpp = 4; break;
        default: return {};
    }

    // Write as 24-bit BMP (convert from BGRA/BGRx to BGR)
    int out_stride = ((w * 3 + 3) / 4) * 4;
    int pixel_data_size = out_stride * h;
    int file_size = 14 + 40 + pixel_data_size;

    std::vector<char> bmp(file_size);
    auto write16 = [&](int off, uint16_t v) { memcpy(&bmp[off], &v, 2); };
    auto write32 = [&](int off, uint32_t v) { memcpy(&bmp[off], &v, 4); };

    // BMP file header
    bmp[0] = 'B'; bmp[1] = 'M';
    write32(2, file_size);
    write32(10, 14 + 40); // pixel data offset

    // DIB header (BITMAPINFOHEADER)
    write32(14, 40);
    write32(18, w);
    write32(22, h);
    write16(26, 1); // planes
    write16(28, 24); // bpp
    write32(34, pixel_data_size);

    auto* src = static_cast<uint8_t*>(buf);
    // BMP stores bottom-to-top, PDFium bitmap is top-to-bottom
    for (int y = 0; y < h; y++) {
        uint8_t* src_row = src + (h - 1 - y) * stride;
        char* dst_row = &bmp[14 + 40 + y * out_stride];
        for (int x = 0; x < w; x++) {
            if (bpp >= 3) {
                dst_row[x * 3 + 0] = src_row[x * bpp + 0]; // B
                dst_row[x * 3 + 1] = src_row[x * bpp + 1]; // G
                dst_row[x * 3 + 2] = src_row[x * bpp + 2]; // R
            } else {
                // Grayscale
                dst_row[x * 3 + 0] = src_row[x];
                dst_row[x * 3 + 1] = src_row[x];
                dst_row[x * 3 + 2] = src_row[x];
            }
        }
    }
    return bmp;
}

// ── PNG writer (zlib-based, no external dependency) ──────

static void png_put32(std::vector<char>& v, uint32_t val) {
    char b[4] = {static_cast<char>((val >> 24) & 0xFF),
                 static_cast<char>((val >> 16) & 0xFF),
                 static_cast<char>((val >> 8) & 0xFF),
                 static_cast<char>(val & 0xFF)};
    v.insert(v.end(), b, b + 4);
}

static void png_write_chunk(std::vector<char>& out, const char type[4],
                             const uint8_t* data, uint32_t len) {
    png_put32(out, len);
    size_t type_pos = out.size();
    out.insert(out.end(), type, type + 4);
    if (data && len > 0)
        out.insert(out.end(), reinterpret_cast<const char*>(data),
                   reinterpret_cast<const char*>(data) + len);
    uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(&out[type_pos]),
                         4 + len);
    png_put32(out, crc);
}

// Encode a PDFium bitmap as PNG (RGB, 8-bit, lossless).
static std::vector<char> bitmap_to_png(FPDF_BITMAP bitmap, int level) {
    int w = FPDFBitmap_GetWidth(bitmap);
    int h = FPDFBitmap_GetHeight(bitmap);
    int stride = FPDFBitmap_GetStride(bitmap);
    int fmt = FPDFBitmap_GetFormat(bitmap);
    auto* src = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
    if (!src || w <= 0 || h <= 0) return {};

    int bpp;
    switch (fmt) {
        case FPDFBitmap_Gray: bpp = 1; break;
        case FPDFBitmap_BGR:  bpp = 3; break;
        case FPDFBitmap_BGRx:
        case FPDFBitmap_BGRA: bpp = 4; break;
        default: return {};
    }

    // Raw scanlines: filter byte (0x00) + RGB triplets per row
    size_t row_bytes = 1 + static_cast<size_t>(w) * 3;
    std::vector<uint8_t> raw(row_bytes * h);
    for (int y = 0; y < h; y++) {
        uint8_t* sr = src + y * stride;
        uint8_t* dr = raw.data() + y * row_bytes;
        dr[0] = 0; // filter: none
        for (int x = 0; x < w; x++) {
            if (bpp >= 3) {
                dr[1 + x*3]     = sr[x*bpp + 2]; // R (BGR→RGB)
                dr[1 + x*3 + 1] = sr[x*bpp + 1]; // G
                dr[1 + x*3 + 2] = sr[x*bpp];     // B
            } else { // grayscale
                dr[1 + x*3] = dr[1 + x*3 + 1] = dr[1 + x*3 + 2] = sr[x];
            }
        }
    }

    // Deflate
    uLong bound = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> deflated(bound);
    uLong deflated_size = bound;
    if (compress2(deflated.data(), &deflated_size, raw.data(),
                  static_cast<uLong>(raw.size()), level) != Z_OK)
        return {};

    raw.clear();
    raw.shrink_to_fit();

    // Assemble PNG
    std::vector<char> png;
    png.reserve(8 + 25 + deflated_size + 24);

    const uint8_t sig[] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    png.insert(png.end(), sig, sig + 8);

    uint8_t ihdr[13] = {};
    ihdr[0] = (w >> 24); ihdr[1] = (w >> 16); ihdr[2] = (w >> 8); ihdr[3] = w;
    ihdr[4] = (h >> 24); ihdr[5] = (h >> 16); ihdr[6] = (h >> 8); ihdr[7] = h;
    ihdr[8] = 8; ihdr[9] = 2; // 8-bit RGB
    png_write_chunk(png, "IHDR", ihdr, 13);
    png_write_chunk(png, "IDAT", deflated.data(), static_cast<uint32_t>(deflated_size));
    png_write_chunk(png, "IEND", nullptr, 0);

    return png;
}

// Build a BMP from raw decoded pixel data at original resolution
static std::vector<char> decoded_pixels_to_bmp(const std::vector<unsigned char>& decoded,
                                                unsigned int w, unsigned int h,
                                                int colorspace, unsigned int bpp) {
    if (decoded.empty() || w == 0 || h == 0) return {};

    // Determine source bytes per pixel from colorspace and bpp
    int src_components;
    switch (colorspace) {
        case FPDF_COLORSPACE_DEVICEGRAY:
        case FPDF_COLORSPACE_CALGRAY:
            src_components = 1;
            break;
        case FPDF_COLORSPACE_DEVICERGB:
        case FPDF_COLORSPACE_CALRGB:
        case FPDF_COLORSPACE_LAB:
        case FPDF_COLORSPACE_ICCBASED:
            src_components = (bpp >= 24) ? (bpp / 8) : 3;
            break;
        case FPDF_COLORSPACE_DEVICECMYK:
            src_components = 4;
            break;
        default:
            src_components = bpp / 8;
            if (src_components == 0) src_components = 3;
            break;
    }

    size_t expected_size = (size_t)w * h * src_components;
    // If decoded size doesn't match, try to infer components from data size
    if (decoded.size() != expected_size) {
        size_t total_pixels = (size_t)w * h;
        if (total_pixels > 0) {
            int inferred = (int)(decoded.size() / total_pixels);
            if (inferred >= 1 && inferred <= 4 && decoded.size() == total_pixels * inferred) {
                src_components = inferred;
            } else {
                return {};  // Can't determine pixel layout
            }
        } else {
            return {};
        }
    }

    // Write as 24-bit BMP
    int out_stride = (((int)w * 3 + 3) / 4) * 4;
    int pixel_data_size = out_stride * (int)h;
    int file_size = 14 + 40 + pixel_data_size;

    std::vector<char> bmp(file_size, 0);
    auto write16 = [&](int off, uint16_t v) { memcpy(&bmp[off], &v, 2); };
    auto write32 = [&](int off, uint32_t v) { memcpy(&bmp[off], &v, 4); };

    bmp[0] = 'B'; bmp[1] = 'M';
    write32(2, (uint32_t)file_size);
    write32(10, 14 + 40);
    write32(14, 40);
    write32(18, (uint32_t)w);
    write32(22, (uint32_t)h);
    write16(26, 1);
    write16(28, 24);
    write32(34, (uint32_t)pixel_data_size);

    int src_stride = (int)w * src_components;

    for (int y = 0; y < (int)h; y++) {
        // BMP is bottom-to-top, PDF image data is top-to-bottom
        const unsigned char* src_row = decoded.data() + ((int)h - 1 - y) * src_stride;
        char* dst_row = &bmp[14 + 40 + y * out_stride];
        for (int x = 0; x < (int)w; x++) {
            uint8_t r, g, b;
            if (src_components == 1) {
                r = g = b = src_row[x];
            } else if (src_components == 3) {
                r = src_row[x * 3 + 0];
                g = src_row[x * 3 + 1];
                b = src_row[x * 3 + 2];
            } else if (src_components == 4) {
                if (colorspace == FPDF_COLORSPACE_DEVICECMYK) {
                    // CMYK to RGB conversion
                    int c = src_row[x * 4 + 0];
                    int m = src_row[x * 4 + 1];
                    int yy = src_row[x * 4 + 2];
                    int k = src_row[x * 4 + 3];
                    r = (uint8_t)(255 - std::min(255, c + k));
                    g = (uint8_t)(255 - std::min(255, m + k));
                    b = (uint8_t)(255 - std::min(255, yy + k));
                } else {
                    // RGBA - drop alpha
                    r = src_row[x * 4 + 0];
                    g = src_row[x * 4 + 1];
                    b = src_row[x * 4 + 2];
                }
            } else {
                r = g = b = src_row[x * src_components];
            }
            // BMP stores as BGR
            dst_row[x * 3 + 0] = (char)b;
            dst_row[x * 3 + 1] = (char)g;
            dst_row[x * 3 + 2] = (char)r;
        }
    }
    return bmp;
}

// ── Page rendering ───────────────────────────────────────

// Render a full page as a 150-DPI PNG with Z_BEST_SPEED compression.
static ImageData render_page_as_image(FPDF_DOCUMENT doc, FPDF_PAGE page,
                                       int page_num, const std::string& output_dir) {
    constexpr double kDPI = 150.0;
    constexpr double kBase = 72.0;
    int rw = static_cast<int>(FPDF_GetPageWidth(page) * kDPI / kBase);
    int rh = static_cast<int>(FPDF_GetPageHeight(page) * kDPI / kBase);
    if (rw <= 0 || rh <= 0) return {};

    FPDF_BITMAP bm = FPDFBitmap_Create(rw, rh, 0);
    if (!bm) return {};
    FPDFBitmap_FillRect(bm, 0, 0, rw, rh, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bm, page, 0, 0, rw, rh, 0, FPDF_PRINTING);

    auto png = bitmap_to_png(bm, Z_BEST_SPEED);
    FPDFBitmap_Destroy(bm);
    if (png.empty()) return {};

    ImageData img;
    img.page_number = page_num;
    img.name = "page" + std::to_string(page_num + 1) + "_render";
    img.format = "png";
    img.width = rw;
    img.height = rh;
    img.data = std::move(png);

    if (!output_dir.empty()) {
        std::string path = output_dir + "/" + img.name + ".png";
        std::ofstream f(path, std::ios::binary);
        if (f) {
            f.write(img.data.data(), static_cast<std::streamsize>(img.data.size()));
            img.saved_path = path;
        }
    }
    return img;
}

// ── Image extraction ─────────────────────────────────────

ImageData extract_single_image(FPDF_DOCUMENT doc, FPDF_PAGE page,
                                FPDF_PAGEOBJECT obj, int page_num, int img_idx,
                                const std::string& output_dir) {
    ImageData img;
    img.page_number = page_num;
    img.name = "page" + std::to_string(page_num + 1) + "_img" + std::to_string(img_idx);

    unsigned int w = 0, h = 0;
    FPDFImageObj_GetImagePixelSize(obj, &w, &h);
    img.width = w;
    img.height = h;

    // Check all filters in the chain (e.g. [/FlateDecode /DCTDecode]).
    std::string jpeg_filter;
    int filter_count = FPDFImageObj_GetImageFilterCount(obj);
    for (int fi = 0; fi < filter_count; fi++) {
        char fbuf[64];
        unsigned long flen = FPDFImageObj_GetImageFilter(obj, fi, fbuf, sizeof(fbuf));
        if (flen > 0) {
            std::string f(fbuf, flen - 1);
            if (f == "DCTDecode" || f == "JPXDecode") {
                jpeg_filter = f;
                break;
            }
        }
    }

    if (!jpeg_filter.empty()) {
        // JPEG/JPEG2000: try decoded data first, then fall back to raw stream.
        img.format = (jpeg_filter == "DCTDecode") ? "jpeg" : "jp2";
        bool extracted = false;

        if (filter_count > 1) {
            unsigned long dec_size = FPDFImageObj_GetImageDataDecoded(obj, nullptr, 0);
            if (dec_size > 2) {
                img.data.resize(dec_size);
                FPDFImageObj_GetImageDataDecoded(obj,
                    reinterpret_cast<unsigned char*>(img.data.data()), dec_size);
                auto* p = reinterpret_cast<unsigned char*>(img.data.data());
                bool valid = (img.format == "jpeg" && p[0] == 0xFF && p[1] == 0xD8) ||
                             (img.format == "jp2" && dec_size >= 12 &&
                              p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x0C);
                if (valid)
                    extracted = true;
                else
                    img.data.clear(); // not valid, fall through
            }
        }

        if (!extracted) {
            unsigned long raw_size = FPDFImageObj_GetImageDataRaw(obj, nullptr, 0);
            if (raw_size > 0) {
                img.data.resize(raw_size);
                FPDFImageObj_GetImageDataRaw(obj,
                    reinterpret_cast<unsigned char*>(img.data.data()), raw_size);
            }
        }
    } else {
        // FlateDecode or other: extract decoded pixel data at original resolution
        img.format = "bmp";
        unsigned long decoded_size = FPDFImageObj_GetImageDataDecoded(obj, nullptr, 0);
        if (decoded_size > 0 && w > 0 && h > 0) {
            std::vector<unsigned char> decoded(decoded_size);
            FPDFImageObj_GetImageDataDecoded(obj, decoded.data(), decoded_size);

            FPDF_IMAGEOBJ_METADATA meta = {};
            FPDFImageObj_GetImageMetadata(obj, page, &meta);

            auto bmp_data = decoded_pixels_to_bmp(decoded, w, h,
                                                   meta.colorspace, meta.bits_per_pixel);
            decoded.clear();
            decoded.shrink_to_fit();

            if (!bmp_data.empty()) {
                img.data = std::move(bmp_data);
            } else {
                FPDF_BITMAP bitmap = FPDFImageObj_GetRenderedBitmap(doc, page, obj);
                if (bitmap) {
                    img.data = bitmap_to_bmp(bitmap);
                    img.width = FPDFBitmap_GetWidth(bitmap);
                    img.height = FPDFBitmap_GetHeight(bitmap);
                    FPDFBitmap_Destroy(bitmap);
                }
            }
        } else {
            FPDF_BITMAP bitmap = FPDFImageObj_GetRenderedBitmap(doc, page, obj);
            if (bitmap) {
                img.data = bitmap_to_bmp(bitmap);
                img.width = FPDFBitmap_GetWidth(bitmap);
                img.height = FPDFBitmap_GetHeight(bitmap);
                FPDFBitmap_Destroy(bitmap);
            }
        }
    }

    if (!output_dir.empty() && !img.data.empty()) {
        std::string ext = (img.format == "jpeg") ? ".jpg" :
                          (img.format == "jp2") ? ".jp2" :
                          (img.format == "bmp") ? ".bmp" : ".bin";
        std::string path = output_dir + "/" + img.name + ext;

        std::ofstream ofs(path, std::ios::binary);
        if (ofs) {
            ofs.write(img.data.data(), img.data.size());
            img.saved_path = path;
        }
    }

    return img;
}

std::vector<ImageData> extract_images(FPDF_DOCUMENT doc, FPDF_PAGE page,
                                       int page_num,
                                       const std::string& output_dir) {
    // Classify image objects: detect layered structure in single pass.
    int obj_count = FPDFPage_CountObjects(page);
    bool has_regular = false;
    bool has_mask = false;

    for (int i = 0; i < obj_count; i++) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) continue;

        FPDF_IMAGEOBJ_METADATA meta = {};
        if (FPDFImageObj_GetImageMetadata(obj, page, &meta) && meta.bits_per_pixel == 1)
            has_mask = true;
        else
            has_regular = true;

        if (has_regular && has_mask) break; // early exit
    }

    // Layered page (background + ImageMask overlays): render as single composite.
    if (has_regular && has_mask) {
        std::vector<ImageData> images;
        auto rendered = render_page_as_image(doc, page, page_num, output_dir);
        if (!rendered.data.empty())
            images.push_back(std::move(rendered));
        return images;
    }

    // Normal: extract each image individually.
    std::vector<ImageData> images;
    int img_idx = 0;

    for (int i = 0; i < obj_count; i++) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) continue;

        auto img = extract_single_image(doc, page, obj, page_num, img_idx, output_dir);
        if (!img.data.empty()) {
            images.push_back(std::move(img));
            img_idx++;
        }
    }
    return images;
}

// ── Markdown formatting ──────────────────────────────────

bool line_in_table(const TextLine& line, const std::vector<TableData>& tables) {
    for (auto& t : tables) {
        double t_bottom = std::min(t.y0, t.y1) - 10.0;
        double t_top = std::max(t.y0, t.y1) + 5.0;
        double t_left = std::min(t.x0, t.x1) - 15.0;
        double t_right = std::max(t.x0, t.x1) + 15.0;
        // Check if line's Y center is within table Y range
        if (line.y_center >= t_bottom && line.y_center <= t_top) {
            // Either the line is fully within the table X range,
            // or it significantly overlaps with the table X range
            if (line.x_left >= t_left && line.x_right <= t_right) {
                return true;
            }
            // Partial overlap: if most of the line is within table bounds
            double overlap_l = std::max(line.x_left, t_left);
            double overlap_r = std::min(line.x_right, t_right);
            if (overlap_r > overlap_l) {
                double overlap = overlap_r - overlap_l;
                double line_width = line.x_right - line.x_left;
                if (line_width > 0 && overlap >= line_width * 0.6) {
                    return true;
                }
            }
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
                if (k > 0) {
                    // Use gap between segments to decide separator
                    double gap = lines[group[k]].x_left - lines[group[k-1]].x_right;
                    if (gap > 100.0)
                        m.text += "\n";  // Large gap = separate lines
                    else
                        m.text += "  ";  // Small gap = same line with spacing
                }
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

        // Skip lines that are only whitespace/pipe characters
        // (artifact from PDF text extraction in table-like regions)
        {
            bool only_filler = true;
            for (char ch : l.text) {
                if (ch != '|' && ch != ' ' && ch != '\t' && ch != '\n') {
                    only_filler = false;
                    break;
                }
            }
            if (only_filler) continue;
        }

        int hlevel = stats.heading_level(l.font_size);

        // Suppress headings for long lines (likely body text with slightly larger font)
        // and for lines that are not bold (weaker heading signal at lower levels)
        if (hlevel >= 3 && !l.is_bold && l.text.size() > 60)
            hlevel = 0;

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

struct BookmarkEntry {
    std::string title;
    int page = -1;   // 0-based page index, -1 if no destination
    int level = 0;    // nesting depth (0 = top-level)
};

void collect_bookmarks(FPDF_DOCUMENT doc, FPDF_BOOKMARK parent,
                       int depth, std::vector<BookmarkEntry>& out) {
    FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, parent);
    while (child) {
        BookmarkEntry entry;
        entry.level = depth;

        // Get title (UTF-16LE from PDFium)
        unsigned long len = FPDFBookmark_GetTitle(child, nullptr, 0);
        if (len > 2) {
            std::vector<char16_t> buf(len / 2);
            FPDFBookmark_GetTitle(child, buf.data(), len);
            // Convert UTF-16LE to UTF-8
            for (size_t i = 0; i < buf.size() && buf[i]; i++) {
                util::append_utf8(entry.title, static_cast<char32_t>(buf[i]));
            }
        }

        // Get destination page
        FPDF_DEST dest = FPDFBookmark_GetDest(doc, child);
        if (!dest) {
            FPDF_ACTION action = FPDFBookmark_GetAction(child);
            if (action && FPDFAction_GetType(action) == PDFACTION_GOTO)
                dest = FPDFAction_GetDest(doc, action);
        }
        if (dest) {
            entry.page = FPDFDest_GetDestPageIndex(doc, dest);
        }

        out.push_back(std::move(entry));

        // Recurse into children
        collect_bookmarks(doc, child, depth + 1, out);

        child = FPDFBookmark_GetNextSibling(doc, child);
    }
}

struct ExtractResult {
    std::vector<std::vector<TextLine>> all_lines;
    std::vector<std::vector<ImageData>> all_images;
    std::vector<std::vector<TableData>> all_tables;
    std::vector<double> page_widths;
    std::vector<double> page_heights;
    std::vector<BookmarkEntry> bookmarks;
    FontStats stats;
    int total_pages = 0;
};

ExtractResult extract_pdf(const std::string& pdf_path, const ConvertOptions& opts) {
    ensure_pdfium_initialized();

    FPDF_DOCUMENT doc = FPDF_LoadDocument(pdf_path.c_str(), nullptr);
    if (!doc) {
        unsigned long err = FPDF_GetLastError();
        if (err == FPDF_ERR_PASSWORD)
            throw std::runtime_error("Encrypted PDF files are not supported: " + pdf_path);
        throw std::runtime_error("Cannot open PDF: " + pdf_path);
    }
    DocGuard doc_guard(doc);

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
        PageGuard page_guard(page);

        result.page_widths[p] = FPDF_GetPageWidth(page);
        result.page_heights[p] = FPDF_GetPageHeight(page);

        FPDF_TEXTPAGE text_page = FPDFText_LoadPage(page);
        if (text_page) {
            TextPageGuard tp_guard(text_page);
            result.all_lines[p] = extract_lines(text_page);
            if (opts.extract_tables) {
                auto segments = extract_line_segments(page);
                result.all_tables[p] = detect_tables(segments, text_page,
                    result.page_widths[p], result.page_heights[p]);
                auto text_tables = detect_text_tables(text_page,
                    result.all_tables[p],
                    result.page_widths[p], result.page_heights[p]);
                for (auto& tt : text_tables)
                    result.all_tables[p].push_back(std::move(tt));
            }
        }

        if (opts.extract_images) {
            result.all_images[p] = extract_images(doc, page, p, image_dir);

            // Fallback: render page as PNG for scanned/vector-only pages
            if (result.all_images[p].empty() && result.all_lines[p].empty()) {
                auto rendered = render_page_as_image(doc, page, p, image_dir);
                if (!rendered.data.empty())
                    result.all_images[p].push_back(std::move(rendered));
            }

            // Release pixel data after writing to disk
            if (!image_dir.empty()) {
                for (auto& img : result.all_images[p]) {
                    if (!img.saved_path.empty()) {
                        img.data.clear();
                        img.data.shrink_to_fit();
                    }
                }
            }
        }
    }

    // Extract bookmarks/outline for TOC
    collect_bookmarks(doc, nullptr, 0, result.bookmarks);

    result.stats.compute(result.all_lines);
    return result;
}

} // anonymous namespace

// ── Public API ───────────────────────────────────────────

static std::string format_bookmarks(const std::vector<BookmarkEntry>& bookmarks,
                                     bool plaintext) {
    if (bookmarks.empty()) return "";
    std::string out;
    for (auto& bm : bookmarks) {
        if (bm.title.empty()) continue;
        if (plaintext) {
            for (int i = 0; i < bm.level; i++) out += "  ";
            out += bm.title;
            if (bm.page >= 0)
                out += " (p." + std::to_string(bm.page + 1) + ")";
            out += "\n";
        } else {
            for (int i = 0; i < bm.level; i++) out += "  ";
            out += "- " + bm.title;
            if (bm.page >= 0)
                out += " *(p." + std::to_string(bm.page + 1) + ")*";
            out += "\n";
        }
    }
    return out;
}

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

    // Insert bookmark TOC if available
    if (!r.bookmarks.empty()) {
        if (!plaintext) full_md += "## Table of Contents\n\n";
        full_md += format_bookmarks(r.bookmarks, plaintext);
        full_md += "\n";
    }

    bool first = true;
    for (int p : page_indices) {
        if (p < 0 || p >= r.total_pages) continue;
        if (!first) {
            if (plaintext)
                full_md += "\n--- Page " + std::to_string(p + 1) + " ---\n\n";
            else
                full_md += "\n## Page " + std::to_string(p + 1) + "\n\n";
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
        chunk.page_number = p + 1;
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
