#pragma once
// .rtf (Rich Text Format) parser.
// State-machine-based parser that extracts text and images from RTF documents.
// Does NOT depend on OLE reader.

#include "jdoc/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jdoc {

class RtfParser {
public:
    explicit RtfParser(const std::string& file_path);

    // Convert the document to a markdown string.
    std::string to_markdown(const ConvertOptions& opts);

    // Convert to page chunks (single chunk for the entire document).
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

private:
    std::string file_path_;
    std::vector<char> raw_data_;

    // Parser state for a single group level.
    struct State {
        bool skip = false;       // Skip this group's text output.
        bool bold = false;
        bool italic = false;
        bool in_pict = false;    // Inside a \pict group.
        int uc = 1;              // Number of bytes to skip after \uN.
    };

    // Extracted image from \pict group.
    struct PictImage {
        std::string format;       // "png", "jpeg", "emf", "wmf"
        unsigned width = 0;       // In twips.
        unsigned height = 0;
        std::string hex_data;     // Hex-encoded image bytes.
        std::vector<char> bin_data; // Raw binary data (from \bin).
    };

    // Parse the RTF content and produce text and images.
    void parse(std::string& out_text, std::vector<PictImage>& out_images);

    // Decode hex string to binary data.
    static std::vector<char> hex_to_binary(const std::string& hex);

};

} // namespace jdoc
