// XLSX parser implementation
// Parses ZIP-based .xlsx files using pugixml for XML processing

#include "ooxml/xlsx_parser.h"
#include "xml_utils.h"
#include "common/file_utils.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace jdoc {

// ── Constructor ─────────────────────────────────────────

XlsxParser::XlsxParser(ZipReader& zip) : zip_(zip) {
    parse_shared_strings();
    parse_workbook();
    parse_workbook_rels();
    parse_styles();
}

// ── Shared strings (xl/sharedStrings.xml) ───────────────

void XlsxParser::parse_shared_strings() {
    if (!zip_.has_entry("xl/sharedStrings.xml")) return;

    auto data = zip_.read_entry("xl/sharedStrings.xml");
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size(), pugi::parse_default | pugi::parse_ws_pcdata)) return;

    // Each <si> element contains one shared string
    // Text can be in <t> directly or in <r><t> runs
    std::vector<pugi::xml_node> si_nodes;
    xml_find_all(doc, "si", si_nodes);

    shared_strings_.reserve(si_nodes.size());

    for (auto& si : si_nodes) {
        // Try direct <t> child first
        auto t_node = xml_child(si, "t");
        if (t_node) {
            shared_strings_.push_back(xml_text_content(t_node));
            continue;
        }

        // Fall back to rich text: <r><t> runs
        std::string text;
        std::vector<pugi::xml_node> t_nodes;
        xml_find_all(si, "t", t_nodes);
        for (auto& t : t_nodes) {
            text += xml_text_content(t);
        }
        shared_strings_.push_back(std::move(text));
    }
}

// ── Workbook (xl/workbook.xml) ──────────────────────────

void XlsxParser::parse_workbook() {
    if (!zip_.has_entry("xl/workbook.xml")) return;

    auto data = zip_.read_entry("xl/workbook.xml");
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size(), pugi::parse_default | pugi::parse_ws_pcdata)) return;

    // Find <sheets><sheet> elements
    std::vector<pugi::xml_node> sheet_nodes;
    xml_find_all(doc, "sheet", sheet_nodes);

    for (auto& sheet : sheet_nodes) {
        SheetInfo info;
        info.name = xml_attr(sheet, "name");
        info.r_id = xml_attr(sheet, "id");

        // Also check for r:id attribute
        if (info.r_id.empty()) {
            for (auto attr = sheet.first_attribute(); attr; attr = attr.next_attribute()) {
                std::string aname = attr.name();
                // Match "r:id" or any attribute ending with ":id" that looks like rId
                if (aname.find(":id") != std::string::npos) {
                    std::string val = attr.value();
                    if (val.find("rId") == 0) {
                        info.r_id = val;
                        break;
                    }
                }
            }
        }

        sheets_.push_back(std::move(info));
    }
}

// ── Workbook relationships (xl/_rels/workbook.xml.rels) ─

void XlsxParser::parse_workbook_rels() {
    const std::string rels_path = "xl/_rels/workbook.xml.rels";
    if (!zip_.has_entry(rels_path)) return;

    auto data = zip_.read_entry(rels_path);
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size(), pugi::parse_default | pugi::parse_ws_pcdata)) return;

    // Build rId -> target map
    std::map<std::string, std::string> id_to_target;
    std::vector<pugi::xml_node> rels;
    xml_find_all(doc, "Relationship", rels);

    for (auto& rel : rels) {
        const char* id = xml_attr(rel, "Id");
        const char* target = xml_attr(rel, "Target");
        if (id[0] && target[0]) {
            id_to_target[id] = target;
        }
    }

    // Resolve sheet file paths
    for (auto& sheet : sheets_) {
        auto it = id_to_target.find(sheet.r_id);
        if (it != id_to_target.end()) {
            std::string target = it->second;
            // Targets are relative to xl/ directory
            if (target.find("xl/") != 0 && target.find('/') == std::string::npos) {
                target = "xl/" + target;
            } else if (target.find("xl/") != 0 &&
                       target.find("worksheets/") == 0) {
                target = "xl/" + target;
            }
            sheet.file_path = target;
        }
    }
}

