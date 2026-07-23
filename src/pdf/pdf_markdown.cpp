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

void collect_bookmarks(PdfDoc& doc, const PdfObj& node, int depth,
                        std::vector<BookmarkEntry>& out) {
    if (depth > 20) return;

    PdfObj item = doc.resolve(node);
    if (!item.is_dict()) return;

    // Process children
    auto first_ref = item.get("First");
    if (first_ref.is_none()) return;

    PdfObj child = doc.resolve(first_ref);
    while (child.is_dict()) {
        BookmarkEntry entry;
        entry.level = depth;

        auto& title = child.get("Title");
        if (title.is_str()) {
            // Check for UTF-16BE BOM
            if (title.str_val.size() >= 2 &&
                static_cast<uint8_t>(title.str_val[0]) == 0xFE &&
                static_cast<uint8_t>(title.str_val[1]) == 0xFF) {
                // UTF-16BE
                for (size_t i = 2; i + 1 < title.str_val.size(); i += 2) {
                    uint32_t cp = (static_cast<uint8_t>(title.str_val[i]) << 8) |
                                   static_cast<uint8_t>(title.str_val[i + 1]);
                    util::append_utf8(entry.title, cp);
                }
            } else {
                // PDFDocEncoding (similar to Latin-1)
                for (unsigned char c : title.str_val) {
                    util::append_utf8(entry.title, static_cast<uint32_t>(c));
                }
            }
        }

        // Get destination page
        auto dest = doc.resolve(child.get("Dest"));
        if (dest.is_arr() && !dest.arr.empty()) {
            auto page_ref = doc.resolve(dest.arr[0]);
            if (page_ref.is_ref()) {
                // Need to map page object number to page index
                entry.page = page_ref.ref_num; // Will remap later
            } else if (page_ref.is_int()) {
                entry.page = page_ref.as_int();
            }
        }
        if (entry.page < 0) {
            auto action = doc.resolve(child.get("A"));
            if (action.is_dict()) {
                auto& s = action.get("S");
                if (s.is_name() && s.str_val == "GoTo") {
                    auto d = doc.resolve(action.get("D"));
                    if (d.is_arr() && !d.arr.empty()) {
                        auto pr = doc.resolve(d.arr[0]);
                        if (pr.is_ref()) entry.page = pr.ref_num;
                        else if (pr.is_int()) entry.page = pr.as_int();
                    }
                }
            }
        }

        if (!entry.title.empty())
            out.push_back(std::move(entry));

        collect_bookmarks(doc, child, depth + 1, out);

        auto next = child.get("Next");
        if (next.is_none() || next.is_ref()) {
            if (next.is_ref()) child = doc.resolve(next);
            else break;
        } else {
            break;
        }
    }
}

// ── Annotation Extraction ────────────────────────────────


std::vector<AnnotEntry> extract_annotations(PdfDoc& doc, const PdfObj& page_obj, double page_h) {
    std::vector<AnnotEntry> result;

    auto annots_ref = page_obj.get("Annots");
    if (annots_ref.is_none()) return result;

    auto annots = doc.resolve(annots_ref);
    if (!annots.is_arr()) return result;

    for (auto& aref : annots.arr) {
        auto annot = doc.resolve(aref);
        if (!annot.is_dict()) continue;

        AnnotEntry entry;

        // Get subtype
        auto& subtype = annot.get("Subtype");
        if (subtype.is_name()) entry.subtype = subtype.str_val;

        // Get position from Rect
        auto& rect = annot.get("Rect");
        if (rect.is_arr() && rect.arr.size() >= 4)
            entry.y = rect.arr[3].as_num(); // top y

        // Extract text content (Contents key)
        auto& contents = annot.get("Contents");
        if (contents.is_str() && !contents.str_val.empty()) {
            auto& s = contents.str_val;
            // Detect UTF-16BE BOM
            if (s.size() >= 2 &&
                static_cast<uint8_t>(s[0]) == 0xFE &&
                static_cast<uint8_t>(s[1]) == 0xFF) {
                for (size_t i = 2; i + 1 < s.size(); i += 2) {
                    uint32_t cp = (static_cast<uint8_t>(s[i]) << 8) |
                                   static_cast<uint8_t>(s[i + 1]);
                    util::append_utf8(entry.text, cp);
                }
            } else {
                for (unsigned char c : s)
                    util::append_utf8(entry.text, static_cast<uint32_t>(c));
            }
        }

        // Extract URI for Link annotations
        if (entry.subtype == "Link") {
            auto action = doc.resolve(annot.get("A"));
            if (action.is_dict()) {
                auto& act_s = action.get("S");
                if (act_s.is_name() && act_s.str_val == "URI") {
                    auto& uri = action.get("URI");
                    if (uri.is_str()) entry.uri = uri.str_val;
                }
            }
        }

        // Only include annotations with actual content
        if (!entry.text.empty() || !entry.uri.empty())
            result.push_back(std::move(entry));
    }

    return result;
}

