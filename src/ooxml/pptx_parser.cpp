// PPTX parser implementation
// Parses ZIP-based .pptx files using pugixml for XML processing

#include "ooxml/pptx_parser.h"
#include "xml_utils.h"
#include "common/file_utils.h"
#include "common/image_utils.h"
#include "common/png_encode.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace jdoc {

// ── Constructor ─────────────────────────────────────────

PptxParser::PptxParser(ZipReader& zip) : zip_(zip) {
    enumerate_slides();
}

// ── Enumerate slides in order ───────────────────────────

void PptxParser::enumerate_slides() {
    auto entries = zip_.entries_with_prefix("ppt/slides/slide");

    struct SlideEntry {
        int number;
        std::string path;
    };
    std::vector<SlideEntry> slides;

    for (auto* entry : entries) {
        const std::string& name = entry->name;
        if (name.find("ppt/slides/slide") != 0) continue;
        if (name.find('/') != std::string("ppt/slides/").size() - 1) {
            std::string after = name.substr(std::string("ppt/slides/").size());
            if (after.find('/') != std::string::npos) continue;
        }

        std::string basename = name.substr(std::string("ppt/slides/slide").size());
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

    out << "|";
    for (size_t c = 0; c < cols; ++c) {
        const std::string& cell = (c < rows[0].size()) ? rows[0][c] : "";
        out << " " << cell << " |";
    }
    out << "\n";

    out << "|";
    for (size_t c = 0; c < cols; ++c) {
        out << " --- |";
    }
    out << "\n";

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

static std::string extract_textbody_text(const pugi::xml_node& txBody);

// ── Chart text extraction ───────────────────────────────

std::string PptxParser::extract_chart_text(const std::string& chart_path) {
    if (!zip_.has_entry(chart_path)) return "";
    auto data = zip_.read_entry(chart_path);
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size())) return "";

    std::string result;

    auto chart = xml_child(doc.first_child(), "chart");
    if (chart) {
        auto title_node = xml_child(chart, "title");
        if (title_node) {
            std::vector<pugi::xml_node> t_nodes;
            xml_find_all(title_node, "t", t_nodes);
            for (auto& t : t_nodes) {
                result += xml_text_content(t);
            }
        }
    }

    std::vector<pugi::xml_node> str_caches;
    xml_find_all(doc, "strCache", str_caches);
    for (auto& cache : str_caches) {
        std::vector<pugi::xml_node> pts;
        xml_find_all(cache, "pt", pts);
        for (auto& pt : pts) {
            auto v = xml_child(pt, "v");
            if (v) {
                std::string val = xml_text_content(v);
                if (!val.empty()) {
                    if (!result.empty()) result += " ";
                    result += val;
                }
            }
        }
    }

    return result;
}

// ── SmartArt/Diagram text extraction ────────────────────

std::string PptxParser::extract_diagram_text(
    const std::string& diagram_data_path) {
    if (!zip_.has_entry(diagram_data_path)) return "";
    auto data = zip_.read_entry(diagram_data_path);
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size())) return "";

    std::string result;
    std::vector<pugi::xml_node> pt_nodes;
    xml_find_all(doc, "pt", pt_nodes);
    for (auto& pt : pt_nodes) {
        const char* pt_type = xml_attr(pt, "type");
        if (strcmp(pt_type, "pres") == 0 ||
            strcmp(pt_type, "sibTrans") == 0 ||
            strcmp(pt_type, "parTrans") == 0)
            continue;

        auto dgm_t = xml_child(pt, "t");
        if (!dgm_t) continue;

        std::string pt_text;
        std::vector<pugi::xml_node> p_nodes;
        xml_find_all(dgm_t, "p", p_nodes);
        for (size_t pi = 0; pi < p_nodes.size(); ++pi) {
            if (pi > 0) pt_text += " ";
            std::vector<pugi::xml_node> r_nodes;
            xml_find_all(p_nodes[pi], "r", r_nodes);
            for (auto& r : r_nodes) {
                auto at = xml_child(r, "t");
                if (at) pt_text += xml_text_content(at);
            }
        }

        if (!pt_text.empty()) {
            if (!result.empty()) result += "\n";
            result += pt_text;
        }
    }
    return result;
}

// ── Notes text extraction ───────────────────────────────

