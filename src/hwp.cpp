// hwp2md.cpp - HWP (binary) to Markdown converter
// Parses HWP 5.x OLE2 compound documents and extracts text, tables, images
// Reference: hwplib (Java) by neolord0
// License: MIT

#include "jdoc/hwp.h"
#include "jdoc/hwp_types.h"
#include "legacy/ole_reader.h"
#include "common/file_utils.h"
#include "common/string_utils.h"
#include <zlib.h>
#include <algorithm>
#include <map>
#include <sstream>
#include <fstream>
#include <iostream>
#include <cstring>
#include <memory>

namespace jdoc {

// ── Zlib decompression ──────────────────────────────────────

static std::vector<uint8_t> decompress(const uint8_t* data, size_t len) {
    // HWP uses raw deflate (no header)
    std::vector<uint8_t> output;
    output.resize(len * 4);  // initial guess

    z_stream strm = {};
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) return {};

    strm.next_in = const_cast<uint8_t*>(data);
    strm.avail_in = len;

    int ret;
    do {
        if (strm.total_out >= output.size()) {
            output.resize(output.size() * 2);
        }
        strm.next_out = output.data() + strm.total_out;
        strm.avail_out = output.size() - strm.total_out;
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR) {
            inflateEnd(&strm);
            return {};
        }
    } while (ret != Z_STREAM_END);

    output.resize(strm.total_out);
    output.shrink_to_fit();
    inflateEnd(&strm);
    return output;
}

// ── OLE stream reading helper ──────────────────────────────

static std::vector<uint8_t> read_ole_stream(OleReader& ole,
                                              const std::string& path) {
    auto data = ole.read_stream(path);
    if (data.empty()) return {};
    // Convert vector<char> to vector<uint8_t>
    return std::vector<uint8_t>(
        reinterpret_cast<const uint8_t*>(data.data()),
        reinterpret_cast<const uint8_t*>(data.data()) + data.size());
}

// ── HWP Document structures ────────────────────────────────

struct HWPBinDataRef {
    uint16_t type = 0;     // 0=LINK, 1=EMBEDDING, 2=STORAGE
    uint16_t bin_data_id = 0;
    std::string extension;
};

struct HWPDocInfo {
    int section_count = 1;
    std::vector<hwp::FaceNameInfo> face_names;
    std::vector<hwp::CharShapeInfo> char_shapes;
    std::vector<hwp::ParaShapeInfo> para_shapes;
    std::vector<HWPBinDataRef> bin_data_refs;
};

struct HWPParaCharShapeEntry {
    uint32_t position = 0;
    uint32_t char_shape_id = 0;
};

struct HWPTableCell {
    std::string text;
    int col_addr = 0;
    int row_addr = 0;
    int col_span = 1;
    int row_span = 1;
};

struct HWPTable {
    int row_count = 0;
    int col_count = 0;
    std::vector<HWPTableCell> cells;  // flat list, placed by col_addr/row_addr
};

struct HWPParagraph {
    uint16_t para_shape_id = 0;
    uint16_t style_id = 0;
    uint32_t char_count = 0;
    uint16_t control_mask = 0;
    std::vector<HWPParaCharShapeEntry> char_shapes;
    std::u16string text;
    std::vector<uint32_t> extend_positions;  // positions of ControlExtend chars

    // Inline controls (tables, images) attached to this paragraph
    std::vector<HWPTable> tables;
    std::vector<int> image_bin_ids;  // binDataId references for images
};

struct HWPEmbeddedImage {
    std::string name;           // e.g. "BIN0001.jpg"
    std::vector<uint8_t> data;
    std::string name_lower;     // lowercase for case-insensitive lookup
};

// ── HWP Parser ──────────────────────────────────────────────

class HWPParser {
public:
    HWPParser(const std::string& path, ConvertOptions opts)
        : opts_(std::move(opts)) {
        // Read entire file into memory for OleReader
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("Cannot open HWP file: " + path);
        f.seekg(0, std::ios::end);
        file_data_.resize(f.tellg());
        f.seekg(0);
        f.read(reinterpret_cast<char*>(file_data_.data()), file_data_.size());
    }

    bool parse() {
        ole_ = std::make_unique<OleReader>(file_data_.data(), file_data_.size());
        if (!ole_->is_open()) {
            throw std::runtime_error("Not a valid HWP file (OLE2 open failed)");
        }

        // 1. Read and validate FileHeader
        parse_file_header();

        // 2. Read DocInfo
        parse_doc_info();

        // 3. Index BinData entries (lazy — actual data loaded on demand)
        index_bin_data_storage();

        // 4. Discover sections
        for (int i = 0; ; i++) {
            std::string section_path = "BodyText/Section" + std::to_string(i);
            auto data = read_ole_stream(*ole_, section_path);
            if (data.empty()) break;
            section_data_.push_back(std::move(data));
        }

        if (section_data_.empty()) {
            throw std::runtime_error("No BodyText sections found in HWP file");
        }

        // Keep ole_ and file_data_ alive for lazy BinData loading.
        // They are released in release_storage() after convert_chunks().

        return true;
    }

