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

// A PDF text string is either UTF-16BE behind a byte-order mark or, failing
// that, PDFDocEncoding — which agrees with Latin-1 over the range producers
// actually use for titles and file names.
static std::string decode_pdf_text(const std::string& raw) {
    std::string out;
    if (raw.size() >= 2 && static_cast<uint8_t>(raw[0]) == 0xFE &&
        static_cast<uint8_t>(raw[1]) == 0xFF) {
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            uint32_t cp = (static_cast<uint8_t>(raw[i]) << 8) |
                           static_cast<uint8_t>(raw[i + 1]);
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < raw.size()) {
                uint32_t low = (static_cast<uint8_t>(raw[i + 2]) << 8) |
                                static_cast<uint8_t>(raw[i + 3]);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                }
            }
            util::append_utf8(out, cp);
        }
    } else {
        for (unsigned char c : raw)
            util::append_utf8(out, static_cast<uint32_t>(c));
    }
    return out;
}

// ── Embedded file attachments ────────────────────────────

// Read one file specification: the name it presents, and the size its embedded
// stream declares. Returns false for a spec that names no file.
static bool read_filespec(PdfDoc& doc, const PdfObj& fs_ref,
                          AttachmentEntry& out) {
    PdfObj fs = doc.resolve(fs_ref);
    if (!fs.is_dict()) return false;

    // /UF is the Unicode name and outranks the others; the platform-specific
    // keys are what older producers wrote.
    for (const char* key : {"UF", "F", "DOS", "Mac", "Unix"}) {
        auto& v = fs.get(key);
        if (v.is_str() && !v.str_val.empty()) {
            out.name = decode_pdf_text(v.str_val);
            break;
        }
    }
    if (out.name.empty()) return false;

    auto& desc = fs.get("Desc");
    if (desc.is_str()) out.desc = decode_pdf_text(desc.str_val);

    PdfObj ef = doc.resolve(fs.get("EF"));
    if (ef.is_dict()) {
        for (const char* key : {"UF", "F"}) {
            PdfObj stream = doc.resolve(ef.get(key));
            if (!stream.is_dict()) continue;
            // /Params /Size is the uncompressed length; /Length is the encoded
            // one, which is the closest stand-in when Params is missing.
            PdfObj params = doc.resolve(stream.get("Params"));
            int64_t size = params.is_dict() ? params.get("Size").as_int() : 0;
            if (size <= 0) size = stream.get("Length").as_int();
            if (size > 0) out.size = static_cast<uint64_t>(size);
            break;
        }
    }
    return true;
}

// Name trees hold their entries in /Names leaves and branch through /Kids.
// `budget` bounds the total nodes visited, not just the depth: a /Kids array
// whose entries point back at their own node fans out 2^depth times, so a
// depth cap alone lets a malformed file spin for hours.
static uint64_t ref_key(const PdfObj& ref) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(ref.ref_num)) << 32) |
           static_cast<uint32_t>(ref.ref_gen);
}

static void walk_embedded_files(
    PdfDoc& doc, const PdfObj& node_ref, int depth, int& budget,
    std::unordered_set<uint64_t>& visited_nodes,
    std::unordered_set<uint64_t>& visited_specs,
    std::vector<AttachmentEntry>& out) {
    if (depth > 32 || budget <= 0 || out.size() >= 4096) return;
    // A malformed /Kids tree can point back to an ancestor or repeat a large
    // subtree many times. Object identity makes that an O(unique nodes) walk;
    // the budget remains as a bound for direct (non-reference) dictionaries.
    if (node_ref.is_ref() && !visited_nodes.insert(ref_key(node_ref)).second)
        return;
    budget--;
    PdfObj node = doc.resolve(node_ref);
    if (!node.is_dict()) return;

    PdfObj names = doc.resolve(node.get("Names"));
    if (names.is_arr()) {
        // Flat [key value key value ...]; the file specs are the odd slots.
        for (size_t i = 1; i < names.arr.size(); i += 2) {
            const PdfObj& spec = names.arr[i];
            // De-duplicate repeated registrations by object identity. Distinct
            // attachments are allowed to share a leaf filename and must both
            // remain visible to the caller.
            if (spec.is_ref() && !visited_specs.insert(ref_key(spec)).second)
                continue;
            AttachmentEntry e;
            if (read_filespec(doc, spec, e)) out.push_back(std::move(e));
            if (out.size() >= 4096) return;
        }
    }

    PdfObj kids = doc.resolve(node.get("Kids"));
    if (kids.is_arr())
        for (auto& kid : kids.arr)
            walk_embedded_files(doc, kid, depth + 1, budget, visited_nodes,
                                visited_specs, out);
}