// ── Markdown Formatting ──────────────────────────────────

bool line_in_table(const TextLine& line, const std::vector<TableData>& tables) {
    for (auto& t : tables) {
        double t_bottom = std::min(t.y0, t.y1) - 10.0;
        double t_top = std::max(t.y0, t.y1) + 5.0;
        double t_left = std::min(t.x0, t.x1) - 15.0;
        double t_right = std::max(t.x0, t.x1) + 15.0;
        if (line.y_center >= t_bottom && line.y_center <= t_top) {
            if (line.x_left >= t_left && line.x_right <= t_right) {
                return true;
            }
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

    // Skip merging when lines have been column-reordered
    for (auto& l : lines)
        if (l.is_column_split) return lines;

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

        // Don't merge column-split lines
        bool has_col_split = false;
        for (auto gi : group)
            if (lines[gi].is_column_split) has_col_split = true;

        if (group.size() == 1 || has_col_split) {
            for (auto gi : group)
                merged.push_back(lines[gi]);
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
                    double gap = lines[group[k]].x_left - lines[group[k-1]].x_right;
                    double avg_font = (lines[group[k]].font_size +
                                       lines[group[k-1]].font_size) / 2.0;
                    double col_gap = std::max(avg_font * 6.0, 60.0);
                    double word_gap = std::max(avg_font * 0.5, 4.0);
                    if (gap > col_gap)
                        m.text += "\n";
                    else if (gap > word_gap)
                        m.text += " ";
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

// Standalone page-number footer lines: "- 3 -", "- ⅰ -", "- iv -"
static bool is_page_number_footer(const std::string& text) {
    std::string s;
    for (char c : text)
        if (c != ' ' && c != '\t') s += c;
    if (s.size() < 3 || s.front() != '-' || s.back() != '-') return false;
    std::string mid = s.substr(1, s.size() - 2);
    if (mid.empty() || mid.size() > 12) return false;

    bool all_digits = true;
    for (char c : mid)
        if (!std::isdigit(static_cast<unsigned char>(c))) { all_digits = false; break; }
    if (all_digits) return true;

    bool all_roman = true;
    for (char c : mid) {
        char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (std::string("ivxlcdm").find(lc) == std::string::npos) { all_roman = false; break; }
    }
    if (all_roman) return true;

    // Unicode roman numerals U+2160–217F (UTF-8: E2 85 A0..BF)
    if (mid.size() % 3 == 0) {
        bool all_uroman = true;
        for (size_t i = 0; i + 2 < mid.size() + 1; i += 3) {
            if (static_cast<unsigned char>(mid[i]) != 0xE2 ||
                static_cast<unsigned char>(mid[i + 1]) != 0x85 ||
                static_cast<unsigned char>(mid[i + 2]) < 0xA0) { all_uroman = false; break; }
        }
        if (all_uroman) return true;
    }
    return false;
}

// Depth of a leading section number: "2.1 ..." → 2, "4.2.1 ..." → 3, else 0
static int section_number_depth(const std::string& text) {
    size_t i = 0;
    int depth = 0;
    while (i < text.size()) {
        size_t start = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) i++;
        if (i == start) return 0;
        depth++;
        if (i < text.size() && text[i] == '.') { i++; continue; }
        break;
    }
    if (i >= text.size() || (text[i] != ' ' && text[i] != '\t')) return 0;
    return depth;
}

std::string page_to_markdown(const std::vector<TextLine>& raw_lines,
                              const FontStats& stats,
                              const std::vector<ImageData>& images,
                              const std::vector<double>& image_y_pos,
                              const std::vector<double>& image_x_pos,
                              const std::vector<TableData>& tables,
                              const std::vector<AnnotEntry>& annots = {},
                              double col_boundary = 0,
                              const std::string& img_ref_prefix = "") {
    auto lines = merge_colinear_lines(raw_lines);

    // Detect if page has column-split lines (for image placement)
    bool has_columns = false;
    for (auto& l : lines)
        if (l.is_column_split) { has_columns = true; break; }

    // Bottom-most text line Y (page-number footers live there)
    double bottom_y = 1e9;
    for (auto& l : lines)
        if (l.y_center < bottom_y) bottom_y = l.y_center;

    std::string md;
    md.reserve(lines.size() * 80);

    // Build sorted insert lists for tables and images by Y position (top-first in PDF coords)
    struct InlineInsert {
        double y_pos;
        double x_pos;
        size_t idx;
        bool is_image; // false = table, true = image
    };
    std::vector<InlineInsert> inserts;
    for (size_t ti = 0; ti < tables.size(); ti++) {
        double tx = (tables[ti].x0 + tables[ti].x1) / 2.0;
        inserts.push_back({std::max(tables[ti].y0, tables[ti].y1), tx, ti, false});
    }
    for (size_t ii = 0; ii < images.size(); ii++) {
        double y = (ii < image_y_pos.size()) ? image_y_pos[ii] : 0.0;
        double x = (ii < image_x_pos.size()) ? image_x_pos[ii] : 0.0;
        inserts.push_back({y, x, ii, true});
    }
    std::sort(inserts.begin(), inserts.end(),
              [](const InlineInsert& a, const InlineInsert& b) { return a.y_pos > b.y_pos; });

    size_t next_insert = 0;

    // For column-split pages, defer inserts whose X doesn't match current text column
    std::vector<size_t> deferred_inserts;

    auto emit_insert = [&](const InlineInsert& ins) {
        if (ins.is_image) {
            auto& img = images[ins.idx];
            std::string ref = img.name + "." + img.format;
            if (!img.saved_path.empty()) {
                auto slash = img.saved_path.find_last_of('/');
                ref = (slash != std::string::npos)
                    ? img.saved_path.substr(slash + 1)
                    : img.saved_path;
            }
            md += "\n![" + img.name + "](" + img_ref_prefix + ref + ")\n";
        } else {
            auto& tbl = tables[ins.idx];
            if (!tbl.title.empty())
                md += "\n" + tbl.title + "\n";
            md += "\n";
            md += format_table(tbl);
            md += "\n";
        }
    };

    auto flush_inserts = [&](double y_threshold, bool is_left_col = false, bool is_right_col = false) {
        while (next_insert < inserts.size() &&
               inserts[next_insert].y_pos >= y_threshold) {
            auto& ins = inserts[next_insert];
            // On column-split pages, defer inserts from the other column
            if (col_boundary > 0 && ins.x_pos > 0) {
                bool ins_is_left = ins.x_pos < col_boundary;
                if ((is_left_col && !ins_is_left) || (is_right_col && ins_is_left)) {
                    deferred_inserts.push_back(next_insert);
                    next_insert++;
                    continue;
                }
            }
            emit_insert(ins);
            next_insert++;
        }
    };

    for (size_t i = 0; i < lines.size(); i++) {
        const auto& l = lines[i];

        bool is_left = l.is_column_split && (l.x_left + l.x_right) / 2.0 < col_boundary;
        bool is_right = l.is_column_split && !is_left;
        flush_inserts(l.y_center, is_left, is_right);

        // Emit deferred inserts (from other column) when their Y matches current line
        for (auto it = deferred_inserts.begin(); it != deferred_inserts.end(); ) {
            auto& ins = inserts[*it];
            bool ins_is_left = ins.x_pos < col_boundary;
            if (ins.y_pos >= l.y_center &&
                ((is_left && ins_is_left) || (is_right && !ins_is_left) || !l.is_column_split)) {
                emit_insert(ins);
                it = deferred_inserts.erase(it);
            } else {
                ++it;
            }
        }

        if (line_in_table(l, tables)) continue;

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

        // Drop standalone page-number footers ("- 3 -") at the page bottom
        if ((i + 2 >= lines.size() || l.y_center <= bottom_y + 5.0) &&
            is_page_number_footer(l.text)) continue;

        int hlevel = stats.heading_level(l.font_size, l.is_bold);

        if (hlevel >= 3 && !l.is_bold && l.text.size() > 60)
            hlevel = 0;

        // Bold numbered section headings at body size ("2.1 ...", "4.2.1 ...")
        if (hlevel == 0 && l.is_bold && l.text.size() < 120) {
            int depth = section_number_depth(l.text);
            if (depth >= 2) hlevel = std::min(depth + 2, 6);
        }

        if (hlevel > 0) {
            // A heading that wraps onto several visual lines is one heading.
            // Fold in immediately following heading lines in the same column
            // whose vertical gap is a single line height — a wrapped
            // continuation, never a separate heading (those have body text, and
            // thus a larger gap, between them). Font size need only be close,
            // not identical, so a small-caps title (whose lines measure at two
            // sizes) still merges; the merged heading takes the strongest level.
            // Whether a line is written mostly in CJK script. Two heading lines
            // in different scripts (a Korean title stacked above its English
            // translation) are separate headings, not a wrapped one.
            auto is_cjk_line = [](const std::string& s) {
                size_t cjk = 0, letters = 0;
                for (size_t k = 0; k < s.size();) {
                    unsigned char c = s[k];
                    uint32_t cp;
                    int n;
                    if (c < 0x80)      { cp = c; n = 1; }
                    else if (c < 0xE0) { cp = c & 0x1F; n = 2; }
                    else if (c < 0xF0) { cp = c & 0x0F; n = 3; }
                    else               { cp = c & 0x07; n = 4; }
                    for (int b = 1; b < n && k + b < s.size(); b++)
                        cp = (cp << 6) | (s[k + b] & 0x3F);
                    k += n;
                    if (cp > 0x3040) { letters++; if (cp >= 0xAC00 && cp <= 0xD7A3) cjk++; }
                    else if ((cp | 0x20) - 'a' < 26u) letters++;
                }
                return letters > 0 && cjk * 2 >= letters;
            };
            std::string heading = l.text;
            bool head_cjk = is_cjk_line(l.text);
            while (i + 1 < lines.size()) {
                const auto& nx = lines[i + 1];
                int nx_level = stats.heading_level(nx.font_size, nx.is_bold);
                bool same_col = nx.is_column_split == l.is_column_split &&
                    ((nx.x_left + nx.x_right) / 2.0 < col_boundary) ==
                    ((l.x_left + l.x_right) / 2.0 < col_boundary);
                double gap = std::fabs(nx.y_center - lines[i].y_center);
                double ratio = std::min(nx.font_size, l.font_size) /
                               std::max(nx.font_size, l.font_size);
                if (nx_level == 0 || line_in_table(nx, tables) || !same_col ||
                    nx.is_bold != l.is_bold || ratio < 0.75 ||
                    is_cjk_line(nx.text) != head_cjk ||
                    gap > l.font_size * 1.5)
                    break;
                heading += ' ';
                heading += nx.text;
                hlevel = std::min(hlevel, nx_level);
                i++;
            }
            if (i > 0) md += '\n';
            for (int h = 0; h < hlevel; h++) md += '#';
            md += ' ';
            md += heading;
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

    // Flush remaining tables and images
    flush_inserts(-1e9);

    // Emit deferred inserts (images/tables from other column)
    for (auto di : deferred_inserts)
        emit_insert(inserts[di]);

    // Append annotations (links, text notes) at end of page
    if (!annots.empty()) {
        bool has_links = false, has_notes = false;
        for (auto& a : annots) {
            if (!a.uri.empty()) has_links = true;
            if (!a.text.empty() && a.subtype != "Link") has_notes = true;
        }
        if (has_links) {
            md += "\n**Links:**\n";
            for (auto& a : annots) {
                if (a.uri.empty()) continue;
                if (!a.text.empty())
                    md += "- [" + a.text + "](" + a.uri + ")\n";
                else
                    md += "- <" + a.uri + ">\n";
            }
        }
        if (has_notes) {
            md += "\n**Notes:**\n";
            for (auto& a : annots) {
                if (a.text.empty() || a.subtype == "Link") continue;
                md += "> " + a.text + "\n\n";
            }
        }
    }

    return md;
}

// ── Core Extraction Logic ────────────────────────────────


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

std::string result_to_markdown(ExtractResult& r, const ConvertOptions& opts) {
    std::vector<int> page_indices;
    if (opts.pages.empty()) {
        for (int i = 0; i < r.total_pages; i++) page_indices.push_back(i);
    } else {
        page_indices = opts.pages;
    }

    bool plaintext = (opts.format == OutputFormat::PLAINTEXT);

    std::string full_md;
    full_md.reserve(64 * 1024);

    if (!r.bookmarks.empty()) {
        if (!plaintext) full_md += "## Table of Contents\n\n";
        full_md += format_bookmarks(r.bookmarks, plaintext);
        full_md += "\n";
    }

    for (int p : page_indices) {
        if (p < 0 || p >= r.total_pages) continue;
        if (!full_md.empty()) full_md += '\n';
        full_md += "--- Page " + std::to_string(p + 1) + " ---\n\n";
        std::string page_md = page_to_markdown(r.all_lines[p], r.stats,
                                                r.all_images[p], r.all_image_y[p], r.all_image_x[p],
                                                r.all_tables[p],
                                                p < (int)r.all_annots.size() ? r.all_annots[p] : std::vector<AnnotEntry>{},
                                                r.col_boundaries[p],
                                                opts.image_ref_prefix);
        if (plaintext)
            full_md += util::strip_markdown(page_md);
        else
            full_md += page_md;
    }
    return full_md;
}

// Build one page's chunk from the extracted result. Moves that page's images
// out of `r`; the caller is expected to release the page's other per-page data
// afterwards when streaming.
static PageChunk build_page_chunk(ExtractResult& r, const ConvertOptions& opts,
                                  bool plaintext, int p) {
    PageChunk chunk;
    chunk.page_number = p + 1;
    chunk.page_width = r.page_widths[p];
    chunk.page_height = r.page_heights[p];
    chunk.body_font_size = r.stats.body_size;
    std::string page_md = page_to_markdown(r.all_lines[p], r.stats,
                                            r.all_images[p], r.all_image_y[p], r.all_image_x[p],
                                            r.all_tables[p],
                                            p < (int)r.all_annots.size() ? r.all_annots[p] : std::vector<AnnotEntry>{},
                                            r.col_boundaries[p],
                                            opts.image_ref_prefix);
    chunk.text = plaintext ? util::strip_markdown(page_md) : page_md;

    // Rendering above already consumed the tables, so move the rows out rather
    // than copying them into the chunk.
    for (auto& td : r.all_tables[p])
        chunk.tables.push_back(std::move(td.rows));

    chunk.images = std::move(r.all_images[p]);
    return chunk;
}

// Streaming primitive shared by the eager and streaming entry points: build each
// page's chunk and hand it to `sink`. When release_per_page is true (streaming),
// each page's remaining buffers are freed right after emit so the residual
// footprint shrinks as the stream advances. The eager collector passes false —
// it destroys `r` immediately after, so the per-page clears would be pure
// overhead (a measurable few-percent on glibc). Output is identical either way;
// document-wide font stats (r.stats) are already computed, so heading detection
// matches. `sink` returning false stops early.
void stream_result_chunks(ExtractResult& r, const ConvertOptions& opts,
                          const PageSink& sink, bool release_per_page) {
    std::vector<int> page_indices;
    if (opts.pages.empty()) {
        for (int i = 0; i < r.total_pages; i++) page_indices.push_back(i);
    } else {
        page_indices = opts.pages;
    }

    bool plaintext = (opts.format == OutputFormat::PLAINTEXT);

    for (int p : page_indices) {
        if (p < 0 || p >= r.total_pages) continue;
        PageChunk chunk = build_page_chunk(r, opts, plaintext, p);

        if (release_per_page) {
            r.all_lines[p] = {};
            r.all_tables[p] = {};
            if (p < (int)r.all_annots.size()) r.all_annots[p] = {};
            r.all_image_y[p] = {};
            r.all_image_x[p] = {};
        }

        if (!sink(std::move(chunk))) return;
    }
}

std::vector<PageChunk> result_to_chunks(ExtractResult& r,
                                               const ConvertOptions& opts) {
    // Eager collection is a thin wrapper over the streaming primitive (single
    // source of truth), with per-page release disabled — see above.
    std::vector<PageChunk> chunks;
    stream_result_chunks(r, opts, [&](PageChunk&& c) {
        chunks.push_back(std::move(c));
        return true;
    }, /*release_per_page=*/false);
    return chunks;
}


}} // namespace jdoc::pdf_detail
