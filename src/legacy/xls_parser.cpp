// .xls (BIFF8) parser implementation.

#include "xls_parser.h"
#include "common/embedded.h"
#include "common/emf_text.h"
#include "common/file_utils.h"
#include "common/image_utils.h"
#include "common/png_encode.h"
#include "common/inflate.h"
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
static constexpr uint16_t RT_MSODRAWINGGROUP = 0x00EB;
static constexpr uint16_t RT_MSODRAWING = 0x00EC;
static constexpr uint16_t RT_HEADER     = 0x0014;
static constexpr uint16_t RT_FOOTER     = 0x0015;
static constexpr uint16_t RT_TXO        = 0x01B6;

// An XLUnicodeString (cch, flags, characters) — 8-bit or UTF-16LE.
static std::string xl_unicode_string(const char* d, size_t n, bool force_wide = false) {
    if (n < 3) return "";
    size_t cch = util::read_u16_le(d);
    bool wide = force_wide || (static_cast<uint8_t>(d[2]) & 0x01) != 0;
    size_t bytes = std::min(wide ? cch * 2 : cch, n - 3);
    if (wide) return util::utf16le_to_utf8(d + 3, bytes & ~size_t(1));
    std::string out;
    for (size_t i = 0; i < bytes; ++i) util::append_cp1252(out, static_cast<uint8_t>(d[3 + i]));
    return out;
}

// The header/footer string with its &-codes (&L &C &R sections, &"font",
// sizes, fields like &P) reduced to the printed text.
static std::string header_footer_text(const std::string& code) {
    std::string out;
    for (size_t i = 0; i < code.size(); ++i) {
        char c = code[i];
        if (c != '&' || i + 1 >= code.size()) { out += c; continue; }
        char k = code[++i];
        if (k == '&') out += '&';
        else if (k == 'L' || k == 'C' || k == 'R') { if (!out.empty()) out += ' '; }
        else if (k == '"') { size_t e = code.find('"', i + 1); i = e == std::string::npos ? code.size() : e; }
        else if (k >= '0' && k <= '9') { while (i + 1 < code.size() && code[i + 1] >= '0' && code[i + 1] <= '9') ++i; }
        else if (k == 'K') i = std::min(code.size() - 1, i + 6);
    }
    return util::trim(out);
}

