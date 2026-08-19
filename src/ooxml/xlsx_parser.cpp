// XLSX parser implementation
// Parses ZIP-based .xlsx files using pugixml for XML processing

#include "ooxml/xlsx_parser.h"
#include "ooxml/xml_stream_scanner.h"
#include "xml_utils.h"
#include "ooxml/embedded_parts.h"
#include "common/file_utils.h"
#include "common/image_utils.h"
#include "common/png_encode.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <unordered_set>
#include <map>
#include <set>
#include <sstream>

namespace jdoc {

namespace {

// Normalize a package relationship Target into a zip member path.
//  - strips a leading '/' (absolute package path, e.g. openpyxl's "/xl/...")
//  - folds "../" segments against base_dir
//  - otherwise treats the target as relative to base_dir
// base_dir examples: "xl" (workbook.xml), "xl/worksheets" (a sheet part).
static std::string resolve_package_path(std::string target,
                                        const std::string& base_dir) {
    if (target.empty()) return target;
    if (target[0] == '/') return target.substr(1);   // "/xl/foo" -> "xl/foo"

    std::string base = base_dir;
    while (target.rfind("../", 0) == 0) {
        target.erase(0, 3);
        auto slash = base.rfind('/');
        base = (slash == std::string::npos) ? "" : base.substr(0, slash);
    }
    if (target.rfind("xl/", 0) == 0) return target;  // already package-root relative
    return base.empty() ? target : base + "/" + target;
}

// Streaming-path limits. Parts larger than the stream threshold never touch
// pugixml; the hard cap bounds decompression of a part whose header lies
// (nothing legitimate approaches it — Excel itself cannot save a part that
// large).
constexpr uint64_t kDefaultStreamThreshold = 256ull << 20;  // 256 MiB
constexpr uint64_t kMaxStreamPartBytes = 16ull << 30;       // 16 GiB
// Excel grid limits, used to clamp attacker-controlled row/col references so
// a lying r="..." attribute cannot force a multi-gigabyte gap fill.
constexpr int kMaxExcelRows = 1048576;
constexpr int kMaxExcelCols = 16384;

// Markdown-table cell sanitization shared by the DOM and streaming paths.
static void sanitize_cell(std::string& v) {
    for (auto& ch : v) {
        if (ch == '|') ch = '/';
        if (ch == '\n') ch = ' ';
    }
}

} // namespace

// ── Constructor ─────────────────────────────────────────

XlsxParser::XlsxParser(ZipReader& zip) : zip_(zip) {
    parse_shared_strings();
    parse_workbook();
    parse_workbook_rels();
    parse_persons();
    parse_styles();
}

// ── Shared strings (xl/sharedStrings.xml) ───────────────

void XlsxParser::parse_shared_strings() {
    const auto* entry = zip_.find_entry("xl/sharedStrings.xml");
    if (!entry) return;

    if (entry->uncompressed_size > stream_threshold()) {
        parse_shared_strings_streamed(*entry);
        return;
    }

    std::vector<char> data;
    pugi::xml_document doc;
    if (!xml_load_part(zip_, "xl/sharedStrings.xml", doc, data,
                       pugi::parse_default | pugi::parse_ws_pcdata))
        return;

    // Each <si> element contains one shared string
    // Text can be in <t> directly or in <r><t> runs
    std::vector<pugi::xml_node> si_nodes;
    xml_find_all(doc, "si", si_nodes);

    shared_strings_.reserve(si_nodes.size(), 0);

    for (auto& si : si_nodes) {
        // Try direct <t> child first
        auto t_node = xml_child(si, "t");
        if (t_node) {
            shared_strings_.push(xml_text_content(t_node));
            continue;
        }

        // Fall back to rich text: <r><t> runs
        std::string text;
        std::vector<pugi::xml_node> t_nodes;
        xml_find_all(si, "t", t_nodes);
        for (auto& t : t_nodes) {
            text += xml_text_content(t);
        }
        shared_strings_.push(text);
    }
}

// ── Streaming (SAX) path ────────────────────────────────
//
// Parts above stream_threshold() are decompressed chunk-by-chunk and scanned
// without a DOM: pugixml's node overhead on a multi-gigabyte sheet exceeds
// any realistic memory budget, and read_entry()'s 1 GiB cap made such parts
// silently vanish ("Empty sheet"). The scanner keeps only one row (sheet) or
// one string (sharedStrings) of state, so peak memory tracks the shared-string
// arena plus the emitted markdown, not the XML.

uint64_t XlsxParser::stream_threshold() {
    // Re-read each call (a few per workbook): tests toggle the env var to
    // force both paths through the same fixtures.
    if (const char* e = std::getenv("JDOC_XLSX_STREAM_THRESHOLD")) {
        char* end = nullptr;
        unsigned long long n = std::strtoull(e, &end, 10);
        if (end && end != e) return static_cast<uint64_t>(n);
    }
    return kDefaultStreamThreshold;
}

bool XlsxParser::sheet_is_streamed(const SheetInfo& info) const {
    if (info.file_path.empty()) return false;
    const auto* e = zip_.find_entry(info.file_path);
    return e && e->uncompressed_size > stream_threshold();
}

void XlsxParser::parse_shared_strings_streamed(const ZipReader::Entry& entry) {
    // Mirrors the DOM rule: a <si> with a direct <t> child contributes only
    // that text; otherwise every descendant <t> outside mc:Fallback counts
    // (rich-text <r><t> runs, and — matching the DOM walker — <rPh> phonetic
    // runs as well).
    struct SstHandler {
        SharedStringStore& store;
        uint64_t part_size;
        bool in_si = false;
        int si_depth = 0;          // element depth below <si>
        int fallback_depth = 0;
        bool has_direct_t = false;
        bool in_direct_t = false;
        bool in_any_t = false;
        std::string acc_direct, acc_all;

        void on_start(std::string_view name, std::string_view tag_body, bool) {
            if (!in_si) {
                if (name == "sst") {
                    // Reserve from the declared uniqueCount, bounded by what
                    // the part size can physically hold (5 bytes = "<si/>").
                    auto uc = xml_stream_attr(tag_body, "uniqueCount");
                    if (uc.empty()) uc = xml_stream_attr(tag_body, "count");
                    uint64_t n = 0;
                    for (char c : uc) {
                        if (c < '0' || c > '9') { n = 0; break; }
                        n = n * 10 + static_cast<uint64_t>(c - '0');
                        if (n > (1ull << 32)) break;
                    }
                    uint64_t bound = part_size / 5;
                    if (n > bound) n = bound;
                    store.reserve(static_cast<size_t>(n),
                                  static_cast<size_t>(part_size * 11 / 20));
                } else if (name == "si") {
                    in_si = true;
                    si_depth = 0;
                    has_direct_t = false;
                    acc_direct.clear();
                    acc_all.clear();
                }
                return;
            }
            si_depth++;
            if (name == "Fallback") fallback_depth++;
            else if (name == "t" && fallback_depth == 0) {
                in_any_t = true;
                if (si_depth == 1) { has_direct_t = true; in_direct_t = true; }
            }
        }
        void on_end(std::string_view name) {
            if (!in_si) return;
            if (si_depth == 0) {
                if (name == "si") {
                    store.push(has_direct_t ? acc_direct : acc_all);
                    in_si = false;
                }
                return;
            }
            if (name == "t") { in_any_t = false; in_direct_t = false; }
            else if (name == "Fallback" && fallback_depth > 0) fallback_depth--;
            si_depth--;
        }
        void on_text(const char* d, size_t n) {
            if (!in_any_t) return;
            acc_all.append(d, n);
            if (in_direct_t) acc_direct.append(d, n);
        }
    };

    SstHandler handler{shared_strings_, entry.uncompressed_size};
    XmlStreamScanner<SstHandler> scanner(handler);

    uint64_t fed = 0;
    bool over_cap = false;
    std::string zip_err;
    bool ok = zip_.read_entry_streamed(entry, [&](const char* d, size_t n) {
        fed += n;
        if (fed > kMaxStreamPartBytes) { over_cap = true; return false; }
        return scanner.feed(d, n);
    }, &zip_err);
    if (ok) ok = scanner.finish();

    if (!ok) {
        std::string why = over_cap ? "part exceeds size cap"
                        : scanner.error() ? scanner.error()
                        : zip_err;
        throw std::runtime_error(
            "XLSX: cannot read xl/sharedStrings.xml (streaming): " + why);
    }
}

// ── Workbook (xl/workbook.xml) ──────────────────────────

void XlsxParser::parse_workbook() {
    std::vector<char> data;
    pugi::xml_document doc;
    if (!xml_load_part(zip_, "xl/workbook.xml", doc, data,
                       pugi::parse_default | pugi::parse_ws_pcdata))
        return;

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
    std::vector<char> data;
    pugi::xml_document doc;
    if (!xml_load_part(zip_, rels_path, doc, data,
                       pugi::parse_default | pugi::parse_ws_pcdata))
        return;

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
            // Targets are relative to the xl/ directory; also handles absolute
            // package paths ("/xl/...") emitted by some writers (e.g. openpyxl).
            sheet.file_path = resolve_package_path(it->second, "xl");
        }
    }
}

