#pragma once
// File and path utility functions
// License: MIT

#include "common/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace jdoc { namespace util {

// Public APIs carry UTF-8 paths on every platform. std::filesystem::u8path
// converts them to UTF-16-backed paths on Windows while remaining a byte path
// on POSIX systems.
inline std::filesystem::path io_path(const std::string& path) {
    return std::filesystem::u8path(path);
}

inline FILE* fopen_utf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    std::wstring wide_mode;
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(mode); *p; ++p)
        wide_mode.push_back(static_cast<wchar_t>(*p));
    return _wfopen(io_path(path).c_str(), wide_mode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// Get lowercase file extension from path (returns ".ext" form, e.g. ".jpg").
inline std::string get_extension(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot);
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

// Get filename from path (everything after last / or \).
inline std::string get_filename(const std::string& path) {
    auto sep = path.find_last_of("/\\");
    return (sep == std::string::npos) ? path : path.substr(sep + 1);
}

// Map file extension to image format string.
// Accepts ".jpg" form (with dot).
inline std::string image_format_from_ext(const std::string& ext) {
    if (ext == ".jpg" || ext == ".jpeg") return "jpeg";
    if (ext == ".png") return "png";
    if (ext == ".gif") return "gif";
    if (ext == ".bmp") return "bmp";
    if (ext == ".tiff" || ext == ".tif") return "tiff";
    if (ext == ".emf") return "emf";
    if (ext == ".wmf") return "wmf";
    if (ext == ".svg") return "svg";
    return "bin";
}

// The extension save_image_to_file() gives a payload of this format. A JPEG
// lands as ".jpg" and an unrecognized format as ".bin", so a caller that names
// the file from the format string alone would point at a file that is not
// there. Kept next to the inverse mapping above; the writer uses it too.
inline std::string image_file_ext(const std::string& format) {
    if (format == "jpeg") return "jpg";
    return format.empty() ? std::string("bin") : format;
}

// The name that belongs in a Markdown image reference. Once an image reaches
// disk that is the basename actually written — collision suffix and all, since
// a name already taken in image_dir is written as "<stem>_1.<ext>". Only an
// image that was never written falls back to the derived name.
inline std::string image_ref_name(const std::string& name,
                                  const std::string& format,
                                  const std::string& saved_path) {
    if (!saved_path.empty()) return get_filename(saved_path);
    return name + "." + image_file_ext(format);
}

// Trim leading/trailing whitespace.
inline std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Byte count for human eyes ("9.7 KB", "2.0 MB", "512 B"). Used wherever a
// listing names a payload jdoc does not open, so the reader can judge whether
// the thing that was left behind mattered.
inline std::string human_bytes(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024ull * 1024)
        snprintf(buf, sizeof buf, "%.1f MB", bytes / (1024.0 * 1024));
    else if (bytes >= 1024)
        snprintf(buf, sizeof buf, "%.1f KB", bytes / 1024.0);
    else
        snprintf(buf, sizeof buf, "%llu B",
                 static_cast<unsigned long long>(bytes));
    return buf;
}

// One line of valid UTF-8, always. A name that jdoc did not author — a PDF
// filespec, a zip entry — reaches the markdown as a list item, and it is
// trusted for neither structure nor encoding: a newline would let the document
// forge headings and lists of its own, and a stray byte would make the whole
// conversion undecodable to a caller that demands UTF-8 (pybind11 raises
// UnicodeDecodeError on the returned string, losing an otherwise fine
// document). Control characters collapse to spaces; invalid sequences are
// repaired.
inline std::string to_single_line(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out += (c < 0x20 || c == 0x7F) ? ' ' : static_cast<char>(c);
    return trim(sanitize_utf8(out));
}

// Escape pipe and newline for markdown table cells.
inline std::string escape_cell(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '|') { out += "\\|"; }
        else if (c == '\n') { out += ' '; }
        else { out += c; }
    }
    // Trim result
    auto start = out.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = out.find_last_not_of(" \t\r\n");
    return out.substr(start, end - start + 1);
}

