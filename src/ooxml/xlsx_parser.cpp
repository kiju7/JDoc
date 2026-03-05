// XLSX parser implementation
// Parses ZIP-based .xlsx files using pugixml for XML processing

#include "ooxml/xlsx_parser.h"
#include "xml_utils.h"
#include "common/file_utils.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <sys/stat.h>

namespace jdoc {

// ── Constructor ─────────────────────────────────────────

XlsxParser::XlsxParser(ZipReader& zip) : zip_(zip) {
    parse_shared_strings();
    parse_workbook();
    parse_workbook_rels();
}

// ── Shared strings (xl/sharedStrings.xml) ───────────────

void XlsxParser::parse_shared_strings() {
    if (!zip_.has_entry("xl/sharedStrings.xml")) return;

    auto data = zip_.read_entry("xl/sharedStrings.xml");
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size())) return;

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
    if (!doc.load_buffer(data.data(), data.size())) return;

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
    if (!doc.load_buffer(data.data(), data.size())) return;

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

// ── Sheet parsing ───────────────────────────────────────

XlsxParser::SheetData XlsxParser::parse_sheet(const SheetInfo& info) {
    SheetData sheet;
    sheet.name = info.name;

    if (info.file_path.empty() || !zip_.has_entry(info.file_path)) {
        return sheet;
    }

    auto data = zip_.read_entry(info.file_path);
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size())) return sheet;

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
                // Numeric or formula result
                auto v_node = xml_child(cell, "v");
                if (v_node) {
                    value = xml_text_content(v_node);
                }
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

    auto entries = zip_.entries_with_prefix("xl/media/");
    for (auto* entry : entries) {
        std::string ext = util::get_extension(entry->name);
        std::string fmt = util::image_format_from_ext(ext);

        ImageData img;
        img.page_number = 1;
        img.name = util::get_filename(entry->name);
        img.format = fmt;

        if (!opts.image_output_dir.empty()) {
            mkdir(opts.image_output_dir.c_str(), 0755);
            std::string out_path =
                opts.image_output_dir + "/" + img.name;
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

        if (i > 0) out << "\n---\n\n";

        // Sheet header
        std::string display_name = sheet.name.empty()
            ? ("Sheet " + std::to_string(sheet_num))
            : sheet.name;
        out << "## " << display_name << "\n\n";

        if (sheet.cells.empty()) {
            out << "*Empty sheet*\n\n";
            continue;
        }

        out << format_sheet_as_table(sheet) << "\n";
    }

    return out.str();
}

// ── to_chunks ───────────────────────────────────────────

std::vector<PageChunk> XlsxParser::to_chunks(
    const ConvertOptions& opts) {

    std::vector<PageChunk> chunks;
    auto all_images = extract_images(opts);

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

        // Attach images to first chunk
        if (chunks.empty() && !all_images.empty()) {
            chunk.images = std::move(all_images);
        }

        chunks.push_back(std::move(chunk));
    }

    return chunks;
}

} // namespace jdoc