    std::vector<PageChunk> convert_chunks() {
        std::vector<PageChunk> chunks;

        for (int i = 0; i < (int)section_data_.size(); i++) {
            if (!opts_.pages.empty()) {
                bool found = false;
                for (int p : opts_.pages) {
                    if (p == i) { found = true; break; }
                }
                if (!found) continue;
            }

            PageChunk chunk;
            chunk.page_number = i + 1;
            // Default A4 page dimensions
            chunk.page_width = 595.28;
            chunk.page_height = 841.88;

            parse_section(i, chunk);
            chunks.push_back(std::move(chunk));
        }

        release_storage();
        return chunks;
    }

    void release_storage() {
        ole_.reset();
        file_data_.clear();
        file_data_.shrink_to_fit();
    }

private:
    ConvertOptions opts_;
    std::vector<uint8_t> file_data_;
    std::unique_ptr<OleReader> ole_;

    bool compressed_ = true;
    uint32_t version_ = 0;

    HWPDocInfo doc_info_;
    std::vector<std::vector<uint8_t>> section_data_;
    // Keyed by lowercase name for O(1) case-insensitive lookup
    std::map<std::string, HWPEmbeddedImage> embedded_images_;
    // Ordered list of image names for index-based fallback
    std::vector<std::string> embedded_image_keys_;

    // ── FileHeader parsing ──────────────────────────────────
    void parse_file_header() {
        auto data = read_ole_stream(*ole_, "FileHeader");
        if (data.size() < 36) {
            throw std::runtime_error("Invalid HWP FileHeader");
        }

        // Verify signature: "HWP Document File" in first 32 bytes
        std::string sig(data.begin(), data.begin() + 17);
        if (sig != "HWP Document File") {
            throw std::runtime_error("Not a valid HWP file (bad signature)");
        }

        // Version at offset 32 (uint32_le)
        version_ = util::read_u32_le(data.data() + 32);

        // Properties at offset 36 (uint32_le)
        if (data.size() >= 40) {
            uint32_t props = util::read_u32_le(data.data() + 36);
            compressed_ = (props & 0x01) != 0;
            bool encrypted = (props & 0x02) != 0;
            if (encrypted) {
                throw std::runtime_error("Encrypted HWP files are not supported");
            }
        }
    }

    // ── DocInfo parsing ─────────────────────────────────────
    void parse_doc_info() {
        auto raw = read_ole_stream(*ole_, "DocInfo");
        if (raw.empty()) return;

        std::vector<uint8_t> data;
        if (compressed_) {
            data = decompress(raw.data(), raw.size());
            if (data.empty()) {
                // Try without decompression (some files aren't actually compressed)
                data = std::move(raw);
            }
        } else {
            data = std::move(raw);
        }

        size_t offset = 0;
        int face_name_lang_idx = 0;  // Track which language's faceNames we're parsing

        // ID mapping counts (tells how many of each record type to expect)
        int id_bin_data_count = 0;
        int id_face_name_count = 0;
        int id_char_shape_count = 0;
        int id_para_shape_count = 0;

        while (offset + 4 <= data.size()) {
            auto hdr = hwp::parse_record_header(data.data(), offset);
            size_t record_end = offset + hdr.size;
            if (record_end > data.size()) break;

            switch (hdr.tag_id) {
            case hwp::DOCUMENT_PROPERTIES:
                if (hdr.size >= 26) {
                    doc_info_.section_count = util::read_u16_le(data.data() + offset);
                }
                break;

            case hwp::ID_MAPPINGS:
                // Parse counts: binData, faceName*7langs, borderFill, charShape, tabDef,
                // numbering, bullet, paraShape, style
                if (hdr.size >= 2 * 14) {
                    id_bin_data_count = util::read_u32_le(data.data() + offset);
                    // faceNames: 7 language groups, each with its own count
                    // Total faceName IDs at offset 4..31 (7 * 4 bytes)
                    id_face_name_count = 0;
                    for (int i = 0; i < 7; i++) {
                        id_face_name_count += util::read_u32_le(data.data() + offset + 4 + i * 4);
                    }
                    // borderFill at offset 32
                    // charShape at offset 36
                    id_char_shape_count = util::read_u32_le(data.data() + offset + 36);
                    // paraShape at offset 48 (after tabDef=40, numbering=44)
                    if (hdr.size >= 56) {
                        id_para_shape_count = util::read_u32_le(data.data() + offset + 52);
                    }
                }
                break;

            case hwp::FACE_NAME:
                parse_face_name(data.data() + offset, hdr.size);
                break;

            case hwp::CHAR_SHAPE:
                parse_char_shape(data.data() + offset, hdr.size);
                break;

            case hwp::PARA_SHAPE:
                parse_para_shape(data.data() + offset, hdr.size);
                break;

            case hwp::BIN_DATA:
                parse_bin_data_ref(data.data() + offset, hdr.size);
                break;
            }

            offset = record_end;
        }
    }

