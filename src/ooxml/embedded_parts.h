#pragma once
// Embedded object parts of an OOXML package.
//
// Word, Excel and PowerPoint keep an inserted object — a spreadsheet behind a
// chart, a CAD drawing dropped into a report — as its own package part under
// <word|xl|ppt>/embeddings/. Each is listed by name, the way the PDF layer
// lists /EmbeddedFiles, followed by its text when it is a document jdoc reads.
// License: MIT

#include "zip_reader.h"
#include "jdoc/types.h"
#include "common/file_utils.h"
#include "common/embedded.h"

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
inline std::string format_embedded_parts(const ZipReader& zip,
                                         const ConvertOptions& opts) {
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
        auto bytes = zip.read_entry(e);
        std::string text = embedded_document_text(
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), leaf, opts);
        if (!text.empty()) list += "\n" + text + "\n\n";
    }
    if (list.empty()) return "";
    return "\n## Attachments\n\n" + list + "\n";
}

// Mirror the listing into the last chunk, so the chunk APIs and the
// whole-document API agree about what the package contains. This is what the
// header/footer and master-layout trailers already do.
inline void append_embedded_parts(const ZipReader& zip, const ConvertOptions& opts,
                                  std::vector<PageChunk>& chunks) {
    std::string block = format_embedded_parts(zip, opts);
    if (block.empty()) return;
    if (chunks.empty()) {
        PageChunk c;
        c.page_number = 1;
        chunks.push_back(std::move(c));
    }
    chunks.back().text += block;
}

} // namespace jdoc
