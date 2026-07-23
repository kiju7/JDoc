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

// Render one <a:p> paragraph to text (<a:br> -> '\n'); defined below, used by
// both parse_table and extract_textbody_text.
static std::string extract_paragraph_text(const pugi::xml_node& p, bool markdown);

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
                cell_text += extract_paragraph_text(paras[i], /*markdown=*/false);
            }
            // Any '\n' (paragraph or <a:br>) collapses to a space so the cell
            // stays on one markdown table row; '|' is escaped.
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


static std::string extract_textbody_text(const pugi::xml_node& txBody);

// ── Chart text extraction ───────────────────────────────

std::string PptxParser::extract_chart_text(const std::string& chart_path) {
    if (!zip_.has_entry(chart_path)) return "";
    auto data = zip_.read_entry(chart_path);
    pugi::xml_document doc;
    if (!doc.load_buffer_inplace(data.data(), data.size())) return "";

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
    if (!doc.load_buffer_inplace(data.data(), data.size())) return "";

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
    if (!doc.load_buffer_inplace(data.data(), data.size())) return "";

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

        // Take every text-bearing shape on the notes page, not just the body
        // placeholder — speaker notes are often typed into a plain text box.
        // Only the furniture placeholders are skipped: the slide thumbnail
        // carries no text, and slide number / date / footer restate the deck
        // chrome rather than the note.
        auto nvSpPr = xml_child(sp, "nvSpPr");
        auto nvPr = nvSpPr ? xml_child(nvSpPr, "nvPr") : pugi::xml_node();
        auto ph = nvPr ? xml_child(nvPr, "ph") : pugi::xml_node();
        if (ph) {
            const char* type = xml_attr(ph, "type");
            if (strcmp(type, "sldImg") == 0 || strcmp(type, "sldNum") == 0 ||
                strcmp(type, "dt") == 0 || strcmp(type, "ftr") == 0)
                continue;
        }

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

// ── Slide master / layout text extraction ───────────────

// True for a placeholder that merely prompts the author ("Click to edit Master
// title style", "마스터 제목 스타일 편집"). A <p:ph> in a master or layout is by
// definition an empty slot: whatever the author actually types lands in the
// slide, so the layout copy is template furniture that would otherwise be
// injected into every document. Keying on the placeholder *type* rather than on
// a list of known prompt strings keeps this working for any authoring locale.
//
// ftr/hdr are deliberately NOT treated as prompts — a footer authored into the
// layout is real content that appears on every slide, and is a place teams
// habitually park a team name or contact.
static bool is_prompt_placeholder(const pugi::xml_node& sp) {
    auto nvSpPr = xml_child(sp, "nvSpPr");
    auto nvPr = nvSpPr ? xml_child(nvSpPr, "nvPr") : pugi::xml_node();
    auto ph = nvPr ? xml_child(nvPr, "ph") : pugi::xml_node();
    if (!ph) return false;  // a plain text box holds authored content

    const char* type = xml_attr(ph, "type");
    // An absent type attribute defaults to "body" per ECMA-376
    if (!type[0]) return true;
    return strcmp(type, "title")   == 0 || strcmp(type, "ctrTitle") == 0 ||
           strcmp(type, "subTitle") == 0 || strcmp(type, "body")    == 0 ||
           strcmp(type, "pic")     == 0 || strcmp(type, "chart")    == 0 ||
           strcmp(type, "tbl")     == 0 || strcmp(type, "dgm")      == 0 ||
           strcmp(type, "media")   == 0 || strcmp(type, "clipArt")  == 0;
}

// Walk a master/layout shape tree, appending authored text to `out`.
void PptxParser::collect_layout_shape_text(const pugi::xml_node& parent,
                                            std::vector<std::string>& out,
                                            int depth) {
    if (depth > kXmlMaxDepth) return;

    for (auto child = parent.first_child(); child; child = child.next_sibling()) {
        const char* name = child.name();
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;

        if (strcmp(local, "sp") == 0 || strcmp(local, "cxnSp") == 0) {
            if (is_prompt_placeholder(child)) continue;
            auto txBody = xml_child(child, "txBody");
            if (!txBody) continue;
            std::string text = extract_textbody_text(txBody);
            if (!text.empty()) out.push_back(std::move(text));
        } else if (strcmp(local, "grpSp") == 0) {
            collect_layout_shape_text(child, out, depth + 1);
        } else if (strcmp(local, "AlternateContent") == 0) {
            for (auto alt = child.first_child(); alt; alt = alt.next_sibling()) {
                const char* an = alt.name();
                const char* acolon = strchr(an, ':');
                const char* alocal = acolon ? acolon + 1 : an;
                if (strcmp(alocal, "Choice") != 0) continue;
                collect_layout_shape_text(alt, out, depth + 1);
            }
        }
    }
}

// Text authored into the slide masters and layouts. It renders on every slide
// that uses them but lives in no slide part, so a reader scanning slide XML
// alone never sees it — a common hiding place for a team name, an author or a
// contact address. Returned de-duplicated and in package order; the caller is
// responsible for dropping anything the slides already said.
std::vector<std::string> PptxParser::collect_master_layout_text() {
    std::vector<std::string> out;
    std::set<std::string> seen;

    // Masters first, then layouts — both sorted so output is deterministic
    std::vector<std::string> parts;
    for (const char* prefix : {"ppt/slideMasters/slideMaster",
                               "ppt/slideLayouts/slideLayout"}) {
        std::vector<std::string> group;
        for (auto* entry : zip_.entries_with_prefix(prefix)) {
            const std::string& nm = entry->name;
            if (nm.size() < 5 || nm.compare(nm.size() - 4, 4, ".xml") != 0) continue;
            group.push_back(nm);
        }
        std::sort(group.begin(), group.end());
        parts.insert(parts.end(), group.begin(), group.end());
    }

    for (const auto& part : parts) {
        auto data = zip_.read_entry(part);
        if (data.empty()) continue;
        pugi::xml_document doc;
        if (!doc.load_buffer_inplace(data.data(), data.size(),
                             pugi::parse_default | pugi::parse_ws_pcdata))
            continue;

        auto cSld = xml_child(doc.first_child(), "cSld");
        if (!cSld) continue;
        auto spTree = xml_child(cSld, "spTree");
        if (!spTree) continue;

        std::vector<std::string> texts;
        collect_layout_shape_text(spTree, texts, 0);
        for (auto& t : texts) {
            // The same footer or logo is repeated across every layout
            if (seen.insert(t).second) out.push_back(std::move(t));
        }
    }
    return out;
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

// Render one <a:p> paragraph to plain text. Consecutive runs of the same style
// are merged (with emphasis markup when markdown=true); an explicit <a:br> soft
// line break becomes '\n'. <a:fld> fields are skipped — in a presentation they
// only render deck chrome (slide number, date, footer).
static std::string extract_paragraph_text(const pugi::xml_node& p, bool markdown) {
    enum RunStyle : uint8_t { PLAIN = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3 };
    struct Token { std::string text; RunStyle style; bool is_break; };

    std::vector<Token> tokens;
    // Walk direct children in document order (not a recursive xml_find_all) so a
    // <a:br> keeps its position between the runs it separates.
    for (auto node = p.first_child(); node; node = node.next_sibling()) {
        const char* name = node.name();
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;

        if (strcmp(local, "br") == 0) {
            tokens.push_back({"", PLAIN, true});
        } else if (strcmp(local, "r") == 0) {
            auto rPr = xml_child(node, "rPr");
            bool bold = false, italic = false;
            if (rPr) {
                const char* b_val = xml_attr(rPr, "b");
                const char* i_val = xml_attr(rPr, "i");
                bold = (b_val[0] && strcmp(b_val, "0") != 0);
                italic = (i_val[0] && strcmp(i_val, "0") != 0);
            }
            std::string run_text;
            std::vector<pugi::xml_node> t_nodes;
            xml_find_all(node, "t", t_nodes);
            for (auto& t : t_nodes) run_text += xml_text_content(t);
            if (run_text.empty()) continue;

            RunStyle s = PLAIN;
            if (bold && italic) s = BOLD_ITALIC;
            else if (bold) s = BOLD;
            else if (italic) s = ITALIC;
            tokens.push_back({std::move(run_text), s, false});
        }
        // Other children (<a:fld>, etc.) are intentionally skipped.
    }

    std::string result;
    for (size_t i = 0; i < tokens.size(); ) {
        if (tokens[i].is_break) { result += "\n"; ++i; continue; }
        size_t j = i + 1;
        while (j < tokens.size() && !tokens[j].is_break &&
               tokens[j].style == tokens[i].style) ++j;
        std::string merged;
        for (size_t k = i; k < j; ++k) merged += tokens[k].text;
        if (markdown) {
            switch (tokens[i].style) {
                case BOLD_ITALIC: result += "***" + merged + "***"; break;
                case BOLD:        result += "**" + merged + "**"; break;
                case ITALIC:      result += "*" + merged + "*"; break;
                default:          result += merged; break;
            }
        } else {
            result += merged;
        }
        i = j;
    }
    return result;
}

static std::string extract_textbody_text(const pugi::xml_node& txBody) {
    std::string result;
    bool first_para = true;

    for (auto child = txBody.first_child(); child; child = child.next_sibling()) {
        const char* name = child.name();
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;
        if (strcmp(local, "p") != 0) continue;

        if (!first_para) result += "\n";
        first_para = false;
        result += extract_paragraph_text(child, /*markdown=*/true);
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

// ── Shape tree dispatch ─────────────────────────────────

// Dispatch every shape-tree child of `parent` to its extractor. Used for both
// <p:spTree> and <p:grpSp>, which hold the same set of child elements.
//
// <mc:AlternateContent> wraps a shape when it needs a legacy rendering (ink,
// WordArt, newer chart types); the shape itself lives in <mc:Choice>, so the
// wrapper must be descended into or the whole shape is lost. Only <mc:Choice>
// is followed — <mc:Fallback> restates the same content and would duplicate it.
void PptxParser::extract_shape_tree(const pugi::xml_node& parent,
                                     const std::map<std::string, std::string>& rels,
                                     SlideContent& content, int depth) {
    if (depth > kXmlMaxDepth) return;

    for (auto child = parent.first_child(); child; child = child.next_sibling()) {
        const char* name = child.name();
        const char* colon = strchr(name, ':');
        const char* local = colon ? colon + 1 : name;

        if (strcmp(local, "sp") == 0 || strcmp(local, "cxnSp") == 0) {
            // A connector (cxnSp) carries an optional label in its own txBody
            extract_shape(child, rels, content);
        } else if (strcmp(local, "pic") == 0) {
            extract_picture(child, rels, content);
        } else if (strcmp(local, "grpSp") == 0) {
            extract_shape_tree(child, rels, content, depth + 1);
        } else if (strcmp(local, "graphicFrame") == 0) {
            extract_graphic_frame(child, rels, content);
        } else if (strcmp(local, "AlternateContent") == 0) {
            for (auto alt = child.first_child(); alt; alt = alt.next_sibling()) {
                const char* an = alt.name();
                const char* acolon = strchr(an, ':');
                const char* alocal = acolon ? acolon + 1 : an;
                if (strcmp(alocal, "Choice") != 0) continue;
                extract_shape_tree(alt, rels, content, depth + 1);
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
    if (!doc.load_buffer_inplace(data.data(), data.size(), pugi::parse_default | pugi::parse_ws_pcdata)) return content;

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

    extract_shape_tree(spTree, rels, content, 0);

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
    if (!doc.load_buffer_inplace(data.data(), data.size(), pugi::parse_default | pugi::parse_ws_pcdata)) return rels;

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
    idx++;

    // Read and measure only — the write happens in resolve_image, after the
    // minimum-size rule has had its say, so a filtered image never leaves an
    // orphaned file behind on disk.
    if (opts.images) {
        img.data = zip_.read_entry(media_path);
        util::populate_image_dimensions(img);
    }

    return img;
}

// ── Media part resolution (one extraction per part) ─────

bool PptxParser::resolve_image(const std::string& media_path, int page_number,
                                const ConvertOptions& opts,
                                ImageData& out, std::string& ref_name) {
    auto hit = media_cache_.find(media_path);
    if (hit.known) {
        if (hit.skipped) return false;
        out = util::MediaCache::reference(hit.image, page_number);
        ref_name = hit.ref_name;
        return true;
    }

    if (!zip_.has_entry(media_path)) {
        media_cache_.insert_skipped(media_path);
        return false;
    }

    ImageData img = extract_image_data(media_path, page_number, opts);
    if (util::is_image_too_small(img, opts.min_image_size)) {
        media_cache_.insert_skipped(media_path);
        return false;
    }

    std::string ext = util::get_extension(media_path);
    ref_name = img.name + (ext.empty() ? ".png" : ext);

    if (opts.images) {
        img.saved_path = util::save_image_to_file(
            opts.image_dir, img.name, img.format,
            img.data.data(), img.data.size());
        if (!img.saved_path.empty()) {
            ref_name = util::get_filename(img.saved_path);
            img.data.clear();
            img.data.shrink_to_fit();
        }
    }

    media_cache_.insert(media_path, img, ref_name);
    out = std::move(img);
    return true;
}

// ── to_markdown ─────────────────────────────────────────

std::string PptxParser::to_markdown(const ConvertOptions& opts) {
    std::ostringstream out;
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
                    // Every reference is rendered, including the second and
                    // later ones — a logo shown on ten slides appears in all
                    // ten — but they all link to the one extracted file.
                    ImageData img;
                    std::string ref_name;
                    if (!resolve_image(elem.text, slide_num, opts, img, ref_name))
                        break;
                    out << "![" << img.name << "](" << opts.image_ref_prefix << ref_name << ")\n\n";
                    break;
                }
                case SlideElement::TABLE:
                    if (opts.tables)
                        out << "\n" << util::format_markdown_table(elem.rows) << "\n";
                    break;
            }
        }

        if (!content.notes.empty()) {
            out << "\n> **Notes:** " << content.notes << "\n\n";
        }
    }

    // Master/layout text closes the document as one block rather than being
    // repeated per slide: it is authored once and would otherwise swamp the
    // body flow of a long deck.
    std::string body = out.str();
    std::string extra = format_master_layout_block(body);
    if (!extra.empty()) {
        if (!body.empty() && body.back() != '\n') body += "\n";
        body += extra;
    }
    return body;
}

// ── Master/layout trailing block ────────────────────────

// Render the master/layout text that the slides did not already state. A layout
// placeholder the author filled in is emitted by the slide itself, so anything
// already present in `body` is dropped rather than repeated.
std::string PptxParser::format_master_layout_block(const std::string& body) {
    auto texts = collect_master_layout_text();
    if (texts.empty()) return "";

    std::string block;
    for (auto& t : texts) {
        if (body.find(t) != std::string::npos) continue;
        block += t + "\n";
    }
    if (block.empty()) return "";
    return "\n--- Slide Master / Layout ---\n\n" + block;
}

// ── to_chunks ───────────────────────────────────────────

std::vector<PageChunk> PptxParser::to_chunks(
    const ConvertOptions& opts) {

    std::vector<PageChunk> chunks;
    ConvertOptions img_opts = opts;
    img_opts.images = true;

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

        // Distinct media parts already listed for this page — a shape group
        // that repeats one picture should list it once per page, not per shape
        std::set<std::string> page_media;

        std::string text;

        if (!content.title.empty()) {
            text += "# "; text += content.title; text += "\n\n";
        }

        for (auto& elem : content.elements) {
            switch (elem.kind) {
                case SlideElement::TEXT:
                    text += elem.text; text += "\n\n";
                    break;
                case SlideElement::IMAGE: {
                    ImageData img;
                    std::string ref_name;
                    if (!resolve_image(elem.text, slide_num, img_opts, img, ref_name))
                        break;
                    text += "!["; text += img.name; text += "]("; text += opts.image_ref_prefix; text += ref_name; text += ")\n\n";

                    // The page lists every distinct image it shows, so the
                    // page-to-image association survives deduplication. A page
                    // that reuses an image extracted earlier gets the shared
                    // name and path with no second copy of the bytes.
                    if (page_media.insert(elem.text).second)
                        chunk.images.push_back(std::move(img));
                    break;
                }
                case SlideElement::TABLE:
                    if (opts.tables) {
                        chunk.tables.push_back(elem.rows);
                        text += "\n"; text += util::format_markdown_table(elem.rows); text += "\n";
                    }
                    break;
            }
        }

        if (!content.notes.empty()) {
            text += "\n> **Notes:** "; text += content.notes; text += "\n\n";
        }

        chunk.text = text;
        chunks.push_back(std::move(chunk));
    }

    // Master/layout text belongs to no slide, so it becomes a trailing chunk
    std::string body;
    size_t body_total = 0;
    for (auto& c : chunks) body_total += c.text.size();
    body.reserve(body_total);
    for (auto& c : chunks) body += c.text;
    std::string extra = format_master_layout_block(body);
    if (!extra.empty()) {
        PageChunk chunk;
        chunk.page_number = static_cast<int>(slide_paths_.size()) + 1;
        chunk.text = extra;
        chunks.push_back(std::move(chunk));
    }

    return chunks;
}

} // namespace jdoc
