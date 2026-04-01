// .xls (BIFF8) parser implementation.

#include "xls_parser.h"
#include "common/file_utils.h"
#include "common/image_utils.h"
#include "common/string_utils.h"
#include "common/binary_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace jdoc {

// BIFF8 record types.
static constexpr uint16_t RT_BOF        = 0x0809;
static constexpr uint16_t RT_EOF        = 0x000A;
static constexpr uint16_t RT_BOUNDSHEET = 0x0085;
static constexpr uint16_t RT_SST        = 0x00FC;
static constexpr uint16_t RT_CONTINUE   = 0x003C;
static constexpr uint16_t RT_LABELSST   = 0x00FD;
static constexpr uint16_t RT_LABEL      = 0x0204;
static constexpr uint16_t RT_NUMBER     = 0x0203;
static constexpr uint16_t RT_RK         = 0x027E;
static constexpr uint16_t RT_MULRK      = 0x00BD;
static constexpr uint16_t RT_FORMULA    = 0x0006;
static constexpr uint16_t RT_STRING     = 0x0207;
static constexpr uint16_t RT_BOOLERR    = 0x0205;
static constexpr uint16_t RT_FORMAT     = 0x041E;
static constexpr uint16_t RT_XF         = 0x00E0;
static constexpr uint16_t RT_FILEPASS   = 0x002F;
static constexpr uint16_t RT_FONT      = 0x0031;
static constexpr uint16_t RT_MSODRAWING = 0x00EC;

// ---------- helpers ----------------------------------------------------------

static uint16_t rd16(const char* p) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    return static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
}

static uint32_t rd32(const char* p) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}

static double rd_double(const char* p) {
    double val;
    std::memcpy(&val, p, 8);
    return val;
}

// ---------- XlsParser --------------------------------------------------------

XlsParser::XlsParser(OleReader& ole) : ole_(ole) {
    parse_workbook();
}

// ---------- RK decoding ------------------------------------------------------

double XlsParser::decode_rk(uint32_t rk) {
    double val;
    bool is_int = (rk & 0x02) != 0;
    bool div100 = (rk & 0x01) != 0;

    if (is_int) {
        int32_t ival = static_cast<int32_t>(rk) >> 2;
        val = static_cast<double>(ival);
    } else {
        uint64_t bits = static_cast<uint64_t>(rk & 0xFFFFFFFC) << 32;
        std::memcpy(&val, &bits, 8);
    }

    if (div100) val /= 100.0;
    return val;
}

// ---------- Number formatting ------------------------------------------------

std::string XlsParser::format_number(double val) {
    if (std::abs(val - std::round(val)) < 1e-9 && std::abs(val) < 1e15) {
        long long ival = static_cast<long long>(std::round(val));
        return std::to_string(ival);
    }
    std::ostringstream oss;
    oss << val;
    return oss.str();
}

