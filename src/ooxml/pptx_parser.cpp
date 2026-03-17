// PPTX parser implementation
// Parses ZIP-based .pptx files using pugixml for XML processing

#include "ooxml/pptx_parser.h"
#include "xml_utils.h"
#include "common/file_utils.h"
#include <algorithm>
#include <cstdlib>
#include <regex>
#include <set>
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
    if (!doc.load_buffer(data.data(), data.size(), pugi::parse_default | pugi::parse_ws_pcdata)) return content;

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

// ── Slide relationship parsing ──────────────────────────

std::map<std::string, std::string> PptxParser::parse_slide_rels(
    const std::string& slide_path) {

    std::map<std::string, std::string> rels;
    // e.g. "ppt/slides/slide1.xml" -> "ppt/slides/_rels/slide1.xml.rels"
    auto slash = slide_path.rfind('/');
    if (slash == std::string::npos) return rels;
    std::string dir = slide_path.substr(0, slash);
    std::string base = slide_path.substr(slash + 1);
    std::string rels_path = dir + "/_rels/" + base + ".rels";

    if (!zip_.has_entry(rels_path)) return rels;

    auto data = zip_.read_entry(rels_path);
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size(), pugi::parse_default | pugi::parse_ws_pcdata)) return rels;

    std::vector<pugi::xml_node> rel_nodes;
    xml_find_all(doc, "Relationship", rel_nodes);

    for (auto& rel : rel_nodes) {
        const char* id = xml_attr(rel, "Id");
        const char* target = xml_attr(rel, "Target");
        if (id[0] && target[0]) {
            std::string full_target = target;
            if (full_target.find("../") == 0) {
                // Relative to slide dir -> resolve to ppt/ level
                full_target = "ppt/" + full_target.substr(3);
            } else if (full_target.find("ppt/") != 0 &&
                       full_target.find("http") != 0 &&
                       full_target[0] != '/') {
                full_target = dir + "/" + full_target;
            }
            rels[id] = full_target;
        }
    }
    return rels;
}

// ── Collect image rIds from shape tree ──────────────────

void PptxParser::collect_image_rids(const pugi::xml_node& node,
                                     std::vector<std::string>& rids) {
    // Look for <a:blip r:embed="rIdX"/> anywhere in the tree
    const char* name = node.name();
    const char* colon = strchr(name, ':');
    const char* local = colon ? colon + 1 : name;

    if (strcmp(local, "blip") == 0) {
        const char* embed = xml_attr(node, "embed");
        if (embed[0]) rids.push_back(embed);
        const char* link = xml_attr(node, "link");
        if (link[0]) rids.push_back(link);
    }

    for (auto child = node.first_child(); child; child = child.next_sibling()) {
        collect_image_rids(child, rids);
    }
}

// ── Image extraction ────────────────────────────────────

std::vector<ImageData> PptxParser::extract_images(
    const ConvertOptions& opts) {

    std::vector<ImageData> images;
    if (!opts.extract_images) return images;

    // Track already-extracted media paths to avoid duplicates
    std::set<std::string> extracted;

    // Per-slide image extraction via relationships
    for (size_t i = 0; i < slide_paths_.size(); ++i) {
        int slide_num = static_cast<int>(i) + 1;
        auto rels = parse_slide_rels(slide_paths_[i]);

        // Parse slide XML to find image references
        auto slide_data = zip_.read_entry(slide_paths_[i]);
        pugi::xml_document doc;
        if (!doc.load_buffer(slide_data.data(), slide_data.size(), pugi::parse_default | pugi::parse_ws_pcdata)) continue;

        std::vector<std::string> rids;
        collect_image_rids(doc, rids);

        for (auto& rid : rids) {
            auto it = rels.find(rid);
            if (it == rels.end()) continue;
            const std::string& media_path = it->second;
            if (!zip_.has_entry(media_path) || extracted.count(media_path)) continue;
            extracted.insert(media_path);

            std::string ext = util::get_extension(media_path);
            std::string fmt = util::image_format_from_ext(ext);

            ImageData img;
            img.page_number = slide_num;
            img.name = util::get_filename(media_path);
            img.format = fmt;

            if (!opts.image_output_dir.empty()) {
                mkdir(opts.image_output_dir.c_str(), 0755);
                std::string out_path = opts.image_output_dir + "/" + img.name;
                const ZipReader::Entry* entry_ptr = nullptr;
                for (auto& e : zip_.entries()) {
                    if (e.name == media_path) { entry_ptr = &e; break; }
                }
                if (entry_ptr && zip_.extract_entry_to_file(*entry_ptr, out_path)) {
                    img.saved_path = out_path;
                }
            } else {
                img.data = zip_.read_entry(media_path);
            }

            images.push_back(std::move(img));
        }
    }

    // Fallback: pick up any remaining files in ppt/media/ not yet extracted
    auto entries = zip_.entries_with_prefix("ppt/media/");
    for (auto* entry : entries) {
        if (extracted.count(entry->name)) continue;
        extracted.insert(entry->name);

        std::string ext = util::get_extension(entry->name);
        std::string fmt = util::image_format_from_ext(ext);

        ImageData img;
        img.page_number = 1;
        img.name = util::get_filename(entry->name);
        img.format = fmt;

        if (!opts.image_output_dir.empty()) {
            mkdir(opts.image_output_dir.c_str(), 0755);
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

    // Extract and reference images
    auto images = extract_images(opts);
    if (!images.empty()) {
        for (auto& img : images) {
            out << "![" << img.name << "](" << img.name << ")\n\n";
        }
    }

    return out.str();
}

// ── to_chunks ───────────────────────────────────────────

std::vector<PageChunk> PptxParser::to_chunks(
    const ConvertOptions& opts) {

    std::vector<PageChunk> chunks;
    // Always enumerate images so we can reference them in text
    ConvertOptions img_opts = opts;
    img_opts.extract_images = true;
    auto all_images = extract_images(img_opts);

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
        chunks.push_back(std::move(chunk));
    }

    // Distribute images to their corresponding slide chunks
    if (!all_images.empty() && !chunks.empty()) {
        for (auto& img : all_images) {
            // Find the chunk matching this image's page_number
            PageChunk* target = &chunks[0];
            for (auto& c : chunks) {
                if (c.page_number == img.page_number) { target = &c; break; }
            }
            std::string ref = img.saved_path.empty() ? "embedded:" + img.name : img.saved_path;
            target->text += "![" + img.name + "](" + ref + ")\n\n";
            target->images.push_back(std::move(img));
        }
    }

    return chunks;
}

} // namespace jdoc
