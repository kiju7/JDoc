// ODF (OpenDocument Format) parser implementation — odt / ods / odp.
// License: MIT

#include "odf/odf_parser.h"
#include "xml_utils.h"
#include "common/file_utils.h"
#include "common/image_utils.h"
#include "common/png_encode.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace jdoc {

namespace {

const char* loc(const pugi::xml_node& n) {
    const char* nm = n.name();
    const char* c = strchr(nm, ':');
    return c ? c + 1 : nm;
}

std::string rstrip(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r'))
        s.pop_back();
    return s;
}

// Collect slide-content frames from a draw:page in document order, descending
// into group shapes but never into <presentation:notes> (handled separately).
void collect_slide_frames(const pugi::xml_node& n,
                          std::vector<pugi::xml_node>& out, int depth = 0) {
    if (depth > 32) return;
    for (auto c = n.first_child(); c; c = c.next_sibling()) {
        const char* l = loc(c);
        if (strcmp(l, "notes") == 0) continue;   // speaker notes: separate path
        if (strcmp(l, "frame") == 0) { out.push_back(c); continue; }
        collect_slide_frames(c, out, depth + 1);
    }
}

// Collect a paragraph's text in document order, honoring soft breaks, tabs and
// the <text:s> repeated-space element.
void collect_para(const pugi::xml_node& node, std::string& out, int depth = 0) {
    if (depth > 64) return;
    for (auto c = node.first_child(); c; c = c.next_sibling()) {
        if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata) {
            out += c.value();
            continue;
        }
        const char* name = loc(c);
        if (strcmp(name, "line-break") == 0) {
            out += "\n";
        } else if (strcmp(name, "tab") == 0) {
            out += " ";
        } else if (strcmp(name, "s") == 0) {
            const char* cnt = xml_attr(c, "c");
            int n = cnt[0] ? std::atoi(cnt) : 1;
            if (n < 1) n = 1;
            out.append(static_cast<size_t>(n), ' ');
        } else {
            collect_para(c, out, depth + 1);  // span, a (hyperlink), etc.
        }
    }
}

} // namespace

OdfParser::OdfParser(ZipReader& zip, DocFormat kind) : zip_(zip), kind_(kind) {}

bool OdfParser::load_content(pugi::xml_document& doc, std::vector<char>& buf) const {
    if (!zip_.has_entry("content.xml")) return false;
    buf = zip_.read_entry("content.xml");
    if (buf.empty()) return false;  // encrypted or corrupt package
    return doc.load_buffer(buf.data(), buf.size(),
                           pugi::parse_default | pugi::parse_ws_pcdata);
}

std::string OdfParser::paragraph_text(const pugi::xml_node& p) const {
    std::string out;
    collect_para(p, out);
    return out;
}

int OdfParser::heading_level(const pugi::xml_node& p) const {
    const char* lvl = xml_attr(p, "outline-level");  // text:outline-level
    if (lvl[0]) {
        int n = std::atoi(lvl);
        if (n >= 1) return std::min(n, 6);
    }
    return 1;
}

std::string OdfParser::format_table(const pugi::xml_node& table) const {
    constexpr int kMaxRepeat = 512;
    constexpr int kMaxCols = 256;
    constexpr size_t kMaxRows = 10000;

    std::vector<std::vector<std::string>> rows;
    std::vector<pugi::xml_node> trs;
    xml_find_all(table, "table-row", trs);

    for (auto& tr : trs) {
        std::vector<std::string> row;
        for (auto cell = tr.first_child(); cell; cell = cell.next_sibling()) {
            const char* nm = loc(cell);
            bool covered = strcmp(nm, "covered-table-cell") == 0;
            if (strcmp(nm, "table-cell") != 0 && !covered) continue;

            std::string text;
            if (!covered) {
                std::vector<pugi::xml_node> ps;
                xml_find_all(cell, "p", ps);
                for (size_t i = 0; i < ps.size(); i++) {
                    if (i) text += " ";
                    text += paragraph_text(ps[i]);
                }
            }
            for (auto& ch : text) {
                if (ch == '|') ch = '/';
                if (ch == '\n') ch = ' ';
            }

            int rep = 1;
            const char* r = xml_attr(cell, "number-columns-repeated");
            if (r[0]) rep = std::atoi(r);
            rep = std::max(1, std::min(rep, kMaxRepeat));
            for (int i = 0; i < rep && (int)row.size() < kMaxCols; i++)
                row.push_back(text);
        }
        while (!row.empty() && row.back().empty()) row.pop_back();  // trailing pad

        int rrep = 1;
        const char* rr = xml_attr(tr, "number-rows-repeated");
        if (rr[0]) rrep = std::atoi(rr);
        rrep = std::max(1, std::min(rrep, kMaxRepeat));
        bool empty_row = row.empty();
        for (int i = 0; i < rrep && rows.size() < kMaxRows; i++) {
            rows.push_back(row);
            if (empty_row) break;  // a repeated blank row is one line, not thousands
        }
    }
    while (!rows.empty() && rows.back().empty()) rows.pop_back();
    if (rows.empty()) return "";

    size_t ncol = 0;
    for (auto& r : rows) ncol = std::max(ncol, r.size());
    if (ncol == 0) return "";

    auto emit = [&](const std::vector<std::string>& r) {
        std::string line = "|";
        for (size_t c = 0; c < ncol; c++)
            line += " " + (c < r.size() ? r[c] : std::string()) + " |";
        line += "\n";
        return line;
    };
    std::string out = emit(rows[0]);
    out += "|";
    for (size_t c = 0; c < ncol; c++) out += " --- |";
    out += "\n";
    for (size_t i = 1; i < rows.size(); i++) out += emit(rows[i]);
    return out;
}

