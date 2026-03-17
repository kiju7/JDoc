// XLSB (Binary Excel) parser implementation
// Parses ZIP-based .xlsb files with binary record streams

#include "ooxml/xlsb_parser.h"
#include "xml_utils.h"
#include "common/file_utils.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace jdoc {

// ── XLSB record type IDs ────────────────────────────────

enum XlsbRecordType : uint16_t {
    BRT_ROW_HDR         = 0x00,
    BRT_CELL_BLANK      = 0x01,
    BRT_CELL_RK         = 0x02,
    BRT_FMLA_ERROR      = 0x03,
    BRT_CELL_BOOL       = 0x04,
    BRT_CELL_REAL       = 0x05,
    BRT_CELL_ISST       = 0x07,
    BRT_FMLA_STRING     = 0x08,
    BRT_FMLA_NUM        = 0x09,
    BRT_SST_ITEM        = 0x13,
    BRT_FMT             = 0x2C,
    BRT_XF              = 0x2F,
    BRT_BUNDLE_SH       = 0x9C,
    BRT_BEGIN_SST       = 0x9F,
};

// ── Constructor ─────────────────────────────────────────

XlsbParser::XlsbParser(ZipReader& zip) : zip_(zip) {
    parse_workbook();
    parse_shared_strings();
    parse_styles();
}

// ── Binary record reading helpers ───────────────────────

uint16_t XlsbParser::read_record_header(const uint8_t* data, size_t& offset,
                                          uint32_t& out_size) {
    // Record type: 1-2 bytes (7-bit variable-length encoding)
    uint16_t rec_type = data[offset++];
    if (rec_type & 0x80) {
        rec_type = (rec_type & 0x7F) | (static_cast<uint16_t>(data[offset++]) << 7);
    }

    // Record size: 1-4 bytes (7-bit variable-length encoding)
    uint32_t size = 0;
    int shift = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = data[offset++];
        size |= static_cast<uint32_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }

    out_size = size;
    return rec_type;
}

std::string XlsbParser::read_xl_widestring(const uint8_t* data, size_t& offset,
                                             size_t end) {
    if (offset + 4 > end) return "";
    uint32_t char_count;
    memcpy(&char_count, data + offset, 4);
    offset += 4;

    if (offset + char_count * 2 > end) return "";

    std::string result;
    for (uint32_t i = 0; i < char_count; i++) {
        uint16_t ch;
        memcpy(&ch, data + offset, 2);
        offset += 2;
        if (ch == 0) break;
        // UTF-16LE to UTF-8
        if (ch < 0x80) {
            result += static_cast<char>(ch);
        } else if (ch < 0x800) {
            result += static_cast<char>(0xC0 | (ch >> 6));
            result += static_cast<char>(0x80 | (ch & 0x3F));
        } else {
            result += static_cast<char>(0xE0 | (ch >> 12));
            result += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (ch & 0x3F));
        }
    }
    return result;
}

// ── Workbook parsing (xl/workbook.bin) ──────────────────