// ── Styles (xl/styles.xml) — number format parsing ──────

void XlsxParser::parse_styles() {
    if (!zip_.has_entry("xl/styles.xml")) return;

    auto data = zip_.read_entry("xl/styles.xml");
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size(), pugi::parse_default | pugi::parse_ws_pcdata)) return;

    // Parse custom number formats: <numFmts><numFmt numFmtId="..." formatCode="..."/>
    std::vector<pugi::xml_node> fmt_nodes;
    xml_find_all(doc, "numFmt", fmt_nodes);
    for (auto& node : fmt_nodes) {
        const char* id_str = xml_attr(node, "numFmtId");
        const char* code = xml_attr(node, "formatCode");
        if (id_str[0] && code[0]) {
            custom_num_fmts_[std::atoi(id_str)] = code;
        }
    }

    // Parse cell formats: <cellXfs><xf numFmtId="..."/>
    std::vector<pugi::xml_node> xf_nodes;
    xml_find_all(doc, "xf", xf_nodes);

    // cellXfs entries come after cellStyleXfs entries.
    // Find the <cellXfs> parent to get the right set.
    std::vector<pugi::xml_node> cell_xfs_nodes;
    xml_find_all(doc, "cellXfs", cell_xfs_nodes);

    if (!cell_xfs_nodes.empty()) {
        auto cellXfs = cell_xfs_nodes[0];
        for (auto xf = cellXfs.first_child(); xf; xf = xf.next_sibling()) {
            const char* name = xf.name();
            const char* colon = strchr(name, ':');
            const char* local = colon ? colon + 1 : name;
            if (strcmp(local, "xf") != 0) continue;

            const char* fmt_id = xml_attr(xf, "numFmtId");
            xf_num_fmt_ids_.push_back(fmt_id[0] ? std::atoi(fmt_id) : 0);
        }
    }
}

// ── Number formatting ───────────────────────────────────

bool XlsxParser::is_date_format(int fmt_id, const std::string& fmt_code) {
    // Built-in date/time format IDs
    if ((fmt_id >= 14 && fmt_id <= 22) ||
        (fmt_id >= 27 && fmt_id <= 36) ||
        (fmt_id >= 45 && fmt_id <= 47) ||
        (fmt_id >= 50 && fmt_id <= 58)) {
        return true;
    }

    // Check custom format code for date/time patterns
    if (fmt_code.empty()) return false;
    std::string lower;
    for (char c : fmt_code) lower += std::tolower(static_cast<unsigned char>(c));

    // Skip escaped chars and quoted strings
    bool in_quote = false;
    for (size_t i = 0; i < lower.size(); i++) {
        if (lower[i] == '"') { in_quote = !in_quote; continue; }
        if (in_quote) continue;
        if (lower[i] == '\\') { i++; continue; }
        char ch = lower[i];
        if (ch == 'y' || ch == 'd') return true;
        // 'm' is date only if not preceded by 'h' or followed by 's'
        if (ch == 'h') return true;
        if (ch == 's' && i > 0) return true;
    }
    return false;
}

std::string XlsxParser::serial_to_date(double serial) {
    // Excel epoch: day 1 = 1900-01-01, with Lotus 1-2-3 bug (day 60 = Feb 29, 1900)
    int days = static_cast<int>(serial);
    if (days < 1) return "0000-00-00";

    // Lotus bug: Excel treats 1900 as a leap year.
    // Day 60 = "Feb 29, 1900" which doesn't exist.
    // For days > 60, subtract 1 to correct. For days <= 60, keep as-is.
    if (days > 60) days--;

    // days is now 1-based from 1900-01-01
    days--; // make 0-based

    int y = 1900;
    while (true) {
        bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        int year_days = leap ? 366 : 365;
        if (days < year_days) break;
        days -= year_days;
        y++;
    }

    bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int m = 0;
    for (m = 0; m < 12; m++) {
        int md = month_days[m] + (m == 1 && leap ? 1 : 0);
        if (days < md) break;
        days -= md;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m + 1, days + 1);
    return buf;
}

