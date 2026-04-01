// .doc (Word Binary) parser implementation.
// Supports Word 6/95 and Word 97-2003 formats.

#include "doc_parser.h"
#include "common/string_utils.h"
#include "common/binary_utils.h"
#include "common/file_utils.h"
#include "common/image_utils.h"
#include "common/png_encode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace jdoc {

// ── Smart quote and special character mapping ───────────────────

// Map CP1252 "smart" characters (0x80-0x9F) to readable Unicode.
// 0x91/0x92 → left/right single quote
// 0x93/0x94 → left/right double quote
// 0x96 → en dash, 0x97 → em dash, 0x85 → ellipsis
static uint32_t map_smart_char(uint8_t ch) {
    switch (ch) {
    case 0x85: return 0x2026;  // …
    case 0x91: return 0x2018;  // '
    case 0x92: return 0x2019;  // '
    case 0x93: return 0x201C;  // "
    case 0x94: return 0x201D;  // "
    case 0x96: return 0x2013;  // –
    case 0x97: return 0x2014;  // —
    case 0xA0: return 0x0020;  // non-breaking space → space
    default:   return 0;       // not a smart char
    }
}

// ── SPRM / CHPX helpers ─────────────────────────────────────────

// SPRM operand size in bytes, derived from opcode bits 15:13.
static size_t sprm_operand_size(uint16_t opcode, const char* data, size_t remaining) {
    switch ((opcode >> 13) & 7) {
    case 0: case 1: return 1;
    case 2: case 4: case 5: return 2;
    case 3: return 4;
    case 7: return 3;
    case 6: return (remaining >= 1) ? 1 + static_cast<uint8_t>(data[0]) : 0;
    default: return 0;
    }
}

// CHPX picture run: FC range + Data stream offset from sprmCPicLocation.
struct PictureFcEntry {
    uint32_t fc_start, fc_end;
    uint32_t data_offset; // operand of sprmCPicLocation
};

// PAPX list info: FC range + list format override index and level.
struct ListFcEntry {
    uint32_t fc_start, fc_end;
    uint16_t ilfo;  // 1-based index into LFO table (0 = no list)
    uint8_t ilvl;   // list level 0-8
};

// Find the list entry covering fc, or nullptr.
static const ListFcEntry* find_list_entry(
    const std::vector<ListFcEntry>& entries, uint32_t fc) {
    auto it = std::upper_bound(entries.begin(), entries.end(), fc,
        [](uint32_t f, const ListFcEntry& e) { return f < e.fc_start; });
    if (it == entries.begin()) return nullptr;
    --it;
    return (fc < it->fc_end) ? &(*it) : nullptr;
}

// Korean syllable 가나다라마바사아자차카타파하 for list numbering.
static const char* KOREAN_SYLLABLES[] = {
    "\xEA\xB0\x80", "\xEB\x82\x98", "\xEB\x8B\xA4", "\xEB\x9D\xBC",
    "\xEB\xA7\x88", "\xEB\xB0\x94", "\xEC\x82\xAC", "\xEC\x95\x84",
    "\xEC\x9E\x90", "\xEC\xB0\xA8", "\xEC\xB9\xB4", "\xED\x83\x80",
    "\xED\x8C\x8C", "\xED\x95\x98"
};
static constexpr int KOREAN_SYLLABLE_COUNT = 14;

// Generate list numbering prefix for a given level and counters.
struct ListCounter {
    int counts[9] = {};
    uint16_t active_ilfo = 0;

    bool has_korean = false; // detected from text content

    std::string next_prefix(uint16_t ilfo, uint8_t ilvl, uint16_t lid) {
        if (ilfo != active_ilfo) {
            std::fill(std::begin(counts), std::end(counts), 0);
            active_ilfo = ilfo;
        }
        counts[ilvl]++;
        for (int i = ilvl + 1; i < 9; ++i) counts[i] = 0;

        bool korean = (lid == 0x0412) || has_korean;
        switch (ilvl) {
        case 0:
            return std::to_string(counts[0]) + ". ";
        case 1:
            return std::to_string(counts[0]) + "."
                 + std::to_string(counts[1]) + ". ";
        case 2:
            if (korean && counts[2] >= 1 && counts[2] <= KOREAN_SYLLABLE_COUNT)
                return std::string(KOREAN_SYLLABLES[counts[2] - 1]) + ". ";
            return std::string(1, 'a' + ((counts[2] - 1) % 26)) + ". ";
        case 3:
            return "(" + std::to_string(counts[3]) + ") ";
        case 4:
            if (korean && counts[4] >= 1 && counts[4] <= KOREAN_SYLLABLE_COUNT)
                return "(" + std::string(KOREAN_SYLLABLES[counts[4] - 1]) + ") ";
            return "(" + std::string(1, 'a' + ((counts[4] - 1) % 26)) + ") ";
        default: {
            // ① ② ③ ... (U+2460 = 0x2460 + n-1, up to ⑳ U+2473)
            int idx = ilvl >= 5 ? ilvl - 5 : 0;
            int num = counts[ilvl];
            if (num >= 1 && num <= 20) {
                std::string s;
                util::append_utf8(s, 0x2460 + num - 1);
                s += ' ';
                return s;
            }
            return std::to_string(num) + ") ";
        }
        }
    }
};

// Check if a PICF at data_offset in the Data stream contains an inline image.
// A PICF with OfficeArt FBSE (0xF007) or BLIP (0xF01A-0xF029) records = image.
static bool picf_has_image(const std::vector<char>& data_stream, uint32_t data_offset) {
    if (data_offset + 6 > data_stream.size()) return false;
    uint32_t lcb = util::read_u32_le(data_stream.data() + data_offset);
    uint16_t cb_header = util::read_u16_le(data_stream.data() + data_offset + 4);
    if (lcb <= cb_header || cb_header < 0x44) return false;

    size_t art_end = std::min(static_cast<size_t>(data_offset + lcb), data_stream.size());
    size_t pos = data_offset + cb_header;
    while (pos + 8 <= art_end) {
        uint16_t rtype = util::read_u16_le(data_stream.data() + pos + 2);
        uint32_t rlen = util::read_u32_le(data_stream.data() + pos + 4);
        if (rtype == 0xF007) return true;                     // FBSE
        if (rtype >= 0xF01A && rtype <= 0xF029) return true;  // standalone BLIP
        if (pos + 8 + rlen > art_end) break;
        pos += 8 + rlen;
    }
    return false;
}

// Check if fc falls within any picture FC entry that has a valid image PICF.
static bool is_picture_fc(const std::vector<PictureFcEntry>& entries, uint32_t fc) {
    auto it = std::upper_bound(entries.begin(), entries.end(), fc,
        [](uint32_t f, const PictureFcEntry& e) { return f < e.fc_start; });
    if (it == entries.begin()) return false;
    --it;
    return fc < it->fc_end;
}