// Create a directory (no error on existing).
inline void ensure_dir(const std::string& dir) {
    if (dir.empty()) return;
    std::error_code ignored;
    std::filesystem::create_directories(io_path(dir), ignored);
}

// Create a directory and any missing parents (mkdir -p).
inline void ensure_dirs(const std::string& dir) {
    ensure_dir(dir);
}

// Render rows as a GitHub-flavored markdown table; the first row is the header.
// Empty input yields "". Column count is the widest row; short rows pad with "".
// Shared by the docx and pptx parsers (previously duplicated verbatim).
inline std::string format_markdown_table(
    const std::vector<std::vector<std::string>>& rows) {
    if (rows.empty()) return "";
    size_t cols = 0;
    for (auto& row : rows) cols = std::max(cols, row.size());
    if (cols == 0) return "";

    std::string out;
    out += "|";
    for (size_t c = 0; c < cols; ++c) {
        const std::string& cell = (c < rows[0].size()) ? rows[0][c] : "";
        out += " "; out += cell; out += " |";
    }
    out += "\n|";
    for (size_t c = 0; c < cols; ++c) out += " --- |";
    out += "\n";
    for (size_t r = 1; r < rows.size(); ++r) {
        out += "|";
        for (size_t c = 0; c < cols; ++c) {
            const std::string& cell = (c < rows[r].size()) ? rows[r][c] : "";
            out += " "; out += cell; out += " |";
        }
        out += "\n";
    }
    return out;
}

// Strip markdown formatting from text, returning plain text.
// Removes: # headings, **bold**, *italic*, ![img](ref), table pipes, --- separators
inline std::string strip_markdown(const std::string& md) {
    std::string result;
    result.reserve(md.size());
    size_t i = 0;
    size_t len = md.size();
    while (i < len) {
        // Start of line
        if (i == 0 || (i > 0 && md[i - 1] == '\n')) {
            // Skip heading markers (# ## ### etc.)
            size_t j = i;
            while (j < len && md[j] == '#') j++;
            if (j > i && j < len && md[j] == ' ') {
                i = j + 1;
                continue;
            }
            // Skip --- horizontal rules
            j = i;
            while (j < len && md[j] == '-') j++;
            if (j - i >= 3 && (j >= len || md[j] == '\n')) {
                i = j;
                continue;
            }
            // Skip table separator lines (| --- | --- |)
            if (md[i] == '|') {
                j = i;
                while (j < len && md[j] != '\n') j++;
                std::string line = md.substr(i, j - i);
                bool is_sep = true;
                for (char c : line) {
                    if (c != '|' && c != '-' && c != ' ' && c != ':') { is_sep = false; break; }
                }
                if (is_sep && line.find('-') != std::string::npos) {
                    i = (j < len) ? j + 1 : j;
                    continue;
                }
            }
        }
        // Image: ![alt](path) -> [image: alt]
        if (md[i] == '!' && i + 1 < len && md[i + 1] == '[') {
            size_t j = i + 2;
            while (j < len && md[j] != ']') j++;
            if (j < len && j + 1 < len && md[j + 1] == '(') {
                size_t k = j + 2;
                while (k < len && md[k] != ')') k++;
                if (k < len) {
                    std::string alt = md.substr(i + 2, j - i - 2);
                    if (!alt.empty()) {
                        result += "[image: " + alt + "]";
                    }
                    i = k + 1;
                    continue;
                }
            }
        }
        // Bold/italic: strip *** ** *
        if (md[i] == '*') {
            size_t stars = 0;
            while (i + stars < len && md[i + stars] == '*') stars++;
            i += stars;
            continue;
        }
        // Table row: strip leading/trailing | and replace inner | with tab
        if ((i == 0 || md[i - 1] == '\n') && md[i] == '|') {
            size_t j = i;
            while (j < len && md[j] != '\n') j++;
            std::string line = md.substr(i, j - i);
            size_t s = 0, e = line.size();
            if (s < e && line[s] == '|') s++;
            if (e > s && line[e - 1] == '|') e--;
            std::string row;
            size_t ci = s;
            while (ci < e) {
                size_t pipe = line.find('|', ci);
                if (pipe == std::string::npos || pipe >= e) pipe = e;
                std::string cell = trim(line.substr(ci, pipe - ci));
                // Strip bold/italic markers from cell text
                std::string clean;
                for (size_t k = 0; k < cell.size(); ) {
                    if (cell[k] == '*') {
                        while (k < cell.size() && cell[k] == '*') k++;
                    } else {
                        clean += cell[k++];
                    }
                }
                cell = std::move(clean);
                if (!row.empty()) row += "  ";
                row += cell;
                ci = pipe + 1;
            }
            result += row;
            result += '\n';
            i = (j < len) ? j + 1 : j;
            continue;
        }
        result += md[i];
        i++;
    }
    return result;
}