std::string XlsxParser::serial_to_time(double serial) {
    double frac = serial - static_cast<int>(serial);
    int total_secs = static_cast<int>(frac * 86400 + 0.5);
    int h = total_secs / 3600;
    int m = (total_secs % 3600) / 60;
    int s = total_secs % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    return buf;
}

std::string XlsxParser::format_number(const std::string& raw_value,
                                       int style_idx) const {
    if (style_idx < 0 || style_idx >= static_cast<int>(xf_num_fmt_ids_.size()))
        return raw_value;

    int fmt_id = xf_num_fmt_ids_[style_idx];
    if (fmt_id == 0) return raw_value; // General

    // Look up format code
    std::string fmt_code;
    auto it = custom_num_fmts_.find(fmt_id);
    if (it != custom_num_fmts_.end()) {
        fmt_code = it->second;
    }

    double val = std::atof(raw_value.c_str());

    // Date/time formats
    if (is_date_format(fmt_id, fmt_code)) {
        // Pure time formats (IDs 18-21, 45-47)
        bool is_time_only = (fmt_id >= 18 && fmt_id <= 21) ||
                            (fmt_id >= 45 && fmt_id <= 47);
        if (is_time_only) {
            return serial_to_time(val);
        }
        // Date (possibly with time)
        std::string result = serial_to_date(val);
        double frac = val - static_cast<int>(val);
        if (frac > 0.0001 && (fmt_id == 22 ||
            fmt_code.find('h') != std::string::npos ||
            fmt_code.find('H') != std::string::npos)) {
            result += " " + serial_to_time(val);
        }
        return result;
    }

    // Percentage formats (IDs 9-10)
    if (fmt_id == 9) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.0f%%", val * 100.0);
        return buf;
    }
    if (fmt_id == 10) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f%%", val * 100.0);
        return buf;
    }

    // Check custom format for percentage
    if (!fmt_code.empty() && fmt_code.find('%') != std::string::npos) {
        // Count decimal places after '0' before '%'
        int decimals = 0;
        bool after_dot = false;
        for (char c : fmt_code) {
            if (c == '.') after_dot = true;
            else if (after_dot && c == '0') decimals++;
            else if (c == '%') break;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%.*f%%", decimals, val * 100.0);
        return buf;
    }

    // Number with comma grouping (IDs 3-4, 37-40)
    if (fmt_id == 3 || fmt_id == 37 || fmt_id == 38) {
        // #,##0
        long long ival = static_cast<long long>(val + (val >= 0 ? 0.5 : -0.5));
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld", ival);
        std::string s = buf;
        // Insert commas
        int start = (s[0] == '-') ? 1 : 0;
        int len = static_cast<int>(s.size()) - start;
        if (len > 3) {
            for (int i = len - 3; i > 0; i -= 3) {
                s.insert(start + i, 1, ',');
            }
        }
        return s;
    }
    if (fmt_id == 4 || fmt_id == 39 || fmt_id == 40) {
        // #,##0.00
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f", val);
        std::string s = buf;
        auto dot = s.find('.');
        int start = (s[0] == '-') ? 1 : 0;
        int int_len = static_cast<int>(dot) - start;
        if (int_len > 3) {
            for (int i = int_len - 3; i > 0; i -= 3) {
                s.insert(start + i, 1, ',');
            }
        }
        return s;
    }

    // Scientific notation (ID 11)
    if (fmt_id == 11) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2E", val);
        return buf;
    }

    // Fraction formats (IDs 12-13) — just show decimal
    if (fmt_id == 12 || fmt_id == 13) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.4g", val);
        return buf;
    }

    // Fixed decimal formats (IDs 1-2)
    if (fmt_id == 1) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.0f", val);
        return buf;
    }
    if (fmt_id == 2) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f", val);
        return buf;
    }

    // Currency formats (IDs 5-8)
    if (fmt_id >= 5 && fmt_id <= 8) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f", val);
        std::string s = buf;
        auto dot = s.find('.');
        int start = (s[0] == '-') ? 1 : 0;
        int int_len = static_cast<int>(dot) - start;
        if (int_len > 3) {
            for (int i = int_len - 3; i > 0; i -= 3) {
                s.insert(start + i, 1, ',');
            }
        }
        return s;
    }

    // For unknown custom formats, try to detect date pattern
    if (!fmt_code.empty()) {
        std::string lower;
        for (char c : fmt_code) lower += std::tolower(static_cast<unsigned char>(c));
        if (lower.find('#') != std::string::npos ||
            lower.find('0') != std::string::npos) {
            // Numeric format — just clean up trailing zeros
            char buf[64];
            int decimals = 0;
            bool after_dot = false;
            for (char c : fmt_code) {
                if (c == '.') after_dot = true;
                else if (after_dot && (c == '0' || c == '#')) decimals++;
            }
            snprintf(buf, sizeof(buf), "%.*f", decimals, val);
            return buf;
        }
    }

    return raw_value;
}

