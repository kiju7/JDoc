// .ppt (PowerPoint Binary) parser implementation.
// Architecture: UserEditAtom chain → persistence directory → classify slides by
// record type (0x3EE=slide, 0x3F8=master, 0x3F0=notes) → extract text per slide.

#include "ppt_parser.h"
#include "common/string_utils.h"
#include "common/binary_utils.h"
#include "common/file_utils.h"
#include "common/image_utils.h"
#include "common/png_encode.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <zlib.h>

namespace jdoc {

// ── PPT record types (MS-PPT specification) ─────────────────

static constexpr uint16_t RT_DOCUMENT               = 0x03E8;
static constexpr uint16_t RT_DOCUMENT_ATOM           = 0x03E9;
static constexpr uint16_t RT_SLIDE                   = 0x03EE;
static constexpr uint16_t RT_SLIDE_ATOM              = 0x03EF;
static constexpr uint16_t RT_NOTES                   = 0x03F0;
static constexpr uint16_t RT_SLIDE_PERSIST_ATOM      = 0x03F3;
static constexpr uint16_t RT_MAIN_MASTER             = 0x03F8;
static constexpr uint16_t RT_SLIDE_LIST_WITH_TEXT    = 0x0FF0;
static constexpr uint16_t RT_TEXT_HEADER_ATOM        = 0x0F9F;
static constexpr uint16_t RT_TEXT_CHARS_ATOM         = 0x0FA0;
static constexpr uint16_t RT_TEXT_BYTES_ATOM         = 0x0FA8;
static constexpr uint16_t RT_OUTLINE_TEXT_REF_ATOM   = 0x0F9E;
static constexpr uint16_t RT_USER_EDIT_ATOM          = 0x0FF5;
static constexpr uint16_t RT_PERSIST_PTR_INCR_BLOCK  = 0x1772;

// ── Binary read helpers ─────────────────────────────────────

static uint16_t rd16(const char* p) {
    auto b = reinterpret_cast<const uint8_t*>(p);
    return uint16_t(b[0]) | (uint16_t(b[1]) << 8);
}

static uint32_t rd32(const char* p) {
    auto b = reinterpret_cast<const uint8_t*>(p);
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) |
           (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}

// ── Record header ───────────────────────────────────────────

struct RecordHeader {
    uint16_t ver_inst;
    uint16_t rec_type;
    uint32_t rec_len;
    bool is_container() const { return (ver_inst & 0x0F) == 0x0F; }
};

static bool read_header(const char* data, size_t size, size_t pos, RecordHeader& hdr) {
    if (pos + 8 > size) return false;
    hdr.ver_inst = rd16(data + pos);
    hdr.rec_type = rd16(data + pos + 2);
    hdr.rec_len  = rd32(data + pos + 4);
    return true;
}

// ── Text decoding ───────────────────────────────────────────

static std::string decode_text_chars(const char* data, size_t len) {
    return util::utf16le_to_utf8(data, len);
}

static std::string decode_text_bytes(const char* data, size_t len) {
    std::string text;
    for (size_t i = 0; i < len; ++i) {
        uint8_t ch = static_cast<uint8_t>(data[i]);
        if (ch == 0x0D) text.push_back('\n');
        else if (ch >= 0x20 || ch == '\t' || ch == '\n')
            text += util::cp1252_to_utf8(ch);
    }
    return text;
}

// ── SlidePersistAtom layout (0x1C bytes) ────────────────────

struct SlidePersistAtom {
    uint32_t psr_reference;   // persistence ID → maps to stream offset
    uint32_t flags;
    int32_t  num_texts;
    uint32_t slide_id;
    uint32_t reserved;
    uint32_t start_offset;    // outline text range
    uint32_t end_offset;
};

static SlidePersistAtom read_spa(const char* data, size_t len) {
    SlidePersistAtom spa{};
    spa.psr_reference = rd32(data);
    if (len >= 8)  spa.flags       = rd32(data + 4);
    if (len >= 12) spa.num_texts   = static_cast<int32_t>(rd32(data + 8));
    if (len >= 16) spa.slide_id    = rd32(data + 12);
    if (len >= 20) spa.reserved    = rd32(data + 16);
    if (len >= 24) spa.start_offset = rd32(data + 20);
    if (len >= 28) spa.end_offset  = rd32(data + 24);
    return spa;
}

// ── PptParser ───────────────────────────────────────────────

PptParser::PptParser(OleReader& ole) : ole_(ole) {
    parse_document();
}

// ── Persistence directory builder ───────────────────────────
// Follows UserEditAtom chain to build psrReference → stream offset mapping.

static std::vector<uint32_t> build_persist_directory(
    const char* data, size_t size, uint32_t first_edit_offset)
{
    std::vector<uint32_t> dir;  // index = psrReference, value = stream offset

    uint32_t edit_offset = first_edit_offset;
    while (edit_offset != 0 && edit_offset + 8 <= size) {
        RecordHeader hdr{};
        if (!read_header(data, size, edit_offset, hdr)) break;
        if (hdr.rec_type != RT_USER_EDIT_ATOM) break;
        if (edit_offset + 8 + hdr.rec_len > size) break;

        const char* atom = data + edit_offset + 8;
        if (hdr.rec_len < 24) break;

        // UserEditAtom layout: [0] lastSlideIdRef, [4] version/minor/major,
        // [8] offsetLastEdit, [12] offsetPersistDirectory, [16] docPersistIdRef, ...
        uint32_t last_edit_offset = rd32(atom + 8);
        uint32_t persist_dir_offset = rd32(atom + 12);

        // Read PersistPtrIncrementalBlock at persist_dir_offset
        RecordHeader phdr{};
        if (persist_dir_offset + 8 <= size &&
            read_header(data, size, persist_dir_offset, phdr) &&
            phdr.rec_type == RT_PERSIST_PTR_INCR_BLOCK)
        {
            size_t pend = persist_dir_offset + 8 + phdr.rec_len;
            if (pend > size) pend = size;
            size_t ppos = persist_dir_offset + 8;

            while (ppos + 4 <= pend) {
                uint32_t info = rd32(data + ppos);
                ppos += 4;
                uint32_t start_id = info & 0x000FFFFF;
                uint32_t count = (info >> 20) & 0xFFF;

                for (uint32_t i = 0; i < count && ppos + 4 <= pend; ++i) {
                    uint32_t offset = rd32(data + ppos);
                    ppos += 4;
                    uint32_t id = start_id + i;
                    if (id >= dir.size()) dir.resize(id + 1, 0);
                    if (dir[id] == 0) dir[id] = offset;  // first write wins (most recent edit)
                }
            }
        }

        if (last_edit_offset == 0 || last_edit_offset == edit_offset) break;
        edit_offset = last_edit_offset;
    }

    return dir;
}

// ── Find first UserEditAtom offset from CurrentUser stream ──

static uint32_t find_first_edit_offset(const char* data, size_t size) {
    // Scan backward from end for UserEditAtom
    if (size < 16) return 0;
    for (size_t pos = size - 8; pos > 0; pos -= 2) {
        RecordHeader hdr{};
        if (read_header(data, size, pos, hdr) && hdr.rec_type == RT_USER_EDIT_ATOM)
            return static_cast<uint32_t>(pos);
    }
    return 0;
}

static uint32_t find_edit_offset_from_current_user(OleReader& ole) {
    auto cu = ole.read_stream("Current User");
    if (cu.empty()) return 0;
    // CurrentUser stream: RecordHeader(8) + CurrentUserAtom
    // CurrentUserAtom: size(4) + headerToken(4) + offsetToCurrentEdit(4) + ...
    if (cu.size() >= 20) {
        return rd32(cu.data() + 16);
    }
    return 0;
}

// ── Extract text from a slide/notes container ───────────────

struct SlideText {
    std::string title;
    std::string body;
    uint32_t notes_id = 0;  // from SlideAtom.notesIdRef
    std::vector<int> blip_indices;  // 1-based BLIP references from shapes
};

// Extract BLIP indices from msofbtOPT (0xF00B) property table
static void collect_blip_refs(const char* data, size_t opt_data_start,
                               uint32_t opt_len, std::vector<int>& out) {
    // msofbtOPT: array of (propId:16, value:32) pairs
    // Property 0x0104 = pib (picture BLIP index, 1-based)
    // Property 0x0186 = fillBlip (fill BLIP index)
    // Bit 14 (0x4000) = complex flag, bit 15 (0x8000) = blipId flag — mask both
    size_t pos = opt_data_start;
    size_t end = opt_data_start + opt_len;
    uint16_t inst = rd16(data + opt_data_start - 8) >> 4;
    uint32_t fill_type = 0xFFFFFFFF;
    uint32_t fill_blip = 0;
    uint32_t pib = 0;
    for (uint16_t i = 0; i < inst && pos + 6 <= end; ++i) {
        uint16_t raw_id = rd16(data + pos);
        uint32_t val = rd32(data + pos + 2);
        pos += 6;
        uint16_t prop_id = raw_id & 0x3FFF;  // strip complex + blipId flags
        if (prop_id == 0x0104) pib = val;
        else if (prop_id == 0x0180) fill_type = val;
        else if (prop_id == 0x0186) fill_blip = val;
    }
    if (pib > 0) out.push_back(static_cast<int>(pib));
    if (fill_blip > 0 && fill_type >= 1 && fill_type <= 3)
        out.push_back(static_cast<int>(fill_blip));
}

static SlideText extract_slide_text(const char* data, size_t size,
                                     size_t container_start, uint32_t container_len) {
    SlideText result;
    size_t end = container_start + container_len;
    if (end > size) end = size;
    size_t pos = container_start;
    uint32_t text_type = 0xFF;

    while (pos + 8 <= end) {
        RecordHeader hdr{};
        if (!read_header(data, size, pos, hdr)) break;

        if (hdr.is_container()) {
            pos += 8;
            continue;
        }

        size_t atom_end = pos + 8 + hdr.rec_len;
        if (atom_end > end) break;
        const char* atom = data + pos + 8;

        if (hdr.rec_type == RT_SLIDE_ATOM && hdr.rec_len >= 24) {
            // notesIdRef is at offset 16 in SlideAtom (after geometry, colors, etc.)
            result.notes_id = rd32(atom + 16);
        } else if (hdr.rec_type == RT_TEXT_HEADER_ATOM && hdr.rec_len >= 4) {
            text_type = rd32(atom);
        } else if (hdr.rec_type == RT_TEXT_CHARS_ATOM) {
            std::string text = decode_text_chars(atom, hdr.rec_len);
            if (!text.empty()) {
                if (text_type == 0) {
                    if (!result.title.empty()) result.title += " ";
                    result.title += text;
                } else {
                    if (!result.body.empty()) result.body += "\n";
                    result.body += text;
                }
            }
            text_type = 0xFF;
        } else if (hdr.rec_type == RT_TEXT_BYTES_ATOM) {
            std::string text = decode_text_bytes(atom, hdr.rec_len);
            if (!text.empty()) {
                if (text_type == 0) {
                    if (!result.title.empty()) result.title += " ";
                    result.title += text;
                } else {
                    if (!result.body.empty()) result.body += "\n";
                    result.body += text;
                }
            }
            text_type = 0xFF;
        } else if (hdr.rec_type == 0xF00B && hdr.rec_len >= 6) {
            // msofbtOPT — collect BLIP references from shape properties
            collect_blip_refs(data, pos + 8, hdr.rec_len, result.blip_indices);
        }

        pos = atom_end;
    }

    return result;
}

// ── Main document parsing ───────────────────────────────────

void PptParser::parse_document() {
    std::vector<char> stream = ole_.read_stream("PowerPoint Document");
    if (stream.empty()) return;
    const char* data = stream.data();
    size_t size = stream.size();

    // Step 1: Build persistence directory (psrReference → stream offset)
    uint32_t edit_offset = find_edit_offset_from_current_user(ole_);
    if (edit_offset == 0) edit_offset = find_first_edit_offset(data, size);
    auto persist_dir = build_persist_directory(data, size, edit_offset);

    auto get_stream_pos = [&](uint32_t psr_ref) -> size_t {
        if (psr_ref < persist_dir.size() && persist_dir[psr_ref] != 0)
            return persist_dir[psr_ref];
        return 0;
    };

    // Step 2: Scan all SlideListWithText containers, collect SlidePersistAtoms,
    // classify each by checking the actual record type at the stream position.
    struct SlideEntry {
        uint32_t psr_reference;
        size_t stream_pos;
        uint16_t record_type;  // 0x3EE=slide, 0x3F8=master, 0x3F0=notes
        uint32_t slide_id;
    };

    std::vector<SlideEntry> slide_entries;
    std::vector<SlideEntry> notes_entries;

    size_t pos = 0;
    while (pos + 8 <= size) {
        RecordHeader hdr{};
        if (!read_header(data, size, pos, hdr)) break;

        if (hdr.is_container()) {
            if (hdr.rec_type == RT_SLIDE_LIST_WITH_TEXT) {
                // Scan inside this container for SlidePersistAtoms
                size_t slt_end = pos + 8 + hdr.rec_len;
                if (slt_end > size) slt_end = size;
                size_t inner = pos + 8;

                while (inner + 8 <= slt_end) {
                    RecordHeader ihdr{};
                    if (!read_header(data, size, inner, ihdr)) break;

                    if (ihdr.is_container()) {
                        inner += 8;
                        continue;
                    }

                    size_t iatom_end = inner + 8 + ihdr.rec_len;
                    if (iatom_end > slt_end) break;

                    if (ihdr.rec_type == RT_SLIDE_PERSIST_ATOM && ihdr.rec_len >= 4) {
                        SlidePersistAtom spa = read_spa(data + inner + 8, ihdr.rec_len);
                        size_t spos = get_stream_pos(spa.psr_reference);

                        uint16_t rtype = 0;
                        if (spos > 0 && spos + 8 <= size) {
                            RecordHeader shdr{};
                            if (read_header(data, size, spos, shdr))
                                rtype = shdr.rec_type;
                        }

                        SlideEntry entry{};
                        entry.psr_reference = spa.psr_reference;
                        entry.stream_pos = spos;
                        entry.record_type = rtype;
                        entry.slide_id = spa.slide_id;

                        if (rtype == RT_SLIDE)
                            slide_entries.push_back(entry);
                        else if (rtype == RT_NOTES)
                            notes_entries.push_back(entry);
                        // Skip masters (RT_MAIN_MASTER = 0x3F8)
                    }

                    inner = iatom_end;
                }

                // Skip past the entire container
                pos = slt_end;
                continue;
            }
            pos += 8;
            continue;
        }

        size_t atom_end = pos + 8 + hdr.rec_len;
        if (atom_end > size) break;
        pos = atom_end;
    }

    // Step 3: Extract text from each slide container
    slides_.reserve(slide_entries.size());
    for (size_t i = 0; i < slide_entries.size(); ++i) {
        auto& se = slide_entries[i];
        Slide slide;
        slide.number = static_cast<int>(i) + 1;

        if (se.stream_pos > 0 && se.stream_pos + 8 <= size) {
            RecordHeader shdr{};
            if (read_header(data, size, se.stream_pos, shdr)) {
                uint32_t clen = shdr.rec_len;
                if (se.stream_pos + 8 + clen > size)
                    clen = static_cast<uint32_t>(size - se.stream_pos - 8);

                SlideText st = extract_slide_text(data, size, se.stream_pos + 8, clen);
                slide.title = std::move(st.title);
                slide.body = std::move(st.body);
                slide.blip_indices = std::move(st.blip_indices);

                // Link notes via notesIdRef → notes_entries.slide_id
                if (st.notes_id != 0) {
                    for (auto& ne : notes_entries) {
                        if (ne.slide_id == st.notes_id && ne.stream_pos > 0 &&
                            ne.stream_pos + 8 <= size) {
                            RecordHeader nhdr{};
                            if (read_header(data, size, ne.stream_pos, nhdr)) {
                                uint32_t nlen = nhdr.rec_len;
                                if (ne.stream_pos + 8 + nlen > size)
                                    nlen = static_cast<uint32_t>(size - ne.stream_pos - 8);
                                SlideText nt = extract_slide_text(data, size,
                                                                   ne.stream_pos + 8, nlen);
                                // Notes body only (skip title which is usually slide number)
                                slide.notes = std::move(nt.body);
                            }
                            break;
                        }
                    }
                }
            }
        }

        slides_.push_back(std::move(slide));
    }

    // Fallback: if no slides found via persistence directory, do simple linear scan
    if (slides_.empty()) {
        uint32_t text_type = 0xFF;
        Slide current;
        current.number = 1;

        pos = 0;
        while (pos + 8 <= size) {
            RecordHeader hdr{};
            if (!read_header(data, size, pos, hdr)) break;

            if (hdr.is_container()) {
                pos += 8;
                continue;
            }

            size_t atom_end = pos + 8 + hdr.rec_len;
            if (atom_end > size) break;
            const char* atom = data + pos + 8;

            if (hdr.rec_type == RT_TEXT_HEADER_ATOM && hdr.rec_len >= 4) {
                text_type = rd32(atom);
            } else if (hdr.rec_type == RT_TEXT_CHARS_ATOM && hdr.rec_len > 0) {
                std::string text = decode_text_chars(atom, hdr.rec_len);
                if (text_type == 0) {
                    if (!current.title.empty()) {
                        slides_.push_back(current);
                        current = Slide{};
                        current.number = static_cast<int>(slides_.size()) + 1;
                    }
                    current.title = text;
                } else {
                    if (!current.body.empty()) current.body += "\n";
                    current.body += text;
                }
                text_type = 0xFF;
            } else if (hdr.rec_type == RT_TEXT_BYTES_ATOM && hdr.rec_len > 0) {
                std::string text = decode_text_bytes(atom, hdr.rec_len);
                if (text_type == 0) {
                    if (!current.title.empty()) {
                        slides_.push_back(current);
                        current = Slide{};
                        current.number = static_cast<int>(slides_.size()) + 1;
                    }
                    current.title = text;
                } else {
                    if (!current.body.empty()) current.body += "\n";
                    current.body += text;
                }
                text_type = 0xFF;
            }

            pos = atom_end;
        }

        if (!current.title.empty() || !current.body.empty())
            slides_.push_back(current);
    }
}

// ── Slide to markdown ───────────────────────────────────────

std::string PptParser::slide_to_markdown(const Slide& slide) {
    std::ostringstream out;

    if (slide.number > 1) out << "\n";
    out << "--- Page " << slide.number << " ---\n\n";

    if (!slide.title.empty()) {
        out << "# " << slide.title << "\n\n";
    }

    if (!slide.body.empty()) {
        std::istringstream iss(slide.body);
        std::string line;
        while (std::getline(iss, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();

            if (line.empty()) {
                out << "\n";
            } else {
                bool is_bullet = false;
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
                if (is_bullet) {
                    out << "- " << line << "\n";
                } else {
                    out << line << "\n";
                }
            }
        }
    }

    // Append notes (blockquote style, matching PPTX)
    if (!slide.notes.empty()) {
        std::istringstream niss(slide.notes);
        std::string nline;
        bool first = true;
        while (std::getline(niss, nline)) {
            while (!nline.empty() && (nline.back() == '\r' || nline.back() == ' '))
                nline.pop_back();
            if (!nline.empty()) {
                if (first) {
                    out << "\n> **Notes:** " << nline << "\n";
                    first = false;
                } else {
                    out << "> " << nline << "\n";
                }
            }
        }
    }

    out << "\n";
    return out.str();
}

// ── Image extraction from Pictures stream ───────────────────

std::vector<ImageData> PptParser::extract_images(unsigned min_image_size) {
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

        if (rec_len == 0) break;
        if (pos + 8 + rec_len > pics.size()) break;

        size_t blip_start = pos + 8;
        size_t blip_end = blip_start + rec_len;

        std::string fmt;
        size_t header_skip = 0;
        bool is_metafile = false;

        switch (rec_type) {
            case 0xF01A: fmt = "emf";  header_skip = 50; is_metafile = true; break;
            case 0xF01B: fmt = "wmf";  header_skip = 50; is_metafile = true; break;
            case 0xF01C: header_skip = 50; is_metafile = true; break;
            case 0xF01D: fmt = "jpeg"; header_skip = 17; break;
            case 0xF01E: fmt = "png";  header_skip = 17; break;
            case 0xF01F: fmt = "bmp";  header_skip = 17; break;
            case 0xF029: fmt = "tiff"; header_skip = 17; break;
            default:
                pos = blip_end;
                continue;
        }

        // 2-UID variant adds 16 bytes
        uint16_t inst = ver_inst >> 4;
        if (!is_metafile) {
            if (inst == 0x46B || inst == 0x6E1 || inst == 0x6E3 || inst == 0x6E5)
                header_skip += 16;
        } else {
            if (inst == 0x3D5 || inst == 0x217)
                header_skip += 16;
        }

        if (!fmt.empty() && rec_len > header_skip) {
            size_t img_offset = blip_start + header_skip;
            size_t img_size = rec_len - header_skip;

            if (img_offset + img_size <= pics.size()) {
                std::vector<char> img_data;

                if (is_metafile) {
                    uLong decomp_size = 0;
                    if (header_skip >= 38)
                        decomp_size = rd32(pics.data() + blip_start + 34);
                    if (decomp_size == 0 || decomp_size > 100 * 1024 * 1024)
                        decomp_size = img_size * 10;

                    img_data.resize(decomp_size);
                    uLong actual_size = decomp_size;
                    int zret = uncompress(
                        reinterpret_cast<Bytef*>(img_data.data()), &actual_size,
                        reinterpret_cast<const Bytef*>(pics.data() + img_offset), img_size);

                    if (zret == Z_OK)
                        img_data.resize(actual_size);
                    else
                        img_data.assign(pics.begin() + img_offset,
                                        pics.begin() + img_offset + img_size);
                } else {
                    img_data.assign(pics.begin() + img_offset,
                                    pics.begin() + img_offset + img_size);
                }

                ImageData img;
                img.page_number = 1;
                img.name = "page1_img" + std::to_string(img_idx++);
                img.format = fmt;
                img.data = std::move(img_data);
                util::populate_image_dimensions(img);
                if (util::is_image_too_small(img, min_image_size)) {
                    --img_idx;
                    pos = blip_end;
                    continue;
                }
                images.push_back(std::move(img));
            }
        }

        pos = blip_end;
    }

    return images;
}

// ── Public API ──────────────────────────────────────────────

std::string PptParser::to_markdown(const ConvertOptions& opts) {
    std::string md;
    auto all_images = extract_images(opts.min_image_size);

    // Save images to disk if requested
    if (opts.extract_images) {
        for (auto& img : all_images) {
            img.saved_path = util::save_image_to_file(
                opts.image_output_dir, img.name, img.format,
                img.data.data(), img.data.size());
            if (!img.saved_path.empty()) {
                img.data.clear();
                img.data.shrink_to_fit();
            }
        }
    }

    for (const auto& slide : slides_) {
        if (!opts.pages.empty()) {
            bool found = false;
            for (int p : opts.pages) {
                if (p == slide.number) { found = true; break; }
            }
            if (!found) continue;
        }
        md += slide_to_markdown(slide);

        // Append images referenced by this slide's shapes
        for (int blip_idx : slide.blip_indices) {
            if (blip_idx >= 1 && blip_idx <= static_cast<int>(all_images.size())) {
                auto& img = all_images[blip_idx - 1];
                std::string filename = img.name + "." + img.format;
                if (opts.extract_images)
                    md += "![" + filename + "](" + opts.image_ref_prefix + filename + ")\n\n";
                else
                    md += "![" + filename + "](" + filename + ")\n\n";
            }
        }
    }

    return md;
}

std::vector<PageChunk> PptParser::to_chunks(const ConvertOptions& opts) {
    std::vector<PageChunk> chunks;
    auto all_images = extract_images(opts.min_image_size);

    if (opts.extract_images) {
        for (auto& img : all_images) {
            img.saved_path = util::save_image_to_file(
                opts.image_output_dir, img.name, img.format,
                img.data.data(), img.data.size());
            if (!img.saved_path.empty()) {
                img.data.clear();
                img.data.shrink_to_fit();
            }
        }
    }

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
        std::string slide_md = slide_to_markdown(slide);

        // Attach images to this slide by BLIP index
        for (int blip_idx : slide.blip_indices) {
            if (blip_idx >= 1 && blip_idx <= static_cast<int>(all_images.size())) {
                auto& img = all_images[blip_idx - 1];
                std::string filename = img.name + "." + img.format;
                if (opts.extract_images)
                    slide_md += "![" + filename + "](" + opts.image_ref_prefix + filename + ")\n\n";
                else
                    slide_md += "![" + filename + "](" + filename + ")\n\n";
                chunk.images.push_back(img);
            }
        }

        chunk.text = std::move(slide_md);
        chunk.page_width = 720.0;
        chunk.page_height = 540.0;
        chunk.body_font_size = 24.0;
        chunks.push_back(std::move(chunk));
    }

    return chunks;
}

} // namespace jdoc
