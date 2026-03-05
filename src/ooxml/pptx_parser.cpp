// PPTX parser implementation
// Parses ZIP-based .pptx files using pugixml for XML processing

#include "ooxml/pptx_parser.h"
#include "xml_utils.h"
#include "common/file_utils.h"
#include <algorithm>
#include <cstdlib>
#include <regex>
#include <sys/stat.h>
#include <sstream>

namespace jdoc {

// ── Constructor ─────────────────────────────────────────

PptxParser::PptxParser(ZipReader& zip) : zip_(zip) {
    enumerate_slides();
}

// ── Enumerate slides in order ───────────────────────────

void PptxParser::enumerate_slides() {
    // Collect all slide entries matching ppt/slides/slideN.xml
    auto entries = zip_.entries_with_prefix("ppt/slides/slide");

    struct SlideEntry {
        int number;
        std::string path;
    };
    std::vector<SlideEntry> slides;

    for (auto* entry : entries) {
        const std::string& name = entry->name;
        // Match "ppt/slides/slide<N>.xml" but not "ppt/slides/slideLayouts/"
        if (name.find("ppt/slides/slide") != 0) continue;
        if (name.find('/') != std::string("ppt/slides/").size() - 1) {
            // Check there's no additional path separator after "ppt/slides/"
            std::string after = name.substr(std::string("ppt/slides/").size());
            if (after.find('/') != std::string::npos) continue;
        }

        // Extract number from "slideN.xml"
        std::string basename = name.substr(std::string("ppt/slides/slide").size());
        // basename is like "1.xml", "12.xml"
        auto dot = basename.find('.');
        if (dot == std::string::npos) continue;
        std::string num_str = basename.substr(0, dot);
        if (num_str.empty()) continue;

        bool all_digits = true;
        for (char c : num_str) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                all_digits = false;
                break;
            }
        }
        if (!all_digits) continue;

        int num = std::atoi(num_str.c_str());
        slides.push_back({num, name});
    }

    // Sort by slide number
    std::sort(slides.begin(), slides.end(),
              [](const SlideEntry& a, const SlideEntry& b) {
                  return a.number < b.number;
              });

    slide_paths_.reserve(slides.size());
    for (auto& s : slides) {
        slide_paths_.push_back(std::move(s.path));
    }
}

// ── Table parsing ───────────────────────────────────────