// ── Cell reference parsing ──────────────────────────────

int XlsxParser::column_to_index(const std::string& col) {
    int result = 0;
    for (char c : col) {
        result = result * 26 + (std::toupper(static_cast<unsigned char>(c)) - 'A' + 1);
    }
    return result - 1; // 0-based
}

std::pair<int, int> XlsxParser::parse_cell_ref(const std::string& ref) {
    // Split "AB123" into column letters "AB" and row number "123"
    std::string col_str;
    std::string row_str;

    for (char c : ref) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            col_str += c;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            row_str += c;
        }
    }

    int col = col_str.empty() ? 0 : column_to_index(col_str);
    int row = row_str.empty() ? 0 : (std::atoi(row_str.c_str()) - 1); // 0-based

    return {col, row};
}

// ── Comments parsing ────────────────────────────────────

std::map<std::string, std::string> XlsxParser::parse_comments(
    const SheetInfo& info) {

    std::map<std::string, std::string> comments;
    if (info.file_path.empty()) return comments;

    // Find sheet rels to locate comments file
    auto slash = info.file_path.rfind('/');
    if (slash == std::string::npos) return comments;
    std::string dir = info.file_path.substr(0, slash);
    std::string base = info.file_path.substr(slash + 1);
    std::string rels_path = dir + "/_rels/" + base + ".rels";

    if (!zip_.has_entry(rels_path)) return comments;
    auto rels_data = zip_.read_entry(rels_path);
    pugi::xml_document rels_doc;
    if (!rels_doc.load_buffer(rels_data.data(), rels_data.size())) return comments;

    // Find comments target
    std::string comments_path;
    std::vector<pugi::xml_node> rel_nodes;
    xml_find_all(rels_doc, "Relationship", rel_nodes);
    for (auto& rel : rel_nodes) {
        const char* type = xml_attr(rel, "Type");
        const char* target = xml_attr(rel, "Target");
        if (!target[0]) continue;
        std::string type_str = type;
        if (type_str.find("/comments") != std::string::npos) {
            comments_path = target;
            if (comments_path.find("../") == 0)
                comments_path = "xl/" + comments_path.substr(3);
            else if (comments_path.find("xl/") != 0)
                comments_path = dir + "/" + comments_path;
            break;
        }
    }

    if (comments_path.empty() || !zip_.has_entry(comments_path))
        return comments;

    auto data = zip_.read_entry(comments_path);
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size(), pugi::parse_default | pugi::parse_ws_pcdata)) return comments;

    // Parse <commentList><comment ref="A1"><text><t>...</t></text></comment>
    std::vector<pugi::xml_node> comment_nodes;
    xml_find_all(doc, "comment", comment_nodes);

    for (auto& node : comment_nodes) {
        const char* ref = xml_attr(node, "ref");
        if (!ref[0]) continue;

        // Get text from <text> child -> <t> or <r><t> runs
        std::string text;
        auto text_node = xml_child(node, "text");
        if (text_node) {
            std::vector<pugi::xml_node> t_nodes;
            xml_find_all(text_node, "t", t_nodes);
            for (auto& t : t_nodes) {
                text += xml_text_content(t);
            }
        }

        if (!text.empty()) {
            comments[ref] = text;
        }
    }

    return comments;
}