void XlsbParser::parse_workbook() {
    // XLSB workbook uses binary records, but relationships are XML
    // Parse relationships first to find sheet paths
    std::map<std::string, std::string> id_to_target;
    if (zip_.has_entry("xl/_rels/workbook.bin.rels")) {
        auto data = zip_.read_entry("xl/_rels/workbook.bin.rels");
        pugi::xml_document doc;
        if (doc.load_buffer(data.data(), data.size())) {
            std::vector<pugi::xml_node> rels;
            xml_find_all(doc, "Relationship", rels);
            for (auto& rel : rels) {
                const char* id = xml_attr(rel, "Id");
                const char* target = xml_attr(rel, "Target");
                if (id[0] && target[0]) {
                    std::string t = target;
                    if (t.find("xl/") != 0 && t.find('/') == std::string::npos)
                        t = "xl/" + t;
                    else if (t.find("xl/") != 0 && t.find("worksheets/") == 0)
                        t = "xl/" + t;
                    id_to_target[id] = t;
                }
            }
        }
    }

    // Parse workbook.bin for sheet names
    if (!zip_.has_entry("xl/workbook.bin")) return;
    auto raw = zip_.read_entry("xl/workbook.bin");
    const uint8_t* data = reinterpret_cast<const uint8_t*>(raw.data());
    size_t size = raw.size();
    size_t offset = 0;
    int rel_idx = 1;

    while (offset + 2 < size) {
        uint32_t rec_size = 0;
        size_t saved = offset;
        uint16_t rec_type = read_record_header(data, offset, rec_size);
        size_t rec_end = offset + rec_size;
        if (rec_end > size) break;

        if (rec_type == BRT_BUNDLE_SH && rec_size >= 12) {
            // BrtBundleSh: uint32 hsState + uint32 iTabID + strRelId + strName
            size_t p = offset;
            p += 8; // skip hsState + iTabID
            std::string rel_id = read_xl_widestring(data, p, rec_end);
            std::string name = read_xl_widestring(data, p, rec_end);

            SheetInfo info;
            info.name = name;
            // Resolve path from rel_id or fallback
            if (!rel_id.empty()) {
                auto it = id_to_target.find(rel_id);
                if (it != id_to_target.end()) info.file_path = it->second;
            }
            if (info.file_path.empty()) {
                // Fallback: try rId{N} pattern
                std::string rId = "rId" + std::to_string(rel_idx);
                auto it = id_to_target.find(rId);
                if (it != id_to_target.end()) info.file_path = it->second;
            }
            sheets_.push_back(std::move(info));
            rel_idx++;
        }

        offset = rec_end;
    }
}

// ── Shared strings (xl/sharedStrings.bin) ───────────────

void XlsbParser::parse_shared_strings() {
    if (!zip_.has_entry("xl/sharedStrings.bin")) return;

    auto raw = zip_.read_entry("xl/sharedStrings.bin");
    const uint8_t* data = reinterpret_cast<const uint8_t*>(raw.data());
    size_t size = raw.size();
    size_t offset = 0;

    while (offset + 2 < size) {
        uint32_t rec_size = 0;
        uint16_t rec_type = read_record_header(data, offset, rec_size);
        size_t rec_end = offset + rec_size;
        if (rec_end > size) break;

        if (rec_type == BRT_BEGIN_SST && rec_size >= 8) {
            // BrtBeginSst: uint32 cstTotal + uint32 cstUnique
            uint32_t unique_count;
            memcpy(&unique_count, data + offset + 4, 4);
            shared_strings_.reserve(unique_count);
        } else if (rec_type == BRT_SST_ITEM) {
            // BrtSSTItem: flags(1) + XLWideString
            size_t p = offset;
            if (p < rec_end) {
                uint8_t flags = data[p++];
                std::string str = read_xl_widestring(data, p, rec_end);
                shared_strings_.push_back(std::move(str));
                (void)flags;
            }
        }

        offset = rec_end;
    }
}

// ── Styles (xl/styles.bin) ──────────────────────────────

void XlsbParser::parse_styles() {
    if (!zip_.has_entry("xl/styles.bin")) return;

    auto raw = zip_.read_entry("xl/styles.bin");
    const uint8_t* data = reinterpret_cast<const uint8_t*>(raw.data());
    size_t size = raw.size();
    size_t offset = 0;
    bool in_cell_xfs = false;

    while (offset + 2 < size) {
        uint32_t rec_size = 0;
        uint16_t rec_type = read_record_header(data, offset, rec_size);
        size_t rec_end = offset + rec_size;
        if (rec_end > size) break;

        if (rec_type == BRT_FMT && rec_size >= 6) {
            // BrtFmt: uint16 fmtId + XLWideString formatCode
            uint16_t fmt_id;
            memcpy(&fmt_id, data + offset, 2);
            size_t p = offset + 2;
            std::string code = read_xl_widestring(data, p, rec_end);
            custom_num_fmts_[fmt_id] = code;
        } else if (rec_type == 0x0263) { // BrtBeginCellXFs
            in_cell_xfs = true;
        } else if (rec_type == 0x0264) { // BrtEndCellXFs
            in_cell_xfs = false;
        } else if (rec_type == BRT_XF && in_cell_xfs && rec_size >= 4) {
            // BrtXf: uint16 ixFmtParent + uint16 iFmt
            uint16_t fmt_id;
            memcpy(&fmt_id, data + offset + 2, 2);
            xf_num_fmt_ids_.push_back(fmt_id);
        }

        offset = rec_end;
    }
}

