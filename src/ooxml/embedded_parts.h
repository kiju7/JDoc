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
#include "jdoc/types.h"
#include "common/file_utils.h"

#include <cstring>
#include <string>
#include <vector>

namespace jdoc {

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
        std::string base = util::to_single_line(leaf);
        if (base.empty()) continue;
        list += "- " + base;
        if (e.uncompressed_size > 0)
            list += " (" + util::human_bytes(e.uncompressed_size) + ")";
        list += "\n";
    }
    if (list.empty()) return "";
    return "\n## Attachments\n\n" + list + "\n";
}

// Mirror the listing into the last chunk, so the chunk APIs and the
// whole-document API agree about what the package contains. This is what the
// header/footer and master-layout trailers already do.
inline void append_embedded_parts(const ZipReader& zip,
                                  std::vector<PageChunk>& chunks) {
    std::string block = format_embedded_parts(zip);
    if (block.empty()) return;
    if (chunks.empty()) {
        PageChunk c;
        c.page_number = 1;
        chunks.push_back(std::move(c));
    }
    chunks.back().text += block;
}

} // namespace jdoc
