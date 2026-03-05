// .rtf (Rich Text Format) parser implementation.

#include "rtf_parser.h"
#include "common/string_utils.h"
#include "common/binary_utils.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stack>

namespace jdoc {

// Destination group names that should be skipped.
static const char* kSkipDestinations[] = {
    "fonttbl", "colortbl", "stylesheet", "info", "header", "footer",
    "footnote", "annotation", "xmlnstbl", "listtable", "listoverridetable",
    "revtbl", "rsidtbl", "generator", "datafield", "fldinst",
    nullptr
};

static bool is_skip_destination(const std::string& name) {
    for (int i = 0; kSkipDestinations[i]; ++i) {
        if (name == kSkipDestinations[i]) return true;
    }
    return false;
}

// ---------- helpers ----------------------------------------------------------

std::vector<char> RtfParser::hex_to_binary(const std::string& hex) {
    std::vector<char> result;
    result.reserve(hex.size() / 2);

    int byte_val = 0;
    bool have_nibble = false;

    for (char c : hex) {
        int nibble = -1;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;

        if (nibble < 0) continue; // Skip whitespace and other non-hex chars.

        if (!have_nibble) {
            byte_val = nibble << 4;
            have_nibble = true;
        } else {
            byte_val |= nibble;
            result.push_back(static_cast<char>(byte_val));
            have_nibble = false;
        }
    }

    return result;
}

// ---------- RtfParser --------------------------------------------------------

RtfParser::RtfParser(const std::string& file_path) : file_path_(file_path) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) return;

    ifs.seekg(0, std::ios::end);
    size_t size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    raw_data_.resize(size);
    ifs.read(raw_data_.data(), size);
}

// ---------- parser -----------------------------------------------------------

