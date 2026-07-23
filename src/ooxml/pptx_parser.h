#pragma once
// PPTX (Office Open XML Presentation) parser
// Extracts text, titles, tables, and images from .pptx files

#include "zip_reader.h"
#include "xml_utils.h"
#include "jdoc/types.h"
#include "common/media_cache.h"
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

    /// Streaming variant: emit one slide chunk at a time (parsing and resolving
    /// each slide's images on demand), so peak memory tracks a single slide and
    /// the first slide is available before the rest are parsed. The trailing
    /// master/layout chunk is emitted last. Byte-identical to to_chunks().
    /// Returns false if the sink stopped early.
    bool to_chunks(const ConvertOptions& opts, const PageSink& sink);

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

    // Per-slide chunk builder shared by the eager and streaming to_chunks.
    PageChunk build_slide_chunk(size_t slide_index, const ConvertOptions& opts);
    bool emit_master_layout(const std::string& body, const PageSink& sink);
    static bool page_wanted(int slide_num, const ConvertOptions& opts);

    SlideContent parse_slide(const std::string& slide_path);
    void extract_shape(const pugi::xml_node& sp,
                       const std::map<std::string, std::string>& rels,
                       SlideContent& content);
    void extract_shape_tree(const pugi::xml_node& parent,
                            const std::map<std::string, std::string>& rels,
                            SlideContent& content, int depth);
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

    // Authored (non-placeholder) text from slide masters and layouts
    void collect_layout_shape_text(const pugi::xml_node& parent,
                                   std::vector<std::string>& out, int depth);
    std::vector<std::string> collect_master_layout_text();
    std::string format_master_layout_block(const std::string& body);

    ImageData extract_image_data(const std::string& media_path,
                                 int page_number,
                                 const ConvertOptions& opts);

    // Resolve a media part to its single extraction. The first reference reads,
    // measures and writes it; later references — the logo that recurs on every
    // slide — reuse that one file and its name. Returns false when the part is
    // absent from the package or filtered out by the minimum-size rule.
    // `out.name`/`ref_name` are the same on every reference, so the markdown
    // links all resolve to one file.
    bool resolve_image(const std::string& media_path, int page_number,
                       const ConvertOptions& opts,
                       ImageData& out, std::string& ref_name);

    // Media parts already extracted, keyed by part path
    util::MediaCache media_cache_;

    // Per-slide image index counter for unified naming
    std::map<int, int> slide_image_idx_;

    std::map<std::string, std::string> parse_slide_rels(const std::string& slide_path);
};

} // namespace jdoc
