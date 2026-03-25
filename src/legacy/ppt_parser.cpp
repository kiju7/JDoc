// .ppt (PowerPoint Binary) parser implementation.

#include "ppt_parser.h"
#include "common/string_utils.h"
#include "common/binary_utils.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <zlib.h>

namespace jdoc {

// PPT record types from MS-PPT specification.
static constexpr uint16_t RT_DOCUMENT              = 0x03E8;  // 1000
static constexpr uint16_t RT_SLIDE_LIST_WITH_TEXT   = 0x0FF0;  // 4080
static constexpr uint16_t RT_SLIDE_PERSIST_ATOM     = 0x03F3;  // 1011
static constexpr uint16_t RT_TEXT_HEADER_ATOM       = 0x0F9F;  // 3999
static constexpr uint16_t RT_TEXT_CHARS_ATOM        = 0x0FA0;  // 4000 (UTF-16LE)
static constexpr uint16_t RT_TEXT_BYTES_ATOM        = 0x0FA8;  // 4008 (single-byte)

// ---------- helpers ----------------------------------------------------------

static uint16_t rd16(const char* p) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    return uint16_t(b[0]) | (uint16_t(b[1]) << 8);
}

static uint32_t rd32(const char* p) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}

// ---------- PptParser --------------------------------------------------------

PptParser::PptParser(OleReader& ole) : ole_(ole) {
    parse_document();
}

// ---------- document parsing -------------------------------------------------