// ── Styles (xl/styles.xml) — number format parsing ──────

void XlsxParser::parse_styles() {
    std::vector<char> data;
    pugi::xml_document doc;
    if (!xml_load_part(zip_, "xl/styles.xml", doc, data,
                       pugi::parse_default | pugi::parse_ws_pcdata))
        return;

    // Parse fonts: <fonts><font><b/> means bold
    std::vector<pugi::xml_node> fonts_nodes;
    xml_find_all(doc, "fonts", fonts_nodes);
    if (!fonts_nodes.empty()) {
        for (auto font = fonts_nodes[0].first_child(); font; font = font.next_sibling()) {
            const char* flocal = xml_local_name(font);
            if (strcmp(flocal, "font") != 0) continue;
            bool bold = xml_child(font, "b") ? true : false;
            font_bold_.push_back(bold);
        }
    }

    // Parse custom number formats: <numFmts><numFmt numFmtId="..." formatCode="..."/>
    std::vector<pugi::xml_node> fmt_nodes;
    xml_find_all(doc, "numFmt", fmt_nodes);
    for (auto& node : fmt_nodes) {
        const char* id_str = xml_attr(node, "numFmtId");
        const char* code = xml_attr(node, "formatCode");
        if (id_str[0] && code[0]) {
            custom_num_fmts_[std::atoi(id_str)] = code;
        }
    }

    // Parse cell formats: <cellXfs><xf numFmtId="..." fontId="..."/>
    std::vector<pugi::xml_node> xf_nodes;
    xml_find_all(doc, "xf", xf_nodes);

    // cellXfs entries come after cellStyleXfs entries.
    // Find the <cellXfs> parent to get the right set.
    std::vector<pugi::xml_node> cell_xfs_nodes;
    xml_find_all(doc, "cellXfs", cell_xfs_nodes);

    if (!cell_xfs_nodes.empty()) {
        auto cellXfs = cell_xfs_nodes[0];
        for (auto xf = cellXfs.first_child(); xf; xf = xf.next_sibling()) {
            const char* local = xml_local_name(xf);
            if (strcmp(local, "xf") != 0) continue;

            const char* fmt_id = xml_attr(xf, "numFmtId");
            xf_num_fmt_ids_.push_back(fmt_id[0] ? std::atoi(fmt_id) : 0);

            const char* font_id = xml_attr(xf, "fontId");
            xf_font_ids_.push_back(font_id[0] ? std::atoi(font_id) : 0);
        }
    }
}

// ── Number formatting ───────────────────────────────────

bool XlsxParser::is_date_format(int fmt_id, const std::string& fmt_code) {
    // Built-in date/time format IDs
    if ((fmt_id >= 14 && fmt_id <= 22) ||
        (fmt_id >= 27 && fmt_id <= 36) ||
        (fmt_id >= 45 && fmt_id <= 47) ||
        (fmt_id >= 50 && fmt_id <= 58)) {
        return true;
    }

    // Check custom format code for date/time patterns
    if (fmt_code.empty()) return false;
    std::string lower;
    for (char c : fmt_code) lower += std::tolower(static_cast<unsigned char>(c));

    // Skip escaped chars and quoted strings
    bool in_quote = false;
    for (size_t i = 0; i < lower.size(); i++) {
        if (lower[i] == '"') { in_quote = !in_quote; continue; }
        if (in_quote) continue;
        if (lower[i] == '\\') { i++; continue; }
        char ch = lower[i];
        if (ch == 'y' || ch == 'd') return true;
        // 'm' is date only if not preceded by 'h' or followed by 's'
        if (ch == 'h') return true;
        if (ch == 's' && i > 0) return true;
    }
    return false;
}

std::string XlsxParser::serial_to_date(double serial) {
    // Excel epoch: day 1 = 1900-01-01, with Lotus 1-2-3 bug (day 60 = Feb 29, 1900)
    int days = static_cast<int>(serial);
    if (days < 1) return "0000-00-00";

    // Lotus bug: Excel treats 1900 as a leap year.
    // Day 60 = "Feb 29, 1900" which doesn't exist.
    // For days > 60, subtract 1 to correct. For days <= 60, keep as-is.
    if (days > 60) days--;

    // days is now 1-based from 1900-01-01
    days--; // make 0-based

    int y = 1900;
    while (true) {
        bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        int year_days = leap ? 366 : 365;
        if (days < year_days) break;
        days -= year_days;
        y++;
    }

    bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int m = 0;
    for (m = 0; m < 12; m++) {
        int md = month_days[m] + (m == 1 && leap ? 1 : 0);
        if (days < md) break;
        days -= md;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m + 1, days + 1);
    return buf;
}