// Replace U+FFFC markers with inline image references; append leftover images.
static std::string replace_image_markers(const std::string& md,
    const std::vector<ImageData>& images, const std::string& prefix) {
    std::string result;
    result.reserve(md.size());
    size_t img_idx = 0;
    for (size_t i = 0; i < md.size(); ++i) {
        if (i + 2 < md.size() &&
            static_cast<uint8_t>(md[i])     == 0xEF &&
            static_cast<uint8_t>(md[i + 1]) == 0xBF &&
            static_cast<uint8_t>(md[i + 2]) == 0xBC) {
            if (img_idx < images.size()) {
                const auto& img = images[img_idx];
                std::string fn = img.name + "." + img.format;
                result += "\n![" + fn + "](" + prefix + fn + ")\n";
            }
            img_idx++;
            i += 2;
        } else {
            result.push_back(md[i]);
        }
    }
    for (; img_idx < images.size(); ++img_idx) {
        const auto& img = images[img_idx];
        std::string fn = img.name + "." + img.format;
        result += "\n![" + fn + "](" + fn + ")\n";
    }
    return result;
}

// Strip U+FFFC markers from text.
static std::string strip_image_markers(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (i + 2 < text.size() &&
            static_cast<uint8_t>(text[i])     == 0xEF &&
            static_cast<uint8_t>(text[i + 1]) == 0xBF &&
            static_cast<uint8_t>(text[i + 2]) == 0xBC) {
            i += 2;
        } else {
            result.push_back(text[i]);
        }
    }
    return result;
}

// ── DocParser ───────────────────────────────────────────────────

DocParser::DocParser(OleReader& ole) : ole_(ole) {}

// ── Character processing (field markers, control chars) ─────────

bool DocParser::process_char(uint32_t ch, std::string& result, bool is_picture) {
    // Inline object placeholder.
    if (ch == 0x01) {
        if (is_picture) {
            result.append("\xEF\xBF\xBC"); // U+FFFC Object Replacement Character
            return true;
        }
        return false;
    }
    if (ch == 0x08) return false;
    // Field begin marker: start tracking nesting.
    if (ch == 0x13) {
        field_depth_++;
        field_show_result_ = false;
        collecting_field_instr_ = true;
        field_instruction_.clear();
        return false;
    }
    // Field separator: switch to showing result text.
    if (ch == 0x14) {
        collecting_field_instr_ = false;

        field_show_result_ = true;
        // Check if instruction is HYPERLINK (skip local bookmark links with \l).
        pending_hyperlink_url_.clear();
        if (field_instruction_.size() > 10) {
            // Field instruction: " HYPERLINK \"url\" " or " HYPERLINK url "
            auto pos = field_instruction_.find("HYPERLINK");
            if (pos != std::string::npos) {
                bool is_local = field_instruction_.find("\\l") != std::string::npos;
                if (!is_local) {
                    auto url_start = field_instruction_.find('"', pos + 9);
                    if (url_start != std::string::npos) {
                        auto url_end = field_instruction_.find('"', url_start + 1);
                        if (url_end != std::string::npos)
                            pending_hyperlink_url_ = field_instruction_.substr(url_start + 1, url_end - url_start - 1);
                    } else {
                        // No quotes — take the rest as URL
                        size_t s = pos + 9;
                        while (s < field_instruction_.size() && field_instruction_[s] == ' ') s++;
                        size_t e = field_instruction_.find(' ', s);
                        if (e == std::string::npos) e = field_instruction_.size();
                        if (e > s) pending_hyperlink_url_ = field_instruction_.substr(s, e - s);
                    }
                }
            }
        }
        if (!pending_hyperlink_url_.empty())
            result += "[";
        return false;
    }
    // Field end marker: close nesting.
    if (ch == 0x15) {
        if (!pending_hyperlink_url_.empty()) {
            result += "](" + pending_hyperlink_url_ + ")";
            pending_hyperlink_url_.clear();
        }
        if (field_depth_ > 0) field_depth_--;
        field_show_result_ = (field_depth_ > 0);
        collecting_field_instr_ = false;
        return false;
    }

    // Inside a field's instruction part (before separator): collect instruction.
    if (field_depth_ > 0 && !field_show_result_) {
        if (collecting_field_instr_ && ch >= 0x20 && ch < 0x80)
            field_instruction_ += static_cast<char>(ch);
        return false;
    }

    // Line/paragraph breaks.
    if (ch == 0x0D || ch == 0x0A) {
        result.push_back('\n');
        return true;
    }
    // Cell mark (0x07): skip here; handled in extract_text_word8 with FC context.
    // For Word 6/95 fallback path, use heuristic.
    if (ch == 0x07) {
        if (!result.empty() && result.back() == '\x1F')
            result.back() = '\n';
        else
            result.push_back('\x1F');
        return true;
    }
    // Tab.
    if (ch == 0x09) {
        result.push_back('\t');
        return true;
    }
    // Page break / section break.
    if (ch == 0x0C) {
        ++page_num_;
        result.append("\n--- Page " + std::to_string(page_num_) + " ---\n");
        return true;
    }
    // Filter remaining control characters.
    if (ch < 0x20 && ch != '\t' && ch != '\n') return false;

    // Smart quotes and special CP1252 characters (in 0x80-0x9F range).
    if (ch >= 0x80 && ch <= 0x9F) {
        uint32_t mapped = map_smart_char(static_cast<uint8_t>(ch));
        if (mapped) {
            util::append_utf8(result, mapped);
            return true;
        }
    }

    // Append as Unicode codepoint.
    util::append_utf8(result, ch);
    return true;
}

// ── CHPX picture FC range extraction (Word 97+) ─────────────────

static constexpr uint16_t SPRM_C_PIC_LOCATION = 0x6A03;
static constexpr size_t CHPX_PAGE_SIZE = 512;

// Build sorted list of FC entries where CHPX has sprmCPicLocation pointing to
// a valid image PICF in the Data stream. Only entries with actual BLIP/FBSE
// records inside the PICF are included (filters out OLE objects, shapes, etc.).
static std::vector<PictureFcEntry> build_picture_fc_entries(
    const std::vector<char>& word_doc,
    const std::vector<char>& table_stream,
    const std::vector<char>& data_stream) {

    std::vector<PictureFcEntry> entries;
    if (word_doc.size() < 0x0102 || table_stream.empty()) return entries;

    uint32_t fc_plcf = util::read_u32_le(word_doc.data() + 0x00FA);
    uint32_t lcb_plcf = util::read_u32_le(word_doc.data() + 0x00FE);
    if (fc_plcf == 0 || lcb_plcf < 12) return entries;
    if (static_cast<size_t>(fc_plcf) + lcb_plcf > table_stream.size()) return entries;

    uint32_t n = (lcb_plcf - 4) / 8;
    if (n == 0 || n > 0xFFFF) return entries;

    const char* plcf = table_stream.data() + fc_plcf;
    const char* pns = plcf + (n + 1) * 4;

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t pn = util::read_u32_le(pns + i * 4) & 0x003FFFFF;
        size_t page_off = static_cast<size_t>(pn) * CHPX_PAGE_SIZE;
        if (page_off + CHPX_PAGE_SIZE > word_doc.size()) continue;

        const char* page = word_doc.data() + page_off;
        uint8_t crun = static_cast<uint8_t>(page[CHPX_PAGE_SIZE - 1]);
        if (crun == 0) continue;

        size_t rgfc_end = static_cast<size_t>(crun + 1) * 4;
        size_t rgb_off = rgfc_end;
        if (rgb_off + crun > CHPX_PAGE_SIZE - 1) continue;

        for (uint8_t r = 0; r < crun; ++r) {
            uint8_t chpx_word_off = static_cast<uint8_t>(page[rgb_off + r]);
            if (chpx_word_off == 0) continue;

            size_t chpx_pos = static_cast<size_t>(chpx_word_off) * 2;
            if (chpx_pos >= CHPX_PAGE_SIZE - 1) continue;

            uint8_t cb = static_cast<uint8_t>(page[chpx_pos]);
            if (cb == 0 || chpx_pos + 1 + cb > CHPX_PAGE_SIZE) continue;

            const char* grpprl = page + chpx_pos + 1;
            size_t pos = 0;
            uint32_t operand = 0;
            bool has_pic = false;

            while (pos + 2 <= cb) {
                uint16_t opcode = util::read_u16_le(grpprl + pos);
                if (opcode == SPRM_C_PIC_LOCATION && pos + 6 <= cb) {
                    operand = util::read_u32_le(grpprl + pos + 2);
                    has_pic = true;
                    break;
                }
                size_t sz = sprm_operand_size(opcode, grpprl + pos + 2, cb - pos - 2);
                if (sz == 0) break;
                pos += 2 + sz;
            }

            if (has_pic && operand != 0 && picf_has_image(data_stream, operand)) {
                uint32_t fc_start = util::read_u32_le(page + r * 4);
                uint32_t fc_end = util::read_u32_le(page + (r + 1) * 4);
                entries.push_back({fc_start, fc_end, operand});
            }
        }
    }

    std::sort(entries.begin(), entries.end(),
        [](const PictureFcEntry& a, const PictureFcEntry& b) {
            return a.fc_start < b.fc_start;
        });
    return entries;
}

