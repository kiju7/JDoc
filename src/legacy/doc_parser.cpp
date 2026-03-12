// .doc (Word Binary) parser implementation.

#include "doc_parser.h"
#include "common/string_utils.h"
#include "common/binary_utils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>
#include <sstream>

namespace jdoc {

// ---------- DocParser --------------------------------------------------------

DocParser::DocParser(OleReader& ole) : ole_(ole) {}

// ---------- text extraction --------------------------------------------------

std::string DocParser::extract_text() {
    std::vector<char> word_doc = ole_.read_stream("WordDocument");
    if (word_doc.size() < 0x60) return "";

    // Parse FIB header.
    uint16_t flags = util::read_u16_le(word_doc.data() + 0x0A);
    bool use_1table = (flags >> 9) & 1;
    std::string table_name = use_1table ? "1Table" : "0Table";

    std::vector<char> table_stream = ole_.read_stream(table_name);
    if (table_stream.empty()) return "";

    // Locate fcClx/lcbClx — the Clx (piece table) position in the table stream.
    // We try multiple methods because the FIB layout varies across Word versions.
    uint32_t fcClx = 0, lcbClx = 0;

    // Helper: validate a candidate fcClx/lcbClx pair.
    // The Clx structure begins with optional Prc (0x01) entries followed by Pcdt (0x02).
    auto valid_clx = [&](uint32_t fc, uint32_t lcb) -> bool {
        if (fc == 0 || lcb == 0) return false;
        if (static_cast<size_t>(fc) + lcb > table_stream.size()) return false;
        // Walk past any Prc (0x01) entries, then expect Pcdt (0x02).
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

    // Method 1: absolute FIB offset 0x01A2 (MS-DOC §2.5.6, Word 97–2007+).
    if (0x01A2 + 8 <= word_doc.size()) {
        uint32_t fc  = util::read_u32_le(word_doc.data() + 0x01A2);
        uint32_t lcb = util::read_u32_le(word_doc.data() + 0x01A2 + 4);
        if (valid_clx(fc, lcb)) { fcClx = fc; lcbClx = lcb; }
    }

    // Method 2: navigate FIB variable-length sections (non-standard layouts).
    if (fcClx == 0 && word_doc.size() >= 0x22) {
        uint16_t csw = util::read_u16_le(word_doc.data() + 0x20);
        size_t pos = 0x22 + csw * 2;
        if (pos + 2 <= word_doc.size()) {
            uint16_t cslw = util::read_u16_le(word_doc.data() + pos);
            pos += 2 + cslw * 4;
        }
        if (pos + 2 <= word_doc.size()) {
            uint16_t cbRgFcLcb = util::read_u16_le(word_doc.data() + pos);
            pos += 2;
            // Try index 65 and 66 (common positions for fcClx).
            for (int idx : {65, 66}) {
                size_t off = pos + idx * 8;
                if (off + 8 > word_doc.size() || idx >= cbRgFcLcb) continue;
                uint32_t fc  = util::read_u32_le(word_doc.data() + off);
                uint32_t lcb = util::read_u32_le(word_doc.data() + off + 4);
                if (valid_clx(fc, lcb)) { fcClx = fc; lcbClx = lcb; break; }
            }
        }
    }

    if (fcClx == 0 || lcbClx == 0) return "";

    // Parse Clx structure.
    const char* clx = table_stream.data() + fcClx;
    size_t clx_end = lcbClx;
    size_t clx_pos = 0;

    // Skip any Prc entries (type 0x01).
    while (clx_pos < clx_end && static_cast<uint8_t>(clx[clx_pos]) == 0x01) {
        if (clx_pos + 3 > clx_end) return "";
        uint16_t prc_len = util::read_u16_le(clx + clx_pos + 1);
        clx_pos += 3 + prc_len;
    }

    // Must be Pcdt (type 0x02).
    if (clx_pos >= clx_end || static_cast<uint8_t>(clx[clx_pos]) != 0x02) return "";
    clx_pos += 1;

    if (clx_pos + 4 > clx_end) return "";
    uint32_t lcb = util::read_u32_le(clx + clx_pos);
    clx_pos += 4;

    // PlcPcd: n+1 CPs (uint32 each) + n PCDs (8 bytes each).
    // lcb = (n+1)*4 + n*8 = 12n + 4 => n = (lcb - 4) / 12
    if (lcb < 4) return "";
    uint32_t n = (lcb - 4) / 12;
    if (n == 0) return "";

    const char* plcpcd = clx + clx_pos;
    size_t plcpcd_size = (n + 1) * 4 + n * 8;
    if (clx_pos + plcpcd_size > clx_end) return "";

    // Read CPs.
    std::vector<uint32_t> cps(n + 1);
    for (uint32_t i = 0; i <= n; ++i) {
        cps[i] = util::read_u32_le(plcpcd + i * 4);
    }

    // Read PCDs.
    const char* pcds = plcpcd + (n + 1) * 4;

    std::string result;

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t char_count = cps[i + 1] - cps[i];
        if (char_count == 0) continue;

        // PCD: 2 bytes (unused) + 4 bytes (fc) + 2 bytes (prm)
        uint32_t fc_raw = util::read_u32_le(pcds + i * 8 + 2);

        bool compressed = (fc_raw & 0x40000000) != 0;
        uint32_t byte_offset = fc_raw & 0x3FFFFFFF;

        if (compressed) {
            // CP1252 encoded, 1 byte per character.
            byte_offset /= 2;
            if (static_cast<size_t>(byte_offset) + char_count > word_doc.size()) continue;
            for (uint32_t j = 0; j < char_count; ++j) {
                uint8_t ch = static_cast<uint8_t>(word_doc[byte_offset + j]);
                if (ch == 0x0D) {
                    result.push_back('\n');
                } else if (ch == 0x07) {
                    result.push_back('\t'); // cell/row mark
                } else if (ch >= 0x20 || ch == '\t' || ch == '\n') {
                    result += util::cp1252_to_utf8(ch);
                }
                // else: filter out control characters
            }
        } else {
            // UTF-16LE, 2 bytes per character.
            size_t byte_len = static_cast<size_t>(char_count) * 2;
            if (static_cast<size_t>(byte_offset) + byte_len > word_doc.size()) continue;
            const char* src = word_doc.data() + byte_offset;
            for (uint32_t j = 0; j < char_count; ++j) {
                uint16_t ch = util::read_u16_le(src + j * 2);
                if (ch == 0x000D) {
                    result.push_back('\n');
                } else if (ch == 0x0007) {
                    result.push_back('\t');
                } else if (ch >= 0x20 || ch == '\t' || ch == '\n') {
                    util::append_utf8(result, ch);
                }
            }
        }
    }

    return result;
}

// ---------- text to markdown -------------------------------------------------

std::string DocParser::text_to_markdown(const std::string& raw_text) {
    // Split into lines.
    std::vector<std::string> lines;
    std::istringstream iss(raw_text);
    std::string line;
    while (std::getline(iss, line)) {
        // Trim trailing \r.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0'))
            line.pop_back();
        lines.push_back(line);
    }

    std::string md;
    md.reserve(raw_text.size() * 2);

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& ln = lines[i];
        if (ln.empty()) {
            md.push_back('\n');
            continue;
        }

        // Check for bullet characters.
        bool is_bullet = false;
        std::string content = ln;
        if (!ln.empty()) {
            // Check for common bullet characters. UTF-8 for bullet is \xE2\x80\xA2.
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
        }

        if (is_bullet) {
            md += "- " + content + "\n";
            continue;
        }

        // Check for ordered list: starts with digit(s) followed by period and space.
        {
            size_t j = 0;
            while (j < ln.size() && ln[j] >= '0' && ln[j] <= '9') ++j;
            if (j > 0 && j < ln.size() && ln[j] == '.') {
                std::string prefix = ln.substr(0, j + 1);
                std::string rest = (j + 1 < ln.size()) ? ln.substr(j + 1) : "";
                while (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
                md += prefix + " " + rest + "\n";
                continue;
            }
        }

        // Heading heuristic: short non-empty line followed by longer text.
        bool is_heading = false;
        if (ln.size() < 80 && ln.size() > 0) {
            // Look ahead for the next non-empty line.
            for (size_t k = i + 1; k < lines.size() && k <= i + 3; ++k) {
                if (lines[k].empty()) continue;
                if (lines[k].size() > ln.size()) {
                    is_heading = true;
                }
                break;
            }
        }

        if (is_heading) {
            md += "## " + ln + "\n\n";
        } else {
            md += ln + "\n";
        }
    }

    return md;
}

// ---------- image detection helpers ------------------------------------------

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
        // Check for "EMF" signature at offset 40.
        if (d[40] == ' ' && d[41] == 'E' && d[42] == 'M' && d[43] == 'F') return "emf";
    }

    return "";
}

