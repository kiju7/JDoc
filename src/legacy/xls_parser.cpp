// .xls (BIFF8) parser implementation.

#include "xls_parser.h"
#include "common/string_utils.h"
#include "common/binary_utils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace jdoc {

// BIFF8 record types.
static constexpr uint16_t RT_BOF        = 0x0809;
static constexpr uint16_t RT_EOF        = 0x000A;
static constexpr uint16_t RT_BOUNDSHEET = 0x0085;
static constexpr uint16_t RT_SST        = 0x00FC;
static constexpr uint16_t RT_CONTINUE   = 0x003C;
static constexpr uint16_t RT_LABELSST   = 0x00FD;
static constexpr uint16_t RT_NUMBER     = 0x0203;
static constexpr uint16_t RT_RK         = 0x027E;
static constexpr uint16_t RT_MULRK      = 0x00BD;
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
        // Signed 30-bit integer (arithmetic shift right by 2).
        int32_t ival = static_cast<int32_t>(rk) >> 2;
        val = static_cast<double>(ival);
    } else {
        // IEEE 754 double with lower 32 bits masked.
        uint64_t bits = static_cast<uint64_t>(rk & 0xFFFFFFFC) << 32;
        std::memcpy(&val, &bits, 8);
    }

    if (div100) val /= 100.0;
    return val;
}

std::string XlsParser::format_number(double val) {
    // Check if it's effectively an integer.
    if (std::abs(val - std::round(val)) < 1e-9 && std::abs(val) < 1e15) {
        long long ival = static_cast<long long>(std::round(val));
        return std::to_string(ival);
    }
    std::ostringstream oss;
    oss << val;
    return oss.str();
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

    // uint32_t total_strings = rd32(data);     // not needed
    uint32_t unique_count  = rd32(data + 4);

    // Concatenate the SST record data with all CONTINUE records to form one buffer.
    // This simplifies string parsing that may span record boundaries.
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

    // First pass: collect BoundSheet names, SST, etc.
    std::vector<std::string> sheet_names;
    int bof_count = 0;
    bool in_globals = false;
    int current_sheet = -1;

    size_t pos = 0;
    uint16_t prev_type = 0;
    std::vector<std::vector<char>> continue_blocks;

    // We need two passes: first for globals (SST, BoundSheet), then for sheet data.
    // Pass 1: globals
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
            if (rec_type == RT_BOUNDSHEET && rec_size >= 8) {
                // offset 6: 1 byte name length, offset 7: 1 byte flags (0=compressed, 1=UTF-16LE)
                uint8_t name_len = static_cast<uint8_t>(rec_data[6]);
                uint8_t name_flags = static_cast<uint8_t>(rec_data[7]);
                std::string sname;
                if (name_flags & 0x01) {
                    // UTF-16LE
                    size_t byte_len = std::min(size_t(name_len) * 2, size_t(rec_size - 8));
                    sname = util::utf16le_to_utf8(rec_data + 8, byte_len);
                } else {
                    // Compressed (CP1252)
                    size_t byte_len = std::min(size_t(name_len), size_t(rec_size - 8));
                    for (size_t i = 0; i < byte_len; ++i) {
                        sname += util::cp1252_to_utf8(static_cast<uint8_t>(rec_data[8 + i]));
                    }
                }
                sheet_names.push_back(sname);
            }

            if (rec_type == RT_SST) {
                // Collect CONTINUE records that follow.
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

    // Pass 2: sheet cell data.
    pos = 0;
    bof_count = 0;
    current_sheet = -1;

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
        } else if (rec_type == RT_EOF) {
            if (current_sheet >= 0) {
                // Sheet ended.
            }
        }

        if (current_sheet >= 0 && current_sheet < static_cast<int>(sheets_.size())) {
            Sheet& sheet = sheets_[current_sheet];

            if (rec_type == RT_LABELSST && rec_size >= 10) {
                uint16_t row = rd16(rec_data);
                uint16_t col = rd16(rec_data + 2);
                // XF index at offset 4 (skip).
                uint32_t sst_idx = rd32(rec_data + 6);
                if (sst_idx < sst_.size()) {
                    sheet.cells.push_back({row, col, sst_[sst_idx]});
                }
            } else if (rec_type == RT_NUMBER && rec_size >= 14) {
                uint16_t row = rd16(rec_data);
                uint16_t col = rd16(rec_data + 2);
                double val = rd_double(rec_data + 6);
                sheet.cells.push_back({row, col, format_number(val)});
            } else if (rec_type == RT_RK && rec_size >= 10) {
                uint16_t row = rd16(rec_data);
                uint16_t col = rd16(rec_data + 2);
                uint32_t rk = rd32(rec_data + 6);
                double val = decode_rk(rk);
                sheet.cells.push_back({row, col, format_number(val)});
            } else if (rec_type == RT_MULRK && rec_size >= 6) {
                uint16_t row = rd16(rec_data);
                uint16_t first_col = rd16(rec_data + 2);
                // Last col is at the end of the record (2 bytes).
                uint16_t last_col = rd16(rec_data + rec_size - 2);
                // Between: (XF index (2 bytes) + RK value (4 bytes)) pairs.
                size_t pair_offset = 4;
                for (uint16_t c = first_col; c <= last_col && pair_offset + 6 <= rec_size - 2u; ++c) {
                    // XF index at pair_offset (2 bytes) -- skip.
                    uint32_t rk = rd32(rec_data + pair_offset + 2);
                    double val = decode_rk(rk);
                    sheet.cells.push_back({row, c, format_number(val)});
                    pair_offset += 6;
                }
            }
        }

        pos += rec_size;
    }
}