void collect_attachments(PdfDoc& doc, const PdfObj& root,
                         std::vector<AttachmentEntry>& out) {
    PdfObj names = doc.resolve(root.get("Names"));
    if (!names.is_dict()) return;
    int budget = 4096;
    std::unordered_set<uint64_t> visited_nodes;
    std::unordered_set<uint64_t> visited_specs;
    walk_embedded_files(doc, names.get("EmbeddedFiles"), 0, budget,
                        visited_nodes, visited_specs, out);
}

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
        if (title.is_str()) entry.title = decode_pdf_text(title.str_val);

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


std::vector<AnnotEntry> extract_annotations(PdfDoc& doc, const PdfObj& page_obj, double page_h,
                                            const double* view_ctm) {
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

        // Get position from Rect (top y in viewing coordinates)
        auto& rect = annot.get("Rect");
        if (rect.is_arr() && rect.arr.size() >= 4) {
            if (view_ctm) {
                // Rotated/offset page: the visual top is the max transformed y
                // over the rect corners.
                double x0 = rect.arr[0].as_num(), y0 = rect.arr[1].as_num();
                double x1 = rect.arr[2].as_num(), y1 = rect.arr[3].as_num();
                double top = -1e18;
                const double cx[4] = {x0, x1, x0, x1};
                const double cy[4] = {y0, y0, y1, y1};
                for (int i = 0; i < 4; i++) {
                    double vx, vy;
                    transform_point(view_ctm, cx[i], cy[i], vx, vy);
                    (void)vx;
                    if (vy > top) top = vy;
                }
                entry.y = top;
            } else {
                entry.y = rect.arr[3].as_num(); // top y
            }
        }

        // Extract text content (Contents key)
        auto& contents = annot.get("Contents");
        if (contents.is_str() && !contents.str_val.empty())
            entry.text = decode_pdf_text(contents.str_val);

        // A paperclip annotation carries a file the page only gestures at. Its
        // note, if any, describes the file without naming it, so the name goes
        // in beside the note — otherwise the page reads as if nothing is there.
        if (entry.subtype == "FileAttachment") {
            AttachmentEntry att;
            if (read_filespec(doc, annot.get("FS"), att)) {
                if (!entry.text.empty()) entry.text += " ";
                entry.text += "[" + util::to_single_line(att.name) + "]";
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

// Whitespace-stripped copy, for order-preserving containment checks.
static std::string squash_ws(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') out += c;
    return out;
}

// Per-table concatenation of all cell text (row-major, whitespace stripped).
// A line that geometrically falls inside a table region is only dropped from
// the prose flow when its characters actually made it into the table's cells;
// otherwise a detector that claimed a region but captured a subset of its
// text would silently delete the rest (an author block swallowed by a phantom
// column once lost six of eight names this way).
std::vector<std::string> table_captured_text(const std::vector<TableData>& tables) {
    std::vector<std::string> joined;
    joined.reserve(tables.size());
    for (auto& t : tables) {
        std::string j;
        for (auto& row : t.rows)
            for (auto& cell : row) j += cell;
        joined.push_back(squash_ws(j));
    }
    return joined;
}

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

// line_in_table plus capture check: true only when the line lies in a table
// region AND its text is present in that table's cells (see
// table_captured_text). A geometric hit whose text was not captured keeps the
// line in the prose flow instead of losing it.
bool line_swallowed_by_table(const TextLine& line,
                             const std::vector<TableData>& tables,
                             const std::vector<std::string>& captured) {
    std::string lt = squash_ws(line.text);
    for (size_t ti = 0; ti < tables.size(); ti++) {
        auto& t = tables[ti];
        double t_bottom = std::min(t.y0, t.y1) - 10.0;
        double t_top = std::max(t.y0, t.y1) + 5.0;
        double t_left = std::min(t.x0, t.x1) - 15.0;
        double t_right = std::max(t.x0, t.x1) + 15.0;
        if (line.y_center < t_bottom || line.y_center > t_top) continue;
        bool geo = false;
        if (line.x_left >= t_left && line.x_right <= t_right) {
            geo = true;
        } else {
            double overlap_l = std::max(line.x_left, t_left);
            double overlap_r = std::min(line.x_right, t_right);
            if (overlap_r > overlap_l) {
                double overlap = overlap_r - overlap_l;
                double line_width = line.x_right - line.x_left;
                if (line_width > 0 && overlap >= line_width * 0.6) geo = true;
            }
        }
        if (!geo) continue;
        // Fully-filler lines ('|', spaces) carry no text to preserve.
        if (lt.empty()) return true;
        if (ti < captured.size() &&
            captured[ti].find(lt) != std::string::npos)
            return true;
        // geometric hit but text not captured: check other tables too before
        // deciding to keep the line.
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

        // Lines sharing a y band may still belong to different runs: a
        // vertical caption's page-space midpoint can land on a body line's
        // baseline, and joining them by x_left welds the caption into the
        // sentence. Merge within a direction only, keeping each direction's
        // lines in the order they were first seen so page order is unchanged.
        std::vector<int16_t> dirs;
        for (auto gi : group)
            if (std::find(dirs.begin(), dirs.end(), lines[gi].rot) == dirs.end())
                dirs.push_back(lines[gi].rot);

        for (int16_t dir : dirs) {
        std::vector<size_t> same;
        for (auto gi : group)
            if (lines[gi].rot == dir) same.push_back(gi);

        if (same.size() == 1 || has_col_split) {
            for (auto gi : same)
                merged.push_back(lines[gi]);
        } else {
            std::sort(same.begin(), same.end(), [&](size_t a, size_t b) {
                return lines[a].x_left < lines[b].x_left;
            });
            const std::vector<size_t>& group = same;
            TextLine m;
            m.y_center = lines[group[0]].y_center;
            m.x_left = lines[group[0]].x_left;
            m.font_size = lines[group[0]].font_size;
            m.is_bold = lines[group[0]].is_bold;
            m.is_italic = lines[group[0]].is_italic;
            m.rot = dir;
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

// Section-number prefix "1", "1.", "2.1", "4.1.2)": numeric segments, an
// optional closing '.' or ')' and required whitespace (ASCII or U+3000)
// before the title text. A large first segment is a year or a data value,
// never a section number.
SectionNumber parse_section_number(const std::string& text) {
    SectionNumber none;
    size_t i = 0;
    int depth = 0;
    long first = 0;
    for (;;) {
        size_t start = i;
        long val = 0;
        while (i < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[i]))) {
            val = val * 10 + (text[i] - '0');
            i++;
        }
        if (i == start) return none;
        if (depth == 0) first = val;
        depth++;
        if (i + 1 < text.size() && text[i] == '.' &&
            std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
            i++;
            continue;
        }
        break;
    }
    // Sections count from 1 ("0.3" is a chart value); paren enumerations
    // ("1) 항목") are list items, not sections, in this corpus.
    if (first < 1 || first > 99 || depth > 4) return none;
    SectionNumber sn;
    if (i < text.size() && text[i] == '.') {
        sn.closed = true;
        i++;
    }
    size_t ws = 0;
    while (i < text.size()) {
        unsigned char c = text[i];
        if (c == ' ' || c == '\t') { i++; ws++; continue; }
        if (c == 0xE3 && i + 2 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0x80 &&
            static_cast<unsigned char>(text[i + 2]) == 0x80) {
            i += 3;
            ws++;
            continue;
        }
        break;
    }
    if (ws == 0 || i >= text.size()) return none;
    sn.depth = depth;
    sn.text_pos = i;
    return sn;
}

// Offset past a superscript footnote marker glued ahead of a section
// number ("1)1. 서론"): an affiliation mark that merged into the heading
// line in x order. Zero when the line has no such prefix.
size_t glued_mark_offset(const std::string& text) {
    size_t d = 0;
    while (d < text.size() && std::isdigit(static_cast<unsigned char>(text[d]))) d++;
    if (d >= 1 && d <= 2 && d < text.size() && text[d] == ')' &&
        parse_section_number(text.substr(d + 1)).depth > 0)
        return d + 1;
    return 0;
}

// "TABLE 2.6.2 ..." / "그림 3 ..." caption headings never wrap onto a
// following line — what follows a caption is its subtitle or the table.
static bool is_caption_heading(const std::string& text) {
    size_t i = 0;
    auto match = [&](const char* w) {
        size_t j = i, k = 0;
        while (w[k] && j < text.size() &&
               std::tolower(static_cast<unsigned char>(text[j])) == w[k]) {
            j++; k++;
        }
        if (w[k]) return false;
        i = j;
        return true;
    };
    bool head = match("table") || match("figure") || match("fig");
    if (!head) {
        if (text.compare(0, 3, "\xed\x91\x9c") == 0) { i = 3; head = true; }
        else if (text.compare(0, 6, "\xea\xb7\xb8\xeb\xa6\xbc") == 0) {
            i = 6;
            head = true;
        }
    }
    if (!head) return false;
    while (i < text.size() && (text[i] == ' ' || text[i] == '.')) i++;
    return i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]));
}

static size_t utf8_length(const std::string& s) {
    size_t n = 0;
    for (char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) n++;
    return n;
}

// Standalone structural keywords the corpus ground truth treats as
// top-level sections regardless of their type size.
bool is_section_keyword(const std::string& text) {
    std::string key;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = text[i];
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == 0xE3 && i + 2 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0x80 &&
            static_cast<unsigned char>(text[i + 2]) == 0x80) {
            i += 3;
            continue;
        }
        key += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : text[i];
        i++;
    }
    while (!key.empty() && (key.back() == ':' || key.back() == '.'))
        key.pop_back();
    return key == "abstract" || key == "references" ||
           key == "acknowledgments" || key == "acknowledgements" ||
           key == "appendix" ||
           key == "\xec\x9a\x94\xec\x95\xbd" ||                           // 요약
           key == "\xec\xb4\x88\xeb\xa1\x9d" ||                           // 초록
           key == "\xea\xb5\xad\xeb\xac\xb8\xec\x9a\x94\xec\x95\xbd" ||   // 국문요약
           key == "\xec\xb0\xb8\xea\xb3\xa0\xeb\xac\xb8\xed\x97\x8c";     // 참고문헌
}