    void parse_face_name(const uint8_t* data, uint32_t size) {
        if (size < 4) return;

        hwp::FaceNameInfo fi;
        fi.id = doc_info_.face_names.size();

        // Property byte at offset 0
        // uint8_t property = data[0];

        // Name string: uint16 length (in chars), then UTF-16LE chars
        // Starting at offset 2
        size_t off = 2;
        if (off + 2 > size) { doc_info_.face_names.push_back(fi); return; }
        uint16_t name_len = util::read_u16_le(data + off);
        off += 2;

        if (off + name_len * 2 <= size) {
            fi.name = util::utf16le_to_utf8(data + off, name_len * 2);
        }

        doc_info_.face_names.push_back(fi);
    }

    void parse_char_shape(const uint8_t* data, uint32_t size) {
        // CharShape binary layout (from hwplib ForCharShape.java):
        // 7 x uint16 faceNameIds     = 14 bytes
        // 7 x uint8  ratios          = 7 bytes
        // 7 x int8   charSpaces      = 7 bytes
        // 7 x uint8  relativeSizes   = 7 bytes
        // 7 x int8   charOffsets     = 7 bytes
        // int32      baseSize        = 4 bytes  (offset 42)
        // uint32     property        = 4 bytes  (offset 46)
        if (size < 50) return;

        hwp::CharShapeInfo cs;
        cs.id = doc_info_.char_shapes.size();

        // Read faceNameIds (7 x uint16)
        for (int i = 0; i < 7; i++) {
            cs.font_ref[i] = util::read_u16_le(data + i * 2);
        }

        // BaseSize at offset 42
        cs.height = util::read_i32_le(data + 42);

        // Property at offset 46
        uint32_t prop = util::read_u32_le(data + 46);
        cs.italic = (prop & 0x01) != 0;
        cs.bold   = (prop & 0x02) != 0;

        // Text color at offset 50 (4 bytes ARGB)
        if (size >= 54) {
            uint32_t color = util::read_u32_le(data + 50);
            char buf[8];
            snprintf(buf, sizeof(buf), "#%06X", color & 0xFFFFFF);
            cs.text_color = buf;
        }

        doc_info_.char_shapes.push_back(cs);
    }

    void parse_para_shape(const uint8_t* data, uint32_t size) {
        if (size < 8) return;

        hwp::ParaShapeInfo ps;
        ps.id = doc_info_.para_shapes.size();

        // Property1 at offset 0 (uint32)
        uint32_t prop1 = util::read_u32_le(data);

        // Alignment from bits 2-4 of property1
        int align = (prop1 >> 2) & 0x07;
        switch (align) {
            case 0: ps.alignment = "JUSTIFY"; break;
            case 1: ps.alignment = "LEFT"; break;
            case 2: ps.alignment = "RIGHT"; break;
            case 3: ps.alignment = "CENTER"; break;
            default: ps.alignment = "JUSTIFY"; break;
        }

        // Outline level (heading): bits 26-28 of property1
        ps.outline_level = (prop1 >> 26) & 0x07;

        doc_info_.para_shapes.push_back(ps);
    }

    void parse_bin_data_ref(const uint8_t* data, uint32_t size) {
        if (size < 2) return;

        HWPBinDataRef ref;
        uint16_t prop = util::read_u16_le(data);
        ref.type = prop & 0x0F;  // lower 4 bits

        size_t off = 2;
        if (ref.type == 0) {
            // LINK: absolute path + relative path
            // Skip for now (not common for images)
        } else if (ref.type == 1) {
            // EMBEDDING
            if (off + 2 <= size) {
                ref.bin_data_id = util::read_u16_le(data + off);
                off += 2;
            }
            if (off + 2 <= size) {
                uint16_t ext_len = util::read_u16_le(data + off);
                off += 2;
                if (off + ext_len * 2 <= size) {
                    ref.extension = util::utf16le_to_utf8(data + off, ext_len * 2);
                }
            }
        }

        doc_info_.bin_data_refs.push_back(ref);
    }

