// DOCX parser implementation
// Parses ZIP-based .docx files using pugixml for XML processing

#include "ooxml/docx_parser.h"
#include "xml_utils.h"
#include "common/file_utils.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <sys/stat.h>
#include <sstream>

namespace jdoc {

// ── Constructor ─────────────────────────────────────────

DocxParser::DocxParser(ZipReader& zip) : zip_(zip) {
    parse_styles();
    parse_numbering();
    parse_relationships();
}

// ── Style parsing (word/styles.xml) ─────────────────────

void DocxParser::parse_styles() {
    if (!zip_.has_entry("word/styles.xml")) return;

    auto data = zip_.read_entry("word/styles.xml");
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size())) return;

    // Find all <w:style> elements
    std::vector<pugi::xml_node> styles;
    xml_find_all(doc, "style", styles);

    for (auto& style : styles) {
        const char* type = xml_attr(style, "type");
        if (std::string(type) != "paragraph") continue;

        const char* style_id = xml_attr(style, "styleId");
        if (!style_id[0]) continue;

        // Check for outlineLvl in pPr (language-independent heading detection)
        auto pPr = xml_child(style, "pPr");
        if (pPr) {
            auto outlineLvl = xml_child(pPr, "outlineLvl");
            if (outlineLvl) {
                const char* val = xml_attr(outlineLvl, "val");
                if (val[0]) {
                    int lvl = std::atoi(val);
                    if (lvl >= 0 && lvl <= 8) {
                        style_heading_map_[style_id] = lvl + 1; // 0-based to 1-based
                    }
                }
            }
        }
    }
}

// ── Numbering parsing (word/numbering.xml) ──────────────

void DocxParser::parse_numbering() {
    if (!zip_.has_entry("word/numbering.xml")) return;

    auto data = zip_.read_entry("word/numbering.xml");
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size())) return;

    // Parse abstractNum definitions
    std::vector<pugi::xml_node> abstract_nums;
    xml_find_all(doc, "abstractNum", abstract_nums);

    for (auto& an : abstract_nums) {
        const char* id_str = xml_attr(an, "abstractNumId");
        if (!id_str[0]) continue;
        int abstract_id = std::atoi(id_str);

        std::vector<pugi::xml_node> lvls;
        xml_find_all(an, "lvl", lvls);
        for (auto& lvl : lvls) {
            const char* ilvl_str = xml_attr(lvl, "ilvl");
            if (!ilvl_str[0]) continue;
            int ilvl = std::atoi(ilvl_str);

            auto numFmt = xml_child(lvl, "numFmt");
            if (numFmt) {
                const char* fmt = xml_attr(numFmt, "val");
                abstract_num_formats_[abstract_id][ilvl] = fmt;
            }
        }
    }

    // Parse num -> abstractNumId mapping
    std::vector<pugi::xml_node> nums;
    xml_find_all(doc, "num", nums);

    for (auto& num : nums) {
        const char* num_id_str = xml_attr(num, "numId");
        if (!num_id_str[0]) continue;
        int num_id = std::atoi(num_id_str);

        auto abstract_ref = xml_child(num, "abstractNumId");
        if (abstract_ref) {
            const char* val = xml_attr(abstract_ref, "val");
            if (val[0]) {
                num_to_abstract_[num_id] = std::atoi(val);
            }
        }
    }
}

// ── Relationship parsing (word/_rels/document.xml.rels) ─

void DocxParser::parse_relationships() {
    const std::string rels_path = "word/_rels/document.xml.rels";
    if (!zip_.has_entry(rels_path)) return;

    auto data = zip_.read_entry(rels_path);
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size())) return;

    std::vector<pugi::xml_node> rels;
    xml_find_all(doc, "Relationship", rels);

    for (auto& rel : rels) {
        const char* id = xml_attr(rel, "Id");
        const char* target = xml_attr(rel, "Target");
        if (id[0] && target[0]) {
            // Targets are relative to word/ directory
            std::string full_target = target;
            if (full_target.find("word/") != 0 &&
                full_target.find("http://") != 0 &&
                full_target.find("https://") != 0 &&
                full_target[0] != '/') {
                full_target = "word/" + full_target;
            }
            rel_targets_[id] = full_target;
        }
    }
}