// Name-list punctuation that betrays an author line under the title:
// interpunct-joined Korean names, affiliation daggers and asterisks, mail
// addresses, a trailing comma continuing the author list.
static bool looks_like_author_line(const std::string& s) {
    std::string t = s;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    if (!t.empty() && t.back() == ',') return true;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = s[i];
        if (c == '@' || c == '*') return true;
        if (c == 0xE2 && i + 2 < s.size()) {
            unsigned char c1 = s[i + 1], c2 = s[i + 2];
            if (c1 == 0x80 && (c2 == 0xA0 || c2 == 0xA1)) return true;  // dagger
            if (c1 == 0x8B && c2 == 0x85) return true;                  // U+22C5
            if (c1 == 0x88 && c2 == 0x97) return true;                  // U+2217
        }
    }
    return false;
}

// Whether a line is written mostly in CJK script. A Korean title stacked
// above its English translation is two headings, not one wrapped heading.
static bool line_is_mostly_cjk(const std::string& s) {
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
            cp = (cp << 6) | (static_cast<unsigned char>(s[k + b]) & 0x3F);
        k += n;
        if (cp > 0x3040) { letters++; if (cp >= 0xAC00 && cp <= 0xD7A3) cjk++; }
        else if ((cp | 0x20) - 'a' < 26u) letters++;
    }
    return letters > 0 && cjk * 2 >= letters;
}

