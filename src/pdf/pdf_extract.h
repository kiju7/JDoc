#pragma once
// pdf_extract.h — internal: aggregate result types and per-cluster entry points.
#include "pdf_content.h"

namespace jdoc { namespace pdf_detail {

struct TableData {
    std::vector<std::vector<std::string>> rows;
    std::string title;  // full-width title row extracted from top of table
    double x0, y0, x1, y1;
    int page = 0;
};

struct FontStats {
    double body_size = 12.0;

    void compute(const std::vector<std::vector<TextLine>>& all_lines) {
        std::map<int, int> counts;
        for (auto& pl : all_lines)
            for (auto& l : pl)
                if (l.font_size > 1.0)
                    counts[static_cast<int>(l.font_size * 10)]++;

        int max_c = 0, max_k = 120;
        for (auto& [k, c] : counts)
            if (c > max_c) { max_c = c; max_k = k; }
        body_size = max_k / 10.0;
        if (body_size < 4.0) body_size = 12.0;
    }

    int heading_level(double fs, bool is_bold = false) const {
        if (fs <= 0) return 0;
        double r = fs / body_size;
        if (r >= 1.8) return 1;
        if (r >= 1.5) return 2;
        if (r >= 1.3) return 3;
        if (is_bold && r >= 1.1) return 3;
        return 0;
    }
};

struct ExtractedImage {
    ImageData img;
    double ctm[6];
};

struct BookmarkEntry {
    std::string title;
    int page = -1;
    int level = 0;
};

struct AnnotEntry {
    std::string text;     // annotation body text
    std::string uri;      // for Link annotations
    std::string subtype;  // Text, Link, FreeText, etc.
    double y = 0;         // vertical position on page
};

struct ExtractResult {
    std::vector<std::vector<TextLine>> all_lines;
    std::vector<std::vector<ImageData>> all_images;
    std::vector<std::vector<double>> all_image_y;  // per-page image Y positions (PDF coords, top=large)
    std::vector<std::vector<double>> all_image_x;  // per-page image X positions
    std::vector<double> col_boundaries;  // per-page column boundary (0 if single-column)
    std::vector<std::vector<TableData>> all_tables;
    std::vector<std::vector<AnnotEntry>> all_annots;
    std::vector<double> page_widths;
    std::vector<double> page_heights;
    std::vector<BookmarkEntry> bookmarks;
    FontStats stats;
    int total_pages = 0;
};

// Cross-translation-unit declarations.
std::vector<TableData> detect_tables(const std::vector<PdfLineSegment>& lines,
                                     const PageCharCache& cache,
                                     double page_width, double page_height);
std::vector<TableData> detect_text_tables(const PageCharCache& cache,
                                          const std::vector<TableData>& existing_tables,
                                          double page_width, double page_height);
std::string format_table(const TableData& table);
std::vector<ExtractedImage> extract_page_images(PdfDoc& doc, const PdfObj& page_obj,
                                                const ContentParseResult& parse_result,
                                                int page_num,
                                                const std::string& output_dir,
                                                unsigned min_image_size = 0);
ImageData render_page_composite(PdfDoc& doc, const PdfObj& page_obj,
                                const ContentParseResult& parse_result,
                                int page_num, double page_w, double page_h,
                                const std::string& output_dir);
void collect_bookmarks(PdfDoc& doc, const PdfObj& node, int depth,
                       std::vector<BookmarkEntry>& out);
std::vector<AnnotEntry> extract_annotations(PdfDoc& doc, const PdfObj& page_obj, double page_h);
std::string result_to_markdown(ExtractResult& r, const ConvertOptions& opts);
std::vector<PageChunk> result_to_chunks(ExtractResult& r, const ConvertOptions& opts);
void stream_result_chunks(ExtractResult& r, const ConvertOptions& opts,
                          const PageSink& sink);

}} // namespace jdoc::pdf_detail