std::vector<ImageData> OdfParser::extract_images(const ConvertOptions& opts) {
    std::vector<ImageData> images;
    if (!opts.images) return images;

    int img_idx = 0;
    for (auto* entry : zip_.entries_with_prefix("Pictures/")) {
        std::string ext = util::get_extension(entry->name);
        ImageData img;
        img.page_number = 1;
        img.name = "page1_img" + std::to_string(img_idx);
        img.format = util::image_format_from_ext(ext);
        img.data = zip_.read_entry(*entry);

        util::populate_image_dimensions(img);
        if (util::is_image_too_small(img, opts.min_image_size)) continue;

        std::string filename = img.name + (ext.empty() ? ".png" : ext);
        img.saved_path = util::save_image_to_file(opts.image_dir, img.name,
                                                  img.format, img.data.data(),
                                                  img.data.size());
        if (!img.saved_path.empty()) {
            filename = util::get_filename(img.saved_path);
            img.data.clear();
            img.data.shrink_to_fit();
        }
        image_name_map_[util::get_filename(entry->name)] = filename;
        images.push_back(std::move(img));
        img_idx++;
    }
    return images;
}

std::string OdfParser::image_ref(const pugi::xml_node& image,
                                 const ConvertOptions& opts) const {
    if (!opts.images) return "";
    const char* href = xml_attr(image, "href");  // xlink:href
    if (!href[0]) return "";
    std::string path = href;
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0) return "";
    std::string base = util::get_filename(path);
    auto it = image_name_map_.find(base);
    if (it == image_name_map_.end()) return "";
    return "![" + it->second + "](" + opts.image_ref_prefix + it->second + ")";
}

// ── odt: office:text ────────────────────────────────────────

std::vector<PageChunk> OdfParser::parse_text(const pugi::xml_node& body,
                                             const ConvertOptions& opts) {
    std::string md;
    for (auto ch = body.first_child(); ch; ch = ch.next_sibling()) {
        const char* nm = loc(ch);
        if (strcmp(nm, "h") == 0) {
            std::string t = rstrip(paragraph_text(ch));
            if (t.empty()) continue;
            md += std::string(heading_level(ch), '#') + " " + t + "\n\n";
        } else if (strcmp(nm, "p") == 0) {
            std::string t = rstrip(paragraph_text(ch));
            if (t.empty()) continue;  // blank spacer paragraph
            md += t + "\n\n";
        } else if (strcmp(nm, "list") == 0) {
            std::vector<pugi::xml_node> items;
            xml_find_all(ch, "list-item", items);
            for (auto& item : items) {
                std::vector<pugi::xml_node> ps;
                xml_find_all(item, "p", ps);
                for (auto& p : ps) {
                    std::string t = rstrip(paragraph_text(p));
                    if (!t.empty()) md += "- " + t + "\n";
                }
            }
            md += "\n";
        } else if (strcmp(nm, "table") == 0) {
            md += format_table(ch) + "\n";
        }
    }
    // Image references (odt images live in draw:frame/draw:image).
    if (opts.images) {
        std::vector<pugi::xml_node> imgs;
        xml_find_all(body, "image", imgs);
        for (auto& im : imgs) {
            std::string ref = image_ref(im, opts);
            if (!ref.empty()) md += ref + "\n\n";
        }
    }

    PageChunk chunk;
    chunk.page_number = 1;
    chunk.text = rstrip(std::move(md));
    return {chunk};
}

// ── ods: office:spreadsheet ─────────────────────────────────