// Whether the text holds at least one letter (Latin, Hangul, CJK) — a line
// of digits and punctuation (a page number, a lone value) is never a
// heading whatever its size.
static bool has_letter_codepoint(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        uint32_t cp;
        int n;
        if (c < 0x80)      { cp = c; n = 1; }
        else if (c < 0xE0) { cp = c & 0x1F; n = 2; }
        else if (c < 0xF0) { cp = c & 0x0F; n = 3; }
        else               { cp = c & 0x07; n = 4; }
        for (int b = 1; b < n && i + b < s.size(); b++)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + b]) & 0x3F);
        i += n;
        if ((cp | 0x20) - 'a' < 26u || cp >= 0x3040) return true;
    }
    return false;
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
    auto captured_cells = table_captured_text(tables);

    // Detect if page has column-split lines (for image placement)
    bool has_columns = false;
    for (auto& l : lines)
        if (l.is_column_split) { has_columns = true; break; }

    // Bottom-most text line Y (page-number footers live there)
    double bottom_y = 1e9;
    for (auto& l : lines)
        if (l.y_center < bottom_y) bottom_y = l.y_center;

    // ── Heading classification (pre-pass) ──
    // Decide every line's heading level up front; the emit loop and the
    // wrap-merge inside it both need the neighbour's verdict. Levels follow
    // the corpus-wide convention — document title → H1; top-level sections
    // (depth-1 numbers, structural keywords, other prominent text) → H2;
    // numbered subsections → H3 — because size ratios alone systematically
    // shifted every level one to two steps down.
    std::vector<int> line_level(lines.size(), 0);
    {
        auto side_of = [&](const TextLine& t) -> int {
            if (!t.is_column_split) return 0;
            return ((t.x_left + t.x_right) / 2.0 < col_boundary) ? 1 : 2;
        };
        double top_y = -1e9, bot_y = 1e9, page_l = 1e9, page_r = 0;
        for (auto& t : lines) {
            top_y = std::max(top_y, t.y_center);
            bot_y = std::min(bot_y, t.y_center);
            page_l = std::min(page_l, t.x_left);
            page_r = std::max(page_r, t.x_right);
        }
        // Typical line pitch: median of same-side consecutive gaps.
        std::vector<double> gap_samples;
        for (size_t i = 1; i < lines.size(); i++) {
            if (side_of(lines[i]) != side_of(lines[i - 1])) continue;
            double g = lines[i - 1].y_center - lines[i].y_center;
            if (g > 0.5 && g < stats.body_size * 4.0) gap_samples.push_back(g);
        }
        double median_gap = stats.body_size * 1.4;
        if (!gap_samples.empty()) {
            std::nth_element(gap_samples.begin(),
                             gap_samples.begin() + gap_samples.size() / 2,
                             gap_samples.end());
            median_gap = gap_samples[gap_samples.size() / 2];
        }
        auto gap_above = [&](size_t i) -> double {
            int side = side_of(lines[i]);
            for (size_t j = i; j-- > 0;) {
                if (side_of(lines[j]) != side) continue;
                return lines[j].y_center - lines[i].y_center;
            }
            return 1e9;   // first line of its column
        };
        // The page's document-title size: the largest heading-tier type in
        // the top region. Only its lines may become H1.
        double h1_size = 0;
        bool has_display = false;
        double max_bold_fs = 0;
        for (auto& t : lines) {
            if (t.rot != 0) continue;
            if (t.font_size >= stats.body_size * 1.3) {
                has_display = true;
                if (t.y_center >= bot_y + (top_y - bot_y) * 0.70)
                    h1_size = std::max(h1_size, t.font_size);
            }
            if (t.is_bold) max_bold_fs = std::max(max_bold_fs, t.font_size);
        }

        // Author lines run in blocks under the title; once one is
        // recognized, its same-size neighbours directly below are authors
        // too even without their own affiliation marks.
        double auth_y = -1e9, auth_fs = 0;
        int auth_side = -1, cover_script = -1;

        for (size_t i = 0; i < lines.size(); i++) {
            const auto& l = lines[i];
            if (l.rot != 0) continue;   // margin banners, rotated stamps
            if (!has_letter_codepoint(l.text)) continue;
            // Pipes come from running heads and table fragments, never from
            // a heading (they would break the markdown line anyway).
            if (l.text.find('|') != std::string::npos) continue;
            size_t cp_len = utf8_length(l.text);
            int base = stats.heading_level(l.font_size, l.is_bold);

            if (is_section_keyword(l.text) && cp_len <= 24) {
                line_level[i] = 2;
                continue;
            }

            SectionNumber sn = parse_section_number(l.text);
            size_t sn_off = 0;
            if (sn.depth == 0) {
                sn_off = glued_mark_offset(l.text);
                if (sn_off) sn = parse_section_number(l.text.substr(sn_off));
            }
            if (sn.depth > 0 && cp_len <= 60 &&
                l.y_center > bot_y + stats.body_size * 2.0) {
                const std::string title = l.text.substr(sn_off + sn.text_pos);
                char lastc = l.text.back();
                bool ok = has_letter_codepoint(title) &&
                          lastc != '.' && lastc != ',' && lastc != ';';
                if (ok && !sn.closed) {
                    // A bare number ("1 Introduction") is weaker evidence:
                    // demand a capitalized or CJK title word.
                    unsigned char c0 = title[0];
                    ok = (c0 >= 'A' && c0 <= 'Z') || c0 >= 0xE0;
                }
                if (ok && base == 0 && !l.is_bold) {
                    // Body-type numbered heading: isolation is the only
                    // signal left — extra space above, and no same-depth
                    // numbered line at normal pitch below (that is a list).
                    ok = gap_above(i) >= median_gap * 1.25;
                    if (ok && i + 1 < lines.size() &&
                        side_of(lines[i + 1]) == side_of(l) &&
                        l.y_center - lines[i + 1].y_center < median_gap * 1.2 &&
                        parse_section_number(lines[i + 1].text).depth ==
                            sn.depth)
                        ok = false;
                }
                if (ok) {
                    line_level[i] = (sn.depth == 1) ? 2 : 3;
                    continue;
                }
            }

            // Author lines are tracked whether or not they reach heading
            // size: the marked names ("Colin Raffel∗") are often set in a
            // different face from their unmarked neighbours, and the chain
            // must not restart between them.
            if (looks_like_author_line(l.text) ||
                (auth_side == side_of(l) &&
                 std::fabs(l.font_size - auth_fs) <= 0.6 &&
                 auth_y - l.y_center > -1.0 &&
                 auth_y - l.y_center <= median_gap * 2.5)) {
                auth_side = side_of(l);
                auth_fs = l.font_size;
                auth_y = l.y_center;
                continue;
            }
            if (base == 0) {
                // Cover pages set the title at the page's own body size, so
                // no size tier exists at all: the largest bold centered
                // block near the top is the document title; a second block
                // in the other script is its translation, one level down.
                if (!has_display && lines.size() <= 30 && l.is_bold &&
                    cp_len <= 40 && l.font_size >= max_bold_fs - 0.1 &&
                    l.y_center >= bot_y + (top_y - bot_y) * 0.55) {
                    double lm = l.x_left - page_l, rm = page_r - l.x_right;
                    if (std::fabs(lm - rm) < (page_r - page_l) * 0.15) {
                        int script = line_is_mostly_cjk(l.text) ? 1 : 0;
                        if (cover_script < 0) cover_script = script;
                        line_level[i] = (script == cover_script) ? 1 : 2;
                    }
                }
                continue;
            }
            if (cp_len > 80 && !l.is_bold) continue;
            bool is_h1 = h1_size > 0 && l.font_size >= h1_size - 0.1 &&
                         l.y_center >= bot_y + (top_y - bot_y) * 0.70;
            if (is_h1) {
                // Titles are set centered (or unmistakably large); a
                // left-aligned top section heading stays H2.
                double lmarg = l.x_left - page_l, rmarg = page_r - l.x_right;
                bool centered =
                    std::fabs(lmarg - rmarg) < (page_r - page_l) * 0.15 &&
                    l.x_right - l.x_left < (page_r - page_l) * 0.95;
                if (!centered && l.font_size < stats.body_size * 1.6)
                    is_h1 = false;
            }
            line_level[i] = is_h1 ? 1 : 2;
        }
    }

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
            const std::string ref =
                util::image_ref_name(img.name, img.format, img.saved_path);
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

        if (line_swallowed_by_table(l, tables, captured_cells)) continue;

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

        int hlevel = line_level[i];

        if (hlevel > 0) {
            // A heading that wraps onto several visual lines is one heading.
            // Fold in immediately following lines of the SAME level in the
            // same column whose vertical gap is about a line height — a
            // wrapped continuation, never a separate heading. Numbered and
            // keyword lines always start their own heading; two scripts (a
            // Korean title over its English translation) stay separate.
            // Font size need only be close, not identical, so a small-caps
            // title (whose lines measure at two sizes) still merges.
            std::string heading = l.text.substr(glued_mark_offset(l.text));
            bool head_cjk = line_is_mostly_cjk(l.text);
            while (!is_caption_heading(heading) && i + 1 < lines.size()) {
                const auto& nx = lines[i + 1];
                bool same_col = nx.is_column_split == l.is_column_split &&
                    ((nx.x_left + nx.x_right) / 2.0 < col_boundary) ==
                    ((l.x_left + l.x_right) / 2.0 < col_boundary);
                double gap = std::fabs(nx.y_center - lines[i].y_center);
                double ratio = std::min(nx.font_size, l.font_size) /
                               std::max(nx.font_size, l.font_size);
                if (line_level[i + 1] == 0 ||
                    parse_section_number(nx.text).depth > 0 ||
                    is_section_keyword(nx.text) ||
                    line_swallowed_by_table(nx, tables, captured_cells) ||
                    !same_col ||
                    nx.is_bold != l.is_bold || ratio < 0.75 ||
                    line_is_mostly_cjk(nx.text) != head_cjk ||
                    gap > l.font_size * 1.9)
                    break;
                heading += ' ';
                heading += nx.text;
                hlevel = std::min(hlevel, line_level[i + 1]);
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


// One line per attached file: its name, how big it is, and whatever the
// producer said about it. The bytes themselves stay in the PDF — this is a
// notice that they exist, not an extraction.
static std::string format_attachments(
    const std::vector<AttachmentEntry>& attachments) {
    std::string out;
    for (auto& a : attachments) {
        // /UF, /F and /Desc are producer-supplied and may carry newlines, so
        // they are flattened before reaching the list — a filespec named
        // "x\n\n## Table of Contents" would otherwise forge document
        // structure in the extracted text.
        out += "- " + util::to_single_line(a.name);
        if (a.size > 0) out += " (" + util::human_bytes(a.size) + ")";
        if (!a.desc.empty()) out += " — " + util::to_single_line(a.desc);
        out += "\n";
    }
    return out;
}

static std::string attachment_block(
    const std::vector<AttachmentEntry>& attachments, bool plaintext) {
    if (attachments.empty()) return "";
    std::string out;
    if (!plaintext) out = "## Attachments\n\n";
    out += format_attachments(attachments);
    out += "\n";
    return out;
}

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

    full_md += attachment_block(r.attachments, plaintext);

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
        if (p < (int)r.page_diags.size() && r.page_diags[p].images_failed > 0 &&
            !plaintext)
            full_md += "<!-- jdoc: " +
                       std::to_string(r.page_diags[p].images_failed) +
                       " image(s) failed to decode on this page -->\n";
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
    if (p < (int)r.page_diags.size() && r.page_diags[p].images_failed > 0) {
        chunk.degraded_images = r.page_diags[p].images_failed;
        if (!plaintext)
            chunk.text += "<!-- jdoc: " +
                          std::to_string(chunk.degraded_images) +
                          " image(s) failed to decode on this page -->\n";
    }

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
    bool first_chunk = true;

    for (int p : page_indices) {
        if (p < 0 || p >= r.total_pages) continue;
        PageChunk chunk = build_page_chunk(r, opts, plaintext, p);
        // Document-level attachments precede the first page in the whole-file
        // API. Mirror them into the first emitted chunk so eager, streaming and
        // whole-document consumers receive the same discoverability metadata.
        if (first_chunk) {
            std::string block = attachment_block(r.attachments, plaintext);
            if (!block.empty()) chunk.text.insert(0, block);
            first_chunk = false;
        }

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
