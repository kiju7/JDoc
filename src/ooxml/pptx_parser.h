#pragma once
// PPTX (Office Open XML Presentation) parser
// Extracts text, titles, tables, and images from .pptx files

#include "zip_reader.h"
#include "xml_utils.h"
#include "jdoc/types.h"
#include <map>
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

    struct SlideElement {
        enum Kind : uint8_t { TEXT, IMAGE, TABLE };
        Kind kind;
        std::string text;   // TEXT: markdown content, IMAGE: media path in ZIP
        std::vector<std::vector<std::string>> rows;  // TABLE only
    };

    struct SlideContent {
        std::string title;
        std::string notes;
        std::vector<SlideElement> elements;
    };

    SlideContent parse_slide(const std::string& slide_path);
    void extract_shape(const pugi::xml_node& sp,
                       const std::map<std::string, std::string>& rels,
                       SlideContent& content);
    void extract_group(const pugi::xml_node& grp_sp,
                       const std::map<std::string, std::string>& rels,
                       SlideContent& content);
    void extract_graphic_frame(const pugi::xml_node& gf,
                               const std::map<std::string, std::string>& rels,
                               SlideContent& content);
    void extract_picture(const pugi::xml_node& pic,
                         const std::map<std::string, std::string>& rels,
                         SlideContent& content);
    std::vector<std::vector<std::string>> parse_table(
        const pugi::xml_node& tbl);
    std::string extract_chart_text(const std::string& chart_path);
    std::string extract_diagram_text(const std::string& diagram_data_path);
    std::string extract_notes_text(const std::string& notes_path);

    ImageData extract_image_data(const std::string& media_path,
                                 int page_number,
                                 const ConvertOptions& opts);

    // Per-slide image index counter for unified naming
    std::map<int, int> slide_image_idx_;

    std::map<std::string, std::string> parse_slide_rels(const std::string& slide_path);

    std::string format_table(
        const std::vector<std::vector<std::string>>& rows);
};

} // namespace jdoc