// ── Sheet parsing ───────────────────────────────────────

XlsxParser::SheetData XlsxParser::parse_sheet(const SheetInfo& info) {
    SheetData sheet;
    sheet.name = info.name;

    if (info.file_path.empty() || !zip_.has_entry(info.file_path)) {
        return sheet;
    }

    auto data = zip_.read_entry(info.file_path);
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size(), pugi::parse_default | pugi::parse_ws_pcdata)) return sheet;

    // Parse comments for this sheet
    auto comments = parse_comments(info);

    // Find <sheetData> element
    std::vector<pugi::xml_node> sheet_data_nodes;
    xml_find_all(doc, "sheetData", sheet_data_nodes);
    if (sheet_data_nodes.empty()) return sheet;

    auto sheetData = sheet_data_nodes[0];

    // Walk <row> elements
    for (auto row_node = sheetData.first_child(); row_node;
         row_node = row_node.next_sibling()) {

        const char* rname = row_node.name();
        const char* rcolon = strchr(rname, ':');
        const char* rlocal = rcolon ? rcolon + 1 : rname;
        if (strcmp(rlocal, "row") != 0) continue;

        // Walk <c> (cell) elements within this row
        for (auto cell = row_node.first_child(); cell;
             cell = cell.next_sibling()) {

            const char* cname = cell.name();
            const char* ccolon = strchr(cname, ':');
            const char* clocal = ccolon ? ccolon + 1 : cname;
            if (strcmp(clocal, "c") != 0) continue;

            // Get cell reference (e.g. "A1")
            const char* ref = xml_attr(cell, "r");
            if (!ref[0]) continue;

            auto [col, row] = parse_cell_ref(ref);

            // Determine cell value based on type
            const char* cell_type = xml_attr(cell, "t");
            const char* style_str = xml_attr(cell, "s");
            int style_idx = style_str[0] ? std::atoi(style_str) : -1;
            std::string value;

            if (std::string(cell_type) == "s") {
                // Shared string reference
                auto v_node = xml_child(cell, "v");
                if (v_node) {
                    std::string idx_str = xml_text_content(v_node);
                    if (!idx_str.empty()) {
                        int idx = std::atoi(idx_str.c_str());
                        if (idx >= 0 &&
                            idx < static_cast<int>(shared_strings_.size())) {
                            value = shared_strings_[idx];
                        }
                    }
                }
            } else if (std::string(cell_type) == "inlineStr") {
                // Inline string: <is><t>text</t></is>
                auto is_node = xml_child(cell, "is");
                if (is_node) {
                    auto t_node = xml_child(is_node, "t");
                    if (t_node) {
                        value = xml_text_content(t_node);
                    } else {
                        // Rich text inside <is>
                        std::vector<pugi::xml_node> t_nodes;
                        xml_find_all(is_node, "t", t_nodes);
                        for (auto& t : t_nodes) {
                            value += xml_text_content(t);
                        }
                    }
                }
            } else if (std::string(cell_type) == "b") {
                // Boolean
                auto v_node = xml_child(cell, "v");
                if (v_node) {
                    std::string v = xml_text_content(v_node);
                    value = (v == "1") ? "TRUE" : "FALSE";
                }
            } else if (std::string(cell_type) == "e") {
                // Error
                auto v_node = xml_child(cell, "v");
                if (v_node) {
                    value = xml_text_content(v_node);
                }
            } else {
                // Numeric or formula result — apply number formatting
                auto v_node = xml_child(cell, "v");
                if (v_node) {
                    std::string raw = xml_text_content(v_node);
                    value = format_number(raw, style_idx);
                }
            }

            // Append comment if exists for this cell
            auto cit = comments.find(ref);
            if (cit != comments.end()) {
                if (!value.empty()) value += " ";
                value += "[" + cit->second + "]";
            }

            if (!value.empty()) {
                // Sanitize for markdown table
                for (auto& ch : value) {
                    if (ch == '|') ch = '/';
                    if (ch == '\n') ch = ' ';
                }
                sheet.cells[row][col] = value;
                sheet.max_row = std::max(sheet.max_row, row);
                sheet.max_col = std::max(sheet.max_col, col);
            }
        }
    }

    return sheet;
}