// ── Image extraction ────────────────────────────────────

std::vector<ImageData> DocxParser::extract_images(
    const ConvertOptions& opts) {

    std::vector<ImageData> images;
    if (!opts.extract_images) return images;

    auto entries = zip_.entries_with_prefix("word/media/");
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

// ── Table formatting ────────────────────────────────────

std::string DocxParser::format_table(
    const std::vector<std::vector<std::string>>& rows) {

    if (rows.empty()) return "";

    // Determine column count from widest row
    size_t cols = 0;
    for (auto& row : rows) {
        cols = std::max(cols, row.size());
    }
    if (cols == 0) return "";

    std::ostringstream out;

    // Header row
    out << "|";
    for (size_t c = 0; c < cols; ++c) {
        const std::string& cell = (c < rows[0].size()) ? rows[0][c] : "";
        out << " " << cell << " |";
    }
    out << "\n";

    // Separator
    out << "|";
    for (size_t c = 0; c < cols; ++c) {
        out << " --- |";
    }
    out << "\n";

    // Data rows
    for (size_t r = 1; r < rows.size(); ++r) {
        out << "|";
        for (size_t c = 0; c < cols; ++c) {
            const std::string& cell = (c < rows[r].size()) ? rows[r][c] : "";
            out << " " << cell << " |";
        }
        out << "\n";
    }

    return out.str();
}

// ── Helpers for detecting image references in a paragraph ─

static std::string find_image_rid(const pugi::xml_node& para) {
    // Look for <w:drawing> -> <wp:inline> or <wp:anchor> -> <a:graphic>
    //   -> <a:graphicData> -> <pic:pic> -> <pic:blipFill> -> <a:blip r:embed="rIdX">
    std::vector<pugi::xml_node> blips;
    xml_find_all(para, "blip", blips);
    for (auto& blip : blips) {
        const char* embed = xml_attr(blip, "embed");
        if (embed[0]) return embed;
        const char* link = xml_attr(blip, "link");
        if (link[0]) return link;
    }

    // Also check VML: <w:pict> -> <v:shape> -> <v:imagedata r:id="rIdX">
    std::vector<pugi::xml_node> imagedata_nodes;
    xml_find_all(para, "imagedata", imagedata_nodes);
    for (auto& imgdata : imagedata_nodes) {
        const char* rid = xml_attr(imgdata, "id");
        if (rid[0]) return rid;
        // Check namespaced r:id attribute
        for (auto attr = imgdata.first_attribute(); attr; attr = attr.next_attribute()) {
            std::string aname = attr.name();
            if (aname == "r:id" || aname.find(":id") != std::string::npos) {
                std::string val = attr.value();
                if (val.find("rId") == 0) return val;
            }
        }
    }

    return "";
}

// ── Check for page break ────────────────────────────────

static bool has_page_break(const pugi::xml_node& para) {
    // Check <w:r><w:br w:type="page"/>
    std::vector<pugi::xml_node> brs;
    xml_find_all(para, "br", brs);
    for (auto& br : brs) {
        const char* type = xml_attr(br, "type");
        if (std::string(type) == "page") return true;
    }

    // Check <w:pPr><w:pageBreakBefore/>
    auto pPr = xml_child(para, "pPr");
    if (pPr) {
        auto pbb = xml_child(pPr, "pageBreakBefore");
        if (pbb) return true;
    }
    return false;
}

// ── Main document parsing ───────────────────────────────

static std::vector<std::vector<std::string>> parse_table_node(
    const pugi::xml_node& tbl) {

    std::vector<std::vector<std::string>> rows;
    std::vector<pugi::xml_node> tr_nodes;
    xml_find_all(tbl, "tr", tr_nodes);

    for (auto& tr : tr_nodes) {
        std::vector<std::string> row;
        std::vector<pugi::xml_node> tc_nodes;
        xml_find_all(tr, "tc", tc_nodes);

        for (auto& tc : tc_nodes) {
            // Collect all text from the cell's paragraphs
            std::string cell_text;
            std::vector<pugi::xml_node> paras;
            xml_find_all(tc, "p", paras);
            for (size_t i = 0; i < paras.size(); ++i) {
                if (i > 0) cell_text += " ";
                std::vector<pugi::xml_node> runs;
                xml_find_all(paras[i], "t", runs);
                for (auto& t : runs) {
                    cell_text += xml_text_content(t);
                }
            }
            // Replace pipe characters to avoid markdown table issues
            for (auto& ch : cell_text) {
                if (ch == '|') ch = '/';
                if (ch == '\n') ch = ' ';
            }
            row.push_back(cell_text);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

struct DocxElement {
    enum Type { PARAGRAPH, TABLE, PAGE_BREAK };
    Type type;
    // For PARAGRAPH:
    std::string text;
    int heading_level = 0;
    bool is_list = false;
    bool is_ordered = false;
    int list_level = 0;
    std::string image_rid;
    // For TABLE:
    std::vector<std::vector<std::string>> table_rows;
};

static std::vector<DocxElement> parse_body(
    const pugi::xml_node& body,
    const std::map<std::string, int>& style_heading_map,
    const std::map<int, int>& num_to_abstract,
    const std::map<int, std::map<int, std::string>>& abstract_num_formats) {

    std::vector<DocxElement> elements;

    for (auto child = body.first_child(); child; child = child.next_sibling()) {
        const char* name = child.name();
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;

        if (strcmp(local, "p") == 0) {
            // Check for page break before processing paragraph content
            if (has_page_break(child)) {
                DocxElement brk;
                brk.type = DocxElement::PAGE_BREAK;
                elements.push_back(brk);
            }

            DocxElement elem;
            elem.type = DocxElement::PARAGRAPH;

            // Extract paragraph properties
            auto pPr = xml_child(child, "pPr");
            if (pPr) {
                // Heading detection via style
                auto pStyle = xml_child(pPr, "pStyle");
                if (pStyle) {
                    const char* val = xml_attr(pStyle, "val");
                    auto it = style_heading_map.find(val);
                    if (it != style_heading_map.end()) {
                        elem.heading_level = it->second;
                    }
                }

                // List detection via numPr
                auto numPr = xml_child(pPr, "numPr");
                if (numPr) {
                    auto ilvl_node = xml_child(numPr, "ilvl");
                    auto numId_node = xml_child(numPr, "numId");
                    int ilvl = 0, numId = 0;
                    if (ilvl_node) {
                        const char* v = xml_attr(ilvl_node, "val");
                        if (v[0]) ilvl = std::atoi(v);
                    }
                    if (numId_node) {
                        const char* v = xml_attr(numId_node, "val");
                        if (v[0]) numId = std::atoi(v);
                    }

                    if (numId > 0) {
                        elem.is_list = true;
                        elem.list_level = ilvl;
                        elem.is_ordered = false; // default

                        auto abs_it = num_to_abstract.find(numId);
                        if (abs_it != num_to_abstract.end()) {
                            int abs_id = abs_it->second;
                            auto fmt_it = abstract_num_formats.find(abs_id);
                            if (fmt_it != abstract_num_formats.end()) {
                                auto lvl_it = fmt_it->second.find(ilvl);
                                if (lvl_it != fmt_it->second.end()) {
                                    const std::string& fmt = lvl_it->second;
                                    if (fmt == "decimal" ||
                                        fmt == "upperRoman" ||
                                        fmt == "lowerRoman" ||
                                        fmt == "upperLetter" ||
                                        fmt == "lowerLetter") {
                                        elem.is_ordered = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Extract runs with formatting
            std::ostringstream para_text;
            std::vector<pugi::xml_node> runs;
            xml_find_all(child, "r", runs);

            for (auto& run : runs) {
                auto rPr = xml_child(run, "rPr");
                bool bold = false, italic = false;
                if (rPr) {
                    bold = xml_child(rPr, "b") ? true : false;
                    italic = xml_child(rPr, "i") ? true : false;
                }

                // Collect text from <w:t> nodes in this run
                std::string run_text;
                std::vector<pugi::xml_node> t_nodes;
                xml_find_all(run, "t", t_nodes);
                for (auto& t : t_nodes) {
                    run_text += xml_text_content(t);
                }

                if (run_text.empty()) continue;

                // Apply formatting only for non-heading paragraphs
                if (elem.heading_level == 0) {
                    if (bold && italic) {
                        para_text << "***" << run_text << "***";
                    } else if (bold) {
                        para_text << "**" << run_text << "**";
                    } else if (italic) {
                        para_text << "*" << run_text << "*";
                    } else {
                        para_text << run_text;
                    }
                } else {
                    para_text << run_text;
                }
            }

            elem.text = para_text.str();

            // Check for embedded image references
            elem.image_rid = find_image_rid(child);

            elements.push_back(std::move(elem));

        } else if (strcmp(local, "tbl") == 0) {
            DocxElement elem;
            elem.type = DocxElement::TABLE;
            elem.table_rows = parse_table_node(child);
            elements.push_back(std::move(elem));
        }
    }

    return elements;
}

// ── to_markdown ─────────────────────────────────────────

std::string DocxParser::to_markdown(const ConvertOptions& opts) {
    if (!zip_.has_entry("word/document.xml")) return "";

    auto data = zip_.read_entry("word/document.xml");
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size())) return "";

    auto body = xml_child(doc, "body");
    if (!body) {
        // Try from the document root's first child
        for (auto root = doc.first_child(); root; root = root.next_sibling()) {
            body = xml_child(root, "body");
            if (body) break;
        }
    }
    if (!body) return "";

    auto elements = parse_body(body, style_heading_map_,
                               num_to_abstract_, abstract_num_formats_);

    std::ostringstream out;
    int ordered_counter = 0;
    bool in_ordered_list = false;

    for (size_t i = 0; i < elements.size(); ++i) {
        auto& elem = elements[i];

        if (elem.type == DocxElement::PAGE_BREAK) {
            out << "\n---\n\n";
            continue;
        }

        if (elem.type == DocxElement::TABLE) {
            if (opts.extract_tables) {
                out << "\n" << format_table(elem.table_rows) << "\n";
            }
            continue;
        }

        // PARAGRAPH
        if (elem.text.empty() && elem.image_rid.empty()) {
            // Reset list state on empty paragraph
            if (in_ordered_list) {
                in_ordered_list = false;
                ordered_counter = 0;
            }
            continue;
        }

        // Image reference
        if (!elem.image_rid.empty()) {
            auto it = rel_targets_.find(elem.image_rid);
            if (it != rel_targets_.end()) {
                std::string img_name = util::get_filename(it->second);
                if (opts.extract_images) {
                    out << "![" << img_name << "](" << img_name << ")\n\n";
                } else {
                    out << "![" << img_name << "](embedded:" << img_name << ")\n\n";
                }
            }
        }

        if (elem.text.empty()) continue;

        // Heading
        if (elem.heading_level > 0) {
            in_ordered_list = false;
            ordered_counter = 0;
            std::string prefix(elem.heading_level, '#');
            out << prefix << " " << elem.text << "\n\n";
            continue;
        }

        // List item
        if (elem.is_list) {
            std::string indent(elem.list_level * 2, ' ');
            if (elem.is_ordered) {
                if (!in_ordered_list) {
                    in_ordered_list = true;
                    ordered_counter = 0;
                }
                ++ordered_counter;
                out << indent << ordered_counter << ". " << elem.text << "\n";
            } else {
                in_ordered_list = false;
                ordered_counter = 0;
                out << indent << "- " << elem.text << "\n";
            }
            continue;
        }

        // Normal paragraph
        in_ordered_list = false;
        ordered_counter = 0;
        out << elem.text << "\n\n";
    }

    // Append image references if extracted
    auto images = extract_images(opts);
    if (!images.empty() && opts.extract_images) {
        // Images already referenced inline where possible
    }

    return out.str();
}

// ── to_chunks ───────────────────────────────────────────

std::vector<PageChunk> DocxParser::to_chunks(
    const ConvertOptions& opts) {

    if (!zip_.has_entry("word/document.xml")) return {};

    auto data = zip_.read_entry("word/document.xml");
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size())) return {};

    auto body = xml_child(doc, "body");
    if (!body) {
        for (auto root = doc.first_child(); root; root = root.next_sibling()) {
            body = xml_child(root, "body");
            if (body) break;
        }
    }
    if (!body) return {};

    auto elements = parse_body(body, style_heading_map_,
                               num_to_abstract_, abstract_num_formats_);

    auto all_images = extract_images(opts);

    std::vector<PageChunk> chunks;
    PageChunk current;
    current.page_number = 1;
    std::ostringstream text;
    int ordered_counter = 0;
    bool in_ordered_list = false;

    auto flush_chunk = [&]() {
        current.text = text.str();
        if (!current.text.empty() || !current.tables.empty() || !current.images.empty()) {
            chunks.push_back(std::move(current));
        }
        current = PageChunk{};
        current.page_number = static_cast<int>(chunks.size()) + 1;
        text.str("");
        text.clear();
        ordered_counter = 0;
        in_ordered_list = false;
    };

    for (auto& elem : elements) {
        if (elem.type == DocxElement::PAGE_BREAK) {
            flush_chunk();
            continue;
        }

        if (elem.type == DocxElement::TABLE) {
            if (opts.extract_tables) {
                // Add table as structured data
                current.tables.push_back(elem.table_rows);
                // Also add to text as markdown table
                text << "\n" << format_table(elem.table_rows) << "\n";
            }
            continue;
        }

        // PARAGRAPH
        // Handle image references even when text is empty
        if (!elem.image_rid.empty()) {
            auto it = rel_targets_.find(elem.image_rid);
            if (it != rel_targets_.end()) {
                std::string img_name = util::get_filename(it->second);
                if (opts.extract_images) {
                    text << "![" << img_name << "](" << img_name << ")\n\n";
                } else {
                    text << "![" << img_name << "](embedded:" << img_name << ")\n\n";
                }
            }
        }

        if (elem.text.empty()) {
            if (in_ordered_list) {
                in_ordered_list = false;
                ordered_counter = 0;
            }
            continue;
        }

        if (elem.heading_level > 0) {
            in_ordered_list = false;
            ordered_counter = 0;
            std::string prefix(elem.heading_level, '#');
            text << prefix << " " << elem.text << "\n\n";
        } else if (elem.is_list) {
            std::string indent(elem.list_level * 2, ' ');
            if (elem.is_ordered) {
                if (!in_ordered_list) {
                    in_ordered_list = true;
                    ordered_counter = 0;
                }
                ++ordered_counter;
                text << indent << ordered_counter << ". " << elem.text << "\n";
            } else {
                in_ordered_list = false;
                ordered_counter = 0;
                text << indent << "- " << elem.text << "\n";
            }
        } else {
            in_ordered_list = false;
            ordered_counter = 0;
            text << elem.text << "\n\n";
        }
    }

    flush_chunk();

    // Attach images to first chunk (create one if needed)
    if (!all_images.empty()) {
        if (chunks.empty()) {
            PageChunk c;
            c.page_number = 1;
            chunks.push_back(std::move(c));
        }
        chunks[0].images = std::move(all_images);
    }

    // Filter by requested pages
    if (!opts.pages.empty()) {
        std::vector<PageChunk> filtered;
        for (auto& chunk : chunks) {
            for (int p : opts.pages) {
                if (chunk.page_number == p) {
                    filtered.push_back(std::move(chunk));
                    break;
                }
            }
        }
        return filtered;
    }

    return chunks;
}

} // namespace jdoc