std::string XlsxParser::serial_to_time(double serial) {
    double frac = serial - static_cast<int>(serial);
    int total_secs = static_cast<int>(frac * 86400 + 0.5);
    int h = total_secs / 3600;
    int m = (total_secs % 3600) / 60;
    int s = total_secs % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    return buf;
}

std::string XlsxParser::format_number(const std::string& raw_value,
                                       int style_idx) const {
    if (style_idx < 0 || style_idx >= static_cast<int>(xf_num_fmt_ids_.size()))
        return raw_value;

    int fmt_id = xf_num_fmt_ids_[style_idx];
    if (fmt_id == 0) return raw_value; // General

    // Look up format code
    std::string fmt_code;
    auto it = custom_num_fmts_.find(fmt_id);
    if (it != custom_num_fmts_.end()) {
        fmt_code = it->second;
    }

    double val = std::atof(raw_value.c_str());

    // Date/time formats
    if (is_date_format(fmt_id, fmt_code)) {
        // Pure time formats (IDs 18-21, 45-47)
        bool is_time_only = (fmt_id >= 18 && fmt_id <= 21) ||
                            (fmt_id >= 45 && fmt_id <= 47);
        if (is_time_only) {
            return serial_to_time(val);
        }
        // Date (possibly with time)
        std::string result = serial_to_date(val);
        double frac = val - static_cast<int>(val);
        if (frac > 0.0001 && (fmt_id == 22 ||
            fmt_code.find('h') != std::string::npos ||
            fmt_code.find('H') != std::string::npos)) {
            result += " " + serial_to_time(val);
        }
        return result;
    }

    // Percentage formats (IDs 9-10)
    if (fmt_id == 9) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.0f%%", val * 100.0);
        return buf;
    }
    if (fmt_id == 10) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f%%", val * 100.0);
        return buf;
    }

    // Check custom format for percentage
    if (!fmt_code.empty() && fmt_code.find('%') != std::string::npos) {
        // Count decimal places after '0' before '%'
        int decimals = 0;
        bool after_dot = false;
        for (char c : fmt_code) {
            if (c == '.') after_dot = true;
            else if (after_dot && c == '0') decimals++;
            else if (c == '%') break;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%.*f%%", decimals, val * 100.0);
        return buf;
    }

    // Number with comma grouping (IDs 3-4, 37-40)
    if (fmt_id == 3 || fmt_id == 37 || fmt_id == 38) {
        // #,##0
        long long ival = static_cast<long long>(val + (val >= 0 ? 0.5 : -0.5));
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld", ival);
        std::string s = buf;
        // Insert commas
        int start = (s[0] == '-') ? 1 : 0;
        int len = static_cast<int>(s.size()) - start;
        if (len > 3) {
            for (int i = len - 3; i > 0; i -= 3) {
                s.insert(start + i, 1, ',');
            }
        }
        return s;
    }
    if (fmt_id == 4 || fmt_id == 39 || fmt_id == 40) {
        // #,##0.00
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f", val);
        std::string s = buf;
        auto dot = s.find('.');
        int start = (s[0] == '-') ? 1 : 0;
        int int_len = static_cast<int>(dot) - start;
        if (int_len > 3) {
            for (int i = int_len - 3; i > 0; i -= 3) {
                s.insert(start + i, 1, ',');
            }
        }
        return s;
    }

    // Scientific notation (ID 11)
    if (fmt_id == 11) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2E", val);
        return buf;
    }

    // Fraction formats (IDs 12-13) — just show decimal
    if (fmt_id == 12 || fmt_id == 13) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.4g", val);
        return buf;
    }

    // Fixed decimal formats (IDs 1-2)
    if (fmt_id == 1) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.0f", val);
        return buf;
    }
    if (fmt_id == 2) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f", val);
        return buf;
    }

    // Currency formats (IDs 5-8)
    if (fmt_id >= 5 && fmt_id <= 8) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f", val);
        std::string s = buf;
        auto dot = s.find('.');
        int start = (s[0] == '-') ? 1 : 0;
        int int_len = static_cast<int>(dot) - start;
        if (int_len > 3) {
            for (int i = int_len - 3; i > 0; i -= 3) {
                s.insert(start + i, 1, ',');
            }
        }
        return s;
    }

    // For unknown custom formats, try to detect date pattern
    if (!fmt_code.empty()) {
        std::string lower;
        for (char c : fmt_code) lower += std::tolower(static_cast<unsigned char>(c));
        if (lower.find('#') != std::string::npos ||
            lower.find('0') != std::string::npos) {
            // Numeric format — just clean up trailing zeros
            char buf[64];
            int decimals = 0;
            bool after_dot = false;
            for (char c : fmt_code) {
                if (c == '.') after_dot = true;
                else if (after_dot && (c == '0' || c == '#')) decimals++;
            }
            snprintf(buf, sizeof(buf), "%.*f", decimals, val);
            return buf;
        }
    }

    return raw_value;
}