// Read image dimensions from JPEG/PNG/GIF/BMP header bytes.
// Returns {width, height}; {0,0} if format unknown or data too short.
inline std::pair<unsigned, unsigned> image_dimensions_from_data(
    const char* data, size_t size) {
    if (!data || size < 8) return {0, 0};
    auto u8 = reinterpret_cast<const uint8_t*>(data);
    auto be16 = [u8](size_t offset) {
        return static_cast<uint16_t>(
            (uint16_t{u8[offset]} << 8) | u8[offset + 1]);
    };
    auto be32 = [u8](size_t offset) {
        return (uint32_t{u8[offset]} << 24) |
               (uint32_t{u8[offset + 1]} << 16) |
               (uint32_t{u8[offset + 2]} << 8) |
               uint32_t{u8[offset + 3]};
    };
    auto le16 = [u8](size_t offset) {
        return static_cast<uint16_t>(
            uint16_t{u8[offset]} | (uint16_t{u8[offset + 1]} << 8));
    };
    auto le32 = [u8](size_t offset) {
        return uint32_t{u8[offset]} |
               (uint32_t{u8[offset + 1]} << 8) |
               (uint32_t{u8[offset + 2]} << 16) |
               (uint32_t{u8[offset + 3]} << 24);
    };

    // PNG: signature(8) + IHDR length(4) + "IHDR"(4) + width(4) + height(4)
    if (size >= 24 && u8[0] == 0x89 && u8[1] == 'P' && u8[2] == 'N' && u8[3] == 'G') {
        return {be32(16), be32(20)};
    }

    // JPEG: find SOF0 (0xFFC0) or SOF2 (0xFFC2) marker
    if (size >= 4 && u8[0] == 0xFF && u8[1] == 0xD8) {
        size_t pos = 2;
        while (pos + 4 < size) {
            if (u8[pos] != 0xFF) { pos++; continue; }
            uint8_t marker = u8[pos + 1];
            if (marker == 0xC0 || marker == 0xC2) {
                if (pos + 9 < size) {
                    return {be16(pos + 7), be16(pos + 5)};
                }
                break;
            }
            if (marker == 0xD9 || marker == 0xDA) break; // EOI or SOS
            if (pos + 3 < size) {
                uint16_t seg_len = be16(pos + 2);
                pos += 2 + seg_len;
            } else break;
        }
        return {0, 0};
    }

    // GIF: "GIF8" + version(2) + width(2 LE) + height(2 LE)
    if (size >= 10 && u8[0] == 'G' && u8[1] == 'I' && u8[2] == 'F') {
        return {le16(6), le16(8)};
    }

    // BMP: "BM" + ... + width(4 LE at 18) + height(4 LE at 22)
    if (size >= 26 && u8[0] == 'B' && u8[1] == 'M') {
        uint32_t width = le32(18);
        int32_t signed_height = static_cast<int32_t>(le32(22));
        if (signed_height == std::numeric_limits<int32_t>::min())
            return {width, 0};
        uint32_t height = static_cast<uint32_t>(
            signed_height < 0 ? -signed_height : signed_height);
        return {width, height};
    }

    return {0, 0};
}

}} // namespace jdoc::util