std::vector<PageChunk> OdfParser::parse_spreadsheet(const pugi::xml_node& body,
                                                    const ConvertOptions& opts) {
    (void)opts;
    std::vector<PageChunk> chunks;
    int sheet_num = 0;
    for (auto ch = body.first_child(); ch; ch = ch.next_sibling()) {
        if (strcmp(loc(ch), "table") != 0) continue;
        sheet_num++;
        const char* name = xml_attr(ch, "name");
        std::string display = name[0] ? std::string(name)
                                      : "Sheet " + std::to_string(sheet_num);
        std::string table = format_table(ch);
        std::string md = "## " + display + "\n\n";
        md += table.empty() ? "*Empty sheet*\n" : table;

        PageChunk chunk;
        chunk.page_number = sheet_num;
        chunk.text = rstrip(std::move(md));
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

// ── odp: office:presentation ────────────────────────────────

std::vector<PageChunk> OdfParser::parse_presentation(const pugi::xml_node& body,
                                                     const ConvertOptions& opts) {
    std::vector<PageChunk> chunks;
    int page_num = 0;
    for (auto page = body.first_child(); page; page = page.next_sibling()) {
        if (strcmp(loc(page), "page") != 0) continue;
        page_num++;

        std::string title;
        std::string md;
        // Frames in document order (excluding the notes subtree); the title
        // frame (presentation:class="title") becomes a heading, the rest
        // contribute body paragraphs.
        std::vector<pugi::xml_node> frames;
        collect_slide_frames(page, frames);
        for (auto& fr : frames) {
            const char* cls = xml_attr(fr, "class");  // presentation:class
            std::vector<pugi::xml_node> ps;
            xml_find_all(fr, "p", ps);
            std::string frame_text;
            for (auto& p : ps) {
                std::string t = rstrip(paragraph_text(p));
                if (!t.empty()) frame_text += t + "\n\n";
            }
            std::vector<pugi::xml_node> tbls;
            xml_find_all(fr, "table", tbls);
            for (auto& tb : tbls) frame_text += format_table(tb) + "\n";

            if (title.empty() && strcmp(cls, "title") == 0 && !frame_text.empty())
                title = rstrip(frame_text);
            else
                md += frame_text;
        }
        if (opts.images) {
            std::vector<pugi::xml_node> imgs;
            xml_find_all(page, "image", imgs);
            for (auto& im : imgs) {
                std::string ref = image_ref(im, opts);
                if (!ref.empty()) md += ref + "\n\n";
            }
        }

        // Speaker notes (presentation:notes) — visible only in the notes view.
        std::string notes;
        auto notes_node = xml_child(page, "notes");
        if (notes_node) {
            std::vector<pugi::xml_node> nps;
            xml_find_all(notes_node, "p", nps);
            for (auto& p : nps) {
                std::string t = rstrip(paragraph_text(p));
                if (!t.empty()) {
                    if (!notes.empty()) notes += "\n";
                    notes += t;
                }
            }
        }

        std::string body_md;
        if (!title.empty()) body_md += "# " + title + "\n\n";
        body_md += md;
        if (!notes.empty()) body_md += "\n> **Notes:** " + notes + "\n";

        PageChunk chunk;
        chunk.page_number = page_num;
        chunk.text = rstrip(std::move(body_md));
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

// ── Entry points ────────────────────────────────────────────

std::vector<PageChunk> OdfParser::to_chunks(const ConvertOptions& opts) {
    pugi::xml_document doc;
    std::vector<char> buf;
    if (!load_content(doc, buf)) return {};

    extract_images(opts);  // saves files + populates image_name_map_

    std::vector<pugi::xml_node> bodies;
    xml_find_all(doc, "body", bodies);
    if (bodies.empty()) return {};
    pugi::xml_node body = bodies[0];  // office:body

    // office:body wraps office:text / office:spreadsheet / office:presentation.
    pugi::xml_node inner = body.first_child();
    while (inner && (inner.type() != pugi::node_element)) inner = inner.next_sibling();
    if (!inner) return {};

    switch (kind_) {
        case DocFormat::ODT: return parse_text(inner, opts);
        case DocFormat::ODS: return parse_spreadsheet(inner, opts);
        case DocFormat::ODP: return parse_presentation(inner, opts);
        default: return {};
    }
}

std::string OdfParser::to_markdown(const ConvertOptions& opts) {
    auto chunks = to_chunks(opts);
    std::string out;
    for (size_t i = 0; i < chunks.size(); i++) {
        if (i > 0) {
            // ODS sheets carry their own "## name" heading; slides get a marker.
            if (kind_ == DocFormat::ODP)
                out += "\n\n--- Page " + std::to_string(chunks[i].page_number) + " ---\n\n";
            else
                out += "\n\n";
        } else if (kind_ == DocFormat::ODP) {
            out += "--- Page " + std::to_string(chunks[i].page_number) + " ---\n\n";
        }
        out += chunks[i].text;
    }
    return out;
}

} // namespace jdoc