// ── CHPX bold FC range extraction (Word 97+) ────────────────────

static constexpr uint16_t SPRM_C_FBOLD = 0x0835;

struct BoldFcEntry { uint32_t fc_start, fc_end; };

// Build sorted list of FC ranges where CHPX has sprmCFBold=1.
static std::vector<BoldFcEntry> build_bold_fc_entries(
    const std::vector<char>& word_doc,
    const std::vector<char>& table_stream) {

    std::vector<BoldFcEntry> entries;
    if (word_doc.size() < 0x0102 || table_stream.empty()) return entries;

    uint32_t fc_plcf = util::read_u32_le(word_doc.data() + 0x00FA);
    uint32_t lcb_plcf = util::read_u32_le(word_doc.data() + 0x00FE);
    if (fc_plcf == 0 || lcb_plcf < 12) return entries;
    if (static_cast<size_t>(fc_plcf) + lcb_plcf > table_stream.size()) return entries;

    uint32_t n = (lcb_plcf - 4) / 8;
    if (n == 0 || n > 0xFFFF) return entries;

    const char* plcf = table_stream.data() + fc_plcf;
    const char* pns = plcf + (n + 1) * 4;

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t pn = util::read_u32_le(pns + i * 4) & 0x003FFFFF;
        size_t page_off = static_cast<size_t>(pn) * CHPX_PAGE_SIZE;
        if (page_off + CHPX_PAGE_SIZE > word_doc.size()) continue;

        const char* page = word_doc.data() + page_off;
        uint8_t crun = static_cast<uint8_t>(page[CHPX_PAGE_SIZE - 1]);
        if (crun == 0) continue;

        size_t rgfc_end = static_cast<size_t>(crun + 1) * 4;
        size_t rgb_off = rgfc_end;
        if (rgb_off + crun > CHPX_PAGE_SIZE - 1) continue;

        for (uint8_t r = 0; r < crun; ++r) {
            uint8_t chpx_word_off = static_cast<uint8_t>(page[rgb_off + r]);
            if (chpx_word_off == 0) continue;

            size_t chpx_pos = static_cast<size_t>(chpx_word_off) * 2;
            if (chpx_pos >= CHPX_PAGE_SIZE - 1) continue;

            uint8_t cb = static_cast<uint8_t>(page[chpx_pos]);
            if (cb == 0 || chpx_pos + 1 + cb > CHPX_PAGE_SIZE) continue;

            const char* grpprl = page + chpx_pos + 1;
            size_t pos = 0;
            bool is_bold = false;

            while (pos + 2 <= cb) {
                uint16_t opcode = util::read_u16_le(grpprl + pos);
                if (opcode == SPRM_C_FBOLD && pos + 3 <= cb) {
                    is_bold = (static_cast<uint8_t>(grpprl[pos + 2]) != 0);
                }
                size_t sz = sprm_operand_size(opcode, grpprl + pos + 2, cb - pos - 2);
                if (sz == 0) break;
                pos += 2 + sz;
            }

            if (is_bold) {
                uint32_t fc_start = util::read_u32_le(page + r * 4);
                uint32_t fc_end = util::read_u32_le(page + (r + 1) * 4);
                entries.push_back({fc_start, fc_end});
            }
        }
    }

    std::sort(entries.begin(), entries.end(),
        [](const BoldFcEntry& a, const BoldFcEntry& b) {
            return a.fc_start < b.fc_start;
        });
    return entries;
}

static bool is_bold_fc(const std::vector<BoldFcEntry>& entries, uint32_t fc) {
    auto it = std::upper_bound(entries.begin(), entries.end(), fc,
        [](uint32_t f, const BoldFcEntry& e) { return f < e.fc_start; });
    if (it == entries.begin()) return false;
    --it;
    return fc < it->fc_end;
}

// ── PAPX paragraph property extraction (Word 97+) ───────────────

static constexpr uint16_t SPRM_P_ILVL    = 0x260A;
static constexpr uint16_t SPRM_P_ILFO    = 0x460B;
static constexpr uint16_t SPRM_PF_TTP    = 0x2417; // Table Terminating Paragraph
static constexpr uint16_t SPRM_PF_INTBL  = 0x2416;
static constexpr size_t PAPX_BX_SIZE = 13; // Word 97+: 1 byte offset + 12 bytes PHE

// FC range for table row-end paragraphs (TTP).
struct TtpFcEntry { uint32_t fc_start, fc_end; };

// FC range for paragraphs inside a table (sprmPFInTable=1).
struct InTableFcEntry { uint32_t fc_start, fc_end; };

struct PapxResult {
    std::vector<ListFcEntry> list_entries;
    std::vector<TtpFcEntry> ttp_entries;
    std::vector<InTableFcEntry> intable_entries;
};

// Check if fc is inside a table paragraph.
static bool is_intable_fc(const std::vector<InTableFcEntry>& entries, uint32_t fc) {
    auto it = std::upper_bound(entries.begin(), entries.end(), fc,
        [](uint32_t f, const InTableFcEntry& e) { return f < e.fc_start; });
    if (it == entries.begin()) return false;
    --it;
    return fc < it->fc_end;
}

// Check if fc falls within any TTP range (table row-end paragraph).
static bool is_ttp_fc(const std::vector<TtpFcEntry>& entries, uint32_t fc) {
    auto it = std::upper_bound(entries.begin(), entries.end(), fc,
        [](uint32_t f, const TtpFcEntry& e) { return f < e.fc_start; });
    if (it == entries.begin()) return false;
    --it;
    return fc < it->fc_end;
}