// The text of a TXO record (a text box, a note): cchText characters carried
// by the CONTINUE records that follow it, each opening with its own
// 8-bit/UTF-16 flag byte.
static std::string txo_text(const std::vector<char>& wb, size_t txo_pos, size_t txo_size,
                            bool force_wide = false) {
    if (txo_size < 12) return "";
    size_t left = util::read_u16_le(wb.data() + txo_pos + 10);
    std::string out;
    size_t pos = txo_pos + txo_size;
    while (left > 0 && pos + 4 <= wb.size() &&
           util::read_u16_le(wb.data() + pos) == RT_CONTINUE) {
        size_t size = util::read_u16_le(wb.data() + pos + 2);
        const char* d = wb.data() + pos + 4;
        pos += 4 + size;
        if (pos > wb.size() || size < 1) break;
        bool wide = force_wide || (static_cast<uint8_t>(d[0]) & 0x01) != 0;
        size_t n = std::min(left, (size - 1) / (wide ? 2 : 1));
        if (wide) out += util::utf16le_to_utf8(d + 1, n * 2);
        else for (size_t i = 0; i < n; ++i) util::append_cp1252(out, static_cast<uint8_t>(d[1 + i]));
        left -= n;
    }
    for (auto& ch : out) if (ch == '\r' || ch == '\n') ch = ' ';
    return util::trim(out);
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

// ---------- SST parsing ------------------------------------------------------

namespace {

// Cursor over the SST record body plus its CONTINUE payloads, decoding
// XLUnicodeRichExtendedStrings across record boundaries (MS-XLS 2.5.293).
// Header fields, rich-text runs and ext data flow across a boundary as plain
// bytes, but character data that resumes in a new record starts with a fresh
// option-flags byte — which may switch the encoding width mid-string. The
// old flat-concatenation approach fed that flags byte into the character
// stream, corrupting every string after the first split (~450 unique strings
// is enough to hit it).
class SstCursor {
public:
    explicit SstCursor(std::vector<std::pair<const char*, size_t>> segs,
                       bool force_wide = false)
        : segs_(std::move(segs)), force_wide_(force_wide) {}

    bool at_end() {
        norm();
        return si_ >= segs_.size();
    }
    bool failed() const { return failed_; }

    // Read one XLUnicodeRichExtendedString.
    std::string read_string() {
        std::string out;
        uint16_t cch = 0;
        uint8_t flags = 0;
        if (!u16(cch) || !u8(flags)) { failed_ = true; return out; }
        bool high  = force_wide_ || (flags & 0x01) != 0;
        bool ext   = (flags & 0x04) != 0;
        bool rich  = (flags & 0x08) != 0;

        uint16_t run_count = 0;
        uint32_t ext_size = 0;
        if (rich && !u16(run_count)) { failed_ = true; return out; }
        if (ext && !u32(ext_size))   { failed_ = true; return out; }

        size_t remaining = cch;
        while (remaining > 0) {
            if (!norm()) { failed_ = true; break; }
            if (entered_mid_chars_) {
                // First byte of a record continuing character data is a new
                // option-flags byte; only bit 0 (fHighByte) applies here.
                uint8_t g = static_cast<uint8_t>(segs_[si_].first[off_]);
                off_++;
                high = force_wide_ || (g & 0x01) != 0;
                entered_mid_chars_ = false;
                continue;  // re-normalize (the flags byte may end the record)
            }
            size_t avail = segs_[si_].second - off_;
            size_t width = high ? 2 : 1;
            size_t take = std::min(remaining, avail / width);
            if (take == 0) {
                // A code unit straddles the boundary — forbidden by the spec;
                // drop the stray byte rather than derail the whole table.
                off_ = segs_[si_].second;
                entered_mid_chars_ = true;
                continue;
            }
            const char* p = segs_[si_].first + off_;
            if (high) {
                out += util::utf16le_to_utf8(p, take * 2);
            } else {
                for (size_t i = 0; i < take; ++i)
                    util::append_cp1252(out, static_cast<uint8_t>(p[i]));
            }
            off_ += take * width;
            remaining -= take;
            if (remaining > 0 && off_ == segs_[si_].second)
                entered_mid_chars_ = true;
        }

        // Hancom Cell marks a string with formatting runs by bit 0 (Excel's
        // fHighByte, which it never needs: its text is always UTF-16) and
        // stores the run count and runs after the characters.
        if (force_wide_ && (flags & 0x01) && !rich && !u16(run_count)) {
            failed_ = true;
            return out;
        }
        skip(size_t(run_count) * 4 + ext_size);
        return out;
    }

private:
    bool force_wide_ = false;  // Hancom Cell: UTF-16 under an 8-bit flag
    std::vector<std::pair<const char*, size_t>> segs_;
    size_t si_ = 0, off_ = 0;
    bool entered_mid_chars_ = false;
    bool failed_ = false;

    // Advance past exhausted segments; false at end of data.
    bool norm() {
        while (si_ < segs_.size() && off_ == segs_[si_].second) {
            si_++;
            off_ = 0;
        }
        return si_ < segs_.size();
    }

    // Plain byte reads that cross segment boundaries transparently (headers,
    // rich runs, ext data — no flags byte is inserted for these).
    bool u8(uint8_t& v) {
        if (!norm()) return false;
        v = static_cast<uint8_t>(segs_[si_].first[off_++]);
        return true;
    }
    bool u16(uint16_t& v) {
        uint8_t a, b;
        if (!u8(a) || !u8(b)) return false;
        v = static_cast<uint16_t>(a | (b << 8));
        return true;
    }
    bool u32(uint32_t& v) {
        uint16_t a, b;
        if (!u16(a) || !u16(b)) return false;
        v = static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 16);
        return true;
    }
    void skip(size_t n) {
        while (n > 0 && norm()) {
            size_t step = std::min(n, segs_[si_].second - off_);
            off_ += step;
            n -= step;
        }
    }
};

} // namespace

void XlsParser::parse_sst(const char* data, size_t len,
                           const std::vector<std::vector<char>>& continues) {
    if (len < 8) return;

    uint32_t unique_count = util::read_u32_le(data + 4);

    // The SST body and each CONTINUE payload stay separate segments so the
    // reader can apply the record-boundary rules above.
    std::vector<std::pair<const char*, size_t>> segs;
    segs.reserve(continues.size() + 1);
    segs.push_back({data + 8, len - 8});
    uint64_t total = len - 8;
    for (const auto& cont : continues) {
        segs.push_back({cont.data(), cont.size()});
        total += cont.size();
    }

    // Bound the reserve by what the bytes can physically hold (3-byte header
    // minimum per string) so a lying count cannot balloon the allocation.
    sst_.reserve(static_cast<size_t>(
        std::min<uint64_t>(unique_count, total / 3 + 1)));

    SstCursor cur(std::move(segs), hancom_wide_);
    for (uint32_t i = 0; i < unique_count && !cur.at_end(); ++i) {
        sst_.push_back(cur.read_string());
        if (cur.failed()) break;
    }
}