// ── Number formatting (same logic as XLSX) ──────────────

bool XlsbParser::is_date_format(int fmt_id, const std::string& fmt_code) {
    if ((fmt_id >= 14 && fmt_id <= 22) ||
        (fmt_id >= 27 && fmt_id <= 36) ||
        (fmt_id >= 45 && fmt_id <= 47) ||
        (fmt_id >= 50 && fmt_id <= 58)) return true;
    if (fmt_code.empty()) return false;
    std::string lower;
    for (char c : fmt_code) lower += std::tolower(static_cast<unsigned char>(c));
    bool in_quote = false;
    for (size_t i = 0; i < lower.size(); i++) {
        if (lower[i] == '"') { in_quote = !in_quote; continue; }
        if (in_quote) continue;
        if (lower[i] == '\\') { i++; continue; }
        char ch = lower[i];
        if (ch == 'y' || ch == 'd' || ch == 'h') return true;
        if (ch == 's' && i > 0) return true;
    }
    return false;
}

std::string XlsbParser::serial_to_date(double serial) {
    int days = static_cast<int>(serial);
    if (days < 1) return "0000-00-00";
    if (days > 60) days--; // Lotus 1-2-3 bug correction
    int y = 1900;
    days--; // 0-based
    while (true) {
        bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        int yd = leap ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        y++;
    }
    bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    static const int md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int m = 0;
    for (m = 0; m < 12; m++) {
        int d = md[m] + (m == 1 && leap ? 1 : 0);
        if (days < d) break;
        days -= d;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m + 1, days + 1);
    return buf;
}

std::string XlsbParser::serial_to_time(double serial) {
    double frac = serial - static_cast<int>(serial);
    int total = static_cast<int>(frac * 86400 + 0.5);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", total/3600, (total%3600)/60, total%60);
    return buf;
}

std::string XlsbParser::format_number(const std::string& raw_value,
                                       int style_idx) const {
    if (style_idx < 0 || style_idx >= static_cast<int>(xf_num_fmt_ids_.size()))
        return raw_value;
    int fmt_id = xf_num_fmt_ids_[style_idx];
    if (fmt_id == 0) return raw_value;

    std::string fmt_code;
    auto it = custom_num_fmts_.find(fmt_id);
    if (it != custom_num_fmts_.end()) fmt_code = it->second;

    double val = std::atof(raw_value.c_str());

    if (is_date_format(fmt_id, fmt_code)) {
        bool time_only = (fmt_id >= 18 && fmt_id <= 21) || (fmt_id >= 45 && fmt_id <= 47);
        if (time_only) return serial_to_time(val);
        std::string result = serial_to_date(val);
        double frac = val - static_cast<int>(val);
        if (frac > 0.0001) result += " " + serial_to_time(val);
        return result;
    }
    if (fmt_id == 9) { char b[64]; snprintf(b, 64, "%.0f%%", val*100); return b; }
    if (fmt_id == 10) { char b[64]; snprintf(b, 64, "%.2f%%", val*100); return b; }
    if (!fmt_code.empty() && fmt_code.find('%') != std::string::npos) {
        char b[64]; snprintf(b, 64, "%.2f%%", val*100); return b;
    }
    if (fmt_id == 4 || fmt_id == 39 || fmt_id == 40) {
        char b[64]; snprintf(b, 64, "%.2f", val);
        std::string s = b;
        auto dot = s.find('.');
        int start = (s[0] == '-') ? 1 : 0;
        int len = static_cast<int>(dot) - start;
        if (len > 3) for (int i = len - 3; i > 0; i -= 3) s.insert(start + i, 1, ',');
        return s;
    }

    return raw_value;
}

// ── RK number decoding ──────────────────────────────────

static double decode_rk(uint32_t rk) {
    double val;
    if (rk & 0x02) {
        // 30-bit signed integer
        int32_t ival = static_cast<int32_t>(rk) >> 2;
        val = ival;
    } else {
        // IEEE 754 double with top 30 bits
        uint64_t bits = static_cast<uint64_t>(rk & 0xFFFFFFFC) << 32;
        memcpy(&val, &bits, 8);
    }
    if (rk & 0x01) val /= 100.0;
    return val;
}