bool XlsxParser::is_bold_style(int style_idx) const {
    if (style_idx < 0 || style_idx >= static_cast<int>(xf_font_ids_.size()))
        return false;
    int font_id = xf_font_ids_[style_idx];
    if (font_id < 0 || font_id >= static_cast<int>(font_bold_.size()))
        return false;
    return font_bold_[font_id];
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

// ── Comments parsing ────────────────────────────────────

std::map<std::string, std::string> XlsxParser::parse_comments(
    const SheetInfo& info) {

    std::map<std::string, std::string> comments;
    if (info.file_path.empty()) return comments;

    // Find sheet rels to locate comments file
    auto slash = info.file_path.rfind('/');
    if (slash == std::string::npos) return comments;
    std::string dir = info.file_path.substr(0, slash);
    std::string base = info.file_path.substr(slash + 1);
    std::string rels_path = dir + "/_rels/" + base + ".rels";

    std::vector<char> rels_data;
    pugi::xml_document rels_doc;
    if (!xml_load_part(zip_, rels_path, rels_doc, rels_data))
        return comments;

    // Locate the legacy (xl/comments*.xml) and modern threaded-comment parts.
    // Both Target forms — relative and absolute ("/xl/...") — are normalized.
    std::string legacy_path, threaded_path;
    std::vector<pugi::xml_node> rel_nodes;
    xml_find_all(rels_doc, "Relationship", rel_nodes);
    for (auto& rel : rel_nodes) {
        const char* type = xml_attr(rel, "Type");
        const char* target = xml_attr(rel, "Target");
        if (!target[0]) continue;
        std::string type_str = type;
        if (type_str.find("threadedComment") != std::string::npos) {
            threaded_path = resolve_package_path(target, dir);
        } else if (type_str.find("/comments") != std::string::npos) {
            legacy_path = resolve_package_path(target, dir);
        }
    }

    // Legacy comments first; threaded comments (author + reply chain) override
    // the same cell when both are present (legacy is kept only for compat).
    if (!legacy_path.empty() && zip_.has_entry(legacy_path))
        parse_legacy_comments(legacy_path, comments);
    if (!threaded_path.empty() && zip_.has_entry(threaded_path))
        parse_threaded_comments(threaded_path, comments);

    return comments;
}

void XlsxParser::parse_legacy_comments(
    const std::string& path, std::map<std::string, std::string>& out) {

    std::vector<char> data;
    pugi::xml_document doc;
    if (!xml_load_part(zip_, path, doc, data,
                       pugi::parse_default | pugi::parse_ws_pcdata))
        return;

    // Parse <commentList><comment ref="A1"><text><t>...</t></text></comment>
    std::vector<pugi::xml_node> comment_nodes;
    xml_find_all(doc, "comment", comment_nodes);

    for (auto& node : comment_nodes) {
        const char* ref = xml_attr(node, "ref");
        if (!ref[0]) continue;

        // Get text from <text> child -> <t> or <r><t> runs
        std::string text;
        auto text_node = xml_child(node, "text");
        if (text_node) {
            std::vector<pugi::xml_node> t_nodes;
            xml_find_all(text_node, "t", t_nodes);
            for (auto& t : t_nodes) {
                text += xml_text_content(t);
            }
        }

        if (!text.empty()) {
            out[ref] = text;
        }
    }
}

void XlsxParser::parse_threaded_comments(
    const std::string& path, std::map<std::string, std::string>& out) {

    std::vector<char> data;
    pugi::xml_document doc;
    if (!xml_load_part(zip_, path, doc, data,
                       pugi::parse_default | pugi::parse_ws_pcdata))
        return;

    // <threadedComment ref="A1" personId="{..}"><text>..</text></threadedComment>
    // Replies to a cell share its ref; join them in document order.
    std::map<std::string, std::string> threads;  // ref -> joined thread
    std::vector<pugi::xml_node> tc_nodes;
    xml_find_all(doc, "threadedComment", tc_nodes);
    for (auto& node : tc_nodes) {
        const char* ref = xml_attr(node, "ref");
        if (!ref[0]) continue;

        std::string text;
        auto text_node = xml_child(node, "text");
        if (text_node) text = xml_text_content(text_node);
        if (text.empty()) continue;

        const char* pid = xml_attr(node, "personId");
        std::string author;
        if (pid[0]) {
            auto it = persons_.find(pid);
            if (it != persons_.end()) author = it->second;
        }
        std::string entry = author.empty() ? text : author + ": " + text;

        auto it = threads.find(ref);
        if (it == threads.end()) threads[ref] = entry;
        else it->second += " / " + entry;
    }

    for (auto& [ref, txt] : threads) out[ref] = txt;
}

// Threaded-comment authors live in xl/persons/person*.xml, referenced by GUID.
void XlsxParser::parse_persons() {
    for (const auto* e : zip_.entries_with_prefix("xl/persons/")) {
        std::vector<char> data;
        pugi::xml_document doc;
        if (!xml_load_part(zip_, e->name, doc, data)) continue;
        std::vector<pugi::xml_node> person_nodes;
        xml_find_all(doc, "person", person_nodes);
        for (auto& p : person_nodes) {
            const char* id = xml_attr(p, "id");
            const char* name = xml_attr(p, "displayName");
            if (id[0] && name[0]) persons_[id] = name;
        }
    }
}

// ── Sheet parsing ───────────────────────────────────────

XlsxParser::SheetData XlsxParser::parse_sheet(const SheetInfo& info) {
    SheetData sheet;
    sheet.name = info.name;

    if (info.file_path.empty() || !zip_.has_entry(info.file_path)) {
        return sheet;
    }

    std::vector<char> data;
    pugi::xml_document doc;
    if (!xml_load_part(zip_, info.file_path, doc, data,
                       pugi::parse_default | pugi::parse_ws_pcdata))
        return sheet;

    // Parse comments for this sheet
    auto comments = parse_comments(info);

    // Find <sheetData> element
    std::vector<pugi::xml_node> sheet_data_nodes;
    xml_find_all(doc, "sheetData", sheet_data_nodes);
    if (sheet_data_nodes.empty()) return sheet;

    auto sheetData = sheet_data_nodes[0];

    // Walk <row> elements
    for (auto row_node = sheetData.first_child(); row_node;
         row_node = row_node.next_sibling()) {

        const char* rlocal = xml_local_name(row_node);
        if (strcmp(rlocal, "row") != 0) continue;

        // Walk <c> (cell) elements within this row
        for (auto cell = row_node.first_child(); cell;
             cell = cell.next_sibling()) {

            const char* clocal = xml_local_name(cell);
            if (strcmp(clocal, "c") != 0) continue;

            // Get cell reference (e.g. "A1")
            const char* ref = xml_attr(cell, "r");
            if (!ref[0]) continue;

            auto [col, row] = parse_cell_ref(ref);

            // Determine cell value based on type
            const char* cell_type = xml_attr(cell, "t");
            const char* style_str = xml_attr(cell, "s");
            int style_idx = style_str[0] ? std::atoi(style_str) : -1;
            std::string value;

            // cell_type is a const char* attribute value; compare directly
            // (strcmp, matching the element-name checks above) rather than
            // constructing a throwaway std::string for every cell.
            if (strcmp(cell_type, "s") == 0) {
                // Shared string reference
                auto v_node = xml_child(cell, "v");
                if (v_node) {
                    std::string idx_str = xml_text_content(v_node);
                    if (!idx_str.empty()) {
                        int idx = std::atoi(idx_str.c_str());
                        if (idx >= 0 &&
                            static_cast<size_t>(idx) < shared_strings_.size()) {
                            auto sv = shared_strings_.get(static_cast<size_t>(idx));
                            value.assign(sv.data(), sv.size());
                        }
                    }
                }
            } else if (strcmp(cell_type, "inlineStr") == 0) {
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
            } else if (strcmp(cell_type, "b") == 0) {
                // Boolean
                auto v_node = xml_child(cell, "v");
                if (v_node) {
                    std::string v = xml_text_content(v_node);
                    value = (v == "1") ? "TRUE" : "FALSE";
                }
            } else if (strcmp(cell_type, "e") == 0) {
                // Error
                auto v_node = xml_child(cell, "v");
                if (v_node) {
                    value = xml_text_content(v_node);
                }
            } else {
                // Numeric or formula result — apply number formatting
                auto v_node = xml_child(cell, "v");
                if (v_node) {
                    std::string raw = xml_text_content(v_node);
                    value = format_number(raw, style_idx);
                }
            }

            // Append comment if exists for this cell
            auto cit = comments.find(ref);
            if (cit != comments.end()) {
                if (!value.empty()) value += " ";
                value += "[" + cit->second + "]";
            }

            if (!value.empty()) {
                // Sanitize for markdown table
                for (auto& ch : value) {
                    if (ch == '|') ch = '/';
                    if (ch == '\n') ch = ' ';
                }
                bool bold = is_bold_style(style_idx);
                sheet.cells.push_back({row, col, {value, bold}});
                sheet.max_row = std::max(sheet.max_row, row);
                sheet.max_col = std::max(sheet.max_col, col);
            }
        }
    }

    // A comment can be anchored to an empty cell, which has no <c> element and
    // is therefore never visited by the loop above. Inject any such unconsumed
    // comment at its own coordinate so the memo is not silently dropped.
    if (!comments.empty()) {
        // The flat vector has no O(log n) membership test, so build the set of
        // already-present coordinates once (packing row/col into a 64-bit key).
        // A comment on a populated cell was already merged above; skip those.
        auto key_of = [](int r, int c) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(r)) << 32) |
                   static_cast<uint32_t>(c);
        };
        std::unordered_set<uint64_t> present;
        present.reserve(sheet.cells.size() * 2 + 1);
        for (const auto& cell : sheet.cells)
            present.insert(key_of(cell.row, cell.col));
        for (const auto& [ref, text] : comments) {
            auto [c, r] = parse_cell_ref(ref);
            if (!present.insert(key_of(r, c)).second) continue;  // already present
            std::string v = "[" + text + "]";
            for (auto& ch : v) {
                if (ch == '|') ch = '/';
                if (ch == '\n') ch = ' ';
            }
            sheet.cells.push_back({r, c, {v, false}});
            sheet.max_row = std::max(sheet.max_row, r);
            sheet.max_col = std::max(sheet.max_col, c);
        }
    }

    // Row-major order so the table renderer can walk the cells with a single
    // advancing cursor. xlsx usually emits cells in this order already, but
    // comment-anchored cells were appended out of band, so sort to be safe.
    std::sort(sheet.cells.begin(), sheet.cells.end(),
              [](const Cell& a, const Cell& b) {
                  return a.row != b.row ? a.row < b.row : a.col < b.col;
              });

    return sheet;
}