std::string PptxParser::extract_notes_text(const std::string& notes_path) {
    if (!zip_.has_entry(notes_path)) return "";
    auto data = zip_.read_entry(notes_path);
    pugi::xml_document doc;
    if (!doc.load_buffer(data.data(), data.size())) return "";

    auto cSld = xml_child(doc.first_child(), "cSld");
    if (!cSld) return "";
    auto spTree = xml_child(cSld, "spTree");
    if (!spTree) return "";

    std::string result;
    for (auto sp = spTree.first_child(); sp; sp = sp.next_sibling()) {
        const char* name = sp.name();
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;
        if (strcmp(local, "sp") != 0) continue;

        auto nvSpPr = xml_child(sp, "nvSpPr");
        if (!nvSpPr) continue;
        auto nvPr = xml_child(nvSpPr, "nvPr");
        if (!nvPr) continue;
        auto ph = xml_child(nvPr, "ph");
        if (!ph) continue;
        const char* type = xml_attr(ph, "type");
        if (strcmp(type, "body") != 0) continue;

        auto txBody = xml_child(sp, "txBody");
        if (!txBody) continue;

        std::string text = extract_textbody_text(txBody);
        if (!text.empty()) {
            if (!result.empty()) result += "\n";
            result += text;
        }
    }
    return result;
}

// ── Shape text extraction ───────────────────────────────

static bool is_title_shape(const pugi::xml_node& sp) {
    auto nvSpPr = xml_child(sp, "nvSpPr");
    if (!nvSpPr) return false;

    auto nvPr = xml_child(nvSpPr, "nvPr");
    if (!nvPr) return false;

    auto ph = xml_child(nvPr, "ph");
    if (!ph) return false;

    const char* type = xml_attr(ph, "type");
    return (strcmp(type, "title") == 0 ||
            strcmp(type, "ctrTitle") == 0);
}

static std::string extract_textbody_text(const pugi::xml_node& txBody) {
    enum RunStyle : uint8_t { PLAIN = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3 };
    struct StyledRun { std::string text; RunStyle style; };

    std::string result;
    bool first_para = true;

    for (auto child = txBody.first_child(); child; child = child.next_sibling()) {
        const char* name = child.name();
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;
        if (strcmp(local, "p") != 0) continue;

        if (!first_para) result += "\n";
        first_para = false;

        std::vector<StyledRun> runs;
        std::vector<pugi::xml_node> r_nodes;
        xml_find_all(child, "r", r_nodes);

        for (auto& run : r_nodes) {
            auto rPr = xml_child(run, "rPr");
            bool bold = false, italic = false;
            if (rPr) {
                const char* b_val = xml_attr(rPr, "b");
                const char* i_val = xml_attr(rPr, "i");
                bold = (b_val[0] && strcmp(b_val, "0") != 0);
                italic = (i_val[0] && strcmp(i_val, "0") != 0);
            }
            std::string run_text;
            std::vector<pugi::xml_node> t_nodes;
            xml_find_all(run, "t", t_nodes);
            for (auto& t : t_nodes) run_text += xml_text_content(t);
            if (run_text.empty()) continue;

            RunStyle s = PLAIN;
            if (bold && italic) s = BOLD_ITALIC;
            else if (bold) s = BOLD;
            else if (italic) s = ITALIC;
            runs.push_back({std::move(run_text), s});
        }

        for (size_t ri = 0; ri < runs.size(); ) {
            size_t rj = ri + 1;
            while (rj < runs.size() && runs[rj].style == runs[ri].style) ++rj;
            std::string merged;
            for (size_t rk = ri; rk < rj; ++rk) merged += runs[rk].text;
            switch (runs[ri].style) {
                case BOLD_ITALIC: result += "***" + merged + "***"; break;
                case BOLD:        result += "**" + merged + "**"; break;
                case ITALIC:      result += "*" + merged + "*"; break;
                default:          result += merged; break;
            }
            ri = rj;
        }
    }

    return result;
}

// ── Extract blip rId from a node ────────────────────────

static const char* find_blip_rid(const pugi::xml_node& node) {
    const char* name = node.name();
    const char* colon = strchr(name, ':');
    const char* local = colon ? colon + 1 : name;

    if (strcmp(local, "blip") == 0) {
        const char* embed = xml_attr(node, "embed");
        if (embed[0]) return embed;
        const char* link = xml_attr(node, "link");
        if (link[0]) return link;
    }

    for (auto child = node.first_child(); child; child = child.next_sibling()) {
        const char* rid = find_blip_rid(child);
        if (rid) return rid;
    }
    return nullptr;
}

// ── Shape element extraction (sp) ───────────────────────

void PptxParser::extract_shape(const pugi::xml_node& sp,
                                const std::map<std::string, std::string>& rels,
                                SlideContent& content) {
    bool is_title = is_title_shape(sp);

    auto txBody = xml_child(sp, "txBody");
    if (txBody) {
        std::string text = extract_textbody_text(txBody);
        if (!text.empty()) {
            if (is_title) {
                content.title = text;
            } else {
                content.elements.push_back({SlideElement::TEXT, std::move(text), {}});
            }
        }
    }

    const char* rid = find_blip_rid(sp);
    if (rid) {
        auto it = rels.find(rid);
        if (it != rels.end() && it->second.find("media/") != std::string::npos) {
            content.elements.push_back({SlideElement::IMAGE, it->second, {}});
        }
    }

    std::vector<pugi::xml_node> tables;
    xml_find_all(sp, "tbl", tables);
    for (auto& tbl : tables) {
        auto rows = parse_table(tbl);
        if (!rows.empty()) {
            content.elements.push_back({SlideElement::TABLE, {}, std::move(rows)});
        }
    }
}