// ---------- Workbook stream ---------------------------------------------------

// Hancom Cell writes the workbook's MSODRAWINGGROUP as one record however
// large its pictures are, with a length field that does not hold the size
// (a 3.6 MB drawing group under a 60 KB length). Every record after it was
// then read from the middle of picture data, no sheet was found and the
// output was empty. The OfficeArt container inside states the real size;
// rewrite such a record as Excel would, an MSODRAWINGGROUP of at most 8224
// bytes followed by CONTINUE records, so every pass reads a normal stream.
static void split_oversized_drawing_group(std::vector<char>& wb) {
    constexpr size_t kMaxRec = 8224;
    // The real end of an MSODRAWINGGROUP whose container overruns its
    // record, or 0 when the record is an ordinary one.
    auto container_end = [&](size_t pos, size_t size, size_t& start) -> size_t {
        // The container sits at the start of the body, or after two bytes
        // Hancom puts before it.
        for (size_t skip : {size_t(0), size_t(2)}) {
            size_t c = pos + 4 + skip;
            if (c + 8 > wb.size()) return 0;
            uint16_t vi = util::read_u16_le(wb.data() + c);
            uint16_t ft = util::read_u16_le(wb.data() + c + 2);
            if ((vi & 0x0F) != 0x0F || ft != 0xF000) continue;
            size_t end = c + 8 + util::read_u32_le(wb.data() + c + 4);
            if (end <= pos + 4 + size || end + 4 > wb.size()) return 0;
            // Excel continues a large group in further MSODRAWINGGROUP or
            // CONTINUE records; only a group with no such follow-up is one
            // Hancom record that overran its length.
            size_t after = pos + 4 + size;
            if (after + 4 <= wb.size()) {
                uint16_t nt = util::read_u16_le(wb.data() + after);
                if (nt == RT_MSODRAWINGGROUP || nt == RT_CONTINUE) return 0;
            }
            // A record must follow where the container ends.
            if (end + 4 + util::read_u16_le(wb.data() + end + 2) > wb.size()) return 0;
            start = c;
            return end;
        }
        return 0;
    };

    size_t pos = 0;
    while (pos + 4 <= wb.size()) {
        uint16_t type = util::read_u16_le(wb.data() + pos);
        size_t size = util::read_u16_le(wb.data() + pos + 2);
        size_t start = 0, end = 0;
        if (type != RT_MSODRAWINGGROUP || !(end = container_end(pos, size, start))) {
            pos += 4 + size;
            continue;
        }
        std::vector<char> out(wb.begin(), wb.begin() + pos);
        for (size_t from = start; from < end;) {
            size_t n = std::min(kMaxRec, end - from);
            uint16_t t = from == start ? RT_MSODRAWINGGROUP : RT_CONTINUE;
            const char hdr[4] = {char(t & 0xFF), char(t >> 8), char(n & 0xFF), char(n >> 8)};
            out.insert(out.end(), hdr, hdr + 4);
            out.insert(out.end(), wb.begin() + from, wb.begin() + from + n);
            from += n;
        }
        pos = out.size();
        out.insert(out.end(), wb.begin() + end, wb.end());
        wb.swap(out);
    }
}

// True when a BOUNDSHEET name is flagged 8-bit but its record holds exactly
// twice its characters with every high byte zero: UTF-16 under an 8-bit
// flag, which only Hancom Cell writes.
static bool hancom_sheet_name(const char* rec, size_t size) {
    if (size < 8) return false;
    size_t cch = static_cast<uint8_t>(rec[6]);
    if ((static_cast<uint8_t>(rec[7]) & 0x01) || cch == 0 || size != 8 + cch * 2)
        return false;
    for (size_t i = 0; i < cch; ++i)
        if (rec[8 + 2 * i + 1] != 0) return false;
    return true;
}