// Check if a TTP entry exists ahead of fc (within +range FCs).
// If the current row has a TTP mark coming soon, we're in a TTP table.
static bool has_ttp_ahead(const std::vector<TtpFcEntry>& entries, uint32_t fc,
                          uint32_t range = 400) {
    auto it = std::lower_bound(entries.begin(), entries.end(), fc,
        [](const TtpFcEntry& e, uint32_t f) { return e.fc_start < f; });
    return it != entries.end() && it->fc_start < fc + range;
}

static PapxResult build_papx_info(
    const std::vector<char>& word_doc,
    const std::vector<char>& table_stream) {

    PapxResult result;
    if (word_doc.size() < 0x010A || table_stream.empty()) return result;

    uint32_t fc_plcf = util::read_u32_le(word_doc.data() + 0x0102);
    uint32_t lcb_plcf = util::read_u32_le(word_doc.data() + 0x0106);
    if (fc_plcf == 0 || lcb_plcf < 12) return result;
    if (static_cast<size_t>(fc_plcf) + lcb_plcf > table_stream.size()) return result;

    uint32_t n = (lcb_plcf - 4) / 8;
    if (n == 0 || n > 0xFFFF) return result;

    const char* plcf = table_stream.data() + fc_plcf;
    const char* pns = plcf + (n + 1) * 4;

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t pn = util::read_u32_le(pns + i * 4) & 0x003FFFFF;
        size_t page_off = static_cast<size_t>(pn) * CHPX_PAGE_SIZE;
        if (page_off + CHPX_PAGE_SIZE > word_doc.size()) continue;

        const char* page = word_doc.data() + page_off;
        uint8_t crun = static_cast<uint8_t>(page[CHPX_PAGE_SIZE - 1]);
        if (crun == 0) continue;

        size_t rgfc_end = static_cast<size_t>(crun + 1) * 4;
        size_t bx_off = rgfc_end;

        for (uint8_t r = 0; r < crun; ++r) {
            if (bx_off + r * PAPX_BX_SIZE >= CHPX_PAGE_SIZE - 1) break;
            uint8_t papx_word_off = static_cast<uint8_t>(page[bx_off + r * PAPX_BX_SIZE]);
            if (papx_word_off == 0) continue;

            size_t papx_pos = static_cast<size_t>(papx_word_off) * 2;
            if (papx_pos >= CHPX_PAGE_SIZE - 1) continue;

            uint8_t cb = static_cast<uint8_t>(page[papx_pos]);
            const char* grpprl;
            size_t grpprl_len;
            if (cb == 0) {
                if (papx_pos + 1 >= CHPX_PAGE_SIZE) continue;
                cb = static_cast<uint8_t>(page[papx_pos + 1]);
                grpprl_len = static_cast<size_t>(cb) * 2;
                grpprl = page + papx_pos + 2;
                if (papx_pos + 2 + grpprl_len > CHPX_PAGE_SIZE) continue;
            } else {
                grpprl_len = static_cast<size_t>(cb) * 2 - 1;
                grpprl = page + papx_pos + 1;
                if (papx_pos + 1 + grpprl_len > CHPX_PAGE_SIZE) continue;
            }

            if (grpprl_len < 4) continue;
            const char* sprms = grpprl + 2;
            size_t sprm_len = grpprl_len - 2;

            uint16_t ilfo = 0;
            uint8_t ilvl = 0;
            bool is_ttp = false;
            bool in_table = false;
            size_t pos = 0;
            while (pos + 2 <= sprm_len) {
                uint16_t opcode = util::read_u16_le(sprms + pos);
                size_t sz = sprm_operand_size(opcode, sprms + pos + 2, sprm_len - pos - 2);
                if (sz == 0) break;
                if (opcode == SPRM_P_ILVL && pos + 3 <= sprm_len)
                    ilvl = static_cast<uint8_t>(sprms[pos + 2]);
                if (opcode == SPRM_P_ILFO && pos + 4 <= sprm_len)
                    ilfo = util::read_u16_le(sprms + pos + 2);
                if (opcode == SPRM_PF_TTP && pos + 3 <= sprm_len &&
                    sprms[pos + 2] != 0)
                    is_ttp = true;
                if (opcode == SPRM_PF_INTBL && pos + 3 <= sprm_len &&
                    sprms[pos + 2] != 0)
                    in_table = true;
                pos += 2 + sz;
            }

            uint32_t fc_start = util::read_u32_le(page + r * 4);
            uint32_t fc_end = util::read_u32_le(page + (r + 1) * 4);

            if (ilfo > 0)
                result.list_entries.push_back({fc_start, fc_end, ilfo, ilvl});
            if (is_ttp)
                result.ttp_entries.push_back({fc_start, fc_end});
            if (in_table)
                result.intable_entries.push_back({fc_start, fc_end});
        }
    }

    auto by_fc = [](auto& a, auto& b) { return a.fc_start < b.fc_start; };
    std::sort(result.list_entries.begin(), result.list_entries.end(), by_fc);
    std::sort(result.ttp_entries.begin(), result.ttp_entries.end(), by_fc);
    std::sort(result.intable_entries.begin(), result.intable_entries.end(), by_fc);
    return result;
}

// ── Main text extraction dispatcher ─────────────────────────────

std::string DocParser::extract_text() {
    std::vector<char> word_doc = ole_.read_stream("WordDocument");
    if (word_doc.size() < 0x20) return "";

    // Read FIB header: wIdent (magic) and nFib (version).
    uint16_t w_ident = util::read_u16_le(word_doc.data());
    uint16_t n_fib = util::read_u16_le(word_doc.data() + 2);

    // Validate magic: 0xA5EC for Word, 0xA5DC for glossary docs.
    if (w_ident != 0xA5EC && w_ident != 0xA5DC) return "";

    // Check encryption (bit 0 of flags at offset 0x0A, bit 8 = fEncrypted).
    if (word_doc.size() >= 0x0C) {
        uint16_t flags = util::read_u16_le(word_doc.data() + 0x0A);
        bool encrypted = (flags & 0x0100) != 0;
        if (encrypted) {
            throw std::runtime_error("Encrypted DOC files are not supported");
        }
    }

    // Read language ID for DBCS detection (offset 0x06).
    if (word_doc.size() >= 0x08) {
        lid_ = util::read_u16_le(word_doc.data() + 0x06);
    }

    // Determine Word version from nFib.
    if (n_fib >= 0x00C1) {        // Word 97+ (nFib = 0x00C1 for Word 97)
        version_ = WordVersion::WORD8_PLUS;
    } else if (n_fib >= 0x0067) { // Word 7 (Word 95)
        version_ = WordVersion::WORD7;
    } else if (n_fib >= 0x0065) { // Word 6
        version_ = WordVersion::WORD6;
    } else {
        return ""; // Too old or unrecognized
    }

    // Reset field state for extraction.
    field_depth_ = 0;
    field_show_result_ = false;

    // Select which table stream to use.
    uint16_t flags = util::read_u16_le(word_doc.data() + 0x0A);
    bool use_1table = (flags >> 9) & 1;
    std::string table_name = use_1table ? "1Table" : "0Table";
    std::vector<char> table_stream = ole_.read_stream(table_name);

    if (version_ == WordVersion::WORD6 || version_ == WordVersion::WORD7) {
        return extract_text_word6(word_doc, table_stream);
    }
    return extract_text_word8(word_doc, table_stream);
}