// ── Streaming sheet → markdown ──────────────────────────
//
// Emits the same dense grid format_sheet_as_table() renders, but row by row
// as the XML streams past, so neither the XML nor the cell set is ever
// resident. Cells arrive in row-major order in real files; the only buffered
// state is the current row. Empty rows are emitted lazily (a run of trailing
// empty rows is never emitted), matching the DOM grid, which ends at the last
// nonempty cell.

uint64_t XlsxParser::stream_sheet_markdown(const SheetInfo& info,
                                           std::string& out) {
    const auto* entry = zip_.find_entry(info.file_path);
    if (!entry) return 0;

    // Comments are human-authored (tiny); group by 0-based (row, col) so the
    // stream can merge them in order, matching the DOM's ref-keyed merge.
    auto comments = parse_comments(info);
    std::map<int, std::map<int, const std::string*>> comments_by_row;
    for (auto& [ref, text] : comments) {
        auto [c, r] = parse_cell_ref(ref);
        if (r >= 0 && r < kMaxExcelRows && c >= 0 && c < kMaxExcelCols)
            comments_by_row[r][c] = &text;
    }

    struct RowCell {
        int col;
        std::string value;
        bool bold;
    };

    struct SheetHandler {
        XlsxParser* p;
        std::string& out;
        const std::map<int, std::map<int, const std::string*>>& cmts;

        // Grid geometry: fixed once the first row is emitted. Zero until the
        // <dimension> element (or, absent one, the first nonempty row) sets it.
        int total_cols = 0;

        // Emission state: rows [0, emitted_rows) are already written.
        int emitted_rows = 0;
        uint64_t cells_out = 0;

        // Current-row state
        bool in_sheet_data = false;
        bool in_row = false;
        int cur_row = -1;
        int next_auto_row = 0;
        std::vector<RowCell> row_cells;

        // Current-cell state
        bool in_c = false;
        int cell_col = 0;
        int next_auto_col = 0;
        char cell_type = 'n';       // 's'hared, 'i'nlineStr, 'b'ool, 'e'rror, 'n'umeric
        int style_idx = -1;
        bool in_v = false, v_seen = false;
        std::string vtext;
        // <is> text collection (direct-<t>-wins rule, same as sharedStrings)
        bool in_is = false;
        int is_depth = 0, fallback_depth = 0;
        bool is_has_direct = false, in_is_direct_t = false, in_is_any_t = false;
        std::string is_direct, is_all;

        static int parse_int(std::string_view s) {
            int v = 0;
            for (char c : s) {
                if (c < '0' || c > '9') return -1;
                if (v > 214748363) return -1;  // overflow guard
                v = v * 10 + (c - '0');
            }
            return s.empty() ? -1 : v;
        }

        void on_start(std::string_view name, std::string_view tag_body,
                      bool /*self_closing*/) {
            if (in_c) {
                if (name == "v") { in_v = true; v_seen = true; vtext.clear(); }
                else if (name == "is") {
                    in_is = true; is_depth = 0; fallback_depth = 0;
                    is_has_direct = false;
                    is_direct.clear(); is_all.clear();
                } else if (in_is) {
                    is_depth++;
                    if (name == "Fallback") fallback_depth++;
                    else if (name == "t" && fallback_depth == 0) {
                        in_is_any_t = true;
                        if (is_depth == 1) { is_has_direct = true; in_is_direct_t = true; }
                    }
                }
                return;
            }
            if (in_row && name == "c") {
                in_c = true;
                v_seen = false;
                vtext.clear();
                in_is = false;
                style_idx = -1;
                cell_type = 'n';

                auto ref = xml_stream_attr(tag_body, "r");
                if (!ref.empty()) {
                    // column letters only; the row is taken from <row r>
                    int col = 0;
                    bool any = false;
                    for (char ch : ref) {
                        if (ch >= 'A' && ch <= 'Z') { col = col * 26 + (ch - 'A' + 1); any = true; }
                        else if (ch >= 'a' && ch <= 'z') { col = col * 26 + (ch - 'a' + 1); any = true; }
                        else break;
                        if (col > kMaxExcelCols) { any = false; break; }
                    }
                    cell_col = any ? col - 1 : next_auto_col;
                } else {
                    cell_col = next_auto_col;
                }
                if (cell_col < 0 || cell_col >= kMaxExcelCols) cell_col = next_auto_col;

                auto t = xml_stream_attr(tag_body, "t");
                if (t == "s") cell_type = 's';
                else if (t == "inlineStr") cell_type = 'i';
                else if (t == "b") cell_type = 'b';
                else if (t == "e") cell_type = 'e';
                else cell_type = 'n';   // numeric, "str", or absent

                auto s = xml_stream_attr(tag_body, "s");
                if (!s.empty()) style_idx = parse_int(s);
                return;
            }
            if (in_sheet_data && name == "row") {
                in_row = true;
                row_cells.clear();
                next_auto_col = 0;
                int r = parse_int(xml_stream_attr(tag_body, "r"));
                // r is 1-based; fall back to sequential numbering, and clamp
                // out-of-order rows forward (the grid cannot rewind).
                cur_row = (r >= 1 && r <= kMaxExcelRows) ? r - 1 : next_auto_row;
                if (cur_row < emitted_rows) cur_row = emitted_rows;
                return;
            }
            if (name == "sheetData") { in_sheet_data = true; return; }
            if (name == "dimension" && total_cols == 0) {
                // "A1:AS1000000" → column extent from the part after ':'
                auto ref = xml_stream_attr(tag_body, "ref");
                auto colon = ref.rfind(':');
                auto last = (colon == std::string_view::npos)
                    ? ref : ref.substr(colon + 1);
                int col = 0;
                for (char ch : last) {
                    if (ch >= 'A' && ch <= 'Z') col = col * 26 + (ch - 'A' + 1);
                    else if (ch >= 'a' && ch <= 'z') col = col * 26 + (ch - 'a' + 1);
                    else break;
                    if (col > kMaxExcelCols) { col = kMaxExcelCols; break; }
                }
                total_cols = col;
            }
        }

        void on_end(std::string_view name) {
            if (in_c) {
                if (name == "c") { finish_cell(); in_c = false; }
                else if (name == "v") in_v = false;
                else if (name == "is" && is_depth == 0) in_is = false;
                else if (in_is) {
                    if (name == "t") { in_is_any_t = false; in_is_direct_t = false; }
                    else if (name == "Fallback" && fallback_depth > 0) fallback_depth--;
                    is_depth--;
                }
                return;
            }
            if (in_row && name == "row") { finish_row(); in_row = false; }
            else if (name == "sheetData") in_sheet_data = false;
        }

        void on_text(const char* d, size_t n) {
            if (in_v) vtext.append(d, n);
            else if (in_is_any_t) {
                is_all.append(d, n);
                if (in_is_direct_t) is_direct.append(d, n);
            }
        }

        void finish_cell() {
            next_auto_col = cell_col + 1;

            std::string value;
            switch (cell_type) {
            case 's':
                if (v_seen && !vtext.empty()) {
                    int idx = parse_int(vtext);
                    if (idx >= 0 &&
                        static_cast<size_t>(idx) < p->shared_strings_.size()) {
                        auto sv = p->shared_strings_.get(static_cast<size_t>(idx));
                        value.assign(sv.data(), sv.size());
                    }
                }
                break;
            case 'i':
                value = is_has_direct ? is_direct : is_all;
                break;
            case 'b':
                if (v_seen) value = (vtext == "1") ? "TRUE" : "FALSE";
                break;
            case 'e':
                if (v_seen) value = vtext;
                break;
            default:
                if (v_seen) value = p->format_number(vtext, style_idx);
                break;
            }

            // Merge this cell's comment (same rule as the DOM path: a comment
            // makes even a value-less cell nonempty).
            auto rit = cmts.find(cur_row);
            if (rit != cmts.end()) {
                auto cit = rit->second.find(cell_col);
                if (cit != rit->second.end()) {
                    if (!value.empty()) value += " ";
                    value += "[" + *cit->second + "]";
                }
            }

            if (value.empty()) return;
            sanitize_cell(value);
            bool bold = p->is_bold_style(style_idx);
            row_cells.push_back({cell_col, std::move(value), bold});
            cells_out++;
        }

        // Inject comments anchored to cells the XML never visited in row r.
        void inject_comment_cells(int r, std::vector<RowCell>& cells) {
            auto rit = cmts.find(r);
            if (rit == cmts.end()) return;
            for (auto& [c, text] : rit->second) {
                bool present = false;
                for (auto& rc : cells) {
                    if (rc.col == c) { present = true; break; }
                }
                if (present) continue;
                std::string v = "[" + *text + "]";
                sanitize_cell(v);
                cells.push_back({c, std::move(v), false});
                cells_out++;
            }
        }

        void finish_row() {
            next_auto_row = cur_row + 1;
            inject_comment_cells(cur_row, row_cells);
            if (row_cells.empty()) return;  // all-empty row: emit lazily
            emit_gap_rows(cur_row);
            emit_row(cur_row, row_cells);
        }

        // Emit rows [emitted_rows, upto) — grid filler. A gap row is empty
        // except for comment-only cells anchored inside it.
        void emit_gap_rows(int upto) {
            std::vector<RowCell> gap;
            for (int g = emitted_rows; g < upto; ++g) {
                gap.clear();
                inject_comment_cells(g, gap);
                emit_row(g, gap);
            }
        }

        void emit_row(int r, std::vector<RowCell>& cells) {
            std::sort(cells.begin(), cells.end(),
                      [](const RowCell& a, const RowCell& b) {
                          return a.col < b.col;
                      });
            // Fix geometry on first emission if <dimension> was absent/empty.
            if (total_cols == 0)
                total_cols = cells.empty() ? 1 : cells.back().col + 1;

            // Width extends past total_cols when a row overflows a lying
            // dimension — content is never dropped, the row is just ragged.
            int width = total_cols;
            if (!cells.empty() && cells.back().col + 1 > width)
                width = cells.back().col + 1;

            size_t ci = 0;
            out += "|";
            for (int c = 0; c < width; ++c) {
                out += " ";
                while (ci < cells.size() && cells[ci].col < c) ++ci;
                if (ci < cells.size() && cells[ci].col == c) {
                    const auto& cell = cells[ci];
                    if (!cell.value.empty() && cell.bold) {
                        out += "**"; out += cell.value; out += "**";
                    } else {
                        out += cell.value;
                    }
                }
                out += " |";
            }
            out += "\n";

            if (r == 0) {
                out += "|";
                for (int c = 0; c < total_cols; ++c) out += " --- |";
                out += "\n";
            }
            emitted_rows = r + 1;
        }

        // Trailing comment-only rows (anchored past the last XML row).
        void flush_trailing() {
            for (auto& [r, cols] : cmts) {
                if (r < emitted_rows) continue;
                std::vector<RowCell> cells;
                inject_comment_cells(r, cells);
                if (cells.empty()) continue;
                emit_gap_rows(r);
                emit_row(r, cells);
            }
        }
    };

    SheetHandler handler{this, out, comments_by_row};
    XmlStreamScanner<SheetHandler> scanner(handler);

    uint64_t fed = 0;
    bool over_cap = false;
    std::string zip_err;
    bool ok = zip_.read_entry_streamed(*entry, [&](const char* d, size_t n) {
        fed += n;
        if (fed > kMaxStreamPartBytes) { over_cap = true; return false; }
        return scanner.feed(d, n);
    }, &zip_err);
    if (ok) ok = scanner.finish();

    if (!ok) {
        std::string why = over_cap ? "part exceeds size cap"
                        : scanner.error() ? scanner.error()
                        : zip_err;
        throw std::runtime_error(
            "XLSX: cannot read sheet '" + info.name + "' (streaming): " + why);
    }

    handler.flush_trailing();
    return handler.cells_out;
}

