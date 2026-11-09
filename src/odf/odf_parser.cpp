// ODF (OpenDocument Format) parser implementation — odt / ods / odp.
//
// One walker reads every part of the document in order: paragraphs, lists,
// sections, tables, frames and drawing shapes (text boxes, custom shapes,
// groups), notes and comments, and embedded objects. Anything anchored inside
// a paragraph (a frame, a footnote, a comment) becomes its own block after
// that paragraph, so its text never runs into the paragraph's words.
// License: MIT

#include "odf/odf_parser.h"
#include "xml_utils.h"
#include "common/embedded.h"
#include "common/file_utils.h"
#include "common/image_utils.h"
#include "common/png_encode.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>

namespace jdoc {

namespace {

constexpr int kMaxDepth = 64;

std::string rstrip(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r'))
        s.pop_back();
    return s;
}

bool is(const pugi::xml_node& n, const char* local) {
    return strcmp(xml_local_name(n), local) == 0;
}

// Elements inside a paragraph that are not part of its line of text: they get
// their own blocks after it.
bool is_anchored(const char* l) {
    return !strcmp(l, "frame") || !strcmp(l, "note") || !strcmp(l, "annotation") ||
           !strcmp(l, "custom-shape") || !strcmp(l, "rect") || !strcmp(l, "ellipse") ||
           !strcmp(l, "g") || !strcmp(l, "polygon") || !strcmp(l, "polyline") ||
           !strcmp(l, "path") || !strcmp(l, "line") || !strcmp(l, "connector") ||
           !strcmp(l, "caption") || !strcmp(l, "measure") || !strcmp(l, "circle") ||
           !strcmp(l, "regular-polygon");
}

// Parts that hold no reader-visible text: change-tracking keeps deleted text,
// index sources hold templates, a note's citation is its number.
bool is_skipped(const char* l) {
    return !strcmp(l, "tracked-changes") || !strcmp(l, "note-citation") ||
           !strcmp(l, "table-of-content-source") || !strcmp(l, "index-title-template") ||
           !strcmp(l, "alphabetical-index-source") || !strcmp(l, "user-index-source") ||
           !strcmp(l, "illustration-index-source") || !strcmp(l, "table-index-source") ||
           !strcmp(l, "object-index-source") || !strcmp(l, "bibliography-source") ||
           !strcmp(l, "sequence-decls") || !strcmp(l, "variable-decls") ||
           !strcmp(l, "user-field-decls") || !strcmp(l, "forms") ||
           !strcmp(l, "automatic-styles") || !strcmp(l, "font-face-decls") ||
           !strcmp(l, "notes") || !strcmp(l, "creator") || !strcmp(l, "date");
}

// A paragraph's own line of text, honoring soft breaks, tabs and <text:s>.
// Anchored objects are collected for the caller instead of inlined.
void collect_para(const pugi::xml_node& node, std::string& out,
                  std::vector<pugi::xml_node>& anchored, int depth = 0) {
    if (depth > kMaxDepth) return;
    for (auto c = node.first_child(); c; c = c.next_sibling()) {
        if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata) {
            out += c.value();
            continue;
        }
        const char* name = xml_local_name(c);
        if (strcmp(name, "line-break") == 0) {
            out += "\n";
        } else if (strcmp(name, "tab") == 0) {
            out += " ";
        } else if (strcmp(name, "s") == 0) {
            const char* cnt = xml_attr(c, "c");
            int n = cnt[0] ? std::atoi(cnt) : 1;
            if (n < 1) n = 1;
            out.append(static_cast<size_t>(std::min(n, 1000)), ' ');
        } else if (is_anchored(name)) {
            anchored.push_back(c);
        } else if (!is_skipped(name)) {
            collect_para(c, out, anchored, depth + 1);  // span, a, field, ...
        }
    }
}

// A table's rows in order, through header-rows / row-groups, never into a
// nested table.
void collect_rows(const pugi::xml_node& n, std::vector<pugi::xml_node>& rows) {
    for (auto c = n.first_child(); c; c = c.next_sibling()) {
        if (is(c, "table-row")) rows.push_back(c);
        else if (is(c, "table-header-rows") || is(c, "table-rows") ||
                 is(c, "table-row-group"))
            collect_rows(c, rows);
    }
}

} // namespace

OdfParser::OdfParser(ZipReader& zip, DocFormat kind, std::string prefix)
    : zip_(zip), kind_(kind), prefix_(std::move(prefix)) {}

