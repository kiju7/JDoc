#pragma once
// HTML/HTM to Markdown parser
// Extracts text, headings, tables, lists, and images from HTML files
// License: MIT

#include "jdoc/types.h"
#include <string>
#include <vector>

namespace jdoc {

class HtmlParser {
public:
    explicit HtmlParser(const std::string& file_path);

    std::string to_markdown(const ConvertOptions& opts);
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

private:
    std::string file_path_;
    std::string raw_html_;

    // Simple HTML tag representation
    struct Tag {
        std::string name;       // lowercase tag name
        bool is_closing = false;
        bool is_self_closing = false;
        std::string get_attr(const std::string& attrs, const std::string& key) const;
        std::string attrs;      // raw attribute string
    };

    // Parse HTML and produce markdown
    std::string convert(const ConvertOptions& opts, std::vector<ImageData>& out_images);

    // Read next tag from position, returns false if no more tags
    bool read_tag(size_t& pos, Tag& tag) const;

    // Read text content until next tag
    std::string read_text(size_t& pos) const;

    // Decode HTML entities (&amp; &lt; etc.)
    static std::string decode_entities(const std::string& text);

    // Check if tag is a block element
    static bool is_block_tag(const std::string& name);

    // Check if tag is a void/self-closing element
    static bool is_void_tag(const std::string& name);
};

} // namespace jdoc
