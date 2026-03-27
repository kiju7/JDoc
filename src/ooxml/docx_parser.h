#pragma once
// DOCX (Office Open XML Word) parser
// Extracts text, headings, tables, lists, and images from .docx files

#include "zip_reader.h"
#include "jdoc/types.h"
#include <string>
#include <vector>
#include <map>

namespace jdoc {

class DocxParser {
public:
    explicit DocxParser(ZipReader& zip);

    /// Convert entire document to a single markdown string.
    std::string to_markdown(const ConvertOptions& opts);

    /// Convert document to per-page chunks (split on page breaks).
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

private:
    ZipReader& zip_;

    // Style ID -> heading level (1-9), 0 means not a heading
    std::map<std::string, int> style_heading_map_;

    // numId -> abstractNumId mapping
    std::map<int, int> num_to_abstract_;
    // abstractNumId -> ilvl -> numFmt ("decimal", "bullet", etc.)
    std::map<int, std::map<int, std::string>> abstract_num_formats_;

    // Relationship ID -> target path (for images and hyperlinks)
    std::map<std::string, std::string> rel_targets_;

    // Original media filename -> unified name (page1_img0, etc.)
    std::map<std::string, std::string> image_name_map_;

    // External hyperlink targets (rId -> URL)
    std::map<std::string, std::string> hyperlink_targets_;

    void parse_styles();
    void parse_numbering();
    void parse_relationships();

    // Extract text from header/footer XML parts.
    std::string extract_headers_footers();

    // Extract text from footnotes/endnotes XML parts.
    std::string extract_footnotes();
    std::string extract_endnotes();

    struct ParagraphInfo {
        std::string text;
        int heading_level = 0;
        bool is_list = false;
        bool is_ordered = false;
        int list_level = 0;
        bool is_page_break = false;
    };

    struct RunInfo {
        std::string text;
        bool bold = false;
        bool italic = false;
    };

    std::vector<ImageData> extract_images(const ConvertOptions& opts);

    std::string format_table(
        const std::vector<std::vector<std::string>>& rows);
};

} // namespace jdoc
