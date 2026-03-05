#pragma once
// .doc (Word Binary / BIFF) parser.
// Extracts text and images from legacy Word documents via OLE streams.

#include "ole_reader.h"
#include "jdoc/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jdoc {

class DocParser {
public:
    explicit DocParser(OleReader& ole);

    // Convert the entire document to a markdown string.
    std::string to_markdown(const ConvertOptions& opts);

    // Convert the document to page chunks (one chunk for the whole doc).
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

private:
    OleReader& ole_;

    // Text extraction from FIB / Clx / piece table.
    std::string extract_text();

    // Image extraction: scan Data stream and WordDocument for embedded images.
    std::vector<ImageData> extract_images();

    // Convert raw extracted text to markdown with heuristic formatting.
    std::string text_to_markdown(const std::string& raw_text);

    // Determine image format from magic bytes; returns format string or empty.
    static std::string detect_image_format(const char* data, size_t len);

    // Find end of JPEG image starting at data.
    static size_t find_jpeg_end(const char* data, size_t len);

    // Find end of PNG image starting at data.
    static size_t find_png_end(const char* data, size_t len);
};

} // namespace jdoc