// ── Format sheet data as markdown table ─────────────────

std::string XlsxParser::format_sheet_as_table(const SheetData& sheet,
                                                int max_rows) {
    if (sheet.cells.empty()) return "";

    int total_rows = sheet.max_row + 1;
    int total_cols = sheet.max_col + 1;
    bool truncated = total_rows > max_rows;
    int display_rows = std::min(total_rows, max_rows);

    std::ostringstream out;

    // Header row (row 0)
    out << "|";
    for (int c = 0; c < total_cols; ++c) {
        auto row_it = sheet.cells.find(0);
        std::string cell;
        if (row_it != sheet.cells.end()) {
            auto col_it = row_it->second.find(c);
            if (col_it != row_it->second.end()) {
                cell = col_it->second;
            }
        }
        out << " " << cell << " |";
    }
    out << "\n";

    // Separator
    out << "|";
    for (int c = 0; c < total_cols; ++c) {
        out << " --- |";
    }
    out << "\n";

    // Data rows
    for (int r = 1; r < display_rows; ++r) {
        out << "|";
        for (int c = 0; c < total_cols; ++c) {
            auto row_it = sheet.cells.find(r);
            std::string cell;
            if (row_it != sheet.cells.end()) {
                auto col_it = row_it->second.find(c);
                if (col_it != row_it->second.end()) {
                    cell = col_it->second;
                }
            }
            out << " " << cell << " |";
        }
        out << "\n";
    }

    if (truncated) {
        out << "\n*... truncated at " << max_rows
            << " rows (total: " << total_rows << " rows)*\n";
    }

    return out.str();
}

// ── Image extraction ────────────────────────────────────