// ── Picture element extraction (pic) ────────────────────

void PptxParser::extract_picture(const pugi::xml_node& pic,
                                  const std::map<std::string, std::string>& rels,
                                  SlideContent& content) {
    const char* rid = find_blip_rid(pic);
    if (!rid) return;

    auto it = rels.find(rid);
    if (it == rels.end()) return;
    if (it->second.find("media/") == std::string::npos) return;

    content.elements.push_back({SlideElement::IMAGE, it->second, {}});
}

// ── Graphic frame extraction (graphicFrame) ─────────────

void PptxParser::extract_graphic_frame(
    const pugi::xml_node& gf,
    const std::map<std::string, std::string>& rels,
    SlideContent& content) {

    std::vector<pugi::xml_node> tables;
    xml_find_all(gf, "tbl", tables);
    for (auto& tbl : tables) {
        auto rows = parse_table(tbl);
        if (!rows.empty()) {
            content.elements.push_back({SlideElement::TABLE, {}, std::move(rows)});
        }
    }

    std::vector<pugi::xml_node> gd_nodes;
    xml_find_all(gf, "graphicData", gd_nodes);
    for (auto& gd : gd_nodes) {
        auto chart_node = xml_child(gd, "chart");
        if (chart_node) {
            const char* rid = xml_attr(chart_node, "id");
            if (rid[0]) {
                auto it = rels.find(rid);
                if (it != rels.end()) {
                    std::string text = extract_chart_text(it->second);
                    if (!text.empty()) {
                        content.elements.push_back({SlideElement::TEXT, std::move(text), {}});
                    }
                }
            }
        }

        auto rel_ids = xml_child(gd, "relIds");
        if (rel_ids) {
            const char* dm_rid = xml_attr(rel_ids, "dm");
            if (dm_rid[0]) {
                auto it = rels.find(dm_rid);
                if (it != rels.end()) {
                    std::string text = extract_diagram_text(it->second);
                    if (!text.empty()) {
                        content.elements.push_back({SlideElement::TEXT, std::move(text), {}});
                    }
                }
            }
        }
    }
}

// ── Group shape extraction ──────────────────────────────