size_t DocParser::find_jpeg_end(const char* data, size_t len) {
    // Scan for FFD9 (end of image marker).
    for (size_t i = 2; i + 1 < len; ++i) {
        if (static_cast<uint8_t>(data[i]) == 0xFF &&
            static_cast<uint8_t>(data[i + 1]) == 0xD9) {
            return i + 2;
        }
    }
    return len; // Fallback: entire buffer.
}

size_t DocParser::find_png_end(const char* data, size_t len) {
    // Scan for IEND chunk: 4 bytes length + "IEND" + 4 bytes CRC.
    const uint8_t iend_sig[4] = {0x49, 0x45, 0x4E, 0x44};
    for (size_t i = 8; i + 7 < len; ++i) {
        if (std::memcmp(data + i + 4, iend_sig, 4) == 0) {
            // IEND chunk: length (4 bytes, should be 0) + IEND (4) + CRC (4) = 12 bytes total.
            return i + 12;
        }
    }
    return len;
}

// ---------- image extraction -------------------------------------------------

std::vector<ImageData> DocParser::extract_images() {
    std::vector<ImageData> images;

    // Try "Data" stream first, then fall back to scanning "WordDocument".
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
            // For other formats, try to use a reasonable heuristic.
            // Look for the next recognizable image or limit to 10MB.
            size_t max_size = std::min(stream.size() - img_start, size_t(10 * 1024 * 1024));
            img_end = img_start + max_size;
        }

        size_t img_size = img_end - img_start;
        if (img_size > 16) { // Skip tiny matches (likely false positives).
            ImageData img;
            img.page_number = 1;
            img.name = "image_" + std::to_string(++img_idx);
            img.format = fmt;
            img.data.assign(stream.begin() + img_start, stream.begin() + img_end);
            images.push_back(std::move(img));
        }

        pos = img_end;
    }

    // Also check for OfficeArt BLIP records in the "Data" stream.
    // Look for BLIP record types: 0xF01D (JPEG), 0xF01E (PNG), etc.
    if (ole_.has_stream("Data")) {
        const auto& data = ole_.read_stream("Data");
        pos = 0;
        while (pos + 8 < data.size()) {
            uint16_t rec_ver_inst = util::read_u16_le(data.data() + pos);
            uint16_t rec_type     = util::read_u16_le(data.data() + pos + 2);
            uint32_t rec_len      = util::read_u32_le(data.data() + pos + 4);

            if (rec_type >= 0xF01A && rec_type <= 0xF029 && rec_len > 17 && pos + 8 + rec_len <= data.size()) {
                size_t header_skip = 17; // 1 byte win32 + 16 bytes UID
                uint16_t inst = rec_ver_inst >> 4;
                // If two UIDs (inst indicates that), add 16 more.
                if (rec_type == 0xF01D || rec_type == 0xF01E || rec_type == 0xF01F || rec_type == 0xF029) {
                    // Check if this has 2 UIDs by looking at recInstance.
                    if (inst == 0x46B || inst == 0x6E1 || inst == 0x6E3 || inst == 0x6E5) {
                        header_skip = 33; // 1 + 16 + 16
                    }
                }

                if (rec_len > header_skip) {
                    std::string fmt;
                    if (rec_type == 0xF01D) fmt = "jpeg";
                    else if (rec_type == 0xF01E) fmt = "png";
                    else if (rec_type == 0xF01A) fmt = "emf";
                    else if (rec_type == 0xF01B) fmt = "wmf";
                    else if (rec_type == 0xF01F) fmt = "bmp";
                    else if (rec_type == 0xF029) fmt = "tiff";

                    if (!fmt.empty()) {
                        size_t img_offset = pos + 8 + header_skip;
                        size_t img_size = rec_len - header_skip;

                        // Avoid duplicates: check if we already found this image.
                        bool duplicate = false;
                        for (const auto& existing : images) {
                            if (existing.data.size() == img_size && img_offset + img_size <= data.size()) {
                                if (std::memcmp(existing.data.data(), data.data() + img_offset,
                                                std::min(size_t(64), img_size)) == 0) {
                                    duplicate = true;
                                    break;
                                }
                            }
                        }

                        if (!duplicate && img_offset + img_size <= data.size()) {
                            ImageData img;
                            img.page_number = 1;
                            img.name = "image_" + std::to_string(++img_idx);
                            img.format = fmt;
                            img.data.assign(data.begin() + img_offset,
                                            data.begin() + img_offset + img_size);
                            images.push_back(std::move(img));
                        }
                    }
                }
                pos += 8 + rec_len;
            } else {
                ++pos;
            }
        }
    }

    return images;
}

// ---------- public API -------------------------------------------------------

std::string DocParser::to_markdown(const ConvertOptions& opts) {
    std::string raw = extract_text();
    std::string md = text_to_markdown(raw);

    if (opts.extract_images) {
        auto images = extract_images();
        if (!images.empty()) {
            md += "\n\n---\n\n";
            for (size_t i = 0; i < images.size(); ++i) {
                md += "![" + images[i].name + "](" + images[i].name + "." + images[i].format + ")\n\n";
            }
        }
    }

    return md;
}

std::vector<PageChunk> DocParser::to_chunks(const ConvertOptions& opts) {
    std::vector<PageChunk> chunks;

    PageChunk chunk;
    chunk.page_number = 1;

    std::string raw = extract_text();
    chunk.text = text_to_markdown(raw);

    if (opts.extract_images) {
        chunk.images = extract_images();
    }

    // Estimate page dimensions (Letter size in points).
    chunk.page_width = 612.0;
    chunk.page_height = 792.0;
    chunk.body_font_size = 12.0;

    chunks.push_back(std::move(chunk));
    return chunks;
}

} // namespace jdoc