// ── Word 6/95 text extraction ───────────────────────────────────

std::string DocParser::extract_text_word6(const std::vector<char>& word_doc,
                                           const std::vector<char>& table_stream) {
    // Word 6/95 FIB: fcClx at offset 0x01A2 is the same as Word 97 in
    // many cases, but if that fails, try the Word 6 layout where the
    // CLX offset is at different positions in the FIB.

    // For Word 6/95, the document text is often stored contiguously
    // starting at FIB offset ccpText. Extract directly from WordDocument.

    if (word_doc.size() < 0x60) return "";

    // ccpText: character count of main text (offset 0x4C for Word 6/95)
    uint32_t ccp_text = 0;
    if (word_doc.size() >= 0x50) {
        ccp_text = util::read_u32_le(word_doc.data() + 0x4C);
    }
    if (ccp_text == 0 || ccp_text > 0x1000000) return "";

    // fcMin: file offset where text begins (offset 0x18)
    uint32_t fc_min = 0;
    if (word_doc.size() >= 0x1C) {
        fc_min = util::read_u32_le(word_doc.data() + 0x18);
    }
    if (fc_min == 0) fc_min = 0x600; // typical default

    // Word 6/95 text is single-byte (CP1252 or DBCS depending on language).
    std::string result;
    result.reserve(ccp_text);

    // Detect if this is a DBCS document.
    bool is_korean = (lid_ == 0x0412);
    bool is_japanese = (lid_ == 0x0411);

    size_t end = std::min(static_cast<size_t>(fc_min + ccp_text), word_doc.size());
    size_t pos = fc_min;

    while (pos < end) {
        uint8_t ch = static_cast<uint8_t>(word_doc[pos]);

        // DBCS: check for lead byte.
        if (is_korean && util::is_cp949_lead(ch) && pos + 1 < end) {
            uint8_t trail = static_cast<uint8_t>(word_doc[pos + 1]);
            result += util::cp949_to_utf8(ch, trail);
            pos += 2;
            continue;
        }
        if (is_japanese && util::is_cp932_lead(ch) && pos + 1 < end) {
            uint8_t trail = static_cast<uint8_t>(word_doc[pos + 1]);
            result += util::cp932_to_utf8(ch, trail);
            pos += 2;
            continue;
        }

        // Single-byte: process through common handler.
        process_char(ch, result);
        pos++;
    }

    // If that yielded nothing, try the piece table approach (same as Word 8).
    if (result.empty() && !table_stream.empty()) {
        return extract_text_word8(word_doc, table_stream);
    }

    return result;
}

// ── Word 97+ text extraction ────────────────────────────────────