bool XlsParser::is_date_format(int fmt_id, const std::string& fmt_code) {
    // Built-in date/time format IDs
    if ((fmt_id >= 14 && fmt_id <= 22) ||
        (fmt_id >= 27 && fmt_id <= 36) ||
        (fmt_id >= 45 && fmt_id <= 47) ||
        (fmt_id >= 50 && fmt_id <= 58)) {
        return true;
    }

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

std::string XlsParser::serial_to_date(double serial) {
    int days = static_cast<int>(serial);
    if (days < 1) return "0000-00-00";

    // Lotus 1-2-3 bug: day 60 = fake Feb 29, 1900. Correct for days > 60.
    if (days > 60) days--;

    days--; // make 0-based from 1900-01-01

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

std::string XlsParser::serial_to_time(double serial) {
    double frac = serial - static_cast<int>(serial);
    int total_secs = static_cast<int>(frac * 86400 + 0.5);
    int h = total_secs / 3600;
    int m = (total_secs % 3600) / 60;
    int s = total_secs % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    return buf;
}

std::string XlsParser::format_cell_number(double val, int xf_index) const {
    if (xf_index < 0 || xf_index >= static_cast<int>(xf_num_fmt_ids_.size()))
        return format_number(val);

    int fmt_id = xf_num_fmt_ids_[xf_index];
    if (fmt_id == 0) return format_number(val); // General

    // Look up custom format code
    std::string fmt_code;
    auto it = custom_num_fmts_.find(fmt_id);
    if (it != custom_num_fmts_.end()) {
        fmt_code = it->second;
    }

    // Date/time formats
    if (is_date_format(fmt_id, fmt_code)) {
        bool is_time_only = (fmt_id >= 18 && fmt_id <= 21) ||
                            (fmt_id >= 45 && fmt_id <= 47);
        if (is_time_only) return serial_to_time(val);

        std::string result = serial_to_date(val);
        double frac = val - static_cast<int>(val);
        if (frac > 0.0001 && (fmt_id == 22 ||
            fmt_code.find('h') != std::string::npos ||
            fmt_code.find('H') != std::string::npos)) {
            result += " " + serial_to_time(val);
        }
        return result;
    }

    // Percentage formats (built-in IDs 9-10)
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

    // Custom percentage
    if (!fmt_code.empty() && fmt_code.find('%') != std::string::npos) {
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

    // Comma-grouped integers (IDs 3, 37, 38)
    if (fmt_id == 3 || fmt_id == 37 || fmt_id == 38) {
        long long ival = static_cast<long long>(val + (val >= 0 ? 0.5 : -0.5));
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld", ival);
        std::string s = buf;
        int start = (s[0] == '-') ? 1 : 0;
        int len = static_cast<int>(s.size()) - start;
        if (len > 3) {
            for (int i = len - 3; i > 0; i -= 3)
                s.insert(start + i, 1, ',');
        }
        return s;
    }

    // Comma-grouped decimals (IDs 4, 39, 40)
    if (fmt_id == 4 || fmt_id == 39 || fmt_id == 40) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f", val);
        std::string s = buf;
        auto dot = s.find('.');
        int start = (s[0] == '-') ? 1 : 0;
        int int_len = static_cast<int>(dot) - start;
        if (int_len > 3) {
            for (int i = int_len - 3; i > 0; i -= 3)
                s.insert(start + i, 1, ',');
        }
        return s;
    }

    // Scientific notation (ID 11)
    if (fmt_id == 11) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2E", val);
        return buf;
    }

    // Fixed decimal (IDs 1-2)
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

    return format_number(val);
}

// ---------- XL Unicode string parsing ----------------------------------------

std::string XlsParser::parse_xl_string(const char* data, size_t len, size_t& pos,
                                        bool* crossed_continue) const {
    if (pos + 3 > len) { pos = len; return ""; }

    uint16_t cch = rd16(data + pos); pos += 2;
    uint8_t flags = static_cast<uint8_t>(data[pos]); pos += 1;

    bool high_byte = (flags & 0x01) != 0;
    bool rich      = (flags & 0x08) != 0;
    bool ext_st    = (flags & 0x04) != 0;

    uint16_t run_count = 0;
    uint32_t ext_size = 0;

    if (rich) {
        if (pos + 2 > len) { pos = len; return ""; }
        run_count = rd16(data + pos); pos += 2;
    }
    if (ext_st) {
        if (pos + 4 > len) { pos = len; return ""; }
        ext_size = rd32(data + pos); pos += 4;
    }

    std::string result;
    size_t bytes_needed = high_byte ? (size_t(cch) * 2) : size_t(cch);

    if (pos + bytes_needed > len) {
        bytes_needed = len - pos;
    }

    if (high_byte) {
        result = util::utf16le_to_utf8(data + pos, bytes_needed);
    } else {
        for (size_t i = 0; i < bytes_needed; ++i) {
            uint8_t ch = static_cast<uint8_t>(data[pos + i]);
            result += util::cp1252_to_utf8(ch);
        }
    }
    pos += bytes_needed;

    // Skip rich text runs.
    size_t rich_bytes = size_t(run_count) * 4;
    if (pos + rich_bytes <= len) pos += rich_bytes;
    else pos = len;

    // Skip ext data.
    if (pos + ext_size <= len) pos += ext_size;
    else pos = len;

    return result;
}

// ---------- SST parsing ------------------------------------------------------

void XlsParser::parse_sst(const char* data, size_t len,
                           const std::vector<std::vector<char>>& continues) {
    if (len < 8) return;

    uint32_t unique_count = rd32(data + 4);

    // Concatenate the SST record data with all CONTINUE records to form one buffer.
    std::vector<char> buf(data + 8, data + len);
    for (const auto& cont : continues) {
        buf.insert(buf.end(), cont.begin(), cont.end());
    }

    size_t pos = 0;
    sst_.reserve(unique_count);
    for (uint32_t i = 0; i < unique_count && pos < buf.size(); ++i) {
        sst_.push_back(parse_xl_string(buf.data(), buf.size(), pos));
    }
}

// ---------- Workbook parsing -------------------------------------------------

void XlsParser::parse_workbook() {
    std::vector<char> wb;
    if (ole_.has_stream("Workbook")) {
        wb = ole_.read_stream("Workbook");
    } else if (ole_.has_stream("Book")) {
        wb = ole_.read_stream("Book");
    }
    if (wb.empty()) return;

    // First pass: collect BoundSheet names, SST, FORMAT, XF records.
    std::vector<std::string> sheet_names;
    int bof_count = 0;
    bool in_globals = false;

    size_t pos = 0;
    std::vector<std::vector<char>> continue_blocks;

    // Pass 1: globals (SST, BoundSheet, FORMAT, XF, FILEPASS)
    while (pos + 4 <= wb.size()) {
        uint16_t rec_type = rd16(wb.data() + pos);
        uint16_t rec_size = rd16(wb.data() + pos + 2);
        pos += 4;

        if (pos + rec_size > wb.size()) break;
        const char* rec_data = wb.data() + pos;

        if (rec_type == RT_BOF) {
            ++bof_count;
            if (bof_count == 1) in_globals = true;
            else break; // End of globals.
        }

        if (in_globals) {
            // Encryption detection: FILEPASS record means file is encrypted.
            if (rec_type == RT_FILEPASS) {
                throw std::runtime_error("XLS file is encrypted");
            }

            if (rec_type == RT_BOUNDSHEET && rec_size >= 8) {
                uint8_t name_len = static_cast<uint8_t>(rec_data[6]);
                uint8_t name_flags = static_cast<uint8_t>(rec_data[7]);
                std::string sname;
                if (name_flags & 0x01) {
                    size_t byte_len = std::min(size_t(name_len) * 2, size_t(rec_size - 8));
                    sname = util::utf16le_to_utf8(rec_data + 8, byte_len);
                } else {
                    size_t byte_len = std::min(size_t(name_len), size_t(rec_size - 8));
                    for (size_t i = 0; i < byte_len; ++i) {
                        sname += util::cp1252_to_utf8(static_cast<uint8_t>(rec_data[8 + i]));
                    }
                }
                sheet_names.push_back(sname);
            }

            // FORMAT record: numFmtId (2 bytes) + formatCode string
            if (rec_type == RT_FORMAT && rec_size >= 5) {
                uint16_t fmt_id = rd16(rec_data);
                // Format string is an XLUnicodeString starting at offset 2
                uint16_t cch = rd16(rec_data + 2);
                uint8_t flags = static_cast<uint8_t>(rec_data[4]);
                bool high_byte = (flags & 0x01) != 0;

                size_t str_start = 5;
                size_t bytes_needed = high_byte ? (size_t(cch) * 2) : size_t(cch);
                if (str_start + bytes_needed <= rec_size) {
                    std::string code;
                    if (high_byte) {
                        code = util::utf16le_to_utf8(rec_data + str_start, bytes_needed);
                    } else {
                        for (size_t i = 0; i < bytes_needed; ++i) {
                            code += util::cp1252_to_utf8(
                                static_cast<uint8_t>(rec_data[str_start + i]));
                        }
                    }
                    custom_num_fmts_[fmt_id] = code;
                }
            }

            // FONT record: bytes 2-3 = options (bit 0=bold weight>=700, bit 1=italic)
            if (rec_type == RT_FONT && rec_size >= 4) {
                FontInfo fi;
                uint16_t options = rd16(rec_data + 2);
                fi.italic = (options & 0x02) != 0;
                // Bold detection: weight field at offset 6 (BIFF8) >= 700
                if (rec_size >= 8) {
                    uint16_t weight = rd16(rec_data + 6);
                    fi.bold = (weight >= 700);
                } else {
                    fi.bold = (options & 0x01) != 0;
                }
                fonts_.push_back(fi);
            }

            // XF record: bytes 0-1 = fontId, bytes 2-3 = numFmtId
            if (rec_type == RT_XF && rec_size >= 4) {
                uint16_t font_id = rd16(rec_data);
                uint16_t fmt_id = rd16(rec_data + 2);
                xf_num_fmt_ids_.push_back(static_cast<int>(fmt_id));
                xf_font_ids_.push_back(static_cast<int>(font_id));
            }

            if (rec_type == RT_SST) {
                continue_blocks.clear();
                size_t look = pos + rec_size;
                while (look + 4 <= wb.size()) {
                    uint16_t ct = rd16(wb.data() + look);
                    uint16_t cs = rd16(wb.data() + look + 2);
                    if (ct != RT_CONTINUE) break;
                    look += 4;
                    if (look + cs > wb.size()) break;
                    continue_blocks.emplace_back(wb.data() + look, wb.data() + look + cs);
                    look += cs;
                }
                parse_sst(rec_data, rec_size, continue_blocks);
            }
        }

        pos += rec_size;
    }

    // Prepare sheets.
    sheets_.resize(sheet_names.size());
    for (size_t i = 0; i < sheet_names.size(); ++i) {
        sheets_[i].name = sheet_names[i];
    }

    // Pass 2: sheet cell data (LABELSST, LABEL, NUMBER, RK, MULRK, FORMULA).
    pos = 0;
    bof_count = 0;
    int current_sheet = -1;
    bool expect_string = false;  // true after FORMULA with string result
    uint16_t formula_row = 0, formula_col = 0;
    int formula_sheet = -1;

    while (pos + 4 <= wb.size()) {
        uint16_t rec_type = rd16(wb.data() + pos);
        uint16_t rec_size = rd16(wb.data() + pos + 2);
        pos += 4;
        if (pos + rec_size > wb.size()) break;
        const char* rec_data = wb.data() + pos;

        if (rec_type == RT_BOF) {
            ++bof_count;
            if (bof_count > 1) {
                current_sheet = bof_count - 2;
            }
        }

        // STRING record follows FORMULA when result is a string.
        if (expect_string && rec_type == RT_STRING && rec_size >= 3) {
            expect_string = false;
            if (formula_sheet >= 0 && formula_sheet < static_cast<int>(sheets_.size())) {
                uint16_t cch = rd16(rec_data);
                uint8_t flags = static_cast<uint8_t>(rec_data[2]);
                bool high_byte = (flags & 0x01) != 0;
                size_t str_start = 3;
                size_t bytes_needed = high_byte ? (size_t(cch) * 2) : size_t(cch);
                std::string val;
                if (str_start + bytes_needed <= rec_size) {
                    if (high_byte) {
                        val = util::utf16le_to_utf8(rec_data + str_start, bytes_needed);
                    } else {
                        for (size_t i = 0; i < bytes_needed; ++i) {
                            val += util::cp1252_to_utf8(
                                static_cast<uint8_t>(rec_data[str_start + i]));
                        }
                    }
                }
                if (!val.empty()) {
                    sheets_[formula_sheet].cells.push_back({formula_row, formula_col, val});
                }
            }
        } else if (rec_type != RT_CONTINUE) {
            expect_string = false;
        }

        // Helper: look up bold/italic from xf index
        auto get_font_info = [&](int xf_idx) -> std::pair<bool,bool> {
            if (xf_idx >= 0 && xf_idx < static_cast<int>(xf_font_ids_.size())) {
                int font_id = xf_font_ids_[xf_idx];
                // BIFF8 skips font index 4 (reserved)
                if (font_id >= 4) font_id--;
                if (font_id >= 0 && font_id < static_cast<int>(fonts_.size()))
                    return {fonts_[font_id].bold, fonts_[font_id].italic};
            }
            return {false, false};
        };

        if (current_sheet >= 0 && current_sheet < static_cast<int>(sheets_.size())) {
            Sheet& sheet = sheets_[current_sheet];

            if (rec_type == RT_LABELSST && rec_size >= 10) {
                uint16_t row = rd16(rec_data);
                uint16_t col = rd16(rec_data + 2);
                uint16_t xf_idx = rd16(rec_data + 4);
                uint32_t sst_idx = rd32(rec_data + 6);
                if (sst_idx < sst_.size()) {
                    auto [b, it] = get_font_info(xf_idx);
                    sheet.cells.push_back({row, col, sst_[sst_idx], b, it});
                }
            } else if (rec_type == RT_BOOLERR && rec_size >= 8) {
                // BOOLERR record: boolean or error cell
                uint16_t row = rd16(rec_data);
                uint16_t col = rd16(rec_data + 2);
                // XF index at offset 4 (2 bytes) — skip
                uint8_t val = static_cast<uint8_t>(rec_data[6]);
                uint8_t is_error = static_cast<uint8_t>(rec_data[7]);
                std::string cell_val;
                if (is_error) {
                    switch (val) {
                        case 0x00: cell_val = "#NULL!"; break;
                        case 0x07: cell_val = "#DIV/0!"; break;
                        case 0x0F: cell_val = "#VALUE!"; break;
                        case 0x17: cell_val = "#REF!"; break;
                        case 0x1D: cell_val = "#NAME?"; break;
                        case 0x24: cell_val = "#NUM!"; break;
                        case 0x2A: cell_val = "#N/A"; break;
                        default:   cell_val = "#ERR"; break;
                    }
                } else {
                    cell_val = val ? "TRUE" : "FALSE";
                }
                sheet.cells.push_back({row, col, cell_val});
            } else if (rec_type == RT_LABEL && rec_size >= 9) {
                // LABEL record: inline string cell (BIFF8)
                uint16_t row = rd16(rec_data);
                uint16_t col = rd16(rec_data + 2);
                // XF index at offset 4 (2 bytes) — skip
                uint16_t cch = rd16(rec_data + 6);
                uint8_t flags = static_cast<uint8_t>(rec_data[8]);
                bool high_byte = (flags & 0x01) != 0;
                size_t str_start = 9;
                size_t bytes_needed = high_byte ? (size_t(cch) * 2) : size_t(cch);
                std::string val;
                if (str_start + bytes_needed <= rec_size) {
                    if (high_byte) {
                        val = util::utf16le_to_utf8(rec_data + str_start, bytes_needed);
                    } else {
                        for (size_t i = 0; i < bytes_needed; ++i) {
                            val += util::cp1252_to_utf8(
                                static_cast<uint8_t>(rec_data[str_start + i]));
                        }
                    }
                }
                if (!val.empty()) {
                    sheet.cells.push_back({row, col, val});
                }
            } else if (rec_type == RT_NUMBER && rec_size >= 14) {
                uint16_t row = rd16(rec_data);
                uint16_t col = rd16(rec_data + 2);
                uint16_t xf_idx = rd16(rec_data + 4);
                double val = rd_double(rec_data + 6);
                sheet.cells.push_back({row, col, format_cell_number(val, xf_idx)});
            } else if (rec_type == RT_RK && rec_size >= 10) {
                uint16_t row = rd16(rec_data);
                uint16_t col = rd16(rec_data + 2);
                uint16_t xf_idx = rd16(rec_data + 4);
                uint32_t rk = rd32(rec_data + 6);
                double val = decode_rk(rk);
                sheet.cells.push_back({row, col, format_cell_number(val, xf_idx)});
            } else if (rec_type == RT_MULRK && rec_size >= 6) {
                uint16_t row = rd16(rec_data);
                uint16_t first_col = rd16(rec_data + 2);
                uint16_t last_col = rd16(rec_data + rec_size - 2);
                size_t pair_offset = 4;
                for (uint16_t c = first_col; c <= last_col && pair_offset + 6 <= rec_size - 2u; ++c) {
                    uint16_t xf_idx = rd16(rec_data + pair_offset);
                    uint32_t rk = rd32(rec_data + pair_offset + 2);
                    double val = decode_rk(rk);
                    sheet.cells.push_back({row, c, format_cell_number(val, xf_idx)});
                    pair_offset += 6;
                }
            } else if (rec_type == RT_FORMULA && rec_size >= 20) {
                // FORMULA record: row(2) + col(2) + xf(2) + result(8) + options(2) + reserved(4)
                uint16_t row = rd16(rec_data);
                uint16_t col = rd16(rec_data + 2);
                uint16_t xf_idx = rd16(rec_data + 4);

                // Result value is 8 bytes at offset 6.
                // Check if it's a special type: bytes 6-7 = 0xFFFF means not a number.
                uint16_t marker = rd16(rec_data + 12);
                if (marker == 0xFFFF) {
                    // Special value: byte at offset 6 indicates type.
                    uint8_t result_type = static_cast<uint8_t>(rec_data[6]);
                    if (result_type == 0) {
                        // String result: follows in a STRING record.
                        expect_string = true;
                        formula_row = row;
                        formula_col = col;
                        formula_sheet = current_sheet;
                    } else if (result_type == 1) {
                        // Boolean: byte at offset 8
                        bool bval = (rec_data[8] != 0);
                        sheet.cells.push_back({row, col, bval ? "TRUE" : "FALSE"});
                    } else if (result_type == 2) {
                        // Error: byte at offset 8
                        uint8_t err = static_cast<uint8_t>(rec_data[8]);
                        std::string err_str;
                        switch (err) {
                            case 0x00: err_str = "#NULL!"; break;
                            case 0x07: err_str = "#DIV/0!"; break;
                            case 0x0F: err_str = "#VALUE!"; break;
                            case 0x17: err_str = "#REF!"; break;
                            case 0x1D: err_str = "#NAME?"; break;
                            case 0x24: err_str = "#NUM!"; break;
                            case 0x2A: err_str = "#N/A"; break;
                            default:   err_str = "#ERR"; break;
                        }
                        sheet.cells.push_back({row, col, err_str});
                    }
                    // result_type == 3: empty cell, skip
                } else {
                    // Numeric result (IEEE 754 double).
                    double val = rd_double(rec_data + 6);
                    sheet.cells.push_back({row, col, format_cell_number(val, xf_idx)});
                }
            }
        }

        pos += rec_size;
    }
}

// ---------- sheet to markdown ------------------------------------------------

std::string XlsParser::sheet_to_markdown(const Sheet& sheet, int sheet_num) {
    if (sheet.cells.empty()) return "";

    // Determine grid dimensions.
    uint16_t max_row = 0, max_col = 0;
    for (const auto& c : sheet.cells) {
        max_row = std::max(max_row, c.row);
        max_col = std::max(max_col, c.col);
    }

    // Limit to reasonable size (avoid massive sparse sheets).
    uint16_t limit_row = std::min(max_row, uint16_t(10000));
    uint16_t limit_col = std::min(max_col, uint16_t(100));

    // Build grid with formatting.
    struct GridCell { std::string value; bool bold = false; bool italic = false; };
    std::vector<std::vector<GridCell>> grid(limit_row + 1,
                                             std::vector<GridCell>(limit_col + 1));
    for (const auto& c : sheet.cells) {
        if (c.row <= limit_row && c.col <= limit_col) {
            grid[c.row][c.col] = {c.value, c.bold, c.italic};
        }
    }

    // Find actual used range (skip fully empty rows/cols at the end).
    int used_rows = 0, used_cols = 0;
    for (int r = 0; r <= limit_row; ++r) {
        for (int c = 0; c <= limit_col; ++c) {
            if (!grid[r][c].value.empty()) {
                used_rows = std::max(used_rows, r + 1);
                used_cols = std::max(used_cols, c + 1);
            }
        }
    }
    if (used_rows == 0 || used_cols == 0) return "";

    // Render markdown table.
    std::ostringstream out;
    out << "--- Page " << sheet_num << " ---\n\n";

    for (int r = 0; r < used_rows; ++r) {
        out << "|";
        for (int c = 0; c < used_cols; ++c) {
            std::string val = grid[r][c].value;
            for (size_t i = 0; i < val.size(); ++i) {
                if (val[i] == '|') val.replace(i, 1, "\\|");
            }
            if (!val.empty()) {
                if (grid[r][c].bold && grid[r][c].italic)
                    val = "***" + val + "***";
                else if (grid[r][c].bold)
                    val = "**" + val + "**";
                else if (grid[r][c].italic)
                    val = "*" + val + "*";
            }
            out << " " << val << " |";
        }
        out << "\n";

        // Header separator after first row.
        if (r == 0) {
            out << "|";
            for (int c = 0; c < used_cols; ++c) {
                out << " --- |";
            }
            out << "\n";
        }
    }

    out << "\n";
    return out.str();
}

// ---------- image extraction -------------------------------------------------

std::vector<ImageData> XlsParser::extract_images(unsigned min_image_size) {
    std::vector<ImageData> images;

    std::vector<char> wb;
    if (ole_.has_stream("Workbook")) {
        wb = ole_.read_stream("Workbook");
    } else if (ole_.has_stream("Book")) {
        wb = ole_.read_stream("Book");
    }
    if (wb.empty()) return images;

    // Collect all MSODRAWING + CONTINUE data.
    std::vector<char> drawing_data;
    size_t pos = 0;

    while (pos + 4 <= wb.size()) {
        uint16_t rec_type = rd16(wb.data() + pos);
        uint16_t rec_size = rd16(wb.data() + pos + 2);
        pos += 4;
        if (pos + rec_size > wb.size()) break;

        if (rec_type == RT_MSODRAWING) {
            drawing_data.insert(drawing_data.end(), wb.data() + pos, wb.data() + pos + rec_size);
            size_t look = pos + rec_size;
            while (look + 4 <= wb.size()) {
                uint16_t ct = rd16(wb.data() + look);
                uint16_t cs = rd16(wb.data() + look + 2);
                if (ct != RT_CONTINUE) break;
                look += 4;
                if (look + cs > wb.size()) break;
                drawing_data.insert(drawing_data.end(), wb.data() + look, wb.data() + look + cs);
                look += cs;
            }
        }

        pos += rec_size;
    }

    if (drawing_data.empty()) return images;

    // Parse OfficeArt containers looking for BLIP records.
    int img_idx = 0;
    pos = 0;
    while (pos + 8 <= drawing_data.size()) {
        uint16_t ver_inst = rd16(drawing_data.data() + pos);
        uint16_t rec_type = rd16(drawing_data.data() + pos + 2);
        uint32_t rec_len  = rd32(drawing_data.data() + pos + 4);

        uint8_t ver = ver_inst & 0x0F;

        if (ver == 0x0F) {
            pos += 8;
            continue;
        }

        if (rec_type >= 0xF01A && rec_type <= 0xF029 && rec_len > 0) {
            size_t blip_start = pos + 8;
            size_t blip_end = blip_start + rec_len;
            if (blip_end > drawing_data.size()) break;

            size_t header_skip = 17;
            std::string fmt;

            switch (rec_type) {
                case 0xF01A: fmt = "emf"; header_skip = 50; break;
                case 0xF01B: fmt = "wmf"; header_skip = 50; break;
                case 0xF01C: header_skip = 50; break;
                case 0xF01D: fmt = "jpeg"; header_skip = 17; break;
                case 0xF01E: fmt = "png"; header_skip = 17; break;
                case 0xF01F: fmt = "bmp"; header_skip = 17; break;
                case 0xF029: fmt = "tiff"; header_skip = 17; break;
            }

            uint16_t inst = ver_inst >> 4;
            if (rec_type == 0xF01D && (inst == 0x46B || inst == 0x6E3)) header_skip = 33;
            if (rec_type == 0xF01E && (inst == 0x6E1 || inst == 0x6E5)) header_skip = 33;

            if (!fmt.empty() && rec_len > header_skip) {
                size_t img_offset = blip_start + header_skip;
                size_t img_size = rec_len - header_skip;
                if (img_offset + img_size <= drawing_data.size()) {
                    ImageData img;
                    img.page_number = 1;
                    img.name = "page1_img" + std::to_string(img_idx++);
                    img.format = fmt;
                    img.data.assign(drawing_data.begin() + img_offset,
                                    drawing_data.begin() + img_offset + img_size);
                    util::populate_image_dimensions(img);
                    if (util::is_image_too_small(img, min_image_size)) {
                        --img_idx;
                    } else {
                        images.push_back(std::move(img));
                    }
                }
            }

            pos = blip_end;
        } else {
            pos += 8 + rec_len;
        }
    }

    return images;
}

// ---------- public API -------------------------------------------------------

std::string XlsParser::to_markdown(const ConvertOptions& opts) {
    std::string md;
    for (size_t i = 0; i < sheets_.size(); i++) {
        md += sheet_to_markdown(sheets_[i], static_cast<int>(i) + 1);
    }

    {
        auto images = extract_images(opts.min_image_size);
        for (const auto& img : images) {
            std::string filename = img.name + "." + img.format;
            if (opts.extract_images)
                md += "![" + filename + "](" + opts.image_ref_prefix + filename + ")\n\n";
            else
                md += "![" + filename + "](" + filename + ")\n\n";
        }
    }

    return md;
}

std::vector<PageChunk> XlsParser::to_chunks(const ConvertOptions& opts) {
    std::vector<PageChunk> chunks;

    auto images = opts.extract_images ? extract_images(opts.min_image_size) : std::vector<ImageData>{};
    int img_per_sheet = images.empty() ? 0 : static_cast<int>((images.size() + sheets_.size() - 1) / sheets_.size());

    for (size_t i = 0; i < sheets_.size(); ++i) {
        PageChunk chunk;
        chunk.page_number = static_cast<int>(i + 1);
        chunk.text = sheet_to_markdown(sheets_[i], static_cast<int>(i) + 1);
        chunk.page_width = 612.0;
        chunk.page_height = 792.0;
        chunk.body_font_size = 10.0;

        if (opts.extract_tables && !sheets_[i].cells.empty()) {
            uint16_t max_row = 0, max_col = 0;
            for (const auto& c : sheets_[i].cells) {
                max_row = std::max(max_row, c.row);
                max_col = std::max(max_col, c.col);
            }
            max_row = std::min(max_row, uint16_t(10000));
            max_col = std::min(max_col, uint16_t(100));

            std::vector<std::vector<std::string>> grid(max_row + 1,
                                                       std::vector<std::string>(max_col + 1));
            for (const auto& c : sheets_[i].cells) {
                if (c.row <= max_row && c.col <= max_col)
                    grid[c.row][c.col] = c.value;
            }

            int used_rows = 0, used_cols = 0;
            for (int r = 0; r <= max_row; ++r)
                for (int c = 0; c <= max_col; ++c)
                    if (!grid[r][c].empty()) {
                        used_rows = std::max(used_rows, r + 1);
                        used_cols = std::max(used_cols, c + 1);
                    }

            if (used_rows > 0 && used_cols > 0) {
                std::vector<std::vector<std::string>> table;
                for (int r = 0; r < used_rows; ++r) {
                    table.emplace_back(grid[r].begin(), grid[r].begin() + used_cols);
                }
                chunk.tables.push_back(std::move(table));
            }
        }

        if (opts.extract_images && img_per_sheet > 0) {
            int start = static_cast<int>(i) * img_per_sheet;
            int end = std::min(start + img_per_sheet, static_cast<int>(images.size()));
            for (int j = start; j < end; ++j) {
                chunk.images.push_back(images[j]);
            }
        }

        chunks.push_back(std::move(chunk));
    }

    return chunks;
}

} // namespace jdoc
