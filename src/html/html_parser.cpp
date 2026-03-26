// HTML/HTM to Markdown parser implementation
// License: MIT

#include "html_parser.h"
#include "common/file_utils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stack>

namespace jdoc {

// ── Constructor ──────────────────────────────────────────

HtmlParser::HtmlParser(const std::string& file_path) : file_path_(file_path) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) return;
    ifs.seekg(0, std::ios::end);
    size_t size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    raw_html_.resize(size);
    ifs.read(&raw_html_[0], size);
}

// ── HTML entity decoding ─────────────────────────────────

std::string HtmlParser::decode_entities(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    size_t i = 0;
    size_t len = text.size();

    while (i < len) {
        if (text[i] == '&') {
            size_t semi = text.find(';', i + 1);
            if (semi != std::string::npos && semi - i < 12) {
                std::string entity = text.substr(i + 1, semi - i - 1);

                if (entity == "amp") result += '&';
                else if (entity == "lt") result += '<';
                else if (entity == "gt") result += '>';
                else if (entity == "quot") result += '"';
                else if (entity == "apos") result += '\'';
                else if (entity == "nbsp") result += ' ';
                else if (entity == "mdash" || entity == "#8212") result += "\xe2\x80\x94";
                else if (entity == "ndash" || entity == "#8211") result += "\xe2\x80\x93";
                else if (entity == "lsquo" || entity == "#8216") result += "\xe2\x80\x98";
                else if (entity == "rsquo" || entity == "#8217") result += "\xe2\x80\x99";
                else if (entity == "ldquo" || entity == "#8220") result += "\xe2\x80\x9c";
                else if (entity == "rdquo" || entity == "#8221") result += "\xe2\x80\x9d";
                else if (entity == "bull" || entity == "#8226") result += "\xe2\x80\xa2";
                else if (entity == "hellip" || entity == "#8230") result += "\xe2\x80\xa6";
                else if (entity == "copy") result += "\xc2\xa9";
                else if (entity == "reg") result += "\xc2\xae";
                else if (entity == "trade") result += "\xe2\x84\xa2";
                else if (!entity.empty() && entity[0] == '#') {
                    // Numeric entity
                    uint32_t cp = 0;
                    if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')) {
                        // Hex: &#xHHHH;
                        for (size_t k = 2; k < entity.size(); k++) {
                            char c = entity[k];
                            if (c >= '0' && c <= '9') cp = cp * 16 + (c - '0');
                            else if (c >= 'a' && c <= 'f') cp = cp * 16 + (c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') cp = cp * 16 + (c - 'A' + 10);
                        }
                    } else {
                        // Decimal: &#NNNN;
                        for (size_t k = 1; k < entity.size(); k++) {
                            if (entity[k] >= '0' && entity[k] <= '9')
                                cp = cp * 10 + (entity[k] - '0');
                        }
                    }
                    if (cp > 0 && cp < 0x110000) {
                        // Encode as UTF-8
                        if (cp < 0x80) {
                            result += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            result += static_cast<char>(0xC0 | (cp >> 6));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            result += static_cast<char>(0xE0 | (cp >> 12));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += static_cast<char>(0xF0 | (cp >> 18));
                            result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                    }
                } else {
                    // Unknown entity, keep as-is
                    result += '&';
                    result += entity;
                    result += ';';
                }
                i = semi + 1;
                continue;
            }
        }
        result += text[i];
        i++;
    }
    return result;
}

// ── Tag helpers ──────────────────────────────────────────

bool HtmlParser::is_block_tag(const std::string& name) {
    static const char* blocks[] = {
        "div", "p", "h1", "h2", "h3", "h4", "h5", "h6",
        "ul", "ol", "li", "table", "tr", "td", "th", "thead", "tbody", "tfoot",
        "blockquote", "pre", "hr", "br", "section", "article", "nav",
        "header", "footer", "main", "aside", "figure", "figcaption",
        "details", "summary", "dl", "dt", "dd",
        nullptr
    };
    for (int i = 0; blocks[i]; i++)
        if (name == blocks[i]) return true;
    return false;
}

bool HtmlParser::is_void_tag(const std::string& name) {
    static const char* voids[] = {
        "br", "hr", "img", "input", "meta", "link", "area",
        "base", "col", "embed", "source", "track", "wbr",
        nullptr
    };
    for (int i = 0; voids[i]; i++)
        if (name == voids[i]) return true;
    return false;
}

std::string HtmlParser::Tag::get_attr(const std::string& attrs_str,
                                       const std::string& key) const {
    // Simple attribute parser: find key="value" or key='value'
    std::string search = key + "=";
    size_t pos = 0;
    while (pos < attrs_str.size()) {
        size_t found = attrs_str.find(search, pos);
        if (found == std::string::npos) break;

        // Make sure it's a word boundary
        if (found > 0 && std::isalnum(static_cast<unsigned char>(attrs_str[found - 1]))) {
            pos = found + 1;
            continue;
        }

        size_t val_start = found + search.size();
        if (val_start >= attrs_str.size()) break;

        char quote = attrs_str[val_start];
        if (quote == '"' || quote == '\'') {
            val_start++;
            size_t val_end = attrs_str.find(quote, val_start);
            if (val_end != std::string::npos)
                return attrs_str.substr(val_start, val_end - val_start);
        } else {
            // Unquoted value: read until whitespace or >
            size_t val_end = val_start;
            while (val_end < attrs_str.size() &&
                   attrs_str[val_end] != ' ' && attrs_str[val_end] != '>')
                val_end++;
            return attrs_str.substr(val_start, val_end - val_start);
        }
        break;
    }
    return "";
}

// ── Tag / text reading ───────────────────────────────────

bool HtmlParser::read_tag(size_t& pos, Tag& tag) const {
    size_t len = raw_html_.size();
    // Find next '<'
    while (pos < len && raw_html_[pos] != '<') pos++;
    if (pos >= len) return false;

    size_t tag_start = pos;
    pos++; // skip '<'

    // Skip whitespace
    while (pos < len && raw_html_[pos] == ' ') pos++;

    // Comment: <!-- ... -->
    if (pos + 2 < len && raw_html_[pos] == '!' &&
        raw_html_[pos + 1] == '-' && raw_html_[pos + 2] == '-') {
        size_t end = raw_html_.find("-->", pos);
        if (end != std::string::npos) pos = end + 3;
        else pos = len;
        tag.name = "!comment";
        return true;
    }

    // DOCTYPE: <!DOCTYPE ...>
    if (pos < len && raw_html_[pos] == '!') {
        size_t end = raw_html_.find('>', pos);
        if (end != std::string::npos) pos = end + 1;
        else pos = len;
        tag.name = "!doctype";
        return true;
    }

    // Processing instruction: <? ... ?>
    if (pos < len && raw_html_[pos] == '?') {
        size_t end = raw_html_.find("?>", pos);
        if (end != std::string::npos) pos = end + 2;
        else pos = len;
        tag.name = "?";
        return true;
    }

    // Closing tag
    tag.is_closing = false;
    if (pos < len && raw_html_[pos] == '/') {
        tag.is_closing = true;
        pos++;
    }

    // Read tag name
    tag.name.clear();
    while (pos < len && raw_html_[pos] != ' ' && raw_html_[pos] != '>' &&
           raw_html_[pos] != '/' && raw_html_[pos] != '\n' &&
           raw_html_[pos] != '\r' && raw_html_[pos] != '\t') {
        tag.name += std::tolower(static_cast<unsigned char>(raw_html_[pos]));
        pos++;
    }

    // Read attributes until '>'
    tag.attrs.clear();
    bool in_quote = false;
    char quote_char = 0;
    while (pos < len) {
        char c = raw_html_[pos];
        if (in_quote) {
            tag.attrs += c;
            if (c == quote_char) in_quote = false;
            pos++;
        } else if (c == '"' || c == '\'') {
            in_quote = true;
            quote_char = c;
            tag.attrs += c;
            pos++;
        } else if (c == '>') {
            pos++; // skip '>'
            break;
        } else if (c == '/' && pos + 1 < len && raw_html_[pos + 1] == '>') {
            tag.is_self_closing = true;
            pos += 2;
            break;
        } else {
            tag.attrs += c;
            pos++;
        }
    }

    if (is_void_tag(tag.name)) tag.is_self_closing = true;

    return true;
}

std::string HtmlParser::read_text(size_t& pos) const {
    std::string text;
    size_t len = raw_html_.size();
    while (pos < len && raw_html_[pos] != '<') {
        text += raw_html_[pos];
        pos++;
    }
    return text;
}

// ── Main conversion ──────────────────────────────────────

std::string HtmlParser::convert(const ConvertOptions& opts,
                                 std::vector<ImageData>& out_images) {
    if (raw_html_.empty()) return "";

    std::string md;
    md.reserve(raw_html_.size() / 2);

    size_t pos = 0;
    size_t len = raw_html_.size();

    // State tracking
    bool in_head = false;
    bool in_title = false;
    std::string title_text;
    bool in_script = false;
    bool in_style = false;
    bool in_pre = false;
    bool in_code = false;
    bool in_bold = false;
    bool in_italic = false;
    bool in_a = false;
    std::string link_href;
    std::string link_text;

    // Table state
    bool in_table = false;
    bool in_thead = false;
    std::vector<std::vector<std::string>> table_rows;
    std::vector<std::string> current_row;
    std::string current_cell;
    bool in_cell = false;

    // List state
    struct ListState { bool ordered; int count; };
    std::stack<ListState> list_stack;
    bool in_li = false;
    std::string li_text;

    int img_idx = 0;

    while (pos < len) {
        // Try to read text first
        if (raw_html_[pos] != '<') {
            std::string text = read_text(pos);
            if (in_script || in_style) continue;
            if (in_head) {
                if (in_title) title_text += text;
                continue;
            }

            text = decode_entities(text);

            if (in_cell) {
                current_cell += text;
                continue;
            }

            if (in_li) {
                li_text += text;
                continue;
            }

            if (in_a) {
                link_text += text;
                continue;
            }

            if (in_pre || in_code) {
                md += text;
                continue;
            }

            // Collapse whitespace for normal text
            for (char c : text) {
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                if (c == ' ' && !md.empty() && md.back() == ' ') continue;
                md += c;
            }
            continue;
        }

        // Read tag
        Tag tag;
        if (!read_tag(pos, tag)) break;

        // Skip comments, doctype, processing instructions
        if (tag.name.empty() || tag.name[0] == '!' || tag.name[0] == '?')
            continue;

        // Skip content inside <script> and <style>
        if (tag.name == "script") {
            if (!tag.is_closing) {
                in_script = true;
                // Fast-forward to </script>
                size_t end = raw_html_.find("</script>", pos);
                if (end == std::string::npos) end = raw_html_.find("</SCRIPT>", pos);
                if (end != std::string::npos) pos = end + 9;
                in_script = false;
            }
            continue;
        }
        if (tag.name == "style") {
            if (!tag.is_closing) {
                in_style = true;
                size_t end = raw_html_.find("</style>", pos);
                if (end == std::string::npos) end = raw_html_.find("</STYLE>", pos);
                if (end != std::string::npos) pos = end + 8;
                in_style = false;
            }
            continue;
        }

        // Head section — extract <title> content
        if (tag.name == "head") { in_head = !tag.is_closing; continue; }
        if (in_head) {
            if (tag.name == "title") in_title = !tag.is_closing;
            continue;
        }

        // ── Headings ─────────────────────────────────
        if (tag.name.size() == 2 && tag.name[0] == 'h' &&
            tag.name[1] >= '1' && tag.name[1] <= '6') {
            if (tag.is_closing) {
                md += "\n\n";
            } else {
                // Ensure newline before heading
                if (!md.empty() && md.back() != '\n') md += '\n';
                int level = tag.name[1] - '0';
                for (int i = 0; i < level; i++) md += '#';
                md += ' ';
            }
            continue;
        }

        // ── Paragraphs and divs ──────────────────────
        if (tag.name == "p" || tag.name == "div" || tag.name == "section" ||
            tag.name == "article" || tag.name == "main") {
            if (tag.is_closing) {
                md += "\n\n";
            } else {
                if (!md.empty() && md.back() != '\n') md += "\n\n";
            }
            continue;
        }

        // ── Line break ──────────────────────────────
        if (tag.name == "br") {
            if (in_cell) current_cell += " ";
            else if (in_li) li_text += " ";
            else md += "\n";
            continue;
        }

        // ── Horizontal rule ─────────────────────────
        if (tag.name == "hr") {
            md += "\n---\n\n";
            continue;
        }

        // ── Bold / Strong ───────────────────────────
        if (tag.name == "b" || tag.name == "strong") {
            if (tag.is_closing) {
                if (in_cell) current_cell += "**";
                else if (in_li) li_text += "**";
                else if (in_a) link_text += "**";
                else md += "**";
                in_bold = false;
            } else {
                if (in_cell) current_cell += "**";
                else if (in_li) li_text += "**";
                else if (in_a) link_text += "**";
                else md += "**";
                in_bold = true;
            }
            continue;
        }

        // ── Italic / Emphasis ───────────────────────
        if (tag.name == "i" || tag.name == "em") {
            if (tag.is_closing) {
                if (in_cell) current_cell += "*";
                else if (in_li) li_text += "*";
                else if (in_a) link_text += "*";
                else md += "*";
                in_italic = false;
            } else {
                if (in_cell) current_cell += "*";
                else if (in_li) li_text += "*";
                else if (in_a) link_text += "*";
                else md += "*";
                in_italic = true;
            }
            continue;
        }

        // ── Code ────────────────────────────────────
        if (tag.name == "code") {
            if (tag.is_closing) {
                if (!in_pre) md += "`";
                in_code = false;
            } else {
                if (!in_pre) md += "`";
                in_code = true;
            }
            continue;
        }

        // ── Preformatted ────────────────────────────
        if (tag.name == "pre") {
            if (tag.is_closing) {
                md += "\n```\n\n";
                in_pre = false;
            } else {
                if (!md.empty() && md.back() != '\n') md += '\n';
                md += "```\n";
                in_pre = true;
            }
            continue;
        }

        // ── Blockquote ──────────────────────────────
        if (tag.name == "blockquote") {
            if (tag.is_closing) {
                md += "\n\n";
            } else {
                if (!md.empty() && md.back() != '\n') md += '\n';
                md += "> ";
            }
            continue;
        }

        // ── Links ───────────────────────────────────
        if (tag.name == "a") {
            if (tag.is_closing) {
                if (!link_text.empty() && !link_href.empty()) {
                    std::string link = "[" + link_text + "](" + link_href + ")";
                    if (in_cell) current_cell += link;
                    else if (in_li) li_text += link;
                    else md += link;
                } else if (!link_text.empty()) {
                    if (in_cell) current_cell += link_text;
                    else if (in_li) li_text += link_text;
                    else md += link_text;
                }
                in_a = false;
                link_text.clear();
                link_href.clear();
            } else {
                in_a = true;
                link_href = tag.get_attr(tag.attrs, "href");
                link_text.clear();
            }
            continue;
        }

        // ── Images ──────────────────────────────────
        if (tag.name == "img") {
            std::string src = tag.get_attr(tag.attrs, "src");
            std::string alt = tag.get_attr(tag.attrs, "alt");
            if (alt.empty()) alt = "image";

            if (!src.empty()) {
                std::string img_md = "![" + alt + "](" + src + ")";
                if (in_cell) current_cell += img_md;
                else if (in_li) li_text += img_md;
                else md += img_md;

                // Create ImageData entry (without actual data for external URLs)
                ImageData img;
                img.page_number = 0;
                img.name = alt.empty() ? ("html_img_" + std::to_string(img_idx)) : alt;
                img.format = "";
                std::string ext = util::get_extension(src);
                if (!ext.empty()) img.format = util::image_format_from_ext(ext);
                out_images.push_back(std::move(img));
                img_idx++;
            }
            continue;
        }

        // ── Lists ───────────────────────────────────
        if (tag.name == "ul") {
            if (tag.is_closing) {
                if (!list_stack.empty()) list_stack.pop();
                if (list_stack.empty()) md += '\n';
            } else {
                if (!md.empty() && md.back() != '\n') md += '\n';
                list_stack.push({false, 0});
            }
            continue;
        }
        if (tag.name == "ol") {
            if (tag.is_closing) {
                if (!list_stack.empty()) list_stack.pop();
                if (list_stack.empty()) md += '\n';
            } else {
                if (!md.empty() && md.back() != '\n') md += '\n';
                list_stack.push({true, 0});
            }
            continue;
        }
        if (tag.name == "li") {
            if (tag.is_closing) {
                // Flush li text
                if (in_li && !list_stack.empty()) {
                    // Trim
                    auto s = li_text.find_first_not_of(" \t\r\n");
                    auto e = li_text.find_last_not_of(" \t\r\n");
                    if (s != std::string::npos)
                        li_text = li_text.substr(s, e - s + 1);
                    else
                        li_text.clear();

                    int depth = (int)list_stack.size() - 1;
                    for (int d = 0; d < depth; d++) md += "  ";

                    auto& ls = list_stack.top();
                    if (ls.ordered) {
                        ls.count++;
                        md += std::to_string(ls.count) + ". ";
                    } else {
                        md += "- ";
                    }
                    md += li_text + "\n";
                }
                in_li = false;
                li_text.clear();
            } else {
                in_li = true;
                li_text.clear();
            }
            continue;
        }

        // ── Tables ──────────────────────────────────
        if (tag.name == "table") {
            if (tag.is_closing) {
                // Flush table
                if (!table_rows.empty()) {
                    md += "\n";
                    // Determine column count
                    size_t n_cols = 0;
                    for (auto& row : table_rows)
                        if (row.size() > n_cols) n_cols = row.size();

                    if (n_cols > 0) {
                        // Compute column widths
                        std::vector<size_t> widths(n_cols, 3);
                        for (auto& row : table_rows)
                            for (size_t c = 0; c < row.size() && c < n_cols; c++)
                                widths[c] = std::max(widths[c], row[c].size());

                        for (size_t r = 0; r < table_rows.size(); r++) {
                            md += "|";
                            for (size_t c = 0; c < n_cols; c++) {
                                std::string cell = (c < table_rows[r].size())
                                    ? table_rows[r][c] : "";
                                md += " " + cell;
                                for (size_t p = cell.size(); p < widths[c]; p++) md += ' ';
                                md += " |";
                            }
                            md += "\n";

                            // Header separator after first row
                            if (r == 0) {
                                md += "|";
                                for (size_t c = 0; c < n_cols; c++) {
                                    md += " ";
                                    for (size_t p = 0; p < widths[c]; p++) md += '-';
                                    md += " |";
                                }
                                md += "\n";
                            }
                        }
                    }
                    md += "\n";
                }
                in_table = false;
                in_thead = false;
                table_rows.clear();
            } else {
                in_table = true;
                table_rows.clear();
            }
            continue;
        }
        if (tag.name == "thead") { in_thead = !tag.is_closing; continue; }
        if (tag.name == "tbody" || tag.name == "tfoot") continue;

        if (tag.name == "tr") {
            if (tag.is_closing) {
                // Flush current row
                if (!current_row.empty()) {
                    table_rows.push_back(std::move(current_row));
                    current_row.clear();
                }
            } else {
                current_row.clear();
            }
            continue;
        }

        if (tag.name == "td" || tag.name == "th") {
            if (tag.is_closing) {
                // Trim cell content
                auto s = current_cell.find_first_not_of(" \t\r\n");
                auto e = current_cell.find_last_not_of(" \t\r\n");
                if (s != std::string::npos)
                    current_cell = current_cell.substr(s, e - s + 1);
                else
                    current_cell.clear();

                // Replace newlines and pipes in cell
                std::string clean;
                for (char c : current_cell) {
                    if (c == '|') clean += "\\|";
                    else if (c == '\n' || c == '\r') clean += ' ';
                    else clean += c;
                }
                current_row.push_back(clean);
                in_cell = false;
                current_cell.clear();
            } else {
                in_cell = true;
                current_cell.clear();
            }
            continue;
        }

        // ── Definition lists ────────────────────────
        if (tag.name == "dl") {
            if (tag.is_closing) md += "\n";
            else if (!md.empty() && md.back() != '\n') md += "\n\n";
            continue;
        }
        if (tag.name == "dt") {
            if (tag.is_closing) md += "\n";
            else md += "**";
            continue;
        }
        if (tag.name == "dd") {
            if (tag.is_closing) md += "\n";
            else md += ": ";
            continue;
        }

        // Ignore other tags
    }

    // Clean up: remove excessive blank lines (more than 2 consecutive newlines)
    std::string result;
    result.reserve(md.size());
    int newline_count = 0;
    for (char c : md) {
        if (c == '\n') {
            newline_count++;
            if (newline_count <= 2) result += c;
        } else {
            newline_count = 0;
            result += c;
        }
    }

    // Prepend page title if extracted from <title> tag
    if (!title_text.empty()) {
        // Trim whitespace
        size_t s = title_text.find_first_not_of(" \t\r\n");
        size_t e = title_text.find_last_not_of(" \t\r\n");
        if (s != std::string::npos) {
            std::string title = decode_entities(title_text.substr(s, e - s + 1));
            result = "# " + title + "\n\n" + result;
        }
    }

    return result;
}

// ── Public API ───────────────────────────────────────────

std::string HtmlParser::to_markdown(const ConvertOptions& opts) {
    std::vector<ImageData> images;
    std::string md = convert(opts, images);

    // Append image references
    if (!images.empty()) {
        md += "\n";
        for (auto& img : images) {
            if (opts.extract_images)
                md += "![" + img.name + "](" + opts.image_ref_prefix + img.name + ")\n";
            else
                md += "![" + img.name + "](embedded:" + img.name + ")\n";
        }
    }

    return md;
}

std::vector<PageChunk> HtmlParser::to_chunks(const ConvertOptions& opts) {
    std::vector<ImageData> images;
    std::string md = convert(opts, images);

    PageChunk chunk;
    chunk.page_number = 1;
    chunk.text = md;
    chunk.page_width = 0;
    chunk.page_height = 0;
    chunk.body_font_size = 0;
    chunk.images = std::move(images);

    return {std::move(chunk)};
}

} // namespace jdoc