std::vector<ImageData> XlsxParser::extract_images(
    const ConvertOptions& opts) {

    std::vector<ImageData> images;
    if (!opts.extract_images) return images;

    std::set<std::string> extracted;

    // Per-sheet image extraction via drawing relationships
    for (size_t i = 0; i < sheets_.size(); ++i) {
        int sheet_num = static_cast<int>(i) + 1;
        const auto& info = sheets_[i];
        if (info.file_path.empty()) continue;

        // Parse sheet rels to find drawing reference
        auto slash = info.file_path.rfind('/');
        if (slash == std::string::npos) continue;
        std::string dir = info.file_path.substr(0, slash);
        std::string base = info.file_path.substr(slash + 1);
        std::string sheet_rels = dir + "/_rels/" + base + ".rels";

        if (!zip_.has_entry(sheet_rels)) continue;
        auto rels_data = zip_.read_entry(sheet_rels);
        pugi::xml_document rels_doc;
        if (!rels_doc.load_buffer(rels_data.data(), rels_data.size())) continue;

        // Find drawing targets (type ends with /drawing)
        std::vector<pugi::xml_node> rel_nodes;
        xml_find_all(rels_doc, "Relationship", rel_nodes);

        for (auto& rel : rel_nodes) {
            const char* type = xml_attr(rel, "Type");
            const char* target = xml_attr(rel, "Target");
            if (!target[0]) continue;

            std::string type_str = type;
            if (type_str.find("/drawing") == std::string::npos) continue;

            // Resolve drawing path
            std::string drawing_path = target;
            if (drawing_path.find("../") == 0) {
                drawing_path = "xl/" + drawing_path.substr(3);
            } else if (drawing_path.find("xl/") != 0) {
                drawing_path = dir + "/" + drawing_path;
            }

            if (!zip_.has_entry(drawing_path)) continue;

            // Parse drawing rels for image targets
            auto draw_slash = drawing_path.rfind('/');
            if (draw_slash == std::string::npos) continue;
            std::string draw_dir = drawing_path.substr(0, draw_slash);
            std::string draw_base = drawing_path.substr(draw_slash + 1);
            std::string draw_rels = draw_dir + "/_rels/" + draw_base + ".rels";

            if (!zip_.has_entry(draw_rels)) continue;
            auto draw_rels_data = zip_.read_entry(draw_rels);
            pugi::xml_document draw_rels_doc;
            if (!draw_rels_doc.load_buffer(draw_rels_data.data(), draw_rels_data.size())) continue;

            std::map<std::string, std::string> draw_rel_map;
            std::vector<pugi::xml_node> draw_rel_nodes;
            xml_find_all(draw_rels_doc, "Relationship", draw_rel_nodes);
            for (auto& dr : draw_rel_nodes) {
                const char* id = xml_attr(dr, "Id");
                const char* tgt = xml_attr(dr, "Target");
                if (id[0] && tgt[0]) {
                    std::string full = tgt;
                    if (full.find("../") == 0) full = "xl/" + full.substr(3);
                    else if (full.find("xl/") != 0) full = draw_dir + "/" + full;
                    draw_rel_map[id] = full;
                }
            }

            // Parse drawing XML to find blip references
            auto draw_data = zip_.read_entry(drawing_path);
            pugi::xml_document draw_doc;
            if (!draw_doc.load_buffer(draw_data.data(), draw_data.size())) continue;

            std::vector<pugi::xml_node> blips;
            xml_find_all(draw_doc, "blip", blips);
            for (auto& blip : blips) {
                const char* embed = xml_attr(blip, "embed");
                if (!embed[0]) continue;
                auto mit = draw_rel_map.find(embed);
                if (mit == draw_rel_map.end()) continue;
                const std::string& media_path = mit->second;
                if (!zip_.has_entry(media_path) || extracted.count(media_path)) continue;
                extracted.insert(media_path);

                ImageData img;
                img.page_number = sheet_num;
                img.name = util::get_filename(media_path);
                img.format = util::image_format_from_ext(util::get_extension(media_path));

                if (!opts.image_output_dir.empty()) {
                    util::ensure_dir(opts.image_output_dir);
                    std::string out_path = opts.image_output_dir + "/" + img.name;
                    for (auto& e : zip_.entries()) {
                        if (e.name == media_path) {
                            if (zip_.extract_entry_to_file(e, out_path))
                                img.saved_path = out_path;
                            break;
                        }
                    }
                } else {
                    img.data = zip_.read_entry(media_path);
                }
                images.push_back(std::move(img));
            }
        }
    }

    // Fallback: pick up any remaining files in xl/media/ not yet extracted
    auto entries = zip_.entries_with_prefix("xl/media/");
    for (auto* entry : entries) {
        if (extracted.count(entry->name)) continue;
        extracted.insert(entry->name);

        ImageData img;
        img.page_number = 1;
        img.name = util::get_filename(entry->name);
        img.format = util::image_format_from_ext(util::get_extension(entry->name));

        if (!opts.image_output_dir.empty()) {
            util::ensure_dir(opts.image_output_dir);
            std::string out_path = opts.image_output_dir + "/" + img.name;
            if (zip_.extract_entry_to_file(*entry, out_path)) {
                img.saved_path = out_path;
            }
        } else {
            img.data = zip_.read_entry(*entry);
        }
        images.push_back(std::move(img));
    }
    return images;
}

// ── to_markdown ─────────────────────────────────────────