// ── Sheet parsing (binary records) ──────────────────────

XlsbParser::SheetData XlsbParser::parse_sheet(const SheetInfo& info) {
    SheetData sheet;
    sheet.name = info.name;

    if (info.file_path.empty() || !zip_.has_entry(info.file_path))
        return sheet;

    auto raw = zip_.read_entry(info.file_path);
    const uint8_t* data = reinterpret_cast<const uint8_t*>(raw.data());
    size_t size = raw.size();
    size_t offset = 0;
    int current_row = 0;

    while (offset + 2 < size) {
        uint32_t rec_size = 0;
        uint16_t rec_type = read_record_header(data, offset, rec_size);
        size_t rec_end = offset + rec_size;
        if (rec_end > size) break;

        switch (rec_type) {
        case BRT_ROW_HDR: {
            if (rec_size >= 4) {
                uint32_t row;
                memcpy(&row, data + offset, 4);
                current_row = static_cast<int>(row);
            }
            break;
        }
        case BRT_CELL_ISST: {
            // col(4) + styleRef(4) + sstIdx(4)
            if (rec_size >= 12) {
                uint32_t col, style, sst_idx;
                memcpy(&col, data + offset, 4);
                memcpy(&style, data + offset + 4, 4);
                memcpy(&sst_idx, data + offset + 8, 4);
                if (sst_idx < shared_strings_.size()) {
                    std::string val = shared_strings_[sst_idx];
                    for (auto& ch : val) { if (ch == '|') ch = '/'; if (ch == '\n') ch = ' '; }
                    sheet.cells[current_row][static_cast<int>(col)] = val;
                    sheet.max_row = std::max(sheet.max_row, current_row);
                    sheet.max_col = std::max(sheet.max_col, static_cast<int>(col));
                }
            }
            break;
        }
        case BRT_CELL_REAL:
        case BRT_FMLA_NUM: {
            // col(4) + styleRef(4) + double(8)
            if (rec_size >= 16) {
                uint32_t col, style;
                double val;
                memcpy(&col, data + offset, 4);
                memcpy(&style, data + offset + 4, 4);
                memcpy(&val, data + offset + 8, 8);
                char buf[64];
                snprintf(buf, sizeof(buf), "%.15g", val);
                std::string formatted = format_number(buf, static_cast<int>(style));
                for (auto& ch : formatted) { if (ch == '|') ch = '/'; if (ch == '\n') ch = ' '; }
                sheet.cells[current_row][static_cast<int>(col)] = formatted;
                sheet.max_row = std::max(sheet.max_row, current_row);
                sheet.max_col = std::max(sheet.max_col, static_cast<int>(col));
            }
            break;
        }
        case BRT_CELL_RK: {
            // col(4) + styleRef(4) + rk(4)
            if (rec_size >= 12) {
                uint32_t col, style, rk;
                memcpy(&col, data + offset, 4);
                memcpy(&style, data + offset + 4, 4);
                memcpy(&rk, data + offset + 8, 4);
                double val = decode_rk(rk);
                char buf[64];
                snprintf(buf, sizeof(buf), "%.15g", val);
                std::string formatted = format_number(buf, static_cast<int>(style));
                for (auto& ch : formatted) { if (ch == '|') ch = '/'; if (ch == '\n') ch = ' '; }
                sheet.cells[current_row][static_cast<int>(col)] = formatted;
                sheet.max_row = std::max(sheet.max_row, current_row);
                sheet.max_col = std::max(sheet.max_col, static_cast<int>(col));
            }
            break;
        }
        case BRT_CELL_BOOL: {
            // col(4) + styleRef(4) + bool(1)
            if (rec_size >= 9) {
                uint32_t col;
                memcpy(&col, data + offset, 4);
                bool bval = data[offset + 8] != 0;
                sheet.cells[current_row][static_cast<int>(col)] = bval ? "TRUE" : "FALSE";
                sheet.max_row = std::max(sheet.max_row, current_row);
                sheet.max_col = std::max(sheet.max_col, static_cast<int>(col));
            }
            break;
        }
        case BRT_FMLA_STRING: {
            // col(4) + styleRef(4) + XLWideString
            if (rec_size >= 12) {
                uint32_t col;
                memcpy(&col, data + offset, 4);
                size_t p = offset + 8;
                std::string val = read_xl_widestring(data, p, rec_end);
                for (auto& ch : val) { if (ch == '|') ch = '/'; if (ch == '\n') ch = ' '; }
                if (!val.empty()) {
                    sheet.cells[current_row][static_cast<int>(col)] = val;
                    sheet.max_row = std::max(sheet.max_row, current_row);
                    sheet.max_col = std::max(sheet.max_col, static_cast<int>(col));
                }
            }
            break;
        }
        default:
            break;
        }

        offset = rec_end;
    }

    return sheet;
}

