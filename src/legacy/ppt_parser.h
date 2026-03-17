#pragma once
// .ppt (PowerPoint Binary) parser.
// Extracts slide text and images from legacy PowerPoint files via OLE streams.

#include "ole_reader.h"
#include "jdoc/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jdoc {

class PptParser {
public:
    explicit PptParser(OleReader& ole);

    // Convert all slides to a markdown string.
    std::string to_markdown(const ConvertOptions& opts);

    // Convert to page chunks (one chunk per slide).
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

private:
    OleReader& ole_;

    // Represents a single extracted slide.
    struct Slide {
        int number = 0;
        std::string title;
        std::string body;
        std::string notes;
    };

    std::vector<Slide> slides_;

    // Parse the "PowerPoint Document" stream and extract slide text.
    void parse_document();

    // Extract images from the "Pictures" stream.
    std::vector<ImageData> extract_images();

    // Format a slide as markdown.
    static std::string slide_to_markdown(const Slide& slide);

};

} // namespace jdoc