// ── Format sheet data as markdown table ─────────────────

std::string XlsxParser::format_sheet_as_table(const SheetData& sheet,
                                                int max_rows) {
    if (sheet.cells.empty()) return "";

    int total_rows = sheet.max_row + 1;
    int total_cols = sheet.max_col + 1;
    // max_rows <= 0 means no limit — extract every row.
    bool truncated = max_rows > 0 && total_rows > max_rows;
    int display_rows = truncated ? max_rows : total_rows;

    std::string out;
    // A wide sheet builds a large table; reserve up front to avoid the repeated
    // reallocations an ostringstream/growing string would otherwise incur.
    out.reserve(static_cast<size_t>(display_rows) * total_cols * 8 + 64);

    // The cells are sorted row-major, so a single cursor advances through them
    // as the dense grid is emitted — no per-cell lookup at all. `ci` is shared
    // across rows and only ever moves forward.
    const auto& cells = sheet.cells;
    const size_t ncells = cells.size();
    size_t ci = 0;
    auto emit_row = [&](int r) {
        out += "|";
        for (int c = 0; c < total_cols; ++c) {
            out += " ";
            while (ci < ncells && (cells[ci].row < r ||
                   (cells[ci].row == r && cells[ci].col < c)))
                ++ci;
            if (ci < ncells && cells[ci].row == r && cells[ci].col == c) {
                const std::string& val = cells[ci].info.value;
                if (!val.empty() && cells[ci].info.bold) {
                    out += "**"; out += val; out += "**";
                } else {
                    out += val;
                }
            }
            out += " |";
        }
        out += "\n";
    };

    // Header row (row 0)
    emit_row(0);

    // Separator
    out += "|";
    for (int c = 0; c < total_cols; ++c) {
        out += " --- |";
    }
    out += "\n";

    // Data rows
    for (int r = 1; r < display_rows; ++r) {
        emit_row(r);
    }

    if (truncated) {
        out += "\n*... truncated at "; out += std::to_string(max_rows);
        out += " rows (total: "; out += std::to_string(total_rows); out += " rows)*\n";
    }

    return out;
}