    // ── BinData storage indexing (lazy — no data read here) ──
    void index_bin_data_storage() {
        auto entries = ole_->entries("BinData");
        for (auto& name : entries) {
            std::string key = name;
            for (auto& c : key) c = std::tolower(c);

            HWPEmbeddedImage img;
            img.name = name;
            img.name_lower = key;
            // img.data left empty — loaded on demand in load_bin_data()

            embedded_image_keys_.push_back(key);
            embedded_images_.emplace(std::move(key), std::move(img));
        }
    }

    // Load BinData on demand from OLE storage.
    // BinData streams contain raw image data (PNG/JPEG/etc.) and are NOT
    // deflate-compressed, even when the HWP compressed flag is set.
    // The compressed flag only applies to DocInfo and BodyText sections.
    std::vector<uint8_t> load_bin_data(const std::string& name) {
        if (!ole_) return {};
        std::string full_path = "BinData/" + name;
        return read_ole_stream(*ole_, full_path);
    }
    // ── Section parsing ─────────────────────────────────────
    void parse_section(int section_idx, PageChunk& chunk) {
        auto& raw = section_data_[section_idx];

        std::vector<uint8_t> data;
        if (compressed_) {
            data = decompress(raw.data(), raw.size());
            if (data.empty()) {
                data = std::move(raw);  // fallback: use raw directly
            } else {
                // Release raw data - no longer needed after decompress
                raw.clear();
                raw.shrink_to_fit();
            }
        } else {
            data = std::move(raw);  // move instead of copy
        }

        // Parse paragraphs from record stream
        std::vector<HWPParagraph> paragraphs;
        parse_paragraph_records(data, paragraphs);

        // Convert paragraphs to markdown
        std::string md;
        md.reserve(data.size());

        std::map<int, int> size_counts;
        int image_idx = 0;

        // Track control index per paragraph for table/image matching
        size_t ctrl_record_idx = 0;

        for (auto& para : paragraphs) {
            std::string para_md = format_paragraph(para, chunk, size_counts, image_idx);
            if (!para_md.empty()) {
                md += para_md;
            }
        }

        // Determine body font size
        int max_count = 0, body_height = 1000;
        for (auto& [h, c] : size_counts) {
            if (c > max_count) { max_count = c; body_height = h; }
        }
        chunk.body_font_size = body_height / 100.0;
        chunk.text = md;
    }