// Hancom Cell's BIFF8 differs from Excel's in the records below; rewrite
// them as Excel would so the parser reads one dialect.
// - Cell records carry the row as 32 bits (Excel: 16), two bytes more than
//   the record should hold, so every cell came out in the wrong place with
//   the wrong value. The high word is dropped.
// - A TXO (text box, note) holds its text inline after the 10-byte header,
//   where Excel puts it in CONTINUE records. It is moved into a CONTINUE.
static void normalize_hancom_biff(std::vector<char>& wb) {
    // True when a cell record has Hancom's shape: a 32-bit row (high word
    // zero) and a size that fits that layout, not Excel's. Variable-size
    // records are checked against their own length fields.
    auto wide_row = [](uint16_t t, const char* b, size_t n) -> bool {
        if (n < 8 || b[2] != 0 || b[3] != 0) return false;
        auto u16 = [&](size_t o) { return size_t(util::read_u16_le(b + o)); };
        switch (t) {
            case 0x0201: return n == 8;                        // BLANK
            case 0x0205: return n == 10;                       // BOOLERR
            case 0x00FD: return n == 12;                       // LABELSST
            case 0x027E: return n == 12;                       // RK
            case 0x0203: return n == 16;                       // NUMBER
            case 0x0208: return n == 18;                       // ROW
            case 0x00BD:                                       // MULRK
                return n >= 14 && (n - 8) % 6 == 0 &&
                       u16(n - 2) >= u16(4) && u16(n - 2) - u16(4) + 1 == (n - 8) / 6;
            case 0x00BE:                                       // MULBLANK
                return n >= 10 && n % 2 == 0 &&
                       u16(n - 2) >= u16(4) && u16(n - 2) - u16(4) + 1 == (n - 8) / 2;
            case 0x0006:                                       // FORMULA
                return n >= 24 && n == 24 + u16(22);
            case 0x0204:                                       // LABEL (UTF-16)
                return n >= 11 && n == 11 + u16(8) * 2;
            default: return false;
        }
    };
    std::vector<char> out;
    out.reserve(wb.size());
    size_t pos = 0;
    auto put_rec = [&](uint16_t t, const char* body, size_t n) {
        const char hdr[4] = {char(t & 0xFF), char(t >> 8), char(n & 0xFF), char(n >> 8)};
        out.insert(out.end(), hdr, hdr + 4);
        out.insert(out.end(), body, body + n);
    };
    while (pos + 4 <= wb.size()) {
        uint16_t t = util::read_u16_le(wb.data() + pos);
        size_t n = util::read_u16_le(wb.data() + pos + 2);
        if (pos + 4 + n > wb.size()) break;
        const char* b = wb.data() + pos + 4;
        pos += 4 + n;
        if (wide_row(t, b, n)) {
            std::string body(b, b + 2);
            body.append(b + 4, b + n);
            put_rec(t, body.data(), body.size());
            continue;
        }
        bool inline_txo = t == RT_TXO && n > 18 &&
                          !(pos + 2 <= wb.size() &&
                            util::read_u16_le(wb.data() + pos) == RT_CONTINUE);
        if (inline_txo) {
            // 10 bytes of TXO fields, cchText, then flag + characters.
            size_t cch = util::read_u16_le(b + 10);
            size_t text_bytes = std::min(cch * 2, n - 13);
            std::string txo(b, b + 18 < b + n ? b + 18 : b + n);
            txo.resize(18, '\0');
            txo[12] = txo[13] = 0;  // cbRuns: no formatting runs follow
            put_rec(t, txo.data(), txo.size());
            std::string cont(1, '\x01');  // UTF-16
            cont.append(b + 13, b + 13 + text_bytes);
            put_rec(RT_CONTINUE, cont.data(), cont.size());
            continue;
        }
        put_rec(t, b, n);
    }
    out.insert(out.end(), wb.begin() + std::min(pos, wb.size()), wb.end());
    wb.swap(out);
}

std::vector<char> XlsParser::read_workbook_stream() {
    std::vector<char> wb;
    if (ole_.has_stream("Workbook")) {
        wb = ole_.read_stream("Workbook");
    } else if (ole_.has_stream("Book")) {
        wb = ole_.read_stream("Book");
    }
    split_oversized_drawing_group(wb);
    // A Hancom workbook names itself through its sheet names (written before
    // any cell), so look there before reading anything else.
    for (size_t pos = 0; pos + 4 <= wb.size();) {
        uint16_t t = util::read_u16_le(wb.data() + pos);
        size_t n = util::read_u16_le(wb.data() + pos + 2);
        if (pos + 4 + n > wb.size() || t == RT_EOF) break;
        if (t == RT_BOUNDSHEET && hancom_sheet_name(wb.data() + pos + 4, n)) {
            hancom_wide_ = true;
            break;
        }
        pos += 4 + n;
    }
    if (hancom_wide_) normalize_hancom_biff(wb);
    return wb;
}

// ---------- Workbook parsing -------------------------------------------------

