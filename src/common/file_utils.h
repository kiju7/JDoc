#pragma once
// File and path utility functions
// License: MIT

#include "jdoc/types.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#ifdef _WIN32
  #include <direct.h>
  #define jdoc_mkdir(path) _mkdir(path)
#else
  #include <sys/stat.h>
  #define jdoc_mkdir(path) mkdir(path, 0755)
#endif

namespace jdoc { namespace util {

// Get lowercase file extension from path (returns ".ext" form, e.g. ".jpg").
inline std::string get_extension(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot);
    for (auto& c : ext) c = std::tolower(static_cast<unsigned char>(c));
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

// Trim leading/trailing whitespace.
inline std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
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
    if (!dir.empty()) jdoc_mkdir(dir.c_str());
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

    // PNG: signature(8) + IHDR length(4) + "IHDR"(4) + width(4) + height(4)
    if (size >= 24 && u8[0] == 0x89 && u8[1] == 'P' && u8[2] == 'N' && u8[3] == 'G') {
        unsigned w = (u8[16] << 24) | (u8[17] << 16) | (u8[18] << 8) | u8[19];
        unsigned h = (u8[20] << 24) | (u8[21] << 16) | (u8[22] << 8) | u8[23];
        return {w, h};
    }

    // JPEG: find SOF0 (0xFFC0) or SOF2 (0xFFC2) marker
    if (size >= 4 && u8[0] == 0xFF && u8[1] == 0xD8) {
        size_t pos = 2;
        while (pos + 4 < size) {
            if (u8[pos] != 0xFF) { pos++; continue; }
            uint8_t marker = u8[pos + 1];
            if (marker == 0xC0 || marker == 0xC2) {
                if (pos + 9 < size) {
                    unsigned h = (u8[pos + 5] << 8) | u8[pos + 6];
                    unsigned w = (u8[pos + 7] << 8) | u8[pos + 8];
                    return {w, h};
                }
                break;
            }
            if (marker == 0xD9 || marker == 0xDA) break; // EOI or SOS
            if (pos + 3 < size) {
                uint16_t seg_len = (u8[pos + 2] << 8) | u8[pos + 3];
                pos += 2 + seg_len;
            } else break;
        }
        return {0, 0};
    }

    // GIF: "GIF8" + version(2) + width(2 LE) + height(2 LE)
    if (size >= 10 && u8[0] == 'G' && u8[1] == 'I' && u8[2] == 'F') {
        unsigned w = u8[6] | (u8[7] << 8);
        unsigned h = u8[8] | (u8[9] << 8);
        return {w, h};
    }

    // BMP: "BM" + ... + width(4 LE at 18) + height(4 LE at 22)
    if (size >= 26 && u8[0] == 'B' && u8[1] == 'M') {
        unsigned w = u8[18] | (u8[19] << 8) | (u8[20] << 16) | (u8[21] << 24);
        int h_signed = static_cast<int>(u8[22] | (u8[23] << 8) | (u8[24] << 16) | (u8[25] << 24));
        unsigned h = static_cast<unsigned>(h_signed < 0 ? -h_signed : h_signed);
        return {w, h};
    }

    return {0, 0};
}

// Populate width/height on ImageData from its raw data if not already set.
inline void populate_image_dimensions(jdoc::ImageData& img) {
    if (img.width > 0 && img.height > 0) return;
    if (img.data.empty()) return;
    auto [w, h] = image_dimensions_from_data(img.data.data(), img.data.size());
    if (w > 0 && h > 0) { img.width = w; img.height = h; }
}

// Check if image is below minimum size threshold.
// Returns true if image should be skipped (both dimensions < min_size).
inline bool is_image_too_small(const jdoc::ImageData& img, unsigned min_size) {
    if (min_size == 0) return false;
    if (img.width == 0 && img.height == 0) return false; // unknown dims, keep
    return img.width < min_size && img.height < min_size;
}

}} // namespace jdoc::util