bool OdfParser::load_part(const char* part, pugi::xml_document& doc,
                          std::vector<char>& buf) const {
    return xml_load_part(zip_, prefix_ + part, doc, buf,
                         pugi::parse_default | pugi::parse_ws_pcdata);
}

int OdfParser::heading_level(const pugi::xml_node& p) const {
    const char* lvl = xml_attr(p, "outline-level");  // text:outline-level
    if (lvl[0]) {
        int n = std::atoi(lvl);
        if (n >= 1) return std::min(n, 6);
    }
    return 1;
}

// Text of a cell: every paragraph in it, nested tables included, joined by a
// space — a markdown table cannot hold blocks.
std::string OdfParser::flat_text(const pugi::xml_node& n, const ConvertOptions& opts,
                                 int depth) {
    std::string md;
    bool flat = flat_;
    flat_ = true;
    walk(n, md, opts, depth);
    flat_ = flat;
    std::string out;
    for (char ch : rstrip(md)) {
        if (ch == '\n') {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else {
            out += ch == '|' ? '/' : ch;
        }
    }
    return out;
}

std::string OdfParser::format_table(const pugi::xml_node& table,
                                    const ConvertOptions& opts, int depth) {
    // No row limit — every row is extracted. Columns are bounded only by the
    // spreadsheet ceiling so a huge `number-columns-repeated`/`-rows-repeated`
    // on an empty filler cell can't blow up memory; real data (a sheet has at
    // most 16384 columns) is never truncated. Empty cells/rows are deferred so
    // trailing padding costs nothing.
    constexpr int kMaxCols = 16384;
    constexpr int kMaxRowRepeat = 1 << 20;  // spreadsheet row ceiling

    std::vector<std::vector<std::string>> rows;
    std::vector<pugi::xml_node> trs;
    collect_rows(table, trs);

    for (auto& tr : trs) {
        std::vector<std::string> row;
        int pending_empty = 0;  // deferred empty columns; trailing ones vanish
        for (auto cell = tr.first_child(); cell; cell = cell.next_sibling()) {
            bool covered = is(cell, "covered-table-cell");
            if (!is(cell, "table-cell") && !covered) continue;

            std::string text;
            if (!covered) {
                text = flat_text(cell, opts, depth + 1);
                // A value with no displayed paragraph (a formula result or a
                // cell written by a tool) still is the cell's content.
                if (text.empty()) {
                    for (const char* a : {"string-value", "value", "date-value",
                                          "time-value", "boolean-value"}) {
                        text = xml_attr(cell, a);
                        if (!text.empty()) break;
                    }
                }
            }

            int rep = 1;
            const char* r = xml_attr(cell, "number-columns-repeated");
            if (r[0]) rep = std::atoi(r);
            if (rep < 1) rep = 1;
            if (rep > kMaxCols) rep = kMaxCols;  // clamp guards int overflow
            if (text.empty()) {
                pending_empty += rep;  // hold back; may be trailing padding
                if (pending_empty > kMaxCols) pending_empty = kMaxCols;
            } else {
                // Materialize any interior empties that precede real content,
                // then the repeated content — both bounded by the ceiling.
                for (; pending_empty > 0 && (int)row.size() < kMaxCols; --pending_empty)
                    row.push_back("");
                pending_empty = 0;
                for (int i = 0; i < rep && (int)row.size() < kMaxCols; i++)
                    row.push_back(text);
            }
        }
        // Any pending_empty left here was trailing padding — dropped.

        int rrep = 1;
        const char* rr = xml_attr(tr, "number-rows-repeated");
        if (rr[0]) rrep = std::atoi(rr);
        if (rrep < 1) rrep = 1;
        if (rrep > kMaxRowRepeat) rrep = kMaxRowRepeat;
        bool empty_row = row.empty();
        for (int i = 0; i < rrep; i++) {
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

void OdfParser::extract_images(const ConvertOptions& opts) {
    if (!opts.images) return;
    int img_idx = 0;
    for (auto* entry : zip_.entries_with_prefix(prefix_ + "Pictures/")) {
        std::string ext = util::get_extension(entry->name);
        ImageData img;
        img.page_number = 1;
        img.name = "page1_img" + std::to_string(img_idx);
        img.format = util::image_format_from_ext(ext);
        img.data = zip_.read_entry(*entry);

        // ext is what the package named the part; store_image keeps it.
        std::string filename = util::store_image(img, opts, ext);
        if (filename.empty()) continue;
        image_name_map_[util::get_filename(entry->name)] = filename;
        img_idx++;
    }
}

std::string OdfParser::image_ref(const pugi::xml_node& image,
                                 const ConvertOptions& opts) const {
    if (!opts.images) return "";
    const char* href = xml_attr(image, "href");  // xlink:href
    if (!href[0]) return "";
    std::string path = href;
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0) return "";
    auto it = image_name_map_.find(util::get_filename(path));
    if (it == image_name_map_.end()) return "";
    return "![" + it->second + "](" + opts.image_ref_prefix + it->second + ")";
}

// An embedded object: a sub-document of this package ("Object 1/content.xml",
// a chart or another ODF document) or a foreign object stored as one part
// (an OLE container holding a spreadsheet, a Word file, ...).
std::string OdfParser::object_text(const pugi::xml_node& obj, const ConvertOptions& opts,
                                   int depth) {
    std::string href = xml_attr(obj, "href");
    if (href.rfind("./", 0) == 0) href = href.substr(2);
    if (href.empty() || href.find("://") != std::string::npos || depth > kMaxDepth)
        return "";
    std::string part = prefix_ + href;
    if (zip_.has_entry(part + "/content.xml")) {
        OdfParser sub(zip_, DocFormat::UNKNOWN, part + "/");
        std::string md;
        for (auto& c : sub.to_chunks(opts)) md += c.text + "\n\n";
        return rstrip(md);
    }
    if (!zip_.has_entry(part)) return "";
    auto bytes = zip_.read_entry(part);
    return embedded_document_text(reinterpret_cast<const uint8_t*>(bytes.data()),
                                  bytes.size(), href, opts);
}

void OdfParser::walk(const pugi::xml_node& n, std::string& md,
                     const ConvertOptions& opts, int depth) {
    if (depth > kMaxDepth) return;
    for (auto c = n.first_child(); c; c = c.next_sibling())
        if (c.type() == pugi::node_element) walk_one(c, md, opts, depth);
}

// One element: a paragraph (its anchored objects after it), a list, a table,
// an image, an object, or a container (section, frame, text box, shape,
// group, note body, comment) walked through.
void OdfParser::walk_one(const pugi::xml_node& c, std::string& md,
                         const ConvertOptions& opts, int depth) {
    if (depth > kMaxDepth || c == skip_) return;
    const char* l = xml_local_name(c);
    if (is_skipped(l)) return;
    if (!strcmp(l, "p") || !strcmp(l, "h")) {
        std::string t;
        std::vector<pugi::xml_node> anchored;
        collect_para(c, t, anchored);
        t = util::trim(t);
        if (!t.empty()) {
            if (!strcmp(l, "h")) md += std::string(heading_level(c), '#') + " ";
            else if (in_list_) md += "- ";
            md += t + "\n\n";
        }
        bool list = in_list_;
        in_list_ = false;
        for (auto& a : anchored) walk_one(a, md, opts, depth + 1);
        in_list_ = list;
    } else if (!strcmp(l, "list")) {
        bool list = in_list_;
        in_list_ = true;
        walk(c, md, opts, depth + 1);
        in_list_ = list;
    } else if (!strcmp(l, "table") && !flat_) {
        std::string t = format_table(c, opts, depth);
        if (!t.empty()) md += t + "\n";
    } else if (!strcmp(l, "image")) {
        std::string ref = image_ref(c, opts);
        if (!ref.empty()) md += ref + "\n\n";
    } else if (!strcmp(l, "object") || !strcmp(l, "object-ole")) {
        std::string t = object_text(c, opts, depth);
        if (!t.empty()) md += t + "\n\n";
    } else {
        walk(c, md, opts, depth + 1);
    }
}

// Header and footer text of the master pages (styles.xml): every distinct
// paragraph, in order.
std::string OdfParser::header_footer_text(const ConvertOptions& opts) {
    pugi::xml_document doc;
    std::vector<char> buf;
    if (!load_part("styles.xml", doc, buf)) return "";
    std::vector<pugi::xml_node> pages;
    xml_find_all(doc, "master-page", pages);
    std::string out;
    std::set<std::string> seen;
    for (auto& mp : pages) {
        for (auto c = mp.first_child(); c; c = c.next_sibling()) {
            const char* l = xml_local_name(c);
            if (strncmp(l, "header", 6) != 0 && strncmp(l, "footer", 6) != 0) continue;
            if (!strcmp(xml_attr(c, "display"), "false")) continue;
            std::string md;
            walk(c, md, opts, 1);
            size_t pos = 0;
            while (pos < md.size()) {
                size_t e = md.find('\n', pos);
                if (e == std::string::npos) e = md.size();
                std::string line = md.substr(pos, e - pos);
                pos = e + 1;
                if (!line.empty() && seen.insert(line).second) out += line + "\n\n";
            }
        }
    }
    return rstrip(out);
}

// ── odt / ods / odp bodies ──────────────────────────────────

std::vector<PageChunk> OdfParser::parse_spreadsheet(const pugi::xml_node& body,
                                                    const ConvertOptions& opts) {
    std::vector<PageChunk> chunks;
    int sheet_num = 0;
    for (auto ch = body.first_child(); ch; ch = ch.next_sibling()) {
        if (!is(ch, "table")) continue;
        sheet_num++;
        const char* name = xml_attr(ch, "name");
        std::string display = name[0] ? std::string(name)
                                      : "Sheet " + std::to_string(sheet_num);
        std::string table = format_table(ch, opts, 0);
        std::string md = "## " + display + "\n\n";
        md += table.empty() ? "*Empty sheet*\n" : table;
        // Shapes, text boxes and charts drawn over the sheet.
        if (auto shapes = xml_child(ch, "shapes")) {
            std::string extra;
            walk(shapes, extra, opts, 1);
            if (!extra.empty()) md += "\n" + extra;
        }

        PageChunk chunk;
        chunk.page_number = sheet_num;
        chunk.text = rstrip(std::move(md));
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

std::vector<PageChunk> OdfParser::parse_presentation(const pugi::xml_node& body,
                                                     const ConvertOptions& opts) {
    std::vector<PageChunk> chunks;
    int page_num = 0;
    for (auto page = body.first_child(); page; page = page.next_sibling()) {
        if (!is(page, "page")) continue;
        page_num++;

        // The title placeholder becomes the slide's heading.
        std::string title;
        skip_ = pugi::xml_node();
        std::vector<pugi::xml_node> frames;
        xml_find_all(page, "frame", frames);
        for (auto& fr : frames) {
            if (strcmp(xml_attr(fr, "class"), "title") != 0) continue;
            std::string t;
            walk(fr, t, opts, 1);
            title = rstrip(t);
            if (!title.empty()) {
                for (auto& ch : title) if (ch == '\n') ch = ' ';
                skip_ = fr;
            }
            break;
        }
        std::string md;
        walk(page, md, opts, 1);
        skip_ = pugi::xml_node();

        // Speaker notes (presentation:notes) — visible only in the notes view.
        std::string notes;
        if (auto notes_node = xml_child(page, "notes")) {
            std::string nmd;
            walk(notes_node, nmd, opts, 1);
            nmd = rstrip(nmd);
            for (size_t i = 0; i < nmd.size(); ++i) {
                if (nmd[i] == '\n' && i + 1 < nmd.size() && nmd[i + 1] == '\n') continue;
                notes += nmd[i];
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
    if (!load_part("content.xml", doc, buf)) return {};

    extract_images(opts);  // saves files + populates image_name_map_

    pugi::xml_node body = xml_child(doc.document_element(), "body");
    if (!body) return {};
    // office:body wraps office:text / spreadsheet / presentation / chart / ...
    pugi::xml_node inner = body.first_child();
    while (inner && inner.type() != pugi::node_element) inner = inner.next_sibling();
    if (!inner) return {};

    std::vector<PageChunk> chunks;
    if (is(inner, "spreadsheet")) {
        chunks = parse_spreadsheet(inner, opts);
    } else if (is(inner, "presentation") || is(inner, "drawing")) {
        chunks = parse_presentation(inner, opts);
    } else {
        PageChunk chunk;
        chunk.page_number = 1;
        std::string md;
        walk(inner, md, opts, 0);
        chunk.text = rstrip(std::move(md));
        chunks.push_back(std::move(chunk));
    }

    std::string hf = header_footer_text(opts);
    if (!hf.empty()) {
        if (chunks.empty()) {
            PageChunk c;
            c.page_number = 1;
            chunks.push_back(std::move(c));
        }
        chunks.back().text += "\n\n--- Header / Footer ---\n\n" + hf;
    }
    return chunks;
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