void RtfParser::parse(std::string& out_text, std::vector<PictImage>& out_images) {
    if (raw_data_.empty()) return;

    const char* data = raw_data_.data();
    size_t len = raw_data_.size();
    size_t pos = 0;

    std::stack<State> state_stack;
    State cur;

    // For tracking bold/italic span boundaries.
    bool was_bold = false;
    bool was_italic = false;

    // Current pict image being assembled.
    PictImage current_pict;
    bool collecting_pict_hex = false;

    // Flag: next group is a destination (set by \*).
    bool next_is_destination = false;

    while (pos < len) {
        char ch = data[pos];

        if (ch == '{') {
            // Push state.
            state_stack.push(cur);
            pos++;

            if (next_is_destination) {
                // The control word following will be a destination.
                // We'll handle it when we parse the control word.
                next_is_destination = false;
            }
            continue;
        }

        if (ch == '}') {
            // Pop state.
            // Close any open formatting spans.
            if (!cur.skip && !cur.in_pict) {
                if (was_bold && !cur.bold) {
                    // Already closed.
                } else if (cur.bold) {
                    out_text += "**";
                    was_bold = false;
                }
                if (was_italic && !cur.italic) {
                    // Already closed.
                } else if (cur.italic) {
                    out_text += "*";
                    was_italic = false;
                }
            }

            if (cur.in_pict && collecting_pict_hex) {
                // Finalize the pict image.
                collecting_pict_hex = false;
                if (!current_pict.hex_data.empty() || !current_pict.bin_data.empty()) {
                    out_images.push_back(current_pict);
                }
                current_pict = PictImage{};
            }

            if (!state_stack.empty()) {
                cur = state_stack.top();
                state_stack.pop();
            }
            pos++;
            continue;
        }

        if (ch == '\\') {
            pos++;
            if (pos >= len) break;

            ch = data[pos];

            // Special two-character escapes.
            if (ch == '\\' || ch == '{' || ch == '}') {
                if (!cur.skip && !cur.in_pict) {
                    out_text.push_back(ch);
                }
                pos++;
                continue;
            }

            // \' hex escape.
            if (ch == '\'') {
                pos++;
                if (pos + 2 <= len) {
                    char h1 = data[pos], h2 = data[pos + 1];
                    auto hex_val = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return 0;
                    };
                    uint8_t byte = static_cast<uint8_t>((hex_val(h1) << 4) | hex_val(h2));
                    if (!cur.skip && !cur.in_pict) {
                        out_text += util::cp1252_to_utf8(byte);
                    }
                    pos += 2;
                }
                continue;
            }

            // \* (destination marker).
            if (ch == '*') {
                next_is_destination = true;
                pos++;
                continue;
            }

            // \~ (non-breaking space).
            if (ch == '~') {
                if (!cur.skip && !cur.in_pict) out_text.push_back(' ');
                pos++;
                continue;
            }

            // \- (optional hyphen) and \_ (non-breaking hyphen).
            if (ch == '-' || ch == '_') {
                pos++;
                continue;
            }

            // Parse control word: letters, optionally followed by integer parameter.
            if (std::isalpha(static_cast<unsigned char>(ch))) {
                std::string word;
                while (pos < len && std::isalpha(static_cast<unsigned char>(data[pos]))) {
                    word.push_back(data[pos]);
                    pos++;
                }

                // Optional numeric parameter (may be negative).
                bool has_param = false;
                int param = 0;
                bool neg = false;
                if (pos < len && data[pos] == '-') {
                    neg = true;
                    pos++;
                }
                if (pos < len && std::isdigit(static_cast<unsigned char>(data[pos]))) {
                    has_param = true;
                    while (pos < len && std::isdigit(static_cast<unsigned char>(data[pos]))) {
                        param = param * 10 + (data[pos] - '0');
                        pos++;
                    }
                    if (neg) param = -param;
                } else if (neg) {
                    // The '-' was not a param sign; back up.
                    pos--;
                }

                // Consume trailing space delimiter (if any).
                if (pos < len && data[pos] == ' ') {
                    pos++;
                }

                // Handle control words.
                if (word == "par" || word == "line") {
                    if (!cur.skip && !cur.in_pict) {
                        // Close formatting before paragraph break.
                        if (was_bold) { out_text += "**"; was_bold = false; }
                        if (was_italic) { out_text += "*"; was_italic = false; }
                        out_text += (word == "par") ? "\n\n" : "\n";
                    }
                } else if (word == "tab") {
                    if (!cur.skip && !cur.in_pict) out_text.push_back('\t');
                } else if (word == "b") {
                    bool new_bold = !has_param || param != 0;
                    if (!cur.skip && !cur.in_pict) {
                        if (new_bold && !cur.bold) {
                            out_text += "**";
                            was_bold = true;
                        } else if (!new_bold && cur.bold && was_bold) {
                            out_text += "**";
                            was_bold = false;
                        }
                    }
                    cur.bold = new_bold;
                } else if (word == "i") {
                    bool new_italic = !has_param || param != 0;
                    if (!cur.skip && !cur.in_pict) {
                        if (new_italic && !cur.italic) {
                            out_text += "*";
                            was_italic = true;
                        } else if (!new_italic && cur.italic && was_italic) {
                            out_text += "*";
                            was_italic = false;
                        }
                    }
                    cur.italic = new_italic;
                } else if (word == "pard") {
                    // Reset paragraph formatting.
                    if (was_bold) { out_text += "**"; was_bold = false; }
                    if (was_italic) { out_text += "*"; was_italic = false; }
                    cur.bold = false;
                    cur.italic = false;
                } else if (word == "u") {
                    // Unicode character.
                    if (has_param && !cur.skip && !cur.in_pict) {
                        int16_t uval = static_cast<int16_t>(param);
                        uint32_t cp = (uval < 0) ? static_cast<uint32_t>(uval + 65536) : static_cast<uint32_t>(uval);
                        util::append_utf8(out_text, cp);
                    }
                    // Skip \uc replacement bytes.
                    int skip_count = cur.uc;
                    for (int s = 0; s < skip_count && pos < len; ++s) {
                        if (data[pos] == '\\') {
                            // Skip escaped char or control word.
                            pos++;
                            if (pos < len && data[pos] == '\'') {
                                pos += 3; // \'XX
                            } else {
                                // Skip to end of control word.
                                while (pos < len && std::isalpha(static_cast<unsigned char>(data[pos]))) pos++;
                                // Skip optional parameter.
                                if (pos < len && (data[pos] == '-' || std::isdigit(static_cast<unsigned char>(data[pos])))) {
                                    if (data[pos] == '-') pos++;
                                    while (pos < len && std::isdigit(static_cast<unsigned char>(data[pos]))) pos++;
                                }
                                if (pos < len && data[pos] == ' ') pos++;
                            }
                        } else if (data[pos] != '{' && data[pos] != '}') {
                            pos++;
                        }
                    }
                } else if (word == "uc") {
                    if (has_param) cur.uc = param;
                } else if (word == "pict") {
                    cur.in_pict = true;
                    collecting_pict_hex = true;
                    current_pict = PictImage{};
                } else if (cur.in_pict) {
                    // Pict-related keywords.
                    if (word == "pngblip") current_pict.format = "png";
                    else if (word == "jpegblip") current_pict.format = "jpeg";
                    else if (word == "emfblip") current_pict.format = "emf";
                    else if (word == "wmetafile") current_pict.format = "wmf";
                    else if (word == "picw" && has_param) current_pict.width = static_cast<unsigned>(param);
                    else if (word == "pich" && has_param) current_pict.height = static_cast<unsigned>(param);
                    else if (word == "bin" && has_param) {
                        // Next N bytes are raw binary.
                        size_t bin_count = static_cast<size_t>(param);
                        if (pos + bin_count <= len) {
                            current_pict.bin_data.insert(current_pict.bin_data.end(),
                                                         data + pos, data + pos + bin_count);
                            pos += bin_count;
                        }
                    }
                } else if (next_is_destination || is_skip_destination(word)) {
                    cur.skip = true;
                    next_is_destination = false;
                }

                continue;
            }

            // Unknown escape; skip.
            pos++;
            continue;
        }

        // Regular character.
        if (cur.in_pict && collecting_pict_hex) {
            // Collect hex data for the pict image.
            if (std::isxdigit(static_cast<unsigned char>(ch))) {
                current_pict.hex_data.push_back(ch);
            }
            // Ignore whitespace and other chars in hex data.
        } else if (!cur.skip) {
            if (ch == '\r' || ch == '\n') {
                // RTF uses \par for paragraph breaks; CR/LF in the file are ignored.
            } else {
                out_text.push_back(ch);
            }
        }

        pos++;
    }

    // Close any remaining formatting.
    if (was_bold) out_text += "**";
    if (was_italic) out_text += "*";
}