void PptxParser::extract_group(const pugi::xml_node& grp_sp,
                                const std::map<std::string, std::string>& rels,
                                SlideContent& content) {
    for (auto child = grp_sp.first_child(); child; child = child.next_sibling()) {
        const char* name = child.name();
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;

        if (strcmp(local, "sp") == 0) {
            extract_shape(child, rels, content);
        } else if (strcmp(local, "pic") == 0) {
            extract_picture(child, rels, content);
        } else if (strcmp(local, "grpSp") == 0) {
            extract_group(child, rels, content);
        } else if (strcmp(local, "graphicFrame") == 0) {
            extract_graphic_frame(child, rels, content);
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

    auto rels = parse_slide_rels(slide_path);

    auto cSld = xml_child(doc.first_child(), "cSld");
    if (!cSld) {
        std::vector<pugi::xml_node> cSlds;
        xml_find_all(doc, "cSld", cSlds);
        if (!cSlds.empty()) cSld = cSlds[0];
    }
    if (!cSld) return content;

    auto spTree = xml_child(cSld, "spTree");
    if (!spTree) return content;

    for (auto child = spTree.first_child(); child; child = child.next_sibling()) {
        const char* name = child.name();
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;

        if (strcmp(local, "sp") == 0) {
            extract_shape(child, rels, content);
        } else if (strcmp(local, "pic") == 0) {
            extract_picture(child, rels, content);
        } else if (strcmp(local, "grpSp") == 0) {
            extract_group(child, rels, content);
        } else if (strcmp(local, "graphicFrame") == 0) {
            extract_graphic_frame(child, rels, content);
        }
    }

    for (auto& [rid, target] : rels) {
        if (target.find("notesSlide") != std::string::npos) {
            content.notes = extract_notes_text(target);
            break;
        }
    }

    return content;
}

// ── Slide relationship parsing ──────────────────────────

std::map<std::string, std::string> PptxParser::parse_slide_rels(
    const std::string& slide_path) {

    std::map<std::string, std::string> rels;
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

// ── Single image extraction ─────────────────────────────

ImageData PptxParser::extract_image_data(const std::string& media_path,
                                          int page_number,
                                          const ConvertOptions& opts) {
    ImageData img;
    img.page_number = page_number;

    std::string ext = util::get_extension(media_path);
    img.format = util::image_format_from_ext(ext);

    // Assign unified name: page{N}_img{M}
    int& idx = slide_image_idx_[page_number];
    img.name = "page" + std::to_string(page_number) + "_img" + std::to_string(idx);
    std::string filename = img.name + (ext.empty() ? ".png" : ext);
    idx++;

    if (opts.extract_images) {
        img.data = zip_.read_entry(media_path);
        util::populate_image_dimensions(img);

        img.saved_path = util::save_image_to_file(
            opts.image_output_dir, img.name, img.format,
            img.data.data(), img.data.size());
        if (!img.saved_path.empty()) {
            img.data.clear();
            img.data.shrink_to_fit();
        }
    }

    return img;
}

// ── to_markdown ─────────────────────────────────────────

std::string PptxParser::to_markdown(const ConvertOptions& opts) {
    std::ostringstream out;
    std::set<std::string> extracted;
    bool first_slide = true;

    for (size_t i = 0; i < slide_paths_.size(); ++i) {
        int slide_num = static_cast<int>(i) + 1;

        if (!opts.pages.empty()) {
            bool found = false;
            for (int p : opts.pages) {
                if (p == slide_num) { found = true; break; }
            }
            if (!found) continue;
        }

        auto content = parse_slide(slide_paths_[i]);

        if (!first_slide) out << "\n";
        first_slide = false;
        out << "--- Page " << slide_num << " ---\n\n";

        if (!content.title.empty()) {
            out << "# " << content.title << "\n\n";
        }

        for (auto& elem : content.elements) {
            switch (elem.kind) {
                case SlideElement::TEXT:
                    out << elem.text << "\n\n";
                    break;
                case SlideElement::IMAGE: {
                    if (!extracted.count(elem.text) && zip_.has_entry(elem.text)) {
                        extracted.insert(elem.text);
                        ImageData img = extract_image_data(elem.text, slide_num, opts);
                        if (util::is_image_too_small(img, opts.min_image_size))
                            break;
                        std::string ref_name = img.name;
                        if (!img.saved_path.empty()) {
                            auto sl = img.saved_path.find_last_of('/');
                            ref_name = (sl != std::string::npos)
                                ? img.saved_path.substr(sl + 1) : img.saved_path;
                        }
                        out << "![" << img.name << "](" << ref_name << ")\n\n";
                    }
                    break;
                }
                case SlideElement::TABLE:
                    if (opts.extract_tables)
                        out << "\n" << format_table(elem.rows) << "\n";
                    break;
            }
        }

        if (!content.notes.empty()) {
            out << "\n> **Notes:** " << content.notes << "\n\n";
        }
    }

    return out.str();
}

// ── to_chunks ───────────────────────────────────────────

std::vector<PageChunk> PptxParser::to_chunks(
    const ConvertOptions& opts) {

    std::vector<PageChunk> chunks;
    std::set<std::string> extracted;
    ConvertOptions img_opts = opts;
    img_opts.extract_images = true;

    for (size_t i = 0; i < slide_paths_.size(); ++i) {
        int slide_num = static_cast<int>(i) + 1;

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
            text << "# " << content.title << "\n\n";
        }

        for (auto& elem : content.elements) {
            switch (elem.kind) {
                case SlideElement::TEXT:
                    text << elem.text << "\n\n";
                    break;
                case SlideElement::IMAGE: {
                    if (!zip_.has_entry(elem.text)) break;
                    bool first_ref = !extracted.count(elem.text);
                    if (first_ref) extracted.insert(elem.text);

                    ImageData img = extract_image_data(elem.text, slide_num, img_opts);
                    if (util::is_image_too_small(img, opts.min_image_size))
                        break;

                    std::string ref_name = img.name;
                    if (!img.saved_path.empty()) {
                        auto slash = img.saved_path.find_last_of('/');
                        ref_name = (slash != std::string::npos)
                            ? img.saved_path.substr(slash + 1) : img.saved_path;
                    }
                    text << "![" << img.name << "](" << ref_name << ")\n\n";

                    if (first_ref)
                        chunk.images.push_back(std::move(img));
                    break;
                }
                case SlideElement::TABLE:
                    if (opts.extract_tables) {
                        chunk.tables.push_back(elem.rows);
                        text << "\n" << format_table(elem.rows) << "\n";
                    }
                    break;
            }
        }

        if (!content.notes.empty()) {
            text << "\n> **Notes:** " << content.notes << "\n\n";
        }

        chunk.text = text.str();
        chunks.push_back(std::move(chunk));
    }

    return chunks;
}

} // namespace jdoc