    void parse_paragraph_records(const std::vector<uint8_t>& data,
                                  std::vector<HWPParagraph>& paragraphs) {
        size_t offset = 0;
        int current_idx = -1;

        // State for control parsing
        enum CtrlState { NONE, IN_TABLE, IN_GSO };
        CtrlState ctrl_state = NONE;
        int current_table_para = -1;
        int current_table_idx = -1;

        // Helper lambdas (declared after variables they capture)
        auto cur_para = [&]() -> HWPParagraph* {
            return current_idx >= 0 ? &paragraphs[current_idx] : nullptr;
        };
        auto cur_table = [&]() -> HWPTable* {
            if (current_table_para < 0 || current_table_idx < 0) return nullptr;
            auto& tbls = paragraphs[current_table_para].tables;
            if (current_table_idx >= (int)tbls.size()) return nullptr;
            return &tbls[current_table_idx];
        };
        int table_total_cells = 0;
        int table_parsed_cells = 0;
        std::string current_cell_text;
        bool in_cell = false;
        int cur_cell_col = 0, cur_cell_row = 0;
        int cur_cell_colspan = 1, cur_cell_rowspan = 1;

        // Stack for nested tables
        struct TableState {
            int tbl_para, tbl_idx, total_cells, parsed_cells;
            std::string cell_text;
            bool was_in_cell;
            int cell_col, cell_row, cell_colspan, cell_rowspan;
        };
        std::vector<TableState> table_stack;

        // Flush current cell into the table's cell list
        auto flush_cell = [&]() {
            auto* tbl = cur_table();
            if (!tbl) return;
            HWPTableCell cell;
            cell.text = util::trim(current_cell_text);
            cell.col_addr = cur_cell_col;
            cell.row_addr = cur_cell_row;
            cell.col_span = cur_cell_colspan;
            cell.row_span = cur_cell_rowspan;
            tbl->cells.push_back(std::move(cell));
            current_cell_text.clear();
        };

        // Restore outer table state after nested table finishes
        auto restore_table_state = [&]() {
            if (!table_stack.empty()) {
                auto& s = table_stack.back();
                current_table_para = s.tbl_para;
                current_table_idx = s.tbl_idx;
                table_total_cells = s.total_cells;
                table_parsed_cells = s.parsed_cells;
                current_cell_text = std::move(s.cell_text);
                in_cell = s.was_in_cell;
                cur_cell_col = s.cell_col;
                cur_cell_row = s.cell_row;
                cur_cell_colspan = s.cell_colspan;
                cur_cell_rowspan = s.cell_rowspan;
                ctrl_state = IN_TABLE;
                table_stack.pop_back();
            } else {
                ctrl_state = NONE;
                current_table_para = -1;
                current_table_idx = -1;
            }
        };

        while (offset + 4 <= data.size()) {
            auto hdr = hwp::parse_record_header(data.data(), offset);
            size_t record_end = offset + hdr.size;
            if (record_end > data.size()) break;

            switch (hdr.tag_id) {
            case hwp::PARA_HEADER: {
                if (in_cell && hdr.level == 0) {
                    flush_cell();
                    in_cell = false;
                    ctrl_state = NONE;
                    current_table_para = -1;
                    current_table_idx = -1;
                }

                // If we're inside a table cell, skip paragraph creation
                if (in_cell) break;

                // Start new paragraph (level 0 = top-level, level > 0 = inside GSO/textbox)
                paragraphs.emplace_back();
                current_idx = (int)paragraphs.size() - 1;
                if (hdr.level == 0) ctrl_state = NONE;

                if (hdr.size >= 6) {
                    auto* p = cur_para();
                    p->char_count = util::read_u32_le(data.data() + offset);
                    p->control_mask = util::read_u16_le(data.data() + offset + 4);
                    if (hdr.size >= 8)
                        p->para_shape_id = util::read_u16_le(data.data() + offset + 6);
                    if (hdr.size >= 10)
                        p->style_id = util::read_u16_le(data.data() + offset + 8);
                }
                break;
            }

            case hwp::PARA_TEXT:
                if (in_cell) {
                    HWPParagraph tmp;
                    parse_para_text(data.data() + offset, hdr.size, tmp);
                    for (auto ch : tmp.text) {
                        util::append_utf8(current_cell_text, ch);
                    }
                    if (!current_cell_text.empty() && current_cell_text.back() != ' ')
                        current_cell_text += ' ';
                } else if (cur_para()) {
                    parse_para_text(data.data() + offset, hdr.size, *cur_para());
                }
                break;

            case hwp::PARA_CHAR_SHAPE:
                if (!in_cell && cur_para()) {
                    parse_para_char_shape(data.data() + offset, hdr.size, *cur_para());
                }
                break;

            case hwp::CTRL_HEADER: {
                if (hdr.size < 4) break;

                if (in_cell && hdr.level == 0) {
                    flush_cell();
                    in_cell = false;
                    ctrl_state = NONE;
                    current_table_para = -1;
                    current_table_idx = -1;
                }

                // If in table but a new ctrl at same level, flush remaining cell
                if (in_cell && ctrl_state == IN_TABLE) {
                    flush_cell();
                    in_cell = false;
                }

                uint32_t ctrl_id = util::read_u32_le(data.data() + offset);
                char c0 = (ctrl_id >> 24) & 0xFF;
                char c1 = (ctrl_id >> 16) & 0xFF;
                char c2 = (ctrl_id >> 8) & 0xFF;
                char c3 = ctrl_id & 0xFF;
                std::string ctrl_str = {c0, c1, c2, c3};

                if (ctrl_str == "tbl ") {
                    // Save current table state for nested tables
                    if (ctrl_state == IN_TABLE && in_cell) {
                        table_stack.push_back({current_table_para, current_table_idx,
                                              table_total_cells, table_parsed_cells,
                                              current_cell_text, in_cell,
                                              cur_cell_col, cur_cell_row,
                                              cur_cell_colspan, cur_cell_rowspan});
                        current_cell_text.clear();
                        in_cell = false;
                    }
                    ctrl_state = IN_TABLE;
                    auto* p = cur_para();
                    if (p) {
                        p->tables.emplace_back();
                        current_table_para = current_idx;
                        current_table_idx = (int)p->tables.size() - 1;
                    }
                    table_parsed_cells = 0;
                    table_total_cells = 0;
                    in_cell = false;
                } else if (ctrl_str == "gso ") {
                    if (ctrl_state != IN_TABLE) {
                        ctrl_state = IN_GSO;
                    }
                }
                break;
            }

            case hwp::TABLE: {
                auto* tbl = cur_table();
                if (ctrl_state == IN_TABLE && tbl && hdr.size >= 8) {
                    tbl->row_count = util::read_u16_le(data.data() + offset + 4);
                    tbl->col_count = util::read_u16_le(data.data() + offset + 6);

                    size_t cpr_off = offset + 18;
                    table_total_cells = 0;
                    for (int r = 0; r < tbl->row_count; r++) {
                        if (cpr_off + 2 <= record_end) {
                            table_total_cells += util::read_u16_le(data.data() + cpr_off);
                            cpr_off += 2;
                        }
                    }
                }
                break;
            }

            case hwp::LIST_HEADER: {
                if (ctrl_state == IN_TABLE && cur_table()) {
                    if (in_cell) {
                        flush_cell();
                        table_parsed_cells++;
                    }
                    in_cell = true;

                    // LIST_HEADER layout for table cells:
                    // paraCount(4) + properties(4) + colAddr(2) + rowAddr(2) + colSpan(2) + rowSpan(2)
                    if (hdr.size >= 16) {
                        cur_cell_col = util::read_u16_le(data.data() + offset + 8);
                        cur_cell_row = util::read_u16_le(data.data() + offset + 10);
                        cur_cell_colspan = util::read_u16_le(data.data() + offset + 12);
                        cur_cell_rowspan = util::read_u16_le(data.data() + offset + 14);
                    } else {
                        cur_cell_col = 0; cur_cell_row = 0;
                        cur_cell_colspan = 1; cur_cell_rowspan = 1;
                    }

                    // Check if all cells parsed -> exit table mode (or restore outer table)
                    if (table_total_cells > 0 && table_parsed_cells >= table_total_cells) {
                        in_cell = false;
                        restore_table_state();
                    }
                }
                break;
            }

            case hwp::SHAPE_COMPONENT: {
                if (ctrl_state == IN_GSO && hdr.size >= 10) {
                    char sc_name[5] = {};
                    memcpy(sc_name, data.data() + offset, 4);
                    if (std::string(sc_name) == "cip$" || std::string(sc_name) == "$pic") {
                        if (hdr.size >= 20) {
                            uint16_t bin_id = util::read_u16_le(data.data() + offset + 18);
                            if (bin_id > 0 && cur_para()) {
                                cur_para()->image_bin_ids.push_back(bin_id);
                            }
                        }
                    }
                }
                break;
            }

            case hwp::SHAPE_COMPONENT_PICTURE: {
                if (ctrl_state == IN_GSO && hdr.size >= 15) {
                    uint16_t bin_id = util::read_u16_le(data.data() + offset + 13);
                    if (bin_id > 0 && cur_para()) {
                        cur_para()->image_bin_ids.push_back(bin_id);
                    }
                    ctrl_state = NONE;
                }
                break;
            }

            case hwp::SHAPE_COMPONENT_OLE: {
                if (ctrl_state == IN_GSO && hdr.size >= 15) {
                    uint16_t bin_id = util::read_u16_le(data.data() + offset + 13);
                    if (bin_id > 0 && cur_para()) {
                        cur_para()->image_bin_ids.push_back(bin_id);
                    }
                    ctrl_state = NONE;
                }
                break;
            }

            default:
                break;
            }

            offset = record_end;
        }

        if (in_cell) flush_cell();
    }