void PptParser::parse_document() {
    std::vector<char> stream = ole_.read_stream("PowerPoint Document");
    if (stream.empty()) return;

    // Linear scan through all records.
    // Track SlideListWithText containers: first = slides, second = notes.
    // Collect text atoms associated with each slide/note.

    int slide_list_count = 0;  // 0=not yet, 1=in slides list, 2=in notes list
    bool in_slide_list = false;
    bool in_notes_list = false;

    int slide_idx = -1;
    int note_idx = -1;
    uint32_t text_type = 0xFF; // From TextHeaderAtom: 0=title, 1=body, etc.

    // Helper to decode text from TextCharsAtom or TextBytesAtom
    auto decode_text_chars = [](const char* data, size_t len) -> std::string {
        return util::utf16le_to_utf8(data, len);
    };
    auto decode_text_bytes = [](const char* data, size_t len) -> std::string {
        std::string text;
        for (size_t i = 0; i < len; ++i) {
            uint8_t ch = static_cast<uint8_t>(data[i]);
            if (ch == 0x0D) text.push_back('\n');
            else if (ch >= 0x20 || ch == '\t' || ch == '\n')
                text += util::cp1252_to_utf8(ch);
        }
        return text;
    };

    size_t pos = 0;
    while (pos + 8 <= stream.size()) {
        uint16_t ver_inst = rd16(stream.data() + pos);
        uint16_t rec_type = rd16(stream.data() + pos + 2);
        uint32_t rec_len  = rd32(stream.data() + pos + 4);

        uint8_t ver = ver_inst & 0x0F;
        bool is_container = (ver == 0x0F);

        if (is_container) {
            if (rec_type == RT_SLIDE_LIST_WITH_TEXT) {
                ++slide_list_count;
                if (slide_list_count == 1) {
                    in_slide_list = true;
                    in_notes_list = false;
                } else if (slide_list_count == 2) {
                    in_slide_list = false;
                    in_notes_list = true;
                    note_idx = -1;
                }
            }
            pos += 8;
            continue;
        }

        // Atom record.
        size_t atom_end = pos + 8 + rec_len;
        if (atom_end > stream.size()) break;
        const char* atom_data = stream.data() + pos + 8;

        if (in_slide_list) {
            if (rec_type == RT_SLIDE_PERSIST_ATOM) {
                slides_.emplace_back();
                slide_idx = static_cast<int>(slides_.size()) - 1;
                slides_[slide_idx].number = slide_idx + 1;
                text_type = 0xFF;
            } else if (rec_type == RT_TEXT_HEADER_ATOM && rec_len >= 4) {
                text_type = rd32(atom_data);
            } else if (rec_type == RT_TEXT_CHARS_ATOM && slide_idx >= 0) {
                std::string text = decode_text_chars(atom_data, rec_len);
                if (text_type == 0) {
                    if (!slides_[slide_idx].title.empty())
                        slides_[slide_idx].title += " ";
                    slides_[slide_idx].title += text;
                } else {
                    if (!slides_[slide_idx].body.empty())
                        slides_[slide_idx].body += "\n";
                    slides_[slide_idx].body += text;
                }
                text_type = 0xFF;
            } else if (rec_type == RT_TEXT_BYTES_ATOM && slide_idx >= 0) {
                std::string text = decode_text_bytes(atom_data, rec_len);
                if (text_type == 0) {
                    if (!slides_[slide_idx].title.empty())
                        slides_[slide_idx].title += " ";
                    slides_[slide_idx].title += text;
                } else {
                    if (!slides_[slide_idx].body.empty())
                        slides_[slide_idx].body += "\n";
                    slides_[slide_idx].body += text;
                }
                text_type = 0xFF;
            }
        } else if (in_notes_list) {
            if (rec_type == RT_SLIDE_PERSIST_ATOM) {
                ++note_idx;
                text_type = 0xFF;
            } else if (rec_type == RT_TEXT_HEADER_ATOM && rec_len >= 4) {
                text_type = rd32(atom_data);
            } else if ((rec_type == RT_TEXT_CHARS_ATOM || rec_type == RT_TEXT_BYTES_ATOM) &&
                       note_idx >= 0 && note_idx < static_cast<int>(slides_.size())) {
                std::string text = (rec_type == RT_TEXT_CHARS_ATOM)
                    ? decode_text_chars(atom_data, rec_len)
                    : decode_text_bytes(atom_data, rec_len);
                // Skip title placeholders in notes (slide number text)
                if (text_type != 0 && !text.empty()) {
                    if (!slides_[note_idx].notes.empty())
                        slides_[note_idx].notes += "\n";
                    slides_[note_idx].notes += text;
                }
                text_type = 0xFF;
            }
        }

        pos = atom_end;
    }

    // If no text was found in SlideListWithText, do a full scan.
    // This handles PPTs where text is in individual Slide containers, not in SLT.
    bool has_any_text = false;
    for (auto& s : slides_) {
        if (!s.title.empty() || !s.body.empty()) { has_any_text = true; break; }
    }
    if (!has_any_text) {
        slides_.clear();
        pos = 0;
        Slide current_slide;
        current_slide.number = 1;
        text_type = 0xFF;

        while (pos + 8 <= stream.size()) {
            uint16_t ver_inst = rd16(stream.data() + pos);
            uint16_t rec_type = rd16(stream.data() + pos + 2);
            uint32_t rec_len  = rd32(stream.data() + pos + 4);
            uint8_t ver = ver_inst & 0x0F;

            if (ver == 0x0F) {
                pos += 8;
                continue;
            }

            size_t atom_end = pos + 8 + rec_len;
            if (atom_end > stream.size()) break;
            const char* atom_data = stream.data() + pos + 8;

            if (rec_type == RT_TEXT_HEADER_ATOM && rec_len >= 4) {
                text_type = rd32(atom_data);
            } else if (rec_type == RT_TEXT_CHARS_ATOM && rec_len > 0) {
                std::string text = util::utf16le_to_utf8(atom_data, rec_len);
                if (text_type == 0) {
                    if (!current_slide.title.empty()) {
                        slides_.push_back(current_slide);
                        current_slide = Slide{};
                        current_slide.number = static_cast<int>(slides_.size()) + 1;
                    }
                    current_slide.title = text;
                } else {
                    if (!current_slide.body.empty()) current_slide.body += "\n";
                    current_slide.body += text;
                }
                text_type = 0xFF;
            } else if (rec_type == RT_TEXT_BYTES_ATOM && rec_len > 0) {
                std::string text;
                for (size_t i = 0; i < rec_len; ++i) {
                    uint8_t ch = static_cast<uint8_t>(atom_data[i]);
                    if (ch == 0x0D) text.push_back('\n');
                    else if (ch >= 0x20 || ch == '\t' || ch == '\n')
                        text += util::cp1252_to_utf8(ch);
                }
                if (text_type == 0) {
                    if (!current_slide.title.empty()) {
                        slides_.push_back(current_slide);
                        current_slide = Slide{};
                        current_slide.number = static_cast<int>(slides_.size()) + 1;
                    }
                    current_slide.title = text;
                } else {
                    if (!current_slide.body.empty()) current_slide.body += "\n";
                    current_slide.body += text;
                }
                text_type = 0xFF;
            }

            pos = atom_end;
        }

        if (!current_slide.title.empty() || !current_slide.body.empty()) {
            slides_.push_back(current_slide);
        }
    }
}

