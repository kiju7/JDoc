// hwpx2md.cpp - HWPX to Markdown converter
// Parses HWPX (ZIP+XML) files and extracts text, tables, images as Markdown
// License: MIT

#include "jdoc/hwpx.h"
#include "jdoc/hwp_types.h"
#include "common/file_utils.h"
#include "common/string_utils.h"
#include "zip_reader.h"
#include <pugixml.hpp>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <iostream>
#include <sys/stat.h>

namespace jdoc {

// ── Markdown escaping ───────────────────────────────────────

static std::string escape_md(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '|' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// ── HWPX Document Parser ───────────────────────────────────

class HWPXParser {
public:
    explicit HWPXParser(const std::string& path, ConvertOptions opts)
        : opts_(std::move(opts)) {
        zip_ = std::make_unique<ZipReader>(path);
        if (!zip_->is_open())
            throw std::runtime_error("Cannot open HWPX file: " + path);
    }

    bool parse() {
        // 1. Verify mimetype
        auto mime_data = zip_->read_entry("mimetype");
        std::string mime(mime_data.begin(), mime_data.end());
        mime = util::trim(mime);
        if (mime != "application/hwp+zip" &&
            mime != "application/vnd.hancom.hwpx.document" &&
            mime != "application/haansofthwpx") {
            throw std::runtime_error("Not a valid HWPX file (mimetype: " + mime + ")");
        }

        // 2. Parse content.hpf for manifest (id -> href mapping)
        parse_content_hpf();

        // 3. Parse header.xml for charPr, paraPr, fontface
        parse_header_xml();

        // 4. Parse each section XML
        for (int i = 0; ; i++) {
            std::string section_path = "Contents/section" + std::to_string(i) + ".xml";
            if (!zip_->has_entry(section_path)) break;
            section_paths_.push_back(section_path);
        }

        return true;
    }

    std::vector<PageChunk> convert_chunks() {
        std::vector<PageChunk> chunks;
        int section_idx = 0;

        for (auto& section_path : section_paths_) {
            // Check page filter
            if (!opts_.pages.empty()) {
                bool found = false;
                for (int p : opts_.pages) {
                    if (p == section_idx) { found = true; break; }
                }
                if (!found) { section_idx++; continue; }
            }

            PageChunk chunk;
            chunk.page_number = section_idx + 1;
            parse_section(section_path, chunk);
            chunks.push_back(std::move(chunk));
            section_idx++;
        }

        return chunks;
    }

private:
    std::unique_ptr<ZipReader> zip_;
    ConvertOptions opts_;

    // Parsed metadata
    std::map<std::string, hwp::BinDataRef> bin_data_map_;  // id -> BinDataRef
    std::vector<hwp::CharShapeInfo> char_shapes_;
    std::vector<hwp::ParaShapeInfo> para_shapes_;
    std::map<std::string, std::vector<hwp::FaceNameInfo>> font_faces_;  // lang -> fonts
    std::vector<std::string> section_paths_;

    // ── content.hpf parsing ─────────────────────────────────
    void parse_content_hpf() {
        auto data = zip_->read_entry("Contents/content.hpf");
        if (data.empty()) return;

        pugi::xml_document doc;
        doc.load_buffer(data.data(), data.size());

        // Find manifest items: <opf:item id="..." href="..." media-type="..."/>
        for (auto item : doc.select_nodes("//item")) {
            auto node = item.node();
            hwp::BinDataRef ref;
            ref.id = node.attribute("id").as_string();
            ref.href = node.attribute("href").as_string();
            ref.media_type = node.attribute("media-type").as_string();
            if (!ref.id.empty() && !ref.href.empty()) {
                bin_data_map_[ref.id] = ref;
            }
        }

        // If XPath doesn't work due to namespaces, try manual traversal
        if (bin_data_map_.empty()) {
            traverse_manifest(doc);
        }
    }

    void traverse_manifest(pugi::xml_node node) {
        for (auto child : node.children()) {
            std::string name = child.name();
            // Match any element named "item" regardless of namespace prefix
            if (name.find("item") != std::string::npos ||
                name == "opf:item") {
                hwp::BinDataRef ref;
                ref.id = child.attribute("id").as_string();
                ref.href = child.attribute("href").as_string();
                ref.media_type = child.attribute("media-type").as_string();
                if (!ref.id.empty() && !ref.href.empty()) {
                    bin_data_map_[ref.id] = ref;
                }
            }
            traverse_manifest(child);
        }
    }

    // ── header.xml parsing ──────────────────────────────────
    void parse_header_xml() {
        auto data = zip_->read_entry("Contents/header.xml");
        if (data.empty()) return;

        pugi::xml_document doc;
        doc.load_buffer(data.data(), data.size());

        // Parse font faces
        parse_font_faces(doc);

        // Parse character properties
        parse_char_properties(doc);

        // Parse paragraph properties
        parse_para_properties(doc);
    }

    void parse_font_faces(pugi::xml_document& doc) {
        // <hh:fontfaces><hh:fontface lang="HANGUL"><hh:font id="0" face="함초롬돋움" .../>
        auto walk = [&](pugi::xml_node node, auto& self) -> void {
            std::string name = node.name();
            if (name == "hh:fontface" || name.find("fontface") != std::string::npos) {
                std::string lang = node.attribute("lang").as_string();
                for (auto font : node.children()) {
                    std::string fn = font.name();
                    if (fn == "hh:font" || fn.find("font") != std::string::npos) {
                        hwp::FaceNameInfo fi;
                        fi.id = font.attribute("id").as_int();
                        fi.name = font.attribute("face").as_string();
                        fi.lang = lang;
                        font_faces_[lang].push_back(fi);
                    }
                }
            }
            for (auto child : node.children()) {
                self(child, self);
            }
        };
        walk(doc, walk);
    }

    void parse_char_properties(pugi::xml_document& doc) {
        // <hh:charProperties><hh:charPr id="0" height="1000" ...>
        auto walk = [&](pugi::xml_node node, auto& self) -> void {
            std::string name = node.name();
            if (name == "hh:charPr" || (name.find("charPr") != std::string::npos && name.find("Properties") == std::string::npos)) {
                hwp::CharShapeInfo cs;
                cs.id = node.attribute("id").as_int();
                cs.height = node.attribute("height").as_int(1000);
                cs.text_color = node.attribute("textColor").as_string("#000000");

                // Parse fontRef child
                for (auto child : node.children()) {
                    std::string cn = child.name();
                    if (cn == "hh:fontRef" || cn.find("fontRef") != std::string::npos) {
                        cs.font_ref[0] = child.attribute("hangul").as_int();
                        cs.font_ref[1] = child.attribute("latin").as_int();
                        cs.font_ref[2] = child.attribute("hanja").as_int();
                        cs.font_ref[3] = child.attribute("japanese").as_int();
                        cs.font_ref[4] = child.attribute("other").as_int();
                        cs.font_ref[5] = child.attribute("symbol").as_int();
                        cs.font_ref[6] = child.attribute("user").as_int();
                    }
                    // <hh:bold/> and <hh:italic/> are empty elements
                    if (cn == "hh:bold" || cn.find("bold") != std::string::npos) {
                        cs.bold = true;
                    }
                    if (cn == "hh:italic" || cn.find("italic") != std::string::npos) {
                        cs.italic = true;
                    }
                }

                // Ensure char_shapes_ vector is large enough
                if (cs.id >= (int)char_shapes_.size()) {
                    char_shapes_.resize(cs.id + 1);
                }
                char_shapes_[cs.id] = cs;
            }
            for (auto child : node.children()) {
                self(child, self);
            }
        };
        walk(doc, walk);
    }

    void parse_para_properties(pugi::xml_document& doc) {
        // <hh:paraProperties><hh:paraPr id="0" ...>
        auto walk = [&](pugi::xml_node node, auto& self) -> void {
            std::string name = node.name();
            if (name == "hh:paraPr" || (name.find("paraPr") != std::string::npos && name.find("Properties") == std::string::npos)) {
                hwp::ParaShapeInfo ps;
                ps.id = node.attribute("id").as_int();
                ps.alignment = node.attribute("align").as_string("");

                // Check for outline level in heading attribute or children
                // outlineLevel may be in a child <hh:heading> element
                for (auto child : node.children()) {
                    std::string cn = child.name();
                    if (cn == "hh:heading" || cn.find("heading") != std::string::npos) {
                        ps.outline_level = child.attribute("level").as_int(0);
                        // Also check "type" attribute
                        std::string type = child.attribute("type").as_string("");
                        if (type == "OUTLINE") {
                            // level is valid
                        }
                    }
                }

                if (ps.id >= (int)para_shapes_.size()) {
                    para_shapes_.resize(ps.id + 1);
                }
                para_shapes_[ps.id] = ps;
            }
            for (auto child : node.children()) {
                self(child, self);
            }
        };
        walk(doc, walk);
    }

    // ── Section XML parsing ─────────────────────────────────
    void parse_section(const std::string& section_path, PageChunk& chunk) {
        auto data = zip_->read_entry(section_path);
        if (data.empty()) return;

        pugi::xml_document doc;
        doc.load_buffer(data.data(), data.size());

        // Find <hs:sec> root
        auto sec = doc.first_child();

        // Extract page dimensions from <hp:secPr><hp:pagePr>
        extract_page_info(sec, chunk);

        // Process paragraphs
        std::string md;
        md.reserve(data.size());

        // Collect font size samples for body_font_size detection
        std::map<int, int> size_counts;
        int image_idx = 0;

        process_paragraphs(sec, md, chunk, size_counts, image_idx, 0);

        // Determine body_font_size (most frequent)
        int max_count = 0;
        int body_height = 1000;
        for (auto& [h, c] : size_counts) {
            if (c > max_count) { max_count = c; body_height = h; }
        }
        chunk.body_font_size = body_height / 100.0;
        chunk.text = md;
    }

    void extract_page_info(pugi::xml_node sec, PageChunk& chunk) {
        // Walk to find <hp:pagePr>
        auto walk = [&](pugi::xml_node node, auto& self, int depth) -> void {
            if (depth > 5) return;
            std::string name = node.name();
            if (name == "hp:pagePr" || name.find("pagePr") != std::string::npos) {
                // width/height are in HWP units (1/7200 inch)
                // Convert to points: value * 72 / 7200 = value / 100
                int w = node.attribute("width").as_int(59528);
                int h = node.attribute("height").as_int(84188);
                chunk.page_width = w / 100.0;   // approximate pt
                chunk.page_height = h / 100.0;
                return;
            }
            for (auto child : node.children()) {
                self(child, self, depth + 1);
            }
        };
        walk(sec, walk, 0);

        // Default A4 if not found
        if (chunk.page_width == 0) chunk.page_width = 595.28;
        if (chunk.page_height == 0) chunk.page_height = 841.88;
    }

    void process_paragraphs(pugi::xml_node parent, std::string& md,
                            PageChunk& chunk, std::map<int, int>& size_counts,
                            int& image_idx, int depth) {
        for (auto child : parent.children()) {
            std::string name = child.name();
            if (name == "hp:p") {
                process_paragraph(child, md, chunk, size_counts, image_idx);
            } else if (depth < 3) {
                // Recurse into containers (but not too deep - avoid secPr etc.)
                process_paragraphs(child, md, chunk, size_counts, image_idx, depth + 1);
            }
        }
    }

    void process_paragraph(pugi::xml_node para, std::string& md,
                           PageChunk& chunk, std::map<int, int>& size_counts,
                           int& image_idx) {
        int para_pr_id = para.attribute("paraPrIDRef").as_int(-1);

        // Check outline level for heading detection
        int outline_level = 0;
        if (para_pr_id >= 0 && para_pr_id < (int)para_shapes_.size()) {
            outline_level = para_shapes_[para_pr_id].outline_level;
        }

        // Process runs to collect text, tables, images
        std::string para_text;
        bool has_table = false;
        bool para_has_content = false;

        for (auto run : para.children()) {
            std::string rn = run.name();
            if (rn != "hp:run") continue;

            int char_pr_id = run.attribute("charPrIDRef").as_int(0);
            bool is_bold = false, is_italic = false;
            int font_height = 1000;

            if (char_pr_id >= 0 && char_pr_id < (int)char_shapes_.size()) {
                auto& cs = char_shapes_[char_pr_id];
                is_bold = cs.bold;
                is_italic = cs.italic;
                font_height = cs.height;
            }

            // Track font size for body detection
            size_counts[font_height]++;

            for (auto item : run.children()) {
                std::string in = item.name();

                if (in == "hp:t") {
                    // Text content
                    std::string text = extract_text(item);
                    if (!text.empty()) {
                        para_has_content = true;
                        if (is_bold && is_italic) {
                            para_text += "***" + text + "***";
                        } else if (is_bold) {
                            para_text += "**" + text + "**";
                        } else if (is_italic) {
                            para_text += "*" + text + "*";
                        } else {
                            para_text += text;
                        }
                    }
                }
                else if (in == "hp:tbl") {
                    // Table
                    if (opts_.extract_tables) {
                        std::string table_md = process_table(item);
                        if (!table_md.empty()) {
                            // Flush preceding text
                            if (!para_text.empty()) {
                                md += para_text + "\n\n";
                                para_text.clear();
                            }
                            md += table_md + "\n";
                            has_table = true;
                            para_has_content = true;
                        }
                    }
                }
                else if (in == "hp:pic") {
                    // Picture
                    auto img = process_picture(item, chunk.page_number, image_idx);
                    if (!img.data.empty()) {
                        chunk.images.push_back(std::move(img));
                        image_idx++;
                        para_has_content = true;
                        // Add image reference in markdown
                        auto& saved = chunk.images.back();
                        if (!saved.saved_path.empty()) {
                            para_text += "![" + saved.name + "](" + saved.saved_path + ")";
                        } else {
                            para_text += "![image](" + saved.name + ")";
                        }
                    }
                }
                // Skip other controls (secPr, ctrl, rect, ellipse, etc.)
            }
        }

        // Format paragraph output
        if (para_has_content && !para_text.empty()) {
            para_text = util::trim(para_text);
            if (para_text.empty()) return;

            if (outline_level > 0 && outline_level <= 6) {
                // Heading
                std::string hashes(outline_level, '#');
                // Strip bold markers from headings (headings are already emphasized)
                std::string clean = para_text;
                while (clean.size() >= 4 && clean.substr(0, 2) == "**" &&
                       clean.substr(clean.size() - 2) == "**") {
                    clean = clean.substr(2, clean.size() - 4);
                }
                md += hashes + " " + clean + "\n\n";
            } else {
                // Check font-size-based heading (fallback)
                // Find dominant charPr for this paragraph
                int dominant_height = get_dominant_height(para);
                double body_size_guess = 1000;  // default 10pt
                // Simple heuristic: if font is significantly larger than typical body
                double ratio = dominant_height / body_size_guess;
                if (ratio >= 2.0) {
                    md += "# " + para_text + "\n\n";
                } else if (ratio >= 1.6) {
                    md += "## " + para_text + "\n\n";
                } else if (ratio >= 1.3) {
                    md += "### " + para_text + "\n\n";
                } else {
                    md += para_text + "\n\n";
                }
            }
        }
    }

    int get_dominant_height(pugi::xml_node para) {
        for (auto run : para.children()) {
            if (std::string(run.name()) != "hp:run") continue;
            int id = run.attribute("charPrIDRef").as_int(0);
            if (id >= 0 && id < (int)char_shapes_.size()) {
                return char_shapes_[id].height;
            }
        }
        return 1000;
    }

    // ── Text extraction from <hp:t> ─────────────────────────
    std::string extract_text(pugi::xml_node t_node) {
        // <hp:t> can contain plain text or mixed content with child elements
        // Plain text: <hp:t>Hello</hp:t>
        // Mixed: <hp:t>Hello<hp:lineBreak/>World</hp:t>

        std::string result;
        for (auto child : t_node.children()) {
            if (child.type() == pugi::node_pcdata) {
                result += child.value();
            } else if (child.type() == pugi::node_element) {
                std::string cn = child.name();
                if (cn == "hp:lineBreak" || cn.find("lineBreak") != std::string::npos) {
                    result += "\n";
                } else if (cn == "hp:tab" || cn.find("tab") != std::string::npos) {
                    result += "\t";
                } else if (cn == "hp:nbSpace" || cn == "hp:fwSpace") {
                    result += " ";
                }
            }
        }

        // If no children, try direct text
        if (result.empty()) {
            const char* text = t_node.child_value();
            if (text && text[0]) result = text;
        }

        return result;
    }

    // ── Table processing ────────────────────────────────────
    std::string process_table(pugi::xml_node tbl) {
        std::vector<std::vector<std::string>> rows;

        int declared_cols = tbl.attribute("colCnt").as_int(0);

        for (auto tr : tbl.children()) {
            if (std::string(tr.name()) != "hp:tr") continue;

            std::vector<std::string> row;
            for (auto tc : tr.children()) {
                if (std::string(tc.name()) != "hp:tc") continue;

                // Extract cell text from subList -> paragraphs
                std::string cell_text;
                extract_cell_text(tc, cell_text);
                row.push_back(util::escape_cell(cell_text));

                // Handle colSpan: add empty cells for spanned columns
                auto span_node = tc.child("hp:cellSpan");
                int col_span = span_node.attribute("colSpan").as_int(1);
                for (int s = 1; s < col_span; s++) {
                    row.push_back("");
                }
            }

            if (!row.empty()) {
                rows.push_back(std::move(row));
            }
        }

        if (rows.empty()) return "";

        // Build markdown table
        // Normalize column count (use declared colCnt or max actual)
        size_t max_cols = declared_cols > 0 ? (size_t)declared_cols : 0;
        for (auto& r : rows) max_cols = std::max(max_cols, r.size());
        for (auto& r : rows) r.resize(max_cols, "");

        std::string md;
        // Header row
        md += "|";
        for (auto& cell : rows[0]) {
            md += " " + cell + " |";
        }
        md += "\n";

        // Separator
        md += "|";
        for (size_t i = 0; i < max_cols; i++) {
            md += " --- |";
        }
        md += "\n";

        // Data rows
        for (size_t i = 1; i < rows.size(); i++) {
            md += "|";
            for (auto& cell : rows[i]) {
                md += " " + cell + " |";
            }
            md += "\n";
        }

        return md;
    }

    void extract_cell_text(pugi::xml_node tc, std::string& out) {
        for (auto child : tc.children()) {
            std::string cn = child.name();
            if (cn == "hp:subList") {
                extract_sublist_text(child, out);
            }
        }
    }

    void extract_sublist_text(pugi::xml_node sublist, std::string& out) {
        for (auto para : sublist.children()) {
            if (std::string(para.name()) != "hp:p") continue;
            if (!out.empty()) out += " ";
            for (auto run : para.children()) {
                if (std::string(run.name()) != "hp:run") continue;
                for (auto item : run.children()) {
                    if (std::string(item.name()) == "hp:t") {
                        out += extract_text(item);
                    }
                }
            }
        }
    }

    // ── Picture processing ──────────────────────────────────
    ImageData process_picture(pugi::xml_node pic, int page_number, int image_idx) {
        ImageData img;
        img.page_number = page_number;

        // Find <hc:img binaryItemIDRef="...">
        std::string bin_ref;
        unsigned width = 0, height = 0;

        auto walk = [&](pugi::xml_node node, auto& self) -> void {
            std::string name = node.name();
            if (name == "hc:img" || name.find(":img") != std::string::npos) {
                bin_ref = node.attribute("binaryItemIDRef").as_string();
            }
            if (name == "hp:orgSz" || name.find("orgSz") != std::string::npos) {
                width = node.attribute("width").as_uint();
                height = node.attribute("height").as_uint();
            }
            if (name == "hp:imgDim" || name.find("imgDim") != std::string::npos) {
                if (node.attribute("dimwidth"))
                    width = node.attribute("dimwidth").as_uint();
                if (node.attribute("dimheight"))
                    height = node.attribute("dimheight").as_uint();
            }
            for (auto child : node.children()) {
                self(child, self);
            }
        };
        walk(pic, walk);

        if (bin_ref.empty()) return img;

        // Resolve binaryItemIDRef -> actual file path in ZIP
        std::string file_path;
        auto it = bin_data_map_.find(bin_ref);
        if (it != bin_data_map_.end()) {
            file_path = it->second.href;
        } else {
            // Try common patterns: BinData/<ref>.jpg, BinData/<ref>.png, etc.
            for (auto& ext : {"jpg", "jpeg", "png", "gif", "bmp"}) {
                std::string try_path = "BinData/" + bin_ref + "." + ext;
                if (zip_->has_entry(try_path)) {
                    file_path = try_path;
                    break;
                }
            }
            // Also try without extension
            if (file_path.empty() && zip_->has_entry("BinData/" + bin_ref)) {
                file_path = "BinData/" + bin_ref;
            }
        }

        if (file_path.empty() || !zip_->has_entry(file_path)) return img;

        // Read image data from ZIP
        auto raw = zip_->read_entry(file_path);
        if (raw.empty()) return img;

        img.data.assign(raw.begin(), raw.end());
        img.name = file_path;
        img.format = util::image_format_from_ext(util::get_extension(file_path));
        img.width = width;
        img.height = height;

        // Save to disk if requested
        if (opts_.extract_images && !opts_.image_output_dir.empty()) {
            util::ensure_dir(opts_.image_output_dir);
            std::string filename = "page" + std::to_string(page_number) +
                                   "_img" + std::to_string(image_idx) +
                                   "." + (img.format == "jpeg" ? "jpg" : img.format);
            std::string save_path = opts_.image_output_dir + "/" + filename;
            FILE* f = fopen(save_path.c_str(), "wb");
            if (f) {
                fwrite(img.data.data(), 1, img.data.size(), f);
                fclose(f);
                img.saved_path = save_path;
            }
        }

        return img;
    }
};

// ── Public API ──────────────────────────────────────────────

std::string hwpx_to_markdown(const std::string& hwpx_path, ConvertOptions opts) {
    HWPXParser parser(hwpx_path, opts);
    parser.parse();
    auto chunks = parser.convert_chunks();

    bool plaintext = (opts.output_format == OutputFormat::PLAINTEXT);

    std::string result;
    for (size_t i = 0; i < chunks.size(); i++) {
        if (plaintext) {
            if (i > 0)
                result += "\n--- Page " + std::to_string(chunks[i].page_number + 1) + " ---\n\n";
            result += util::strip_markdown(chunks[i].text);
        } else {
            result += chunks[i].text;
        }
    }
    return result;
}

std::vector<PageChunk> hwpx_to_markdown_chunks(const std::string& hwpx_path,
                                                ConvertOptions opts) {
    opts.page_chunks = true;
    HWPXParser parser(hwpx_path, opts);
    parser.parse();
    auto chunks = parser.convert_chunks();

    if (opts.output_format == OutputFormat::PLAINTEXT) {
        for (auto& chunk : chunks)
            chunk.text = util::strip_markdown(chunk.text);
    }
    return chunks;
}

} // namespace jdoc