    void parse_para_text(const uint8_t* data, uint32_t size, HWPParagraph& para) {
        size_t off = 0;
        uint32_t char_pos = 0;

        while (off + 1 < size) {
            uint16_t code = util::read_u16_le(data + off);
            off += 2;

            auto type = hwp::classify_hwp_char(code);
            switch (type) {
            case hwp::HWPCharType::Normal:
                para.text.push_back(static_cast<char16_t>(code));
                char_pos++;
                break;

            case hwp::HWPCharType::ControlChar:
                if (code == 13 || code == 10) {
                    para.text.push_back(u'\n');
                } else if (code == 9) {
                    para.text.push_back(u'\t');
                }
                char_pos++;
                break;

            case hwp::HWPCharType::ControlExtend:
                // 16 bytes total: 2 (code) + 12 (addition) + 2 (code)
                para.extend_positions.push_back(char_pos);
                if (off + 14 <= size) {
                    off += 14;  // skip 12 addition bytes + 2 trailing code bytes
                }
                char_pos += 8;  // ControlExtend occupies 8 char positions
                break;

            case hwp::HWPCharType::ControlInline:
                // 16 bytes total like ControlExtend
                if (off + 14 <= size) {
                    off += 14;
                }
                char_pos += 8;
                break;
            }
        }
    }

    void parse_para_char_shape(const uint8_t* data, uint32_t size,
                                HWPParagraph& para) {
        // Array of (uint32 position, uint32 charShapeId) pairs
        size_t off = 0;
        while (off + 8 <= size) {
            HWPParaCharShapeEntry entry;
            entry.position = util::read_u32_le(data + off);
            entry.char_shape_id = util::read_u32_le(data + off + 4);
            para.char_shapes.push_back(entry);
            off += 8;
        }
    }

