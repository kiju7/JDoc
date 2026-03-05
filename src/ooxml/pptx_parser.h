#pragma once
// PPTX (Office Open XML Presentation) parser
// Extracts text, titles, tables, and images from .pptx files

#include "zip_reader.h"
#include "xml_utils.h"
#include "jdoc/types.h"
#include <string>
#include <vector>

namespace jdoc {

class PptxParser {
public:
    explicit PptxParser(ZipReader& zip);

    /// Convert entire presentation to a single markdown string.
    std::string to_markdown(const ConvertOptions& opts);

    /// Convert presentation to per-slide chunks.
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

private:
    ZipReader& zip_;

    // Sorted list of slide entry paths (e.g. "ppt/slides/slide1.xml")
    std::vector<std::string> slide_paths_;

    void enumerate_slides();

    struct SlideContent {
        std::string title;
        std::string body_text;
        std::vector<std::vector<std::vector<std::string>>> tables;
    };

    SlideContent parse_slide(const std::string& slide_path);
    void extract_text_from_shape(const pugi::xml_node& sp,
                                  SlideContent& content);
    void extract_text_from_group(const pugi::xml_node& grpSp,
                                  SlideContent& content);
    std::vector<std::vector<std::string>> parse_table(
        const pugi::xml_node& tbl);

    std::vector<ImageData> extract_images(
        const ConvertOptions& opts);

    std::string format_table(
        const std::vector<std::vector<std::string>>& rows);
};

} // namespace jdoc
