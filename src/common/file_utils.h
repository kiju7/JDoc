#pragma once
// File and path utility functions
// License: MIT

#include <algorithm>
#include <cctype>
#include <string>
#include <sys/stat.h>

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
    if (!dir.empty()) mkdir(dir.c_str(), 0755);
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
        // Image: ![alt](path) -> skip entirely
        if (md[i] == '!' && i + 1 < len && md[i + 1] == '[') {
            size_t j = i + 2;
            while (j < len && md[j] != ']') j++;
            if (j < len && j + 1 < len && md[j + 1] == '(') {
                size_t k = j + 2;
                while (k < len && md[k] != ')') k++;
                if (k < len) { i = k + 1; continue; }
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
                if (!row.empty()) row += '\t';
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

}} // namespace jdoc::util