    // ── Table formatting ─────────────────────────────────────
    std::string format_table(const HWPTable& table) {
        if (table.cells.empty() || table.col_count == 0 || table.row_count == 0) {
            // If there are cells but row/col is 0, emit cell text as body text
            if (!table.cells.empty()) {
                std::string fallback;
                for (auto& cell : table.cells) {
                    if (!cell.text.empty()) {
                        if (!fallback.empty()) fallback += "\n\n";
                        fallback += cell.text;
                    }
                }
                return fallback.empty() ? "" : fallback + "\n\n";
            }
            return "";
        }

        // Build a 2D grid using cell addresses and spans
        std::vector<std::vector<std::string>> grid(
            table.row_count, std::vector<std::string>(table.col_count));

        for (auto& cell : table.cells) {
            if (cell.row_addr < table.row_count && cell.col_addr < table.col_count) {
                grid[cell.row_addr][cell.col_addr] = util::escape_cell(cell.text);
            }
        }

        std::string md;
        for (int r = 0; r < table.row_count; r++) {
            md += "|";
            for (int c = 0; c < table.col_count; c++) {
                md += " " + grid[r][c] + " |";
            }
            md += "\n";

            if (r == 0) {
                md += "|";
                for (int c = 0; c < table.col_count; c++) {
                    md += " --- |";
                }
                md += "\n";
            }
        }
        md += "\n";
        return md;
    }

    // ── Image handling ─────────────────────────────────────────
    // Resolve the BinData entry name for a given bin_id.
    HWPEmbeddedImage* find_image_entry(int bin_id) {
        if (bin_id <= 0 || bin_id > (int)doc_info_.bin_data_refs.size()) return nullptr;
        auto& ref = doc_info_.bin_data_refs[bin_id - 1];
        std::string ext = ref.extension;
        if (ext.empty()) ext = "jpg";

        char hex_name[32];
        snprintf(hex_name, sizeof(hex_name), "bin%04x.%s",
                 ref.bin_data_id, ext.c_str());
        std::string lookup_key = hex_name;
        for (auto& c : lookup_key) c = std::tolower(c);

        auto it = embedded_images_.find(lookup_key);
        if (it != embedded_images_.end()) return &it->second;

        // Fallback: match by index in insertion order
        if (bin_id <= (int)embedded_image_keys_.size()) {
            auto fit = embedded_images_.find(embedded_image_keys_[bin_id - 1]);
            if (fit != embedded_images_.end()) return &fit->second;
        }
        return nullptr;
    }