void XlsParser::parse_workbook() {
    std::vector<char> wb = read_workbook_stream();
    if (wb.empty()) return;

    // First pass: collect BoundSheet names, SST, FORMAT, XF records.
    std::vector<std::string> sheet_names;
    int bof_count = 0;
    bool in_globals = false;

    size_t pos = 0;
    std::vector<std::vector<char>> continue_blocks;

    // Pass 1: globals (SST, BoundSheet, FORMAT, XF, FILEPASS)
    while (pos + 4 <= wb.size()) {
        uint16_t rec_type = util::read_u16_le(wb.data() + pos);
        uint16_t rec_size = util::read_u16_le(wb.data() + pos + 2);
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
                // Hancom Cell flags the name as 8-bit but writes it as UTF-16:
                // the record then holds exactly twice the characters, every
                // high byte zero ("t\0e\0x\0t\0"). Read as 8-bit it came out
                // as "t\0e\0", NULs and all.
                bool wide = (name_flags & 0x01) != 0;
                if (!wide && name_len > 0 && rec_size == 8 + size_t(name_len) * 2) {
                    wide = true;
                    for (size_t i = 0; i < name_len; ++i)
                        if (rec_data[8 + 2 * i + 1] != 0) { wide = false; break; }
                    // The same writer flags every string 8-bit and writes it
                    // as UTF-16: shared strings, labels, formats, text boxes.
                    if (wide) hancom_wide_ = true;
                }
                if (wide) {
                    size_t byte_len = std::min(size_t(name_len) * 2, size_t(rec_size - 8));
                    sname = util::utf16le_to_utf8(rec_data + 8, byte_len);
                } else {
                    size_t byte_len = std::min(size_t(name_len), size_t(rec_size - 8));
                    for (size_t i = 0; i < byte_len; ++i) {
                        util::append_cp1252(sname, static_cast<uint8_t>(rec_data[8 + i]));
                    }
                }
                sheet_names.push_back(sname);
            }

            // FORMAT record: numFmtId (2 bytes) + formatCode string
            if (rec_type == RT_FORMAT && rec_size >= 5) {
                uint16_t fmt_id = util::read_u16_le(rec_data);
                // Format string is an XLUnicodeString starting at offset 2
                uint16_t cch = util::read_u16_le(rec_data + 2);
                uint8_t flags = static_cast<uint8_t>(rec_data[4]);
                bool high_byte = hancom_wide_ || (flags & 0x01) != 0;

                size_t str_start = 5;
                size_t bytes_needed = high_byte ? (size_t(cch) * 2) : size_t(cch);
                if (str_start + bytes_needed <= rec_size) {
                    std::string code;
                    if (high_byte) {
                        code = util::utf16le_to_utf8(rec_data + str_start, bytes_needed);
                    } else {
                        for (size_t i = 0; i < bytes_needed; ++i) {
                            util::append_cp1252(code,
                                static_cast<uint8_t>(rec_data[str_start + i]));
                        }
                    }
                    custom_num_fmts_[fmt_id] = code;
                }
            }

            // FONT record: bytes 2-3 = options (bit 0=bold weight>=700, bit 1=italic)
            if (rec_type == RT_FONT && rec_size >= 4) {
                FontInfo fi;
                uint16_t options = util::read_u16_le(rec_data + 2);
                fi.italic = (options & 0x02) != 0;
                // Bold detection: weight field at offset 6 (BIFF8) >= 700
                if (rec_size >= 8) {
                    uint16_t weight = util::read_u16_le(rec_data + 6);
                    fi.bold = (weight >= 700);
                } else {
                    fi.bold = (options & 0x01) != 0;
                }
                fonts_.push_back(fi);
            }

            // XF record: bytes 0-1 = fontId, bytes 2-3 = numFmtId
            if (rec_type == RT_XF && rec_size >= 4) {
                uint16_t font_id = util::read_u16_le(rec_data);
                uint16_t fmt_id = util::read_u16_le(rec_data + 2);
                xf_num_fmt_ids_.push_back(static_cast<int>(fmt_id));
                xf_font_ids_.push_back(static_cast<int>(font_id));
            }

            if (rec_type == RT_SST) {
                continue_blocks.clear();
                size_t look = pos + rec_size;
                while (look + 4 <= wb.size()) {
                    uint16_t ct = util::read_u16_le(wb.data() + look);
                    uint16_t cs = util::read_u16_le(wb.data() + look + 2);
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
    int bof_depth = 0, sheet_index = -1;
    int current_sheet = -1;
    bool expect_string = false;  // true after FORMULA with string result
    uint16_t formula_row = 0, formula_col = 0;
    int formula_sheet = -1;

    while (pos + 4 <= wb.size()) {
        uint16_t rec_type = util::read_u16_le(wb.data() + pos);
        uint16_t rec_size = util::read_u16_le(wb.data() + pos + 2);
        pos += 4;
        if (pos + rec_size > wb.size()) break;
        const char* rec_data = wb.data() + pos;

        // Substreams nest: a chart embedded in a worksheet opens its own
        // BOF..EOF inside the sheet's, and is not a sheet of the workbook.
        if (rec_type == RT_BOF) {
            if (++bof_depth == 1 && ++bof_count > 1) sheet_index = bof_count - 2;
            current_sheet = bof_depth == 1 ? sheet_index : -1;
        } else if (rec_type == RT_EOF && bof_depth > 0) {
            if (--bof_depth == 1) current_sheet = sheet_index;
        }

        // STRING record follows FORMULA when result is a string.
        if (expect_string && rec_type == RT_STRING && rec_size >= 3) {
            expect_string = false;
            if (formula_sheet >= 0 && formula_sheet < static_cast<int>(sheets_.size())) {
                uint16_t cch = util::read_u16_le(rec_data);
                uint8_t flags = static_cast<uint8_t>(rec_data[2]);
                bool high_byte = hancom_wide_ || (flags & 0x01) != 0;
                size_t str_start = 3;
                size_t bytes_needed = high_byte ? (size_t(cch) * 2) : size_t(cch);
                std::string val;
                if (str_start + bytes_needed <= rec_size) {
                    if (high_byte) {
                        val = util::utf16le_to_utf8(rec_data + str_start, bytes_needed);
                    } else {
                        for (size_t i = 0; i < bytes_needed; ++i) {
                            util::append_cp1252(val,
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

            if ((rec_type == RT_HEADER || rec_type == RT_FOOTER) && rec_size >= 3) {
                std::string t = header_footer_text(xl_unicode_string(rec_data, rec_size, hancom_wide_));
                if (!t.empty()) sheet.texts.push_back(std::move(t));
            } else if (rec_type == RT_TXO) {
                std::string t = txo_text(wb, pos, rec_size, hancom_wide_);
                if (!t.empty()) sheet.texts.push_back(std::move(t));
            } else if (rec_type == RT_LABELSST && rec_size >= 10) {
                uint16_t row = util::read_u16_le(rec_data);
                uint16_t col = util::read_u16_le(rec_data + 2);
                uint16_t xf_idx = util::read_u16_le(rec_data + 4);
                uint32_t sst_idx = util::read_u32_le(rec_data + 6);
                if (sst_idx < sst_.size()) {
                    auto [b, it] = get_font_info(xf_idx);
                    sheet.cells.push_back({row, col, sst_[sst_idx], b, it});
                }
            } else if (rec_type == RT_BOOLERR && rec_size >= 8) {
                // BOOLERR record: boolean or error cell
                uint16_t row = util::read_u16_le(rec_data);
                uint16_t col = util::read_u16_le(rec_data + 2);
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
                uint16_t row = util::read_u16_le(rec_data);
                uint16_t col = util::read_u16_le(rec_data + 2);
                // XF index at offset 4 (2 bytes) — skip
                uint16_t cch = util::read_u16_le(rec_data + 6);
                uint8_t flags = static_cast<uint8_t>(rec_data[8]);
                bool high_byte = hancom_wide_ || (flags & 0x01) != 0;
                size_t str_start = 9;
                size_t bytes_needed = high_byte ? (size_t(cch) * 2) : size_t(cch);
                std::string val;
                if (str_start + bytes_needed <= rec_size) {
                    if (high_byte) {
                        val = util::utf16le_to_utf8(rec_data + str_start, bytes_needed);
                    } else {
                        for (size_t i = 0; i < bytes_needed; ++i) {
                            util::append_cp1252(val,
                                static_cast<uint8_t>(rec_data[str_start + i]));
                        }
                    }
                }
                if (!val.empty()) {
                    sheet.cells.push_back({row, col, val});
                }
            } else if (rec_type == RT_NUMBER && rec_size >= 14) {
                uint16_t row = util::read_u16_le(rec_data);
                uint16_t col = util::read_u16_le(rec_data + 2);
                uint16_t xf_idx = util::read_u16_le(rec_data + 4);
                double val = util::read_f64_le(rec_data + 6);
                sheet.cells.push_back({row, col, format_cell_number(val, xf_idx)});
            } else if (rec_type == RT_RK && rec_size >= 10) {
                uint16_t row = util::read_u16_le(rec_data);
                uint16_t col = util::read_u16_le(rec_data + 2);
                uint16_t xf_idx = util::read_u16_le(rec_data + 4);
                uint32_t rk = util::read_u32_le(rec_data + 6);
                double val = decode_rk(rk);
                sheet.cells.push_back({row, col, format_cell_number(val, xf_idx)});
            } else if (rec_type == RT_MULRK && rec_size >= 6) {
                uint16_t row = util::read_u16_le(rec_data);
                uint16_t first_col = util::read_u16_le(rec_data + 2);
                uint16_t last_col = util::read_u16_le(rec_data + rec_size - 2);
                size_t pair_offset = 4;
                for (uint16_t c = first_col; c <= last_col && pair_offset + 6 <= rec_size - 2u; ++c) {
                    uint16_t xf_idx = util::read_u16_le(rec_data + pair_offset);
                    uint32_t rk = util::read_u32_le(rec_data + pair_offset + 2);
                    double val = decode_rk(rk);
                    sheet.cells.push_back({row, c, format_cell_number(val, xf_idx)});
                    pair_offset += 6;
                }
            } else if (rec_type == RT_FORMULA && rec_size >= 20) {
                // FORMULA record: row(2) + col(2) + xf(2) + result(8) + options(2) + reserved(4)
                uint16_t row = util::read_u16_le(rec_data);
                uint16_t col = util::read_u16_le(rec_data + 2);
                uint16_t xf_idx = util::read_u16_le(rec_data + 4);

                // Result value is 8 bytes at offset 6.
                // Check if it's a special type: bytes 6-7 = 0xFFFF means not a number.
                uint16_t marker = util::read_u16_le(rec_data + 12);
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
                    double val = util::read_f64_le(rec_data + 6);
                    sheet.cells.push_back({row, col, format_cell_number(val, xf_idx)});
                }
            }
        }

        pos += rec_size;
    }
}

// ---------- sheet to markdown ------------------------------------------------

std::string XlsParser::sheet_to_markdown(const Sheet& sheet, int sheet_num) {
    std::string md = sheet_table(sheet);
    for (const auto& t : sheet.texts) md += t + "\n\n";
    if (md.empty()) return "";
    std::string display_name = sheet.name.empty()
        ? ("Sheet " + std::to_string(sheet_num))
        : sheet.name;
    return "## " + display_name + "\n\n" + md;
}

std::string XlsParser::sheet_table(const Sheet& sheet) {
    if (sheet.cells.empty()) return "";

    // Determine grid dimensions.
    uint16_t max_row = 0, max_col = 0;
    for (const auto& c : sheet.cells) {
        max_row = std::max(max_row, c.row);
        max_col = std::max(max_col, c.col);
    }

    // No size limit — emit the full grid (BIFF rows/cols are 16-bit bounded).
    uint16_t limit_row = max_row;
    uint16_t limit_col = max_col;

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

    std::vector<char> wb = read_workbook_stream();
    if (wb.empty()) return images;

    // Collect the drawing records with their CONTINUEs. The pictures
    // themselves live in the workbook-wide MSODRAWINGGROUP (its BStore holds
    // one FBSE per picture, the BLIP inline after it); a sheet's MSODRAWING
    // only points at them. Reading the sheet records alone found nothing.
    std::vector<char> drawing_data;
    size_t pos = 0;

    while (pos + 4 <= wb.size()) {
        uint16_t rec_type = util::read_u16_le(wb.data() + pos);
        uint16_t rec_size = util::read_u16_le(wb.data() + pos + 2);
        pos += 4;
        if (pos + rec_size > wb.size()) break;

        if (rec_type == RT_MSODRAWINGGROUP || rec_type == RT_MSODRAWING) {
            drawing_data.insert(drawing_data.end(), wb.data() + pos, wb.data() + pos + rec_size);
            size_t look = pos + rec_size;
            while (look + 4 <= wb.size()) {
                uint16_t ct = util::read_u16_le(wb.data() + look);
                uint16_t cs = util::read_u16_le(wb.data() + look + 2);
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
        uint16_t ver_inst = util::read_u16_le(drawing_data.data() + pos);
        uint16_t rec_type = util::read_u16_le(drawing_data.data() + pos + 2);
        uint32_t rec_len  = util::read_u32_le(drawing_data.data() + pos + 4);

        uint8_t ver = ver_inst & 0x0F;

        if (ver == 0x0F) {
            pos += 8;
            continue;
        }

        // FBSE: a 36-byte header and an optional name, then the BLIP it
        // describes. Step onto that BLIP; the loop reads it next.
        if (rec_type == 0xF007) {
            size_t fbse_end = pos + 8 + size_t(rec_len);
            if (rec_len >= 36 && fbse_end <= drawing_data.size()) {
                uint8_t cb_name = static_cast<uint8_t>(drawing_data[pos + 8 + 33]);
                size_t blip = pos + 8 + 36 + cb_name;
                if (blip + 8 <= fbse_end) {
                    pos = blip;
                    continue;
                }
            }
            pos = fbse_end;
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
                    // Metafile text: the BLIPFileData is usually zlib-compressed.
                    // Try the bytes raw first (uncompressed metafiles), then
                    // inflate. emf_extract_text validates the header, so a wrong
                    // guess simply yields no text.
                    if (fmt == "emf" || fmt == "wmf") {
                        const uint8_t* p =
                            reinterpret_cast<const uint8_t*>(img.data.data());
                        std::string t = metafile_extract_text(fmt.c_str(), p,
                                                              img.data.size());
                        if (t.empty()) {
                            auto dec = inflate_zlib(p, img.data.size(),
                                                    img.data.size() * 10);
                            if (!dec.empty())
                                t = metafile_extract_text(fmt.c_str(),
                                                          dec.data(), dec.size());
                        }
                        img.embedded_text = std::move(t);
                    }
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
        // Write the images out, the way every other parser does; a
        // reference to a file that was never created helps no one.
        // An Escher BLIP record declares a type rather than a filename, so
        // store_image settles the extension from the format. An image that
        // could not be written is not referenced.
        // Without image extraction nothing is linked, as in every other
        // parser, but a metafile's text is still part of the document.
        for (auto& img : images) {
            if (opts.images) {
                const std::string filename = util::store_image(img, opts);
                if (filename.empty()) continue;
                md += "![" + filename + "](" + opts.image_ref_prefix + filename + ")\n\n";
            }
            if (!img.embedded_text.empty())
                md += img.embedded_text + "\n\n";
        }
    }

    return md + ole_storage_attachments(ole_, opts);
}

std::vector<PageChunk> XlsParser::to_chunks(const ConvertOptions& opts) {
    std::vector<PageChunk> chunks;

    auto images = opts.images ? extract_images(opts.min_image_size) : std::vector<ImageData>{};
    if (opts.images) {
        // An image that could not be written is dropped rather than handed
        // over as a chunk image pointing at nothing.
        std::vector<ImageData> kept;
        for (auto& img : images)
            if (!util::store_image(img, opts).empty())
                kept.push_back(std::move(img));
        images = std::move(kept);
    }
    // A workbook whose sheets did not parse still divides here; guard the
    // zero before it becomes a division by zero.
    int img_per_sheet =
        (images.empty() || sheets_.empty())
            ? 0
            : static_cast<int>((images.size() + sheets_.size() - 1) /
                               sheets_.size());

    for (size_t i = 0; i < sheets_.size(); ++i) {
        PageChunk chunk;
        chunk.page_number = static_cast<int>(i + 1);
        chunk.text = sheet_to_markdown(sheets_[i], static_cast<int>(i) + 1);
        chunk.page_width = 612.0;
        chunk.page_height = 792.0;
        chunk.body_font_size = 10.0;

        if (opts.tables && !sheets_[i].cells.empty()) {
            uint16_t max_row = 0, max_col = 0;
            for (const auto& c : sheets_[i].cells) {
                max_row = std::max(max_row, c.row);
                max_col = std::max(max_col, c.col);
            }
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

        if (opts.images && img_per_sheet > 0) {
            int start = static_cast<int>(i) * img_per_sheet;
            int end = std::min(start + img_per_sheet, static_cast<int>(images.size()));
            for (int j = start; j < end; ++j) {
                // The reference goes in the text too, the same as to_markdown:
                // a chunk consumer should be told which picture the sheet
                // shows, not just handed the bytes.
                const std::string ref = util::image_ref_name(
                    images[j].name, images[j].format, images[j].saved_path);
                chunk.text += "\n\n![" + ref + "](" +
                              opts.image_ref_prefix + ref + ")";
                if (!images[j].embedded_text.empty())
                    chunk.text += "\n\n" + images[j].embedded_text;
                chunk.images.push_back(images[j]);
            }
        }

        chunks.push_back(std::move(chunk));
    }

    std::string att = ole_storage_attachments(ole_, opts);
    if (!att.empty() && !chunks.empty()) chunks.back().text += att;
    return chunks;
}

} // namespace jdoc
