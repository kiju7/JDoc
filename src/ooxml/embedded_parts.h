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

#include <string>

namespace jdoc {

// Markdown for a package's embedded object parts, or "" when it has none.
// Includes the trailing blank line so callers can append it unconditionally.
inline std::string format_embedded_parts(const ZipReader& zip) {
    std::string list;
    for (const auto& e : zip.entries()) {
        // Any of the three prefixes; matching the segment rather than each
        // one keeps this from caring which application wrote the package.
        if (e.name.find("/embeddings/") == std::string::npos) continue;
        std::string base = util::get_filename(e.name);
        if (base.empty()) continue;   // the directory entry itself
        list += "- " + base;
        if (e.uncompressed_size > 0)
            list += " (" + util::human_bytes(e.uncompressed_size) + ")";
        list += "\n";
    }
    if (list.empty()) return "";
    return "\n## Attachments\n\n" + list + "\n";
}

} // namespace jdoc