std::string DocParser::extract_text_word8(const std::vector<char>& word_doc,
                                           const std::vector<char>& table_stream) {
    if (table_stream.empty()) return "";
    if (word_doc.size() < 0x60) return "";

    // Locate fcClx/lcbClx — the Clx (piece table) in the table stream.
    uint32_t fc_clx = 0, lcb_clx = 0;

    // Validate a candidate fcClx/lcbClx pair.
    auto valid_clx = [&](uint32_t fc, uint32_t lcb) -> bool {
        if (fc == 0 || lcb == 0) return false;
        if (static_cast<size_t>(fc) + lcb > table_stream.size()) return false;
        size_t base = fc;
        size_t p = 0;
        while (p < lcb && base + p < table_stream.size() &&
               static_cast<uint8_t>(table_stream[base + p]) == 0x01) {
            if (p + 3 > lcb || base + p + 3 > table_stream.size()) return false;
            uint16_t prc_len = util::read_u16_le(table_stream.data() + base + p + 1);
            p += 3 + prc_len;
        }
        return p < lcb && base + p < table_stream.size() &&
               static_cast<uint8_t>(table_stream[base + p]) == 0x02;
    };

    // Method 1: absolute FIB offset 0x01A2 (MS-DOC §2.5.6, Word 97+).
    if (0x01A2 + 8 <= word_doc.size()) {
        uint32_t fc  = util::read_u32_le(word_doc.data() + 0x01A2);
        uint32_t lcb = util::read_u32_le(word_doc.data() + 0x01A2 + 4);
        if (valid_clx(fc, lcb)) { fc_clx = fc; lcb_clx = lcb; }
    }

    // Method 2: navigate FIB variable-length sections.
    if (fc_clx == 0 && word_doc.size() >= 0x22) {
        uint16_t csw = util::read_u16_le(word_doc.data() + 0x20);
        size_t pos = 0x22 + csw * 2;
        if (pos + 2 <= word_doc.size()) {
            uint16_t cslw = util::read_u16_le(word_doc.data() + pos);
            pos += 2 + cslw * 4;
        }
        if (pos + 2 <= word_doc.size()) {
            uint16_t cb_rg = util::read_u16_le(word_doc.data() + pos);
            pos += 2;
            for (int idx : {65, 66}) {
                size_t off = pos + idx * 8;
                if (off + 8 > word_doc.size() || idx >= cb_rg) continue;
                uint32_t fc  = util::read_u32_le(word_doc.data() + off);
                uint32_t lcb = util::read_u32_le(word_doc.data() + off + 4);
                if (valid_clx(fc, lcb)) { fc_clx = fc; lcb_clx = lcb; break; }
            }
        }
    }

    if (fc_clx == 0 || lcb_clx == 0) return "";

    // Parse Clx structure.
    const char* clx = table_stream.data() + fc_clx;
    size_t clx_end = lcb_clx;
    size_t clx_pos = 0;

    // Skip Prc entries (type 0x01).
    while (clx_pos < clx_end && static_cast<uint8_t>(clx[clx_pos]) == 0x01) {
        if (clx_pos + 3 > clx_end) return "";
        uint16_t prc_len = util::read_u16_le(clx + clx_pos + 1);
        clx_pos += 3 + prc_len;
    }

    // Must be Pcdt (type 0x02).
    if (clx_pos >= clx_end || static_cast<uint8_t>(clx[clx_pos]) != 0x02) return "";
    clx_pos++;

    if (clx_pos + 4 > clx_end) return "";
    uint32_t lcb = util::read_u32_le(clx + clx_pos);
    clx_pos += 4;

    // PlcPcd: (n+1) CPs + n PCDs.
    if (lcb < 4) return "";
    uint32_t n = (lcb - 4) / 12;
    if (n == 0 || n > 0x31FFF) return ""; // safety limit

    const char* plcpcd = clx + clx_pos;
    size_t plcpcd_size = (n + 1) * 4 + n * 8;
    if (clx_pos + plcpcd_size > clx_end) return "";

    // Read CPs.
    std::vector<uint32_t> cps(n + 1);
    for (uint32_t i = 0; i <= n; ++i) {
        cps[i] = util::read_u32_le(plcpcd + i * 4);
    }

    const char* pcds = plcpcd + (n + 1) * 4;
    std::string result;

    bool is_korean = (lid_ == 0x0412);
    bool is_japanese = (lid_ == 0x0411);

    std::vector<char> data_stream;
    if (ole_.has_stream("Data"))
        data_stream = ole_.read_stream("Data");
    auto pic_entries = build_picture_fc_entries(word_doc, table_stream, data_stream);
    auto papx = build_papx_info(word_doc, table_stream);
    auto bold_entries = build_bold_fc_entries(word_doc, table_stream);
    ListCounter list_counter;
    data_stream.clear();
    data_stream.shrink_to_fit();

    // Insert list numbering prefix at paragraph start when PAPX has ilfo > 0.
    auto inject_list_prefix = [&](uint32_t fc) {
        const auto* le = find_list_entry(papx.list_entries, fc);
        if (!le) return;
        // Top-level section heading: insert separator.
        if (le->ilvl == 0 && result.size() > 10) {
            if (!result.empty() && result.back() != '\n')
                result.push_back('\n');
            result.append("---\n");
        }
        result += list_counter.next_prefix(le->ilfo, le->ilvl, lid_);
    };

    bool at_para_start = true; // first paragraph
    bool in_bold = false;

    // Helper: update bold state, emitting ** markers at transitions.
    auto update_bold = [&](uint32_t fc) {
        bool bold_now = is_bold_fc(bold_entries, fc);
        if (bold_now != in_bold) {
            result.append("**");
            in_bold = bold_now;
        }
    };

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t char_count = cps[i + 1] - cps[i];
        if (char_count == 0 || char_count > 0x1000000) continue;

        // PCD: 2 bytes (unused) + 4 bytes (fc) + 2 bytes (prm).
        uint32_t fc_raw = util::read_u32_le(pcds + i * 8 + 2);
        bool compressed = (fc_raw & 0x40000000) != 0;
        uint32_t fc_base = fc_raw & 0x3FFFFFFF;

        if (compressed) {
            // CP1252 or DBCS, 1 byte per CP position.
            uint32_t byte_offset = fc_base / 2;
            if (static_cast<size_t>(byte_offset) + char_count > word_doc.size()) continue;

            size_t pos = byte_offset;
            size_t end = byte_offset + char_count;
            uint32_t local_off = 0;

            while (pos < end) {
                uint8_t ch = static_cast<uint8_t>(word_doc[pos]);

                if (at_para_start && ch >= 0x20) {
                    inject_list_prefix(fc_base + local_off);
                    at_para_start = false;
                }

                // DBCS lead byte check.
                if (is_korean && util::is_cp949_lead(ch) && pos + 1 < end) {
                    update_bold(fc_base + local_off);
                    uint8_t trail = static_cast<uint8_t>(word_doc[pos + 1]);
                    result += util::cp949_to_utf8(ch, trail);
                    pos += 2;
                    local_off += 2;
                    continue;
                }
                if (is_japanese && util::is_cp932_lead(ch) && pos + 1 < end) {
                    update_bold(fc_base + local_off);
                    uint8_t trail = static_cast<uint8_t>(word_doc[pos + 1]);
                    result += util::cp932_to_utf8(ch, trail);
                    pos += 2;
                    local_off += 2;
                    continue;
                }

                if (ch == 0x0D || ch == 0x0A || ch == 0x07) {
                    if (in_bold) { result.append("**"); in_bold = false; }
                    at_para_start = true;
                }

                // Cell mark: TTP = row end, else = cell separator.
                if (ch == 0x07) {
                    uint32_t fc = fc_base + local_off;
                    if (is_ttp_fc(papx.ttp_entries, fc)) {
                        result.push_back('\n');
                    } else if (!result.empty() && result.back() == '\x1F' &&
                               !has_ttp_ahead(papx.ttp_entries, fc)) {
                        result.back() = '\n';
                    } else {
                        result.push_back('\x1F');
                    }
                    pos++;
                    local_off++;
                    continue;
                }

                // Paragraph mark inside a table cell → space (don't break the row).
                if ((ch == 0x0D || ch == 0x0A) &&
                    is_intable_fc(papx.intable_entries, fc_base + local_off)) {
                    result.push_back(' ');
                    pos++;
                    local_off++;
                    continue;
                }

                bool is_pic = (ch == 0x01) &&
                    is_picture_fc(pic_entries, fc_base + local_off);
                if (ch >= 0x20 && !is_pic)
                    update_bold(fc_base + local_off);
                process_char(ch, result, is_pic);
                pos++;
                local_off++;
            }
        } else {
            // UTF-16LE, 2 bytes per character.
            size_t byte_len = static_cast<size_t>(char_count) * 2;
            if (static_cast<size_t>(fc_base) + byte_len > word_doc.size()) continue;
            const char* src = word_doc.data() + fc_base;

            for (uint32_t j = 0; j < char_count; ++j) {
                uint16_t ch = util::read_u16_le(src + j * 2);

                if (at_para_start && ch >= 0x20) {
                    inject_list_prefix(fc_base + j * 2);
                    at_para_start = false;
                }

                // Handle surrogate pairs.
                if (ch >= 0xD800 && ch <= 0xDBFF && j + 1 < char_count) {
                    uint16_t low = util::read_u16_le(src + (j + 1) * 2);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        update_bold(fc_base + j * 2);
                        uint32_t cp = 0x10000
                            + ((static_cast<uint32_t>(ch - 0xD800) << 10)
                               | (low - 0xDC00));
                        process_char(cp, result);
                        j++;
                        continue;
                    }
                }

                if (ch == 0x0D || ch == 0x0A || ch == 0x07) {
                    if (in_bold) { result.append("**"); in_bold = false; }
                    at_para_start = true;
                }
                if (!list_counter.has_korean && ch >= 0xAC00 && ch <= 0xD7A3)
                    list_counter.has_korean = true;

                if (ch == 0x07) {
                    uint32_t fc = fc_base + j * 2;
                    if (is_ttp_fc(papx.ttp_entries, fc)) {
                        result.push_back('\n');
                    } else if (!result.empty() && result.back() == '\x1F' &&
                               !has_ttp_ahead(papx.ttp_entries, fc)) {
                        result.back() = '\n';
                    } else {
                        result.push_back('\x1F');
                    }
                    continue;
                }

                // Paragraph mark inside a table cell → space.
                if ((ch == 0x0D || ch == 0x0A) &&
                    is_intable_fc(papx.intable_entries, fc_base + j * 2)) {
                    result.push_back(' ');
                    continue;
                }

                bool is_pic = (ch == 0x01) &&
                    is_picture_fc(pic_entries, fc_base + j * 2);
                if (ch >= 0x20 && !is_pic)
                    update_bold(fc_base + j * 2);
                process_char(ch, result, is_pic);
            }
        }
    }

    if (in_bold) result.append("**");
    return result;
}

// ── Text to Markdown ────────────────────────────────────────────

