// hwp2md.cpp - HWP (binary) to Markdown converter
// Parses HWP 5.x OLE2 compound documents and extracts text, tables, images
// Reference: hwplib (Java) by neolord0
// License: MIT

#include "jdoc/hwp.h"
#include "jdoc/hwp_types.h"
#include "legacy/ole_reader.h"
#include "common/file_utils.h"
#include "common/image_utils.h"
#include "common/png_encode.h"
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
    uint16_t compress = 0; // 0=default(compressed), 1=compressed, 2=no compression
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
        : opts_(std::move(opts)), path_(path) {}

    bool parse() {
        ole_ = std::make_unique<OleReader>(path_);
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

        // Keep ole_ alive for lazy BinData loading.
        // Released in release_storage() after convert_chunks().

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
            section_data_[i].clear();
            section_data_[i].shrink_to_fit();
            chunks.push_back(std::move(chunk));
        }

        release_storage();
        return chunks;
    }

    void release_storage() {
        ole_.reset();
    }

private:
    ConvertOptions opts_;
    std::string path_;
    std::unique_ptr<OleReader> ole_;

    bool compressed_ = true;
    uint32_t version_ = 0;
    int sequential_bin_id_ = 0;  // sequential image counter for fallback bin_id

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
        ref.compress = (prop >> 4) & 0x03;  // bits 4-5: compression

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
        auto raw = read_ole_stream(*ole_, full_path);
        if (raw.empty()) return raw;
        // BinData streams are deflate-compressed when the document is compressed
        if (compressed_) {
            auto dec = decompress(raw.data(), raw.size());
            if (!dec.empty()) return dec;
        }
        return raw;
    }
    // ── Section parsing ─────────────────────────────────────
    void parse_section(int section_idx, PageChunk& chunk) {
        sequential_bin_id_ = 0;
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
        RecordCursor cur(data.data(), data.size());
        read_paragraph_list(cur, 0, paragraphs);

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

    // ── RecordCursor: sequential record reader ───────────────
    struct Record {
        uint16_t tag = 0;
        uint16_t level = 0;
        uint32_t size = 0;
        const uint8_t* data = nullptr;
    };

    class RecordCursor {
        const uint8_t* buf_;
        size_t len_;
        size_t pos_ = 0;
    public:
        RecordCursor(const uint8_t* buf, size_t len) : buf_(buf), len_(len) {}

        bool has_next() const { return pos_ + 4 <= len_; }

        Record peek() const {
            Record r;
            if (pos_ + 4 > len_) return r;
            size_t tmp = pos_;
            uint32_t val = util::read_u32_le(buf_ + tmp);
            tmp += 4;
            r.tag   = val & 0x3FF;
            r.level = (val >> 10) & 0x3FF;
            r.size  = (val >> 20) & 0xFFF;
            if (r.size == 0xFFF && tmp + 4 <= len_) {
                r.size = util::read_u32_le(buf_ + tmp);
                tmp += 4;
            }
            if (tmp + r.size <= len_)
                r.data = buf_ + tmp;
            else
                r.data = nullptr;
            return r;
        }

        Record next() {
            Record r = peek();
            // Advance past header
            uint32_t val = util::read_u32_le(buf_ + pos_);
            pos_ += 4;
            uint32_t raw_size = (val >> 20) & 0xFFF;
            if (raw_size == 0xFFF) pos_ += 4;
            // Advance past data
            pos_ += r.size;
            if (pos_ > len_) pos_ = len_;
            return r;
        }

        void skip() { next(); }
    };

    // ── Recursive descent paragraph parser ──────────────────

    // Read a list of paragraphs.
    // expected_level: the level paragraphs must be at (or -1 to accept any PARA_HEADER)
    void read_paragraph_list(RecordCursor& cur, int expected_level,
                             std::vector<HWPParagraph>& out) {
        while (cur.has_next()) {
            auto r = cur.peek();
            if (r.tag != hwp::PARA_HEADER) return;
            // If expected_level >= 0, only accept paragraphs at that exact level
            if (expected_level >= 0 && r.level != (uint16_t)expected_level) return;
            HWPParagraph para;
            read_paragraph(cur, r.level, para);
            out.push_back(std::move(para));
        }
    }

    // Read a single paragraph: PARA_HEADER + PARA_TEXT + PARA_CHAR_SHAPE +
    // PARA_LINE_SEG + PARA_RANGE_TAG + controls
    void read_paragraph(RecordCursor& cur, uint16_t para_level, HWPParagraph& para) {
        // Consume PARA_HEADER
        auto hdr = cur.next();
        if (hdr.tag != hwp::PARA_HEADER) return;
        if (hdr.data && hdr.size >= 6) {
            para.char_count = util::read_u32_le(hdr.data);
            para.control_mask = util::read_u16_le(hdr.data + 4);
            if (hdr.size >= 8)
                para.para_shape_id = util::read_u16_le(hdr.data + 6);
            if (hdr.size >= 10)
                para.style_id = util::read_u16_le(hdr.data + 8);
        }

        // Consume optional PARA_TEXT, PARA_CHAR_SHAPE, PARA_LINE_SEG, PARA_RANGE_TAG
        while (cur.has_next()) {
            auto r = cur.peek();
            if (r.level <= para_level) break;
            if (r.tag == hwp::PARA_TEXT) {
                auto rec = cur.next();
                if (rec.data) parse_para_text(rec.data, rec.size, para);
            } else if (r.tag == hwp::PARA_CHAR_SHAPE) {
                auto rec = cur.next();
                if (rec.data) parse_para_char_shape(rec.data, rec.size, para);
            } else if (r.tag == hwp::PARA_LINE_SEG || r.tag == hwp::PARA_RANGE_TAG) {
                cur.skip();
            } else if (r.tag == hwp::CTRL_HEADER) {
                read_control_dispatch(cur, para_level, para);
            } else {
                // Unknown record inside paragraph — skip if deeper
                if (r.level > para_level) cur.skip();
                else break;
            }
        }
    }

    // Dispatch a CTRL_HEADER: peek ctrl_id, call appropriate reader
    void read_control_dispatch(RecordCursor& cur, uint16_t para_level, HWPParagraph& para) {
        auto r = cur.peek();
        if (r.tag != hwp::CTRL_HEADER || !r.data || r.size < 4) {
            cur.skip();
            return;
        }
        uint16_t ctrl_level = r.level;
        uint32_t ctrl_id = util::read_u32_le(r.data);
        char c0 = (ctrl_id >> 24) & 0xFF;
        char c1 = (ctrl_id >> 16) & 0xFF;
        char c2 = (ctrl_id >> 8) & 0xFF;
        char c3 = ctrl_id & 0xFF;
        std::string ctrl_str = {c0, c1, c2, c3};

        cur.skip(); // consume CTRL_HEADER

        if (ctrl_str == "tbl ") {
            read_table(cur, ctrl_level, para);
        } else if (ctrl_str == "gso ") {
            std::string dummy;
            read_gso(cur, ctrl_level, para, false, dummy);
        } else {
            // Skip all records belonging to this control
            skip_control(cur, ctrl_level);
        }
    }

    // Skip records deeper than ctrl_level (unknown control types)
    void skip_control(RecordCursor& cur, uint16_t ctrl_level) {
        while (cur.has_next()) {
            auto r = cur.peek();
            if (r.level <= ctrl_level) return;
            cur.skip();
        }
    }

    // Read a table: TABLE record + N cells via LIST_HEADER
    void read_table(RecordCursor& cur, uint16_t ctrl_level, HWPParagraph& para) {
        // Expect TABLE record next
        if (!cur.has_next()) return;
        auto r = cur.peek();
        if (r.tag != hwp::TABLE) {
            skip_control(cur, ctrl_level);
            return;
        }

        HWPTable table;
        auto tbl_rec = cur.next();
        if (tbl_rec.data && tbl_rec.size >= 8) {
            table.row_count = util::read_u16_le(tbl_rec.data + 4);
            table.col_count = util::read_u16_le(tbl_rec.data + 6);
        }

        // Count total cells from cellsPerRow array at offset 18
        int total_cells = 0;
        if (tbl_rec.data && tbl_rec.size >= 18) {
            size_t cpr_off = 18;
            for (int row = 0; row < table.row_count; row++) {
                if (cpr_off + 2 <= tbl_rec.size) {
                    total_cells += util::read_u16_le(tbl_rec.data + cpr_off);
                    cpr_off += 2;
                }
            }
        }
        if (total_cells == 0)
            total_cells = table.row_count * table.col_count;

        // Read cells: each begins with LIST_HEADER
        for (int ci = 0; ci < total_cells && cur.has_next(); ci++) {
            auto pk = cur.peek();
            if (pk.level <= ctrl_level) break;
            if (pk.tag == hwp::LIST_HEADER) {
                HWPTableCell cell = read_cell(cur);
                table.cells.push_back(std::move(cell));
            } else {
                cur.skip();
                ci--;
            }
        }

        // Skip any remaining deeper records (e.g. CTRL_DATA after cells)
        while (cur.has_next()) {
            auto pk = cur.peek();
            if (pk.level <= ctrl_level) break;
            cur.skip();
        }

        para.tables.push_back(std::move(table));
    }

    // Read a single cell: LIST_HEADER + paragraph list
    // Cell paragraphs are flattened into cell.text with \x01 image markers and \x02 breaks
    HWPTableCell read_cell(RecordCursor& cur) {
        HWPTableCell cell;
        auto lh = cur.next(); // consume LIST_HEADER
        if (lh.tag != hwp::LIST_HEADER) return cell;

        uint16_t cell_level = lh.level;

        if (lh.data && lh.size >= 16) {
            cell.col_addr  = util::read_u16_le(lh.data + 8);
            cell.row_addr  = util::read_u16_le(lh.data + 10);
            cell.col_span  = util::read_u16_le(lh.data + 12);
            cell.row_span  = util::read_u16_le(lh.data + 14);
        }

        // Read cell paragraphs recursively.
        // Cell paragraphs are at the same level as LIST_HEADER (siblings),
        // so we accept records at cell_level (not just > cell_level).
        std::vector<HWPParagraph> cell_paras;
        while (cur.has_next()) {
            auto pk = cur.peek();
            if (pk.level < cell_level) break;
            if (pk.tag == hwp::PARA_HEADER) {
                HWPParagraph cpara;
                read_paragraph(cur, pk.level, cpara);
                cell_paras.push_back(std::move(cpara));
            } else if (pk.tag == hwp::LIST_HEADER && pk.level == cell_level) {
                // Next cell's LIST_HEADER — stop reading this cell
                break;
            } else {
                cur.skip();
            }
        }

        // Flatten cell paragraphs into cell.text
        flatten_cell_paragraphs(cell_paras, cell.text);

        cell.text = util::trim(cell.text);
        return cell;
    }

    // Flatten paragraphs into a cell text string with inline markers
    void flatten_cell_paragraphs(std::vector<HWPParagraph>& paras, std::string& out) {
        for (size_t pi = 0; pi < paras.size(); pi++) {
            auto& cpara = paras[pi];

            // Flatten nested tables recursively, then clear to prevent
            // format_paragraph from re-rendering them
            for (auto& tbl : cpara.tables) {
                for (auto& tcell : tbl.cells) {
                    if (!tcell.text.empty()) {
                        if (!out.empty()) out += '\x02';
                        out += tcell.text;
                    }
                }
            }
            cpara.tables.clear();

            // Append images as \x01{bid}\x01 markers, then clear
            for (int bid : cpara.image_bin_ids) {
                out += '\x01';
                out += std::to_string(bid);
                out += '\x01';
            }
            cpara.image_bin_ids.clear();

            // Append text
            std::string txt;
            for (auto ch : cpara.text) {
                if (!is_hwp_printable(ch)) continue;
                util::append_utf8(txt, ch);
            }
            txt = util::trim(txt);
            if (!txt.empty()) {
                if (!out.empty() && out.back() != '\x02') {
                    // Add separator between paragraphs
                    if (!out.empty()) out += ' ';
                }
                out += txt;
            }
        }
    }

    // Extract binItemID from SHAPE_COMPONENT fillInfo (for 'cip$'/'$pic' images).
    // Returns bin_id > 0 on success, 0 if not an image shape.
    int extract_shape_component_image(const uint8_t* data, uint32_t size) {
        if (size < 8) return 0;
        // Check name: first 4 bytes
        char name[5] = {};
        memcpy(name, data, 4);
        bool is_pic = (std::string(name) == "cip$" || std::string(name) == "$pic");
        if (!is_pic) return 0;

        // Layout after name(4) + id2(4):
        // commonPart: 42 bytes fixed + renderingInfo (variable)
        size_t off = 8; // skip name + id2
        // commonPart fixed fields: 42 bytes
        off += 42; // off = 50
        if (off + 2 > size) return 0;
        uint16_t mat_count = util::read_u16_le(data + off);
        off += 2;
        off += 48; // translation matrix (6 doubles)
        off += (size_t)mat_count * 96;
        if (off > size) return 0;

        // lineInfo: color(4) + thickness(4) + property(4) + outlineStyle(1) = 13 bytes
        off += 13;
        if (off + 4 > size) return 0;

        // fillInfo: type(4)
        uint32_t fill_type = util::read_u32_le(data + off);
        off += 4;
        if (fill_type == 0) {
            return 0;
        }

        // patternFill: if bit 0 set, 12 bytes
        if (fill_type & 0x01) {
            off += 12; // backColor(4) + patternColor(4) + patternType(4)
        }

        // gradientFill: if bit 1 set
        if (fill_type & 0x02) {
            off += 1; // gradientType
            off += 4; // startAngle
            off += 4; // centerX
            off += 4; // centerY
            off += 4; // blurringDegree
            if (off + 4 > size) return 0;
            uint32_t color_count = util::read_u32_le(data + off);
            off += 4;
            if (color_count > 2)
                off += (size_t)color_count * 4; // changePoints
            off += (size_t)color_count * 4;     // colors
        }

        // imageFill: if bit 2 set
        if (fill_type & 0x04) {
            // imageFillType(1) + brightness(1) + contrast(1) + effect(1) + binItemID(2)
            off += 1 + 1 + 1 + 1;
            if (off + 2 > size) return 0;
            uint16_t bin_id = util::read_u16_le(data + off);
            return bin_id;
        }

        return 0;
    }

    // Helper: emit a resolved bin_id to either cell_text or para.image_bin_ids
    void emit_image(int bin_id, bool in_cell, std::string& cell_text, HWPParagraph& para) {
        if (bin_id <= 0) return;
        if (in_cell) {
            cell_text += '\x01';
            cell_text += std::to_string(bin_id);
            cell_text += '\x01';
        } else {
            para.image_bin_ids.push_back(bin_id);
        }
    }

    // Read GSO (graphic shape object): SHAPE_COMPONENT + picture/OLE/container
    void read_gso(RecordCursor& cur, uint16_t ctrl_level, HWPParagraph& para,
                  bool in_cell, std::string& cell_text) {
        bool sc_pending = false;

        while (cur.has_next()) {
            auto r = cur.peek();
            if (r.level <= ctrl_level) break;

            if (r.tag == hwp::SHAPE_COMPONENT_PICTURE) {
                auto rec = cur.next();
                if (sc_pending) {
                    sc_pending = false;
                    sequential_bin_id_++;
                    int bin_id = 0;
                    if (rec.data && rec.size >= 73)
                        bin_id = util::read_u16_le(rec.data + 71);
                    if (bin_id == 0) bin_id = sequential_bin_id_;
                    emit_image(bin_id, in_cell, cell_text, para);
                }
            } else if (r.tag == hwp::SHAPE_COMPONENT_OLE) {
                auto rec = cur.next();
                if (sc_pending) {
                    sc_pending = false;
                    sequential_bin_id_++;
                    int bin_id = 0;
                    if (rec.data && rec.size >= 14)
                        bin_id = util::read_u16_le(rec.data + 12);
                    if (bin_id == 0) bin_id = sequential_bin_id_;
                    emit_image(bin_id, in_cell, cell_text, para);
                }
            } else if (r.tag == hwp::SHAPE_COMPONENT) {
                auto rec = cur.next();
                if (rec.data && rec.size >= 4) {
                    char sc_name[5] = {};
                    memcpy(sc_name, rec.data, 4);
                    bool is_pic = (std::string(sc_name) == "cip$" ||
                                   std::string(sc_name) == "$pic");
                    if (is_pic) {
                        // Try fillInfo extraction from SHAPE_COMPONENT data
                        int fill_bid = extract_shape_component_image(rec.data, rec.size);
                        if (fill_bid > 0) {
                            sequential_bin_id_++;
                            emit_image(fill_bid, in_cell, cell_text, para);
                            // Skip subsequent PICTURE/OLE records for this gso
                            sc_pending = false;
                        } else {
                            sc_pending = true; // Wait for child PICTURE/OLE record
                        }
                    }
                }
            } else if (r.tag == hwp::CTRL_DATA) {
                cur.skip();
            } else if (r.tag == hwp::LIST_HEADER) {
                auto lh = cur.next();
                uint16_t lh_level = lh.level;
                while (cur.has_next()) {
                    auto pk = cur.peek();
                    if (pk.level <= lh_level) break;
                    cur.skip();
                }
            } else if (r.tag == hwp::CTRL_HEADER) {
                // Flush pending image before nested control
                if (sc_pending) {
                    sc_pending = false;
                    sequential_bin_id_++;
                    emit_image(sequential_bin_id_, in_cell, cell_text, para);
                }
                auto pk = cur.peek();
                if (pk.data && pk.size >= 4) {
                    uint32_t nested_id = util::read_u32_le(pk.data);
                    char nc0 = (nested_id >> 24) & 0xFF;
                    char nc1 = (nested_id >> 16) & 0xFF;
                    char nc2 = (nested_id >> 8) & 0xFF;
                    char nc3 = nested_id & 0xFF;
                    std::string nested_str = {nc0, nc1, nc2, nc3};
                    uint16_t nested_level = pk.level;
                    cur.skip(); // consume CTRL_HEADER
                    if (nested_str == "gso ") {
                        read_gso(cur, nested_level, para, in_cell, cell_text);
                    } else {
                        skip_control(cur, nested_level);
                    }
                } else {
                    cur.skip();
                }
            } else {
                cur.skip();
            }
        }

        // Flush pending image at end of GSO
        if (sc_pending) {
            sc_pending = false;
            sequential_bin_id_++;
            emit_image(sequential_bin_id_, in_cell, cell_text, para);
        }
    }

    static bool is_hwp_printable(char16_t ch) {
        if (ch < 0x20) return ch == '\t' || ch == '\n';
        // Filter ranges that never appear in Korean documents:
        // 0x0100-0x02FF: Latin Extended / IPA / Spacing Modifiers (special font glyphs)
        // 0x0500-0x0EFF: Armenian..Lao (HWP special font mapped codes)
        // 0x0F00-0x0FFF: HWP internal codes
        if (ch >= 0x0100 && ch <= 0x02FF) return false;
        if (ch >= 0x0500 && ch <= 0x0FFF) return false;
        return true;
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
                if (is_hwp_printable(static_cast<char16_t>(code)))
                    para.text.push_back(static_cast<char16_t>(code));
                else
                    para.text.push_back(0);
                char_pos++;
                break;

            case hwp::HWPCharType::ControlChar:
                if (code == 13 || code == 10) {
                    para.text.push_back(u'\n');
                } else if (code == 9) {
                    para.text.push_back(u'\t');
                } else {
                    para.text.push_back(0);  // placeholder
                }
                char_pos++;
                break;

            case hwp::HWPCharType::ControlExtend:
                // 16 bytes total: 2 (code) + 12 (addition) + 2 (code)
                para.extend_positions.push_back(char_pos);
                if (off + 14 <= size) {
                    off += 14;  // skip 12 addition bytes + 2 trailing code bytes
                }
                // Add placeholders to keep para.text in sync with char_pos
                for (int k = 0; k < 8; k++) para.text.push_back(0);
                char_pos += 8;
                break;

            case hwp::HWPCharType::ControlInline:
                // 16 bytes total like ControlExtend
                if (off + 14 <= size) {
                    off += 14;
                }
                for (int k = 0; k < 8; k++) para.text.push_back(0);
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
    std::string format_table(const HWPTable& table, PageChunk& chunk, int& image_idx) {
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
                // Resolve inline image markers (\x01{bid}\x01) before escaping
                std::string resolved;
                resolved.reserve(cell.text.size());
                for (size_t i = 0; i < cell.text.size(); i++) {
                    if (cell.text[i] == '\x01') {
                        size_t end = cell.text.find('\x01', i + 1);
                        if (end != std::string::npos) {
                            int bid = std::atoi(cell.text.substr(i + 1, end - i - 1).c_str());
                            i = end;
                            if (bid > 0) {
                                std::string img_md = format_image(bid, chunk, image_idx);
                                while (!img_md.empty() && img_md.back() == '\n')
                                    img_md.pop_back();
                                if (!resolved.empty() && resolved.back() != ' ')
                                    resolved += ' ';
                                resolved += img_md;
                            }
                        }
                    } else {
                        resolved += cell.text[i];
                    }
                }
                std::string content = util::escape_cell(resolved);
                // Replace \x02 line break markers with <br>
                for (size_t p = 0; p < content.size(); p++) {
                    if (content[p] == '\x02')
                        content.replace(p, 1, "<br>");
                }
                grid[cell.row_addr][cell.col_addr] = content;
            }
        }

        // Trim trailing empty columns
        int used_cols = table.col_count;
        while (used_cols > 1) {
            bool col_empty = true;
            for (int r = 0; r < table.row_count; r++) {
                if (!grid[r][used_cols - 1].empty()) {
                    col_empty = false;
                    break;
                }
            }
            if (!col_empty) break;
            used_cols--;
        }

        std::string md;
        for (int r = 0; r < table.row_count; r++) {
            md += "|";
            for (int c = 0; c < used_cols; c++) {
                md += " " + grid[r][c] + " |";
            }
            md += "\n";

            if (r == 0) {
                md += "|";
                for (int c = 0; c < used_cols; c++) {
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
        // Primary: look up via bin_data_refs table
        if (bin_id > 0 && bin_id <= (int)doc_info_.bin_data_refs.size()) {
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
        }

        // Fallback: match by index in insertion order
        if (bin_id > 0 && bin_id <= (int)embedded_image_keys_.size()) {
            auto fit = embedded_images_.find(embedded_image_keys_[bin_id - 1]);
            if (fit != embedded_images_.end()) return &fit->second;
        }
        return nullptr;
    }

    std::string format_image(int bin_id, PageChunk& chunk, int& image_idx) {
        auto* entry = find_image_entry(bin_id);
        if (!entry) return "";

        std::string ext = "jpg";
        if (bin_id > 0 && bin_id <= (int)doc_info_.bin_data_refs.size()) {
            auto& ref = doc_info_.bin_data_refs[bin_id - 1];
            if (!ref.extension.empty()) ext = ref.extension;
        }

        std::string unified = "page" + std::to_string(chunk.page_number)
                             + "_img" + std::to_string(image_idx);
        std::string filename = unified + "." + ext;

        // When images not requested, emit embedded reference without loading data
        if (!opts_.extract_images) {
            image_idx++;
            return "![" + unified + "](embedded:" + unified + ")\n\n";
        }

        // Detect format from file extension in OLE name
        {
            auto dot = entry->name.rfind('.');
            if (dot != std::string::npos) {
                std::string name_ext = entry->name.substr(dot + 1);
                for (auto& c : name_ext) c = std::tolower(c);
                if (name_ext == "png" || name_ext == "jpg" || name_ext == "jpeg" ||
                    name_ext == "gif" || name_ext == "bmp" || name_ext == "emf")
                    ext = name_ext;
            }
        }

        // Read raw OLE stream
        std::string stream_path = "BinData/" + entry->name;
        if (!ole_) return "";

        auto raw = ole_->read_stream(stream_path);
        if (raw.empty()) return "";

        bool compressed = true;
        if (bin_id > 0 && bin_id <= (int)doc_info_.bin_data_refs.size())
            compressed = doc_info_.bin_data_refs[bin_id - 1].compress != 2;

        // Decompress, detect format, save to disk or hold in memory
        std::vector<uint8_t> image_data;
        if (compressed) {
            image_data = decompress(
                reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
            if (image_data.empty())
                image_data.assign(raw.begin(), raw.end());
        } else {
            image_data.assign(raw.begin(), raw.end());
        }
        raw.clear(); raw.shrink_to_fit();

        // Detect actual format from magic bytes
        std::string actual_fmt = util::detect_image_format(
            image_data.data(), image_data.size());
        if (!actual_fmt.empty()) ext = actual_fmt;

        filename = unified + "." + (ext == "jpeg" ? "jpg" : ext);
        std::string saved_path;
        if (!opts_.image_output_dir.empty()) {
            util::ensure_dir(opts_.image_output_dir);
            saved_path = opts_.image_output_dir + "/" + filename;
            std::ofstream ofs(saved_path, std::ios::binary);
            if (!ofs) return "";
            ofs.write(reinterpret_cast<const char*>(image_data.data()),
                      image_data.size());
        }

        ImageData idata;
        idata.page_number = chunk.page_number;
        idata.name = unified;
        idata.format = ext;
        idata.saved_path = saved_path;
        if (opts_.image_output_dir.empty()) {
            idata.data.assign(reinterpret_cast<const char*>(image_data.data()),
                              reinterpret_cast<const char*>(image_data.data()) + image_data.size());
        }
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
                result += format_table(tbl, chunk, image_idx);
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
                char16_t ch = para.text[i];
                if (!is_hwp_printable(ch)) continue;
                util::append_utf8(span.text, ch);
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
                    char16_t ch = para.text[pos];
                    if (!is_hwp_printable(ch)) continue;
                    util::append_utf8(span.text, ch);
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