// ── Image extraction ────────────────────────────────────

std::vector<ImageData> XlsxParser::extract_images(
    const ConvertOptions& opts) {

    std::vector<ImageData> images;
    if (!opts.images) return images;

    std::set<std::string> extracted;

    // Per-sheet image extraction via drawing relationships
    int img_idx = 0;
    for (size_t i = 0; i < sheets_.size(); ++i) {
        int sheet_num = static_cast<int>(i) + 1;
        const auto& info = sheets_[i];
        if (info.file_path.empty()) continue;

        // Parse sheet rels to find drawing reference
        auto slash = info.file_path.rfind('/');
        if (slash == std::string::npos) continue;
        std::string dir = info.file_path.substr(0, slash);
        std::string base = info.file_path.substr(slash + 1);
        std::string sheet_rels = dir + "/_rels/" + base + ".rels";

        std::vector<char> rels_data;
        pugi::xml_document rels_doc;
        if (!xml_load_part(zip_, sheet_rels, rels_doc, rels_data)) continue;

        // Find drawing targets (type ends with /drawing)
        std::vector<pugi::xml_node> rel_nodes;
        xml_find_all(rels_doc, "Relationship", rel_nodes);

        for (auto& rel : rel_nodes) {
            const char* type = xml_attr(rel, "Type");
            const char* target = xml_attr(rel, "Target");
            if (!target[0]) continue;

            std::string type_str = type;
            if (type_str.find("/drawing") == std::string::npos) continue;

            // Resolve drawing path (relative, "../", or absolute "/xl/...").
            std::string drawing_path = resolve_package_path(target, dir);

            if (!zip_.has_entry(drawing_path)) continue;

            // Parse drawing rels for image targets
            auto draw_slash = drawing_path.rfind('/');
            if (draw_slash == std::string::npos) continue;
            std::string draw_dir = drawing_path.substr(0, draw_slash);
            std::string draw_base = drawing_path.substr(draw_slash + 1);
            std::string draw_rels = draw_dir + "/_rels/" + draw_base + ".rels";

            std::vector<char> draw_rels_data;
            pugi::xml_document draw_rels_doc;
            if (!xml_load_part(zip_, draw_rels, draw_rels_doc,
                               draw_rels_data))
                continue;

            std::map<std::string, std::string> draw_rel_map;
            std::vector<pugi::xml_node> draw_rel_nodes;
            xml_find_all(draw_rels_doc, "Relationship", draw_rel_nodes);
            for (auto& dr : draw_rel_nodes) {
                const char* id = xml_attr(dr, "Id");
                const char* tgt = xml_attr(dr, "Target");
                if (id[0] && tgt[0]) {
                    draw_rel_map[id] = resolve_package_path(tgt, draw_dir);
                }
            }

            // Parse drawing XML to find blip references
            std::vector<char> draw_data;
            pugi::xml_document draw_doc;
            if (!xml_load_part(zip_, drawing_path, draw_doc, draw_data))
                continue;

            std::vector<pugi::xml_node> blips;
            xml_find_all(draw_doc, "blip", blips);
            for (auto& blip : blips) {
                const char* embed = xml_attr(blip, "embed");
                if (!embed[0]) continue;
                auto mit = draw_rel_map.find(embed);
                if (mit == draw_rel_map.end()) continue;
                const std::string& media_path = mit->second;
                if (!zip_.has_entry(media_path) || extracted.count(media_path)) continue;
                extracted.insert(media_path);

                ImageData img;
                img.page_number = sheet_num;
                std::string ext = util::get_extension(media_path);
                img.format = util::image_format_from_ext(ext);
                img.name = "page" + std::to_string(sheet_num) + "_img" + std::to_string(img_idx);

                img.data = zip_.read_entry(media_path);
                util::populate_image_dimensions(img);
                if (util::is_image_too_small(img, opts.min_image_size))
                    continue;
                img_idx++;

                img.saved_path = util::save_image_to_file(
                    opts.image_dir, img.name, img.format,
                    img.data.data(), img.data.size());
                if (!img.saved_path.empty()) {
                    img.data.clear();
                    img.data.shrink_to_fit();
                }
                images.push_back(std::move(img));
            }
        }
    }

    // Fallback: pick up any remaining files in xl/media/ not yet extracted
    auto entries = zip_.entries_with_prefix("xl/media/");
    for (auto* entry : entries) {
        if (extracted.count(entry->name)) continue;
        extracted.insert(entry->name);

        ImageData img;
        img.page_number = 1;
        std::string ext = util::get_extension(entry->name);
        img.format = util::image_format_from_ext(ext);
        img.name = "page1_img" + std::to_string(img_idx);

        img.data = zip_.read_entry(*entry);
        util::populate_image_dimensions(img);
        if (util::is_image_too_small(img, opts.min_image_size))
            continue;
        img_idx++;

        img.saved_path = util::save_image_to_file(
            opts.image_dir, img.name, img.format,
            img.data.data(), img.data.size());
        if (!img.saved_path.empty()) {
            img.data.clear();
            img.data.shrink_to_fit();
        }
        images.push_back(std::move(img));
    }
    return images;
}

// ── to_markdown ─────────────────────────────────────────

