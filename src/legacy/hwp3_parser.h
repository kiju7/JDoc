#pragma once
// .hwp 3.x (Hangul Word Processor 2.x/3.x legacy binary) parser.
// Extracts body text (paragraphs, table cells, header/footer, footnotes)
// as markdown. Images and complex formatting are skipped.
// Reference: HWP 3.0 file format spec (Hancom); cross-checked against
// the LibreOffice hwpfilter implementation.
// Does NOT depend on OLE reader (HWP 3.x is a flat binary stream).

#include "jdoc/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jdoc {

// True if the buffer starts with an HWP 2.x/3.x signature
// ("HWP Document File V...").
bool is_hwp3_signature(const uint8_t* data, size_t size);

class Hwp3Parser {
public:
    explicit Hwp3Parser(const std::string& file_path);  // reads the file
    Hwp3Parser(const uint8_t* data, size_t size);       // borrows the buffer

    // Convert the document to a markdown string.
    std::string to_markdown(const ConvertOptions& opts);

    // Convert to page chunks (single chunk for the entire document).
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

private:
    std::vector<uint8_t> owned_;        // backing store for the file variant
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

} // namespace jdoc