// ---------- sheet to markdown ------------------------------------------------

std::string XlsParser::sheet_to_markdown(const Sheet& sheet) {
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

    // Build grid.
    std::vector<std::vector<std::string>> grid(limit_row + 1,
                                                std::vector<std::string>(limit_col + 1));
    for (const auto& c : sheet.cells) {
        if (c.row <= limit_row && c.col <= limit_col) {
            grid[c.row][c.col] = c.value;
        }
    }

    // Find actual used range (skip fully empty rows/cols at the end).
    int used_rows = 0, used_cols = 0;
    for (int r = 0; r <= limit_row; ++r) {
        for (int c = 0; c <= limit_col; ++c) {
            if (!grid[r][c].empty()) {
                used_rows = std::max(used_rows, r + 1);
                used_cols = std::max(used_cols, c + 1);
            }
        }
    }
    if (used_rows == 0 || used_cols == 0) return "";

    // Render markdown table.
    std::ostringstream out;
    out << "### " << sheet.name << "\n\n";

    for (int r = 0; r < used_rows; ++r) {
        out << "|";
        for (int c = 0; c < used_cols; ++c) {
            // Escape pipe characters in cell values.
            std::string val = grid[r][c];
            for (size_t i = 0; i < val.size(); ++i) {
                if (val[i] == '|') val.replace(i, 1, "\\|");
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

std::vector<ImageData> XlsParser::extract_images() {
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
            // Append CONTINUE records.
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
            // Container: recurse into it (just advance past header).
            pos += 8;
            continue;
        }

        if (rec_type >= 0xF01A && rec_type <= 0xF029 && rec_len > 0) {
            // BLIP record.
            size_t blip_start = pos + 8;
            size_t blip_end = blip_start + rec_len;
            if (blip_end > drawing_data.size()) break;

            size_t header_skip = 17; // default: 1 byte + 16 bytes UID
            std::string fmt;

            switch (rec_type) {
                case 0xF01A: fmt = "emf"; header_skip = 50; break;
                case 0xF01B: fmt = "wmf"; header_skip = 50; break;
                case 0xF01C: header_skip = 50; break; // PICT
                case 0xF01D: fmt = "jpeg"; header_skip = 17; break;
                case 0xF01E: fmt = "png"; header_skip = 17; break;
                case 0xF01F: fmt = "bmp"; header_skip = 17; break;
                case 0xF029: fmt = "tiff"; header_skip = 17; break;
            }

            // Check for 2 UIDs.
            uint16_t inst = ver_inst >> 4;
            if (rec_type == 0xF01D && (inst == 0x46B || inst == 0x6E3)) header_skip = 33;
            if (rec_type == 0xF01E && (inst == 0x6E1 || inst == 0x6E5)) header_skip = 33;

            if (!fmt.empty() && rec_len > header_skip) {
                size_t img_offset = blip_start + header_skip;
                size_t img_size = rec_len - header_skip;
                if (img_offset + img_size <= drawing_data.size()) {
                    ImageData img;
                    img.page_number = 1;
                    img.name = "xls_image_" + std::to_string(++img_idx);
                    img.format = fmt;
                    img.data.assign(drawing_data.begin() + img_offset,
                                    drawing_data.begin() + img_offset + img_size);
                    images.push_back(std::move(img));
                }
            }

            pos = blip_end;
        } else {
            // Non-BLIP atom: skip.
            pos += 8 + rec_len;
        }
    }

    return images;
}

// ---------- public API -------------------------------------------------------

std::string XlsParser::to_markdown(const ConvertOptions& opts) {
    std::string md;
    for (const auto& sheet : sheets_) {
        md += sheet_to_markdown(sheet);
    }

    if (opts.extract_images) {
        auto images = extract_images();
        for (const auto& img : images) {
            md += "![" + img.name + "](" + img.name + "." + img.format + ")\n\n";
        }
    }

    return md;
}

std::vector<PageChunk> XlsParser::to_chunks(const ConvertOptions& opts) {
    std::vector<PageChunk> chunks;

    auto images = opts.extract_images ? extract_images() : std::vector<ImageData>{};
    int img_per_sheet = images.empty() ? 0 : static_cast<int>((images.size() + sheets_.size() - 1) / sheets_.size());

    for (size_t i = 0; i < sheets_.size(); ++i) {
        PageChunk chunk;
        chunk.page_number = static_cast<int>(i + 1);
        chunk.text = sheet_to_markdown(sheets_[i]);
        chunk.page_width = 612.0;
        chunk.page_height = 792.0;
        chunk.body_font_size = 10.0;

        // Build the tables data structure.
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

            // Trim empty rows/cols.
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

        // Distribute images to chunks (best-effort).
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