// ---------- public API -------------------------------------------------------

std::string RtfParser::to_markdown(const ConvertOptions& opts) {
    std::string text;
    std::vector<PictImage> pict_images;
    parse(text, pict_images);

    if (opts.extract_images && !pict_images.empty()) {
        text += "\n\n---\n\n";
        for (size_t i = 0; i < pict_images.size(); ++i) {
            const auto& pi = pict_images[i];
            std::string name = "rtf_image_" + std::to_string(i + 1);
            std::string ext = pi.format.empty() ? "bin" : pi.format;
            text += "![" + name + "](" + name + "." + ext + ")\n\n";
        }
    }

    return text;
}

std::vector<PageChunk> RtfParser::to_chunks(const ConvertOptions& opts) {
    std::string text;
    std::vector<PictImage> pict_images;
    parse(text, pict_images);

    PageChunk chunk;
    chunk.page_number = 1;
    chunk.text = text;
    chunk.page_width = 612.0;
    chunk.page_height = 792.0;
    chunk.body_font_size = 12.0;

    if (opts.extract_images) {
        for (size_t i = 0; i < pict_images.size(); ++i) {
            const auto& pi = pict_images[i];
            ImageData img;
            img.page_number = 1;
            img.name = "rtf_image_" + std::to_string(i + 1);
            img.format = pi.format.empty() ? "bin" : pi.format;
            img.width = pi.width;
            img.height = pi.height;

            // Decode image data.
            if (!pi.bin_data.empty()) {
                img.data = pi.bin_data;
            } else if (!pi.hex_data.empty()) {
                img.data = hex_to_binary(pi.hex_data);
            }

            chunk.images.push_back(std::move(img));
        }
    }

    std::vector<PageChunk> chunks;
    chunks.push_back(std::move(chunk));
    return chunks;
}

} // namespace jdoc