// ---------- slide to markdown ------------------------------------------------

std::string PptParser::slide_to_markdown(const Slide& slide) {
    std::ostringstream out;

    out << "--- Page " << slide.number << " ---\n\n";

    if (!slide.title.empty()) {
        out << "# " << slide.title << "\n\n";
    }

    if (!slide.body.empty()) {
        // Split body into lines and format.
        std::istringstream iss(slide.body);
        std::string line;
        while (std::getline(iss, line)) {
            // Trim trailing whitespace.
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();

            if (line.empty()) {
                out << "\n";
            } else {
                // Check for bullet.
                bool is_bullet = false;
                if (!line.empty()) {
                    char first = line[0];
                    if ((first == '-' || first == '*') && line.size() > 1 &&
                        (line[1] == ' ' || line[1] == '\t')) {
                        is_bullet = true;
                    }
                    // UTF-8 bullet: E2 80 A2
                    if (line.size() >= 3 &&
                        static_cast<uint8_t>(line[0]) == 0xE2 &&
                        static_cast<uint8_t>(line[1]) == 0x80 &&
                        static_cast<uint8_t>(line[2]) == 0xA2) {
                        is_bullet = true;
                        line = line.substr(3);
                        while (!line.empty() && line[0] == ' ') line = line.substr(1);
                    }
                }
                if (is_bullet) {
                    out << "- " << line << "\n";
                } else {
                    out << line << "\n";
                }
            }
        }
    }

    // Append notes if present
    if (!slide.notes.empty()) {
        out << "\n**Notes:**\n";
        std::istringstream niss(slide.notes);
        std::string nline;
        while (std::getline(niss, nline)) {
            while (!nline.empty() && (nline.back() == '\r' || nline.back() == ' '))
                nline.pop_back();
            if (!nline.empty()) out << nline << "\n";
        }
    }

    out << "\n";
    return out.str();
}

// ---------- image extraction -------------------------------------------------