// ── Format as table ─────────────────────────────────────

std::string XlsbParser::format_sheet_as_table(const SheetData& sheet,
                                                int max_rows) {
    if (sheet.cells.empty()) return "";

    int total_rows = sheet.max_row + 1;
    int total_cols = sheet.max_col + 1;
    bool truncated = total_rows > max_rows;
    int display_rows = std::min(total_rows, max_rows);

    std::ostringstream out;

    out << "|";
    for (int c = 0; c < total_cols; ++c) {
        auto row_it = sheet.cells.find(0);
        std::string cell;
        if (row_it != sheet.cells.end()) {
            auto col_it = row_it->second.find(c);
            if (col_it != row_it->second.end()) cell = col_it->second;
        }
        out << " " << cell << " |";
    }
    out << "\n|";
    for (int c = 0; c < total_cols; ++c) out << " --- |";
    out << "\n";

    for (int r = 1; r < display_rows; ++r) {
        out << "|";
        for (int c = 0; c < total_cols; ++c) {
            auto row_it = sheet.cells.find(r);
            std::string cell;
            if (row_it != sheet.cells.end()) {
                auto col_it = row_it->second.find(c);
                if (col_it != row_it->second.end()) cell = col_it->second;
            }
            out << " " << cell << " |";
        }
        out << "\n";
    }

    if (truncated) {
        out << "\n*... truncated at " << max_rows
            << " rows (total: " << total_rows << " rows)*\n";
    }
    return out.str();
}

// ── Public API ──────────────────────────────────────────

std::string XlsbParser::to_markdown(const ConvertOptions& opts) {
    std::ostringstream out;

    for (size_t i = 0; i < sheets_.size(); ++i) {
        int sheet_num = static_cast<int>(i) + 1;
        if (!opts.pages.empty()) {
            bool found = false;
            for (int p : opts.pages) { if (p == sheet_num) { found = true; break; } }
            if (!found) continue;
        }

        auto sheet = parse_sheet(sheets_[i]);
        if (i > 0) out << "\n---\n\n";

        std::string name = sheet.name.empty()
            ? ("Sheet " + std::to_string(sheet_num)) : sheet.name;
        out << "## " << name << "\n\n";

        if (sheet.cells.empty()) {
            out << "*Empty sheet*\n\n";
            continue;
        }
        out << format_sheet_as_table(sheet) << "\n";
    }
    return out.str();
}

std::vector<PageChunk> XlsbParser::to_chunks(const ConvertOptions& opts) {
    std::vector<PageChunk> chunks;

    for (size_t i = 0; i < sheets_.size(); ++i) {
        int sheet_num = static_cast<int>(i) + 1;
        if (!opts.pages.empty()) {
            bool found = false;
            for (int p : opts.pages) { if (p == sheet_num) { found = true; break; } }
            if (!found) continue;
        }

        auto sheet = parse_sheet(sheets_[i]);
        PageChunk chunk;
        chunk.page_number = sheet_num;

        std::ostringstream text;
        std::string name = sheet.name.empty()
            ? ("Sheet " + std::to_string(sheet_num)) : sheet.name;
        text << "## " << name << "\n\n";

        if (!sheet.cells.empty()) {
            text << format_sheet_as_table(sheet) << "\n";
        } else {
            text << "*Empty sheet*\n\n";
        }

        chunk.text = text.str();
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

} // namespace jdoc