    std::string format_image(int bin_id, PageChunk& chunk, int& image_idx) {
        if (bin_id <= 0 || bin_id > (int)doc_info_.bin_data_refs.size()) return "";

        auto* entry = find_image_entry(bin_id);
        if (!entry) return "";

        auto& ref = doc_info_.bin_data_refs[bin_id - 1];
        std::string ext = ref.extension;
        if (ext.empty()) ext = "jpg";

        std::string unified = "page" + std::to_string(chunk.page_number)
                             + "_img" + std::to_string(image_idx);
        std::string filename = unified + "." + ext;

        // When images not requested, emit embedded reference without loading data
        if (!opts_.extract_images) {
            image_idx++;
            return "![" + unified + "](embedded:" + unified + ")\n\n";
        }

        // Lazy load from OLE storage
        if (entry->data.empty()) {
            entry->data = load_bin_data(entry->name);
            if (entry->data.empty()) return "";
        }

        // Check dimensions before saving
        {
            auto [iw, ih] = util::image_dimensions_from_data(
                reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
            if (opts_.min_image_size > 0 && iw > 0 && ih > 0 &&
                iw < opts_.min_image_size && ih < opts_.min_image_size)
                return "";
        }

        std::string saved_path;
        if (!opts_.image_output_dir.empty()) {
            util::ensure_dir(opts_.image_output_dir);
            saved_path = opts_.image_output_dir + "/" + filename;
            std::ofstream out(saved_path, std::ios::binary);
            if (out) {
                out.write(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
            }
        }

        ImageData idata;
        idata.page_number = chunk.page_number;
        idata.name = unified;
        idata.format = ext;
        idata.data.assign(reinterpret_cast<const char*>(entry->data.data()),
                          reinterpret_cast<const char*>(entry->data.data()) + entry->data.size());
        entry->data.clear();
        entry->data.shrink_to_fit();
        util::populate_image_dimensions(idata);

        idata.saved_path = saved_path;
        chunk.images.push_back(std::move(idata));

        image_idx++;

        return "![" + unified + "](" + filename + ")\n\n";
    }

    // ── Paragraph to Markdown ───────────────────────────────
    std::string format_paragraph(HWPParagraph& para, PageChunk& chunk,
                                  std::map<int, int>& size_counts, int& image_idx) {
        std::string result;

        // First, handle any tables attached to this paragraph
        if (opts_.extract_tables) {
            for (auto& tbl : para.tables) {
                result += format_table(tbl);
            }
        }

        // Handle any images attached to this paragraph
        for (int bin_id : para.image_bin_ids) {
            result += format_image(bin_id, chunk, image_idx);
        }

        if (para.text.empty()) return result;

        // Convert UTF-16 text to UTF-8, applying formatting
        std::string text;
        text.reserve(para.text.size() * 2);

        // Get paragraph-level info
        int outline_level = 0;
        if (para.para_shape_id < doc_info_.para_shapes.size()) {
            outline_level = doc_info_.para_shapes[para.para_shape_id].outline_level;
        }

        // Determine CharShape for each character position
        struct FormattedSpan {
            std::string text;
            bool bold = false;
            bool italic = false;
            int height = 1000;
        };
        std::vector<FormattedSpan> spans;

        if (para.char_shapes.empty()) {
            FormattedSpan span;
            for (size_t i = 0; i < para.text.size(); i++) {
                util::append_utf8(span.text, para.text[i]);
            }
            spans.push_back(std::move(span));
        } else {
            for (size_t si = 0; si < para.char_shapes.size(); si++) {
                uint32_t start = para.char_shapes[si].position;
                uint32_t end = (si + 1 < para.char_shapes.size())
                                ? para.char_shapes[si + 1].position
                                : (uint32_t)para.text.size();
                uint32_t cs_id = para.char_shapes[si].char_shape_id;

                FormattedSpan span;
                if (cs_id < doc_info_.char_shapes.size()) {
                    auto& cs = doc_info_.char_shapes[cs_id];
                    span.bold = cs.bold;
                    span.italic = cs.italic;
                    span.height = cs.height;
                    size_counts[cs.height]++;
                }

                for (uint32_t pos = start; pos < end && pos < para.text.size(); pos++) {
                    util::append_utf8(span.text, para.text[pos]);
                }

                if (!span.text.empty()) {
                    spans.push_back(std::move(span));
                }
            }
        }

        // Build text with inline formatting
        for (auto& span : spans) {
            std::string t = util::trim(span.text);
            if (t.empty()) {
                text += span.text;
                continue;
            }
            if (span.bold && span.italic) {
                text += "***" + t + "***";
            } else if (span.bold) {
                text += "**" + t + "**";
            } else if (span.italic) {
                text += "*" + t + "*";
            } else {
                text += span.text;
            }
        }

        text = util::trim(text);
        if (text.empty()) return result;

        while (!text.empty() && text.back() == '\n') text.pop_back();
        text = util::trim(text);
        if (text.empty()) return result;

        // Format as heading or body
        if (outline_level > 0 && outline_level <= 6) {
            std::string hashes(outline_level, '#');
            std::string clean = text;
            while (clean.size() >= 4 && clean.substr(0, 2) == "**" &&
                   clean.substr(clean.size() - 2) == "**") {
                clean = clean.substr(2, clean.size() - 4);
            }
            return result + hashes + " " + clean + "\n\n";
        }

        // Font-size based heading fallback
        int dominant_height = 1000;
        if (!para.char_shapes.empty() &&
            para.char_shapes[0].char_shape_id < doc_info_.char_shapes.size()) {
            dominant_height = doc_info_.char_shapes[para.char_shapes[0].char_shape_id].height;
        }

        double ratio = dominant_height / 1000.0;
        if (ratio >= 2.0) {
            return result + "# " + text + "\n\n";
        } else if (ratio >= 1.6) {
            return result + "## " + text + "\n\n";
        } else if (ratio >= 1.3) {
            return result + "### " + text + "\n\n";
        }

        return result + text + "\n\n";
    }
};

// ── Public API ──────────────────────────────────────────────

std::string hwp_to_markdown(const std::string& hwp_path, ConvertOptions opts) {
    HWPParser parser(hwp_path, opts);
    parser.parse();
    auto chunks = parser.convert_chunks();

    bool plaintext = (opts.output_format == OutputFormat::PLAINTEXT);

    std::string result;
    for (size_t i = 0; i < chunks.size(); i++) {
        if (i > 0) result += "\n";
        if (plaintext)
            result += util::strip_markdown(chunks[i].text);
        else
            result += chunks[i].text;
    }
    return result;
}

std::vector<PageChunk> hwp_to_markdown_chunks(const std::string& hwp_path,
                                               ConvertOptions opts) {
    opts.page_chunks = true;
    HWPParser parser(hwp_path, opts);
    parser.parse();
    auto chunks = parser.convert_chunks();

    if (opts.output_format == OutputFormat::PLAINTEXT) {
        for (auto& chunk : chunks)
            chunk.text = util::strip_markdown(chunk.text);
    }
    return chunks;
}

} // namespace jdoc