std::string DocParser::text_to_markdown(const std::string& raw_text) {
    std::vector<std::string> lines;
    std::istringstream iss(raw_text);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0'))
            line.pop_back();
        lines.push_back(line);
    }

    // Helper: count cell separators (\x1F from 0x07 cell marks).
    // Subtract one if the line ends with \x1F (trailing empty cell from row-end mark).
    auto count_cells = [](const std::string& s) {
        int n = 0;
        for (char c : s) if (c == '\x1F') n++;
        if (n > 0 && !s.empty() && s.back() == '\x1F') n--;
        return n;
    };

    // Helper: convert cell-separated line to markdown table row, pad to target_cols.
    auto cells_to_row = [](const std::string& s, int target_cols) {
        std::vector<std::string> cells;
        std::string cell;
        for (char c : s) {
            if (c == '\x1F') {
                cells.push_back(cell);
                cell.clear();
            } else {
                cell += c;
            }
        }
        if (!cell.empty() || cells.empty())
            cells.push_back(cell);

        // Strip one trailing empty cell (from last cell-end 0x07 before row-end).
        if (cells.size() >= 2 && cells.back().empty())
            cells.pop_back();

        // Pad to target column count
        while (static_cast<int>(cells.size()) < target_cols)
            cells.emplace_back();

        std::string row = "|";
        for (auto& c : cells) {
            for (auto& ch : c) { if (ch == '|') ch = '/'; if (ch == '\n') ch = ' '; }
            row += " " + c + " |";
        }
        return row;
    };

    std::string md;
    md.reserve(raw_text.size() * 2);
    int consecutive_empty = 0;

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& ln = lines[i];

        // Limit consecutive empty lines to 1
        if (ln.empty()) {
            consecutive_empty++;
            if (consecutive_empty <= 1) md.push_back('\n');
            continue;
        }
        consecutive_empty = 0;

        // Table detection: 1+ cell separator (\x1F) from DOC cell marks
        int cells = count_cells(ln);
        if (cells >= 1) {
            // Check if this line has actual content (not just tabs/spaces)
            bool has_content = false;
            for (char c : ln) {
                if (c != '\t' && c != ' ') { has_content = true; break; }
            }
            if (!has_content) {
                // Skip blank table rows
                md.push_back('\n');
                continue;
            }

            // Collect table rows. Merge cell-less lines with the next cell line
            // (handles merged-cell splits where a row's first cell is isolated).
            int max_cols = cells + 1;
            std::vector<std::string> merged_rows;
            std::string pending_prefix;
            while (i < lines.size()) {
                int cc = count_cells(lines[i]);
                if (cc >= 1) {
                    std::string row = lines[i];
                    if (!pending_prefix.empty()) {
                        row = pending_prefix + "\x1F" + row;
                        pending_prefix.clear();
                    }
                    max_cols = std::max(max_cols, count_cells(row) + 1);
                    bool row_content = false;
                    for (char c : row) {
                        if (c != '\x1F' && c != ' ') { row_content = true; break; }
                    }
                    if (row_content)
                        merged_rows.push_back(row);
                    i++;
                } else if (!lines[i].empty() && !merged_rows.empty()) {
                    // Non-cell line after a table row: merge only if it looks
                    // like a split merged-cell fragment (short, digit-only).
                    // Longer lines are likely section titles — break the table.
                    bool is_short_fragment = (lines[i].size() <= 4);
                    if (is_short_fragment &&
                        i + 1 < lines.size() && count_cells(lines[i + 1]) >= 1) {
                        pending_prefix = lines[i];
                        i++;
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }
            if (!pending_prefix.empty()) {
                // Leftover prefix with no cell line following: treat as text
                merged_rows.push_back(pending_prefix);
            }
            if (!merged_rows.empty()) {
                for (size_t ri = 0; ri < merged_rows.size(); ri++) {
                    md += cells_to_row(merged_rows[ri], max_cols) + "\n";
                    if (ri == 0) {
                        md += "|";
                        for (int c = 0; c < max_cols; c++) md += " --- |";
                        md += "\n";
                    }
                }
                md += "\n";
            }
            i--;
            continue;
        }

        // Bullet detection
        bool is_bullet = false;
        std::string content = ln;
        char first = ln[0];
        if (first == '-' || first == '*') {
            if (ln.size() > 1 && (ln[1] == ' ' || ln[1] == '\t')) {
                is_bullet = true;
                content = ln.substr(2);
            }
        } else if (ln.size() >= 3 &&
                   static_cast<uint8_t>(ln[0]) == 0xE2 &&
                   static_cast<uint8_t>(ln[1]) == 0x80 &&
                   static_cast<uint8_t>(ln[2]) == 0xA2) {
            is_bullet = true;
            content = ln.substr(3);
            while (!content.empty() && content[0] == ' ') content = content.substr(1);
        }
        if (is_bullet) {
            md += "- " + content + "\n";
            continue;
        }

        // Ordered list: only match simple "N." prefix (not "N.N." multi-level).
        {
            size_t j = 0;
            while (j < ln.size() && ln[j] >= '0' && ln[j] <= '9') ++j;
            if (j > 0 && j < ln.size() && ln[j] == '.') {
                // Skip if next char is also a digit (multi-level like "1.1.")
                bool multi_level = (j + 1 < ln.size() && ln[j + 1] >= '0' && ln[j + 1] <= '9');
                if (!multi_level) {
                    std::string prefix = ln.substr(0, j + 1);
                    std::string rest = (j + 1 < ln.size()) ? ln.substr(j + 1) : "";
                    while (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
                    md += prefix + " " + rest + "\n";
                    continue;
                }
            }
        }

        // Heading heuristic: short line (≤80 chars), not a list, surrounded by
        // empty lines or at document start, likely a heading/title.
        bool prev_empty = (i == 0) || (i > 0 && lines[i-1].empty());
        bool next_empty = (i + 1 >= lines.size()) || lines[i + 1].empty();
        bool is_short = (ln.size() <= 80);
        bool no_period = (ln.back() != '.' && ln.back() != ';' && ln.back() != ',');

        bool has_nonws = false;
        for (char c : ln) { if (c != ' ' && c != '\t') { has_nonws = true; break; } }
        if (prev_empty && next_empty && is_short && no_period && ln.size() > 1 && has_nonws) {
            // Looks like a heading — use ## (H2) for general section titles
            md += "## " + ln + "\n";
            continue;
        }

        md += ln + "\n";
    }

    return md;
}

// ── Image detection helpers ─────────────────────────────────────

std::string DocParser::detect_image_format(const char* data, size_t len) {
    if (len < 8) return "";
    const uint8_t* d = reinterpret_cast<const uint8_t*>(data);

    if (d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "jpeg";
    if (d[0] == 0x89 && d[1] == 0x50 && d[2] == 0x4E && d[3] == 0x47 &&
        d[4] == 0x0D && d[5] == 0x0A && d[6] == 0x1A && d[7] == 0x0A) return "png";
    if (d[0] == 0x47 && d[1] == 0x49 && d[2] == 0x46 && d[3] == 0x38) return "gif";
    if (d[0] == 0x42 && d[1] == 0x4D) return "bmp";
    if (d[0] == 0x49 && d[1] == 0x49 && d[2] == 0x2A && d[3] == 0x00) return "tiff";
    if (d[0] == 0x4D && d[1] == 0x4D && d[2] == 0x00 && d[3] == 0x2A) return "tiff";
    if (len >= 4 && d[0] == 0xD7 && d[1] == 0xCD && d[2] == 0xC6 && d[3] == 0x9A) return "wmf";
    if (len >= 44 && d[0] == 0x01 && d[1] == 0x00 && d[2] == 0x00 && d[3] == 0x00) {
        if (d[40] == ' ' && d[41] == 'E' && d[42] == 'M' && d[43] == 'F') return "emf";
    }

    return "";
}

size_t DocParser::find_jpeg_end(const char* data, size_t len) {
    for (size_t i = 2; i + 1 < len; ++i) {
        if (static_cast<uint8_t>(data[i]) == 0xFF &&
            static_cast<uint8_t>(data[i + 1]) == 0xD9) {
            return i + 2;
        }
    }
    return len;
}

size_t DocParser::find_png_end(const char* data, size_t len) {
    const uint8_t iend_sig[4] = {0x49, 0x45, 0x4E, 0x44};
    for (size_t i = 8; i + 7 < len; ++i) {
        if (std::memcmp(data + i + 4, iend_sig, 4) == 0) {
            return i + 12;
        }
    }
    return len;
}

// ── Image extraction ────────────────────────────────────────────

std::vector<ImageData> DocParser::extract_images(unsigned min_image_size) {
    std::vector<ImageData> images;

    // Try "Data" stream first, then fall back to "WordDocument".
    std::vector<char> stream;
    if (ole_.has_stream("Data")) {
        stream = ole_.read_stream("Data");
    }
    if (stream.empty()) {
        stream = ole_.read_stream("WordDocument");
    }
    if (stream.empty()) return images;

    int img_idx = 0;
    size_t pos = 0;
    while (pos + 8 < stream.size()) {
        std::string fmt = detect_image_format(stream.data() + pos, stream.size() - pos);
        if (fmt.empty()) {
            ++pos;
            continue;
        }

        size_t img_start = pos;
        size_t img_end = stream.size();

        if (fmt == "jpeg") {
            img_end = img_start + find_jpeg_end(stream.data() + img_start, stream.size() - img_start);
        } else if (fmt == "png") {
            img_end = img_start + find_png_end(stream.data() + img_start, stream.size() - img_start);
        } else {
            size_t max_size = std::min(stream.size() - img_start, size_t(10 * 1024 * 1024));
            img_end = img_start + max_size;
        }

        size_t img_size = img_end - img_start;
        if (img_size > 16) {
            ImageData img;
            img.page_number = 1;
            img.name = "page1_img" + std::to_string(img_idx++);
            img.format = fmt;
            img.data.assign(stream.begin() + img_start, stream.begin() + img_end);
            util::populate_image_dimensions(img);
            if (util::is_image_too_small(img, min_image_size)) {
                --img_idx;
            } else {
                images.push_back(std::move(img));
            }
        }

        pos = img_end;
    }

    // Parse OfficeArt records in the "Data" stream for BLIP images.
    // Handles both standalone BLIPs and BLIPs inline within FBSE records.
    if (ole_.has_stream("Data")) {
        const auto& data = ole_.read_stream("Data");

        auto extract_blip = [&](size_t blip_pos, size_t blip_end) {
            if (blip_pos + 8 > blip_end) return;
            uint16_t bvi = util::read_u16_le(data.data() + blip_pos);
            uint16_t btype = util::read_u16_le(data.data() + blip_pos + 2);
            uint32_t blen = util::read_u32_le(data.data() + blip_pos + 4);
            uint16_t binst = bvi >> 4;

            std::string fmt;
            if (btype == 0xF01D) fmt = "jpeg";
            else if (btype == 0xF01E) fmt = "png";
            else if (btype == 0xF01A) fmt = "emf";
            else if (btype == 0xF01B) fmt = "wmf";
            else if (btype == 0xF01F) fmt = "bmp";
            else if (btype == 0xF029) fmt = "tiff";
            if (fmt.empty()) return;

            // BLIP header: 8 (record) + 16 (UID) + 1 (tag) = 25 bytes
            // Two-UID variant: 8 + 32 + 1 = 41 bytes
            size_t skip = 25;
            if (binst == 0x46B || binst == 0x6E1 || binst == 0x6E3 || binst == 0x6E5)
                skip = 41;

            size_t img_start = blip_pos + skip;
            size_t img_end = std::min(blip_pos + 8 + blen, blip_end);
            if (img_start >= img_end) return;
            size_t img_size = img_end - img_start;

            // Deduplicate by first 64 bytes
            for (const auto& existing : images) {
                if (existing.data.size() == img_size) {
                    if (std::memcmp(existing.data.data(), data.data() + img_start,
                                    std::min(size_t(64), img_size)) == 0)
                        return;
                }
            }

            ImageData img;
            img.page_number = 1;
            img.name = "page1_img" + std::to_string(img_idx++);
            img.format = fmt;
            img.data.assign(data.begin() + img_start, data.begin() + img_start + img_size);
            util::populate_image_dimensions(img);
            if (util::is_image_too_small(img, min_image_size)) {
                --img_idx;
                return;
            }
            images.push_back(std::move(img));
        };

        pos = 0;
        while (pos + 8 < data.size()) {
            uint16_t rvi = util::read_u16_le(data.data() + pos);
            uint16_t rtype = util::read_u16_le(data.data() + pos + 2);
            uint32_t rlen = util::read_u32_le(data.data() + pos + 4);

            if (rtype >= 0xF000 && rtype <= 0xF200 && pos + 8 + rlen <= data.size()) {
                if (rtype == 0xF000 || rtype == 0xF001) {
                    // Container — descend into children
                    pos += 8;
                    continue;
                }
                if (rtype == 0xF007 && rlen >= 36) {
                    // FBSE — may contain inline BLIP after 36-byte header
                    uint8_t cbName = data[pos + 8 + 33];
                    size_t blip_start = pos + 8 + 36 + cbName;
                    size_t fbse_end = pos + 8 + rlen;
                    if (blip_start + 8 < fbse_end) {
                        extract_blip(blip_start, fbse_end);
                    }
                } else if (rtype >= 0xF01A && rtype <= 0xF029) {
                    // Standalone BLIP
                    extract_blip(pos, pos + 8 + rlen);
                }
                pos += 8 + rlen;
            } else {
                ++pos;
            }
        }
    }

    return images;
}

// ── Public API ──────────────────────────────────────────────────

std::string DocParser::to_markdown(const ConvertOptions& opts) {
    page_num_ = 1;
    std::string raw = extract_text();
    std::string md = "--- Page 1 ---\n\n" + text_to_markdown(raw);

    auto images = extract_images(opts.min_image_size);

    if (opts.extract_images) {
        for (auto& img : images) {
            img.saved_path = util::save_image_to_file(
                opts.image_output_dir, img.name, img.format,
                img.data.data(), img.data.size());
            if (!img.saved_path.empty()) {
                img.data.clear();
                img.data.shrink_to_fit();
            }
        }
    }

    return replace_image_markers(md, images, opts.image_ref_prefix);
}

std::vector<PageChunk> DocParser::to_chunks(const ConvertOptions& opts) {
    std::vector<PageChunk> chunks;

    PageChunk chunk;
    chunk.page_number = 1;

    std::string raw = extract_text();
    chunk.text = strip_image_markers(text_to_markdown(raw));

    if (opts.extract_images) {
        chunk.images = extract_images(opts.min_image_size);
    }

    chunk.page_width = 612.0;
    chunk.page_height = 792.0;
    chunk.body_font_size = 12.0;

    chunks.push_back(std::move(chunk));
    return chunks;
}

} // namespace jdoc