std::string XlsxParser::to_markdown(const ConvertOptions& opts) {
    std::ostringstream out;

    for (size_t i = 0; i < sheets_.size(); ++i) {
        int sheet_num = static_cast<int>(i) + 1;

        // Filter by requested pages (sheets treated as pages)
        if (!opts.pages.empty()) {
            bool found = false;
            for (int p : opts.pages) {
                if (p == sheet_num) { found = true; break; }
            }
            if (!found) continue;
        }

        auto sheet = parse_sheet(sheets_[i]);

        if (i > 0) out << "\n";
        out << "--- Page " << sheet_num << " ---\n\n";

        if (sheet.cells.empty()) {
            out << "*Empty sheet*\n\n";
            continue;
        }

        out << format_sheet_as_table(sheet) << "\n";
    }

    // Extract and reference images
    auto images = extract_images(opts);
    if (!images.empty()) {
        for (auto& img : images) {
            if (opts.extract_images)
                out << "![" << img.name << "](" << opts.image_ref_prefix << img.name << ")\n\n";
            else
                out << "![" << img.name << "](embedded:" << img.name << ")\n\n";
        }
    }

    return out.str();
}

// ── to_chunks ───────────────────────────────────────────

std::vector<PageChunk> XlsxParser::to_chunks(
    const ConvertOptions& opts) {

    std::vector<PageChunk> chunks;
    // Always enumerate images so we can reference them in text
    ConvertOptions img_opts = opts;
    img_opts.extract_images = true;
    auto all_images = extract_images(img_opts);

    for (size_t i = 0; i < sheets_.size(); ++i) {
        int sheet_num = static_cast<int>(i) + 1;

        // Filter by requested pages (sheets treated as pages)
        if (!opts.pages.empty()) {
            bool found = false;
            for (int p : opts.pages) {
                if (p == sheet_num) { found = true; break; }
            }
            if (!found) continue;
        }

        auto sheet = parse_sheet(sheets_[i]);

        PageChunk chunk;
        chunk.page_number = sheet_num;

        std::ostringstream text;
        std::string display_name = sheet.name.empty()
            ? ("Sheet " + std::to_string(sheet_num))
            : sheet.name;
        text << "## " << display_name << "\n\n";

        if (!sheet.cells.empty()) {
            text << format_sheet_as_table(sheet) << "\n";

            // Build structured table data for the chunk
            // Convert sparse grid to dense 2D vector
            if (opts.extract_tables) {
                int total_rows = sheet.max_row + 1;
                int total_cols = sheet.max_col + 1;
                int display_rows = std::min(total_rows, 10000);

                std::vector<std::vector<std::string>> table;
                table.reserve(display_rows);

                for (int r = 0; r < display_rows; ++r) {
                    std::vector<std::string> row;
                    row.reserve(total_cols);
                    auto row_it = sheet.cells.find(r);
                    for (int c = 0; c < total_cols; ++c) {
                        if (row_it != sheet.cells.end()) {
                            auto col_it = row_it->second.find(c);
                            if (col_it != row_it->second.end()) {
                                row.push_back(col_it->second);
                                continue;
                            }
                        }
                        row.push_back("");
                    }
                    table.push_back(std::move(row));
                }

                chunk.tables.push_back(std::move(table));
            }
        } else {
            text << "*Empty sheet*\n\n";
        }

        chunk.text = text.str();
        chunks.push_back(std::move(chunk));
    }

    // Distribute images to their corresponding sheet chunks
    if (!all_images.empty() && !chunks.empty()) {
        for (auto& img : all_images) {
            PageChunk* target = &chunks[0];
            for (auto& c : chunks) {
                if (c.page_number == img.page_number) { target = &c; break; }
            }
            std::string ref = img.saved_path.empty()
                ? "embedded:" + img.name
                : opts.image_ref_prefix + img.name;
            target->text += "![" + img.name + "](" + ref + ")\n\n";
            target->images.push_back(std::move(img));
        }
    }

    return chunks;
}

} // namespace jdoc