std::vector<std::vector<std::string>> PptxParser::parse_table(
    const pugi::xml_node& tbl) {

    std::vector<std::vector<std::string>> rows;
    std::vector<pugi::xml_node> tr_nodes;
    xml_find_all(tbl, "tr", tr_nodes);

    for (auto& tr : tr_nodes) {
        std::vector<std::string> row;
        std::vector<pugi::xml_node> tc_nodes;
        xml_find_all(tr, "tc", tc_nodes);

        for (auto& tc : tc_nodes) {
            std::string cell_text;
            std::vector<pugi::xml_node> paras;
            xml_find_all(tc, "p", paras);
            for (size_t i = 0; i < paras.size(); ++i) {
                if (i > 0) cell_text += " ";
                std::vector<pugi::xml_node> t_nodes;
                xml_find_all(paras[i], "t", t_nodes);
                for (auto& t : t_nodes) {
                    cell_text += xml_text_content(t);
                }
            }
            // Sanitize for markdown table
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

// ── Table formatting ────────────────────────────────────

std::string PptxParser::format_table(
    const std::vector<std::vector<std::string>>& rows) {

    if (rows.empty()) return "";

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

// ── Shape text extraction ───────────────────────────────

static bool is_title_shape(const pugi::xml_node& sp) {
    // Check <p:nvSpPr><p:nvPr><p:ph type="title"/> or type="ctrTitle"
    auto nvSpPr = xml_child(sp, "nvSpPr");
    if (!nvSpPr) return false;

    auto nvPr = xml_child(nvSpPr, "nvPr");
    if (!nvPr) return false;

    auto ph = xml_child(nvPr, "ph");
    if (!ph) return false;

    const char* type = xml_attr(ph, "type");
    return (std::string(type) == "title" ||
            std::string(type) == "ctrTitle");
}

static std::string extract_textbody_text(const pugi::xml_node& txBody) {
    std::ostringstream text;
    bool first_para = true;

    for (auto child = txBody.first_child(); child; child = child.next_sibling()) {
        const char* name = child.name();
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;

        if (strcmp(local, "p") != 0) continue;

        if (!first_para) text << "\n";
        first_para = false;

        std::vector<pugi::xml_node> runs;
        xml_find_all(child, "r", runs);

        for (auto& run : runs) {
            std::vector<pugi::xml_node> t_nodes;
            xml_find_all(run, "t", t_nodes);
            for (auto& t : t_nodes) {
                text << xml_text_content(t);
            }
        }
    }

    return text.str();
}

void PptxParser::extract_text_from_shape(const pugi::xml_node& sp,
                                          SlideContent& content) {
    bool is_title = is_title_shape(sp);

    auto txBody = xml_child(sp, "txBody");
    if (!txBody) return;

    std::string text = extract_textbody_text(txBody);
    if (text.empty()) return;

    if (is_title) {
        content.title = text;
    } else {
        if (!content.body_text.empty()) {
            content.body_text += "\n\n";
        }
        content.body_text += text;
    }

    // Check for tables inside the shape
    std::vector<pugi::xml_node> tables;
    xml_find_all(sp, "tbl", tables);
    for (auto& tbl : tables) {
        auto rows = parse_table(tbl);
        if (!rows.empty()) {
            content.tables.push_back(std::move(rows));
        }
    }
}

void PptxParser::extract_text_from_group(const pugi::xml_node& grpSp,
                                          SlideContent& content) {
    for (auto child = grpSp.first_child(); child; child = child.next_sibling()) {
        const char* name = child.name();
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;

        if (strcmp(local, "sp") == 0) {
            extract_text_from_shape(child, content);
        } else if (strcmp(local, "grpSp") == 0) {
            extract_text_from_group(child, content); // recursive
        } else if (strcmp(local, "graphicFrame") == 0) {
            // Tables can also appear in graphic frames
            std::vector<pugi::xml_node> tables;
            xml_find_all(child, "tbl", tables);
            for (auto& tbl : tables) {
                auto rows = parse_table(tbl);
                if (!rows.empty()) {
                    content.tables.push_back(std::move(rows));
                }
            }
        }
    }
}

// ── Slide parsing ───────────────────────────────────────

PptxParser::SlideContent PptxParser::parse_slide(
    const std::string& slide_path) {

    SlideContent content;
    auto data = zip_.read_entry(slide_path);
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size())) return content;

    // Find the shape tree: <p:cSld><p:spTree>
    auto cSld = xml_child(doc.first_child(), "cSld");
    if (!cSld) {
        // Try deeper
        std::vector<pugi::xml_node> cSlds;
        xml_find_all(doc, "cSld", cSlds);
        if (!cSlds.empty()) cSld = cSlds[0];
    }
    if (!cSld) return content;

    auto spTree = xml_child(cSld, "spTree");
    if (!spTree) return content;

    // Walk the shape tree
    for (auto child = spTree.first_child(); child; child = child.next_sibling()) {
        const char* name = child.name();
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;

        if (strcmp(local, "sp") == 0) {
            extract_text_from_shape(child, content);
        } else if (strcmp(local, "grpSp") == 0) {
            extract_text_from_group(child, content);
        } else if (strcmp(local, "graphicFrame") == 0) {
            // Tables in graphic frames
            std::vector<pugi::xml_node> tables;
            xml_find_all(child, "tbl", tables);
            for (auto& tbl : tables) {
                auto rows = parse_table(tbl);
                if (!rows.empty()) {
                    content.tables.push_back(std::move(rows));
                }
            }
        }
    }

    return content;
}

// ── Image extraction ────────────────────────────────────

std::vector<ImageData> PptxParser::extract_images(
    const ConvertOptions& opts) {

    std::vector<ImageData> images;
    if (!opts.extract_images) return images;

    auto entries = zip_.entries_with_prefix("ppt/media/");
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

std::string PptxParser::to_markdown(const ConvertOptions& opts) {
    std::ostringstream out;
    auto all_images = extract_images(opts);

    for (size_t i = 0; i < slide_paths_.size(); ++i) {
        int slide_num = static_cast<int>(i) + 1;

        // Filter by requested pages
        if (!opts.pages.empty()) {
            bool found = false;
            for (int p : opts.pages) {
                if (p == slide_num) { found = true; break; }
            }
            if (!found) continue;
        }

        auto content = parse_slide(slide_paths_[i]);

        if (i > 0) out << "\n---\n\n";

        // Slide header
        out << "## Slide " << slide_num << "\n\n";

        // Title
        if (!content.title.empty()) {
            out << "### " << content.title << "\n\n";
        }

        // Body text
        if (!content.body_text.empty()) {
            out << content.body_text << "\n\n";
        }

        // Tables
        if (opts.extract_tables) {
            for (auto& table : content.tables) {
                out << "\n" << format_table(table) << "\n";
            }
        }
    }

    return out.str();
}

// ── to_chunks ───────────────────────────────────────────

std::vector<PageChunk> PptxParser::to_chunks(
    const ConvertOptions& opts) {

    std::vector<PageChunk> chunks;
    auto all_images = extract_images(opts);

    for (size_t i = 0; i < slide_paths_.size(); ++i) {
        int slide_num = static_cast<int>(i) + 1;

        // Filter by requested pages
        if (!opts.pages.empty()) {
            bool found = false;
            for (int p : opts.pages) {
                if (p == slide_num) { found = true; break; }
            }
            if (!found) continue;
        }

        auto content = parse_slide(slide_paths_[i]);

        PageChunk chunk;
        chunk.page_number = slide_num;

        std::ostringstream text;

        if (!content.title.empty()) {
            text << "### " << content.title << "\n\n";
        }

        if (!content.body_text.empty()) {
            text << content.body_text << "\n\n";
        }

        if (opts.extract_tables) {
            for (auto& table : content.tables) {
                chunk.tables.push_back(table);
                text << "\n" << format_table(table) << "\n";
            }
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