std::vector<ImageData> PptParser::extract_images() {
    std::vector<ImageData> images;

    if (!ole_.has_stream("Pictures")) return images;

    std::vector<char> pics = ole_.read_stream("Pictures");
    if (pics.empty()) return images;

    int img_idx = 0;
    size_t pos = 0;

    while (pos + 8 <= pics.size()) {
        uint16_t ver_inst = rd16(pics.data() + pos);
        uint16_t rec_type = rd16(pics.data() + pos + 2);
        uint32_t rec_len  = rd32(pics.data() + pos + 4);

        if (rec_len == 0 || pos + 8 + rec_len > pics.size()) {
            pos += 8;
            continue;
        }

        size_t blip_start = pos + 8;
        size_t blip_end = blip_start + rec_len;

        std::string fmt;
        size_t header_skip = 0;
        bool is_metafile = false;

        switch (rec_type) {
            case 0xF01A: // EMF
                fmt = "emf";
                header_skip = 50;
                is_metafile = true;
                break;
            case 0xF01B: // WMF
                fmt = "wmf";
                header_skip = 50;
                is_metafile = true;
                break;
            case 0xF01C: // PICT
                header_skip = 50;
                is_metafile = true;
                break;
            case 0xF01D: // JPEG
                fmt = "jpeg";
                header_skip = 17;
                break;
            case 0xF01E: // PNG
                fmt = "png";
                header_skip = 17;
                break;
            case 0xF01F: // DIB
                fmt = "bmp";
                header_skip = 17;
                break;
            case 0xF029: // TIFF
                fmt = "tiff";
                header_skip = 17;
                break;
            default:
                // Not a BLIP; skip.
                pos = blip_end;
                continue;
        }

        // Check for 2 UIDs: if recInstance indicates it, add 16 more bytes.
        uint16_t inst = ver_inst >> 4;
        if (!is_metafile) {
            // For non-metafile BLIPs, the recInstance distinguishes 1-UID vs 2-UID.
            // Standard instances: JPEG=0x46A(1UID)/0x46B(2UID), PNG=0x6E0/0x6E1, etc.
            if (inst == 0x46B || inst == 0x6E1 || inst == 0x6E3 || inst == 0x6E5) {
                header_skip += 16;
            }
        } else {
            // Metafile BLIPs: check similarly.
            // EMF: 0x3D4(1UID)/0x3D5(2UID), WMF: 0x216/0x217
            if (inst == 0x3D5 || inst == 0x217) {
                header_skip += 16;
            }
        }

        if (!fmt.empty() && rec_len > header_skip) {
            size_t img_offset = blip_start + header_skip;
            size_t img_size = rec_len - header_skip;

            if (img_offset + img_size <= pics.size()) {
                std::vector<char> img_data;

                if (is_metafile) {
                    // Metafile BLIPs are zlib-compressed. Try to decompress.
                    // The compressed data may have a decompressed size stored in the
                    // BLIP header at offset 34 (4 bytes, LE).
                    uLong decomp_size = 0;
                    if (header_skip >= 38) {
                        decomp_size = rd32(pics.data() + blip_start + 34);
                    }
                    if (decomp_size == 0 || decomp_size > 100 * 1024 * 1024) {
                        decomp_size = img_size * 10; // Estimate.
                    }

                    img_data.resize(decomp_size);
                    uLong actual_size = decomp_size;
                    int zret = uncompress(
                        reinterpret_cast<Bytef*>(img_data.data()), &actual_size,
                        reinterpret_cast<const Bytef*>(pics.data() + img_offset), img_size);

                    if (zret == Z_OK) {
                        img_data.resize(actual_size);
                    } else {
                        // Decompression failed; store raw data.
                        img_data.assign(pics.begin() + img_offset,
                                        pics.begin() + img_offset + img_size);
                    }
                } else {
                    img_data.assign(pics.begin() + img_offset,
                                    pics.begin() + img_offset + img_size);
                }

                ImageData img;
                img.page_number = img_idx + 1; // Approximate slide mapping.
                img.name = "slide_image_" + std::to_string(++img_idx);
                img.format = fmt;
                img.data = std::move(img_data);
                images.push_back(std::move(img));
            }
        }

        pos = blip_end;
    }

    return images;
}

// ---------- public API -------------------------------------------------------

std::string PptParser::to_markdown(const ConvertOptions& opts) {
    std::string md;

    for (const auto& slide : slides_) {
        // Check if this slide is in the requested pages.
        if (!opts.pages.empty()) {
            bool found = false;
            for (int p : opts.pages) {
                if (p == slide.number) { found = true; break; }
            }
            if (!found) continue;
        }
        md += slide_to_markdown(slide);
    }

    if (opts.extract_images) {
        auto images = extract_images();
        for (const auto& img : images) {
            md += "![" + img.name + "](" + img.name + "." + img.format + ")\n\n";
        }
    }

    return md;
}

std::vector<PageChunk> PptParser::to_chunks(const ConvertOptions& opts) {
    std::vector<PageChunk> chunks;
    auto images = opts.extract_images ? extract_images() : std::vector<ImageData>{};

    for (const auto& slide : slides_) {
        if (!opts.pages.empty()) {
            bool found = false;
            for (int p : opts.pages) {
                if (p == slide.number) { found = true; break; }
            }
            if (!found) continue;
        }

        PageChunk chunk;
        chunk.page_number = slide.number;
        chunk.text = slide_to_markdown(slide);
        chunk.page_width = 720.0;   // Standard PPT width in points.
        chunk.page_height = 540.0;  // Standard PPT height in points.
        chunk.body_font_size = 24.0;

        // Attach images that map to this slide.
        for (const auto& img : images) {
            if (img.page_number == slide.number) {
                chunk.images.push_back(img);
            }
        }

        chunks.push_back(std::move(chunk));
    }

    return chunks;
}

} // namespace jdoc
