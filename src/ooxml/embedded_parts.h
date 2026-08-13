#pragma once
// Embedded object parts of an OOXML package.
//
// Word, Excel and PowerPoint keep an inserted object — a spreadsheet behind a
// chart, a CAD drawing dropped into a report — as its own package part under
// <word|xl|ppt>/embeddings/. jdoc does not open those payloads, and dropping
// them without a word left callers indexing the document with no idea the file
// was there. This lists them, the way the PDF layer lists /EmbeddedFiles.
// License: MIT

#include "zip_reader.h"
#include "common/file_utils.h"

#include <cstring>
#include <string>

namespace jdoc {

// The part name is attacker-controlled — a zip entry name may hold newlines,
// and one carrying "\n\n## Table of Contents\n\n- ..." would forge headings in
// the output. Control characters collapse to spaces so a name can only ever be
// one list item.
inline std::string sanitize_part_name(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name)
        out += (c < 0x20 || c == 0x7F) ? ' ' : static_cast<char>(c);
    return util::trim(out);
}

// True for a part directly inside one of the three embedding directories.
// Matching the "/embeddings/" segment anywhere would also claim
// word/media/embeddings/logo.png and anything a crafted package cared to name.
inline bool is_embedding_part(const std::string& name, std::string& leaf) {
    static const char* kDirs[] = {
        "word/embeddings/", "xl/embeddings/", "ppt/embeddings/",
    };
    for (const char* dir : kDirs) {
        size_t n = std::strlen(dir);
        if (name.compare(0, n, dir) != 0) continue;
        leaf = name.substr(n);
        // Directory entry, or something nested deeper than the parts Office
        // writes — neither is an embedded object.
        return !leaf.empty() && leaf.find('/') == std::string::npos;
    }
    return false;
}

// Markdown for a package's embedded object parts, or "" when it has none.
// Includes the trailing blank line so callers can append it unconditionally.
inline std::string format_embedded_parts(const ZipReader& zip) {
    std::string list;
    for (const auto& e : zip.entries()) {
        std::string leaf;
        if (!is_embedding_part(e.name, leaf)) continue;
        std::string base = sanitize_part_name(leaf);
        if (base.empty()) continue;
        list += "- " + base;
        if (e.uncompressed_size > 0)
            list += " (" + util::human_bytes(e.uncompressed_size) + ")";
        list += "\n";
    }
    if (list.empty()) return "";
    return "\n## Attachments\n\n" + list + "\n";
}

} // namespace jdoc