std::string XlsxParser::to_markdown(const ConvertOptions& opts) {
    // Accumulate into a plain string: a streamed multi-GB sheet appends here
    // directly, and an ostringstream would double the peak (buffer + .str()).
    std::string out;

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

        if (sheet_is_streamed(sheets_[i])) {
            if (i > 0) out += "\n";
            std::string display_name = sheets_[i].name.empty()
                ? ("Sheet " + std::to_string(sheet_num))
                : sheets_[i].name;
            out += "## "; out += display_name; out += "\n\n";
            if (stream_sheet_markdown(sheets_[i], out) == 0)
                out += "*Empty sheet*\n\n";
            else
                out += "\n";
            continue;
        }

        auto sheet = parse_sheet(sheets_[i]);

        if (i > 0) out += "\n";
        std::string display_name = sheet.name.empty()
            ? ("Sheet " + std::to_string(sheet_num))
            : sheet.name;
        out += "## "; out += display_name; out += "\n\n";

        if (sheet.cells.empty()) {
            out += "*Empty sheet*\n\n";
            continue;
        }

        out += format_sheet_as_table(sheet); out += "\n";
    }

    // Extract and reference images
    auto images = extract_images(opts);
    for (auto& img : images) {
        // Name the file that was actually written, extension and
        // collision suffix included; a bare img.name pointed at nothing.
        const std::string ref =
            util::image_ref_name(img.name, img.format, img.saved_path);
        out += "![" ; out += img.name; out += "](";
        if (opts.images) out += opts.image_ref_prefix;
        out += ref;
        out += ")\n\n";
    }

    out += format_embedded_parts(zip_);
    return out;
}

// ── to_chunks ───────────────────────────────────────────

bool XlsxParser::sheet_wanted(int sheet_num, const ConvertOptions& opts) {
    if (opts.pages.empty()) return true;
    for (int p : opts.pages) if (p == sheet_num) return true;
    return false;
}

// Build one sheet's base chunk (heading + table markdown + structured tables).
// Images are attached separately (see to_chunks), matching the eager path where
// images are distributed after all sheet chunks are built.
PageChunk XlsxParser::build_sheet_chunk(size_t i, const ConvertOptions& opts) {
    int sheet_num = static_cast<int>(i) + 1;

    if (sheet_is_streamed(sheets_[i])) {
        PageChunk chunk;
        chunk.page_number = sheet_num;
        std::string display_name = sheets_[i].name.empty()
            ? ("Sheet " + std::to_string(sheet_num))
            : sheets_[i].name;
        chunk.text += "## "; chunk.text += display_name; chunk.text += "\n\n";
        if (stream_sheet_markdown(sheets_[i], chunk.text) == 0)
            chunk.text += "*Empty sheet*\n\n";
        else
            chunk.text += "\n";
        // Structured chunk.tables are intentionally skipped for streamed
        // sheets: a per-cell std::string grid over tens of millions of cells
        // is exactly the allocation pattern this path exists to avoid. The
        // markdown text carries the full content.
        return chunk;
    }

    auto sheet = parse_sheet(sheets_[i]);

    PageChunk chunk;
    chunk.page_number = sheet_num;

    std::string text;
    std::string display_name = sheet.name.empty()
        ? ("Sheet " + std::to_string(sheet_num))
        : sheet.name;
    text += "## "; text += display_name; text += "\n\n";

    if (!sheet.cells.empty()) {
        text += format_sheet_as_table(sheet); text += "\n";

        // Build structured table data for the chunk.
        // Walk the row-major cell vector with a single advancing cursor (same
        // as format_sheet_as_table), filling gaps with empty strings.
        if (opts.tables) {
            int total_rows = sheet.max_row + 1;
            int total_cols = sheet.max_col + 1;
            int display_rows = total_rows;  // no row limit

            std::vector<std::vector<std::string>> table;
            table.reserve(display_rows);

            const auto& cells = sheet.cells;
            const size_t ncells = cells.size();
            size_t ci = 0;
            for (int r = 0; r < display_rows; ++r) {
                std::vector<std::string> row;
                row.reserve(total_cols);
                for (int c = 0; c < total_cols; ++c) {
                    while (ci < ncells && (cells[ci].row < r ||
                           (cells[ci].row == r && cells[ci].col < c)))
                        ++ci;
                    if (ci < ncells && cells[ci].row == r && cells[ci].col == c) {
                        const std::string& val = cells[ci].info.value;
                        if (!val.empty() && cells[ci].info.bold)
                            row.push_back("**" + val + "**");
                        else
                            row.push_back(val);
                    } else {
                        row.push_back("");
                    }
                }
                table.push_back(std::move(row));
            }

            chunk.tables.push_back(std::move(table));
        }
    } else {
        text += "*Empty sheet*\n\n";
    }

    chunk.text = std::move(text);
    return chunk;
}

std::vector<PageChunk> XlsxParser::to_chunks(const ConvertOptions& opts) {
    std::vector<PageChunk> chunks;
    to_chunks(opts, [&](PageChunk&& c) {
        chunks.push_back(std::move(c));
        return true;
    });
    append_embedded_parts(zip_, chunks);
    return chunks;
}

bool XlsxParser::to_chunks(const ConvertOptions& opts, const PageSink& sink) {
    // Always enumerate images up front so they can be referenced in text. This
    // means image bytes are resident regardless of streaming (bounded like the
    // eager path); streaming still shrinks the per-sheet table working set.
    ConvertOptions img_opts = opts;
    img_opts.images = true;
    auto all_images = extract_images(img_opts);

    // The eager path attaches each image to the chunk whose page_number matches,
    // else to the first chunk. Replicate that: an image targets its own sheet if
    // that sheet is present, otherwise the first present sheet. Precompute the
    // first present sheet so orphans land there (matching chunks[0] in eager).
    int first_present = -1;
    for (size_t i = 0; i < sheets_.size(); ++i) {
        if (sheet_wanted(static_cast<int>(i) + 1, opts)) {
            first_present = static_cast<int>(i) + 1;
            break;
        }
    }
    if (first_present < 0) return true;  // no sheets → eager returns empty

    auto target_sheet = [&](const ImageData& img) -> int {
        int n = img.page_number;
        if (n >= 1 && n <= static_cast<int>(sheets_.size()) && sheet_wanted(n, opts))
            return n;
        return first_present;  // orphan → first present chunk
    };

    for (size_t i = 0; i < sheets_.size(); ++i) {
        int sheet_num = static_cast<int>(i) + 1;
        if (!sheet_wanted(sheet_num, opts)) continue;

        PageChunk chunk = build_sheet_chunk(i, opts);

        // Attach the images targeting this sheet, in all_images order (the first
        // present sheet also collects orphans). Each image targets exactly one
        // sheet, so moving it out here is safe.
        for (auto& img : all_images) {
            if (target_sheet(img) != sheet_num) continue;
            const std::string ref_name =
                util::image_ref_name(img.name, img.format, img.saved_path);
            chunk.text += "![" + img.name + "](" +
                          opts.image_ref_prefix + ref_name + ")\n\n";
            chunk.images.push_back(std::move(img));
        }

        if (!sink(std::move(chunk))) return false;
    }
    return true;
}

} // namespace jdoc
