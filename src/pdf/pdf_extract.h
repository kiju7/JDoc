#pragma once
// pdf_extract.h — internal: aggregate result types and per-cluster entry points.
#include "pdf_content.h"

namespace jdoc { namespace pdf_detail {

struct TableData {
    std::vector<std::vector<std::string>> rows;
    std::string title;  // full-width title row extracted from top of table
    double x0, y0, x1, y1;
    int page = 0;
    // What the detection keyed on. Ruled/shading tables are drawn geometry
    // (their paths must not be mistaken for a vector figure); text tables
    // are alignment-only and own no paths.
    enum Kind { RULED, SHADING, TEXT } kind = RULED;
    // Header labels that straddle several columns ("BLEU" over EN-DE|EN-FR):
    // per column of rows[0], how many columns the label starting there
    // covers (0 = an ordinary cell). Consumed by merge_header_rows.
    std::vector<int> header_spans;
    // Per cell, whether every glyph is in a bold face; parallel to rows.
    // Kept out of the cell text so the shape heuristics (a marker row of
    // "1 | 2*", a numeric column) still see the bare text; format_table
    // applies it, and ignores a mask that has fallen out of step with rows.
    std::vector<std::vector<uint8_t>> cell_bold;
};

// Fold physical header rows into one: a second row whose first cell is
// blank (a units row: "(Acc)") or that fills the columns the first row left
// empty completes the header rather than opening the data.
void merge_header_rows(TableData& table);

// Ruled-table assembly helpers are exposed in the internal header so geometry
// regressions can be tested without manufacturing a complete PDF document.
std::vector<double> find_column_boundaries(
    const std::vector<PdfLineSegment>& v_lines,
    const std::vector<PdfLineSegment>& h_lines,
    double table_left, double table_right,
    double table_bot, double table_top,
    const std::vector<double>& row_ys);
TableData build_table(const std::vector<double>& row_ys,
                      const std::vector<PdfLineSegment>& h_lines,
                      const std::vector<PdfLineSegment>& v_lines,
                      const PageCharCache& cache);

// Heading-classification helpers (pdf_markdown.cpp), exposed the same way
// so the section-number and standalone-emphasis rules can be tested directly.
struct SectionNumber {
    int depth = 0;        // numeric segments; 0 = no section number
    size_t text_pos = 0;  // first byte of the title text
    bool closed = false;  // '.' closed the number
};
SectionNumber parse_section_number(const std::string& text);
size_t glued_mark_offset(const std::string& text);
bool line_all_caps(const std::string& text);
bool line_letter_spaced(const std::string& text);

struct FontStats {
    double body_size = 12.0;
    // Document-wide frequency of (rounded size, weight) line styles. The
    // standalone-heading rule uses it: a bold line only signals a heading
    // when the document uses that bold style sparingly.
    std::map<int, int> style_counts;
    int style_total = 0;

    static int style_key(double fs, bool bold) {
        return static_cast<int>(fs * 10) * 2 + (bold ? 1 : 0);
    }
    double style_share(double fs, bool bold) const {
        if (style_total == 0) return 0.0;
        auto it = style_counts.find(style_key(fs, bold));
        return it == style_counts.end()
                   ? 0.0
                   : static_cast<double>(it->second) / style_total;
    }

    void compute(const std::vector<std::vector<TextLine>>& all_lines,
                 const std::vector<std::vector<TableData>>& all_tables = {}) {
        // Table interiors skew the mode toward cell type (dense parameter
        // grids, chart labels): the body size is the most common size of
        // the text OUTSIDE tables. When everything sits in tables, fall
        // back to counting every line.
        std::map<int, int> counts, counts_all;
        for (size_t p = 0; p < all_lines.size(); p++) {
            const std::vector<TableData>* tabs =
                p < all_tables.size() ? &all_tables[p] : nullptr;
            for (auto& l : all_lines[p]) {
                if (l.font_size <= 1.0) continue;
                style_counts[style_key(l.font_size, l.is_bold)]++;
                style_total++;
                int key = static_cast<int>(l.font_size * 10);
                counts_all[key]++;
                bool in_table = false;
                if (tabs) {
                    double mid_x = (l.x_left + l.x_right) / 2.0;
                    for (auto& t : *tabs) {
                        if (l.y_center >= std::min(t.y0, t.y1) - 2.0 &&
                            l.y_center <= std::max(t.y0, t.y1) + 2.0 &&
                            mid_x >= std::min(t.x0, t.x1) - 2.0 &&
                            mid_x <= std::max(t.x0, t.x1) + 2.0) {
                            in_table = true;
                            break;
                        }
                    }
                }
                if (!in_table) counts[key]++;
            }
        }
        int kept = 0;
        for (auto& [k, c] : counts) kept += c;
        // A table-dominated page leaves too few free lines to vote (a page
        // that is one big table plus its caption): fall back to all lines.
        if (kept < 8) counts = std::move(counts_all);

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

// Per-page record of image decodes that produced no output. Failures used to
// vanish silently, leaving a blank composite indistinguishable from a clean
// one; callers and tests need the counts to detect degraded pages.
struct PageRenderDiag {
    int images_total = 0;        // image placements attempted
    int images_failed = 0;       // placements that produced no output
    int unsupported_filter = 0;  // JBIG2/JPX variant or unknown filter rejected
    int decode_size_mismatch = 0;
    int inline_images = 0;
    int inline_scan_bailouts = 0;
    int shading_unsupported = 0;
};

inline void discard_image_payload(ImageData& image) {
    decltype(image.data){}.swap(image.data);
    decltype(image.pixels){}.swap(image.pixels);
}

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

// A file carried inside the PDF (/Names /EmbeddedFiles). jdoc does not parse
// the payload — a CAD drawing or a spreadsheet attached to a document is not
// something it converts — but listing it keeps the attachment from vanishing
// silently from the output, which would leave callers indexing the PDF unaware
// the file is even there.
struct AttachmentEntry {
    std::string name;   // /UF, else /F
    std::string desc;   // /Desc, when the producer set one
    uint64_t size = 0;  // uncompressed bytes; 0 when the PDF does not say
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
    std::vector<PageRenderDiag> page_diags;
    std::vector<BookmarkEntry> bookmarks;
    std::vector<AttachmentEntry> attachments;
    FontStats stats;
    int total_pages = 0;
};

// Cross-translation-unit declarations.
std::vector<TableData> detect_tables(const std::vector<PdfLineSegment>& lines,
                                     const PageCharCache& cache,
                                     double page_width, double page_height);
std::vector<TableData> detect_shading_tables(
    const std::vector<PdfFillRect>& fill_rects,
    const PageCharCache& cache,
    const std::vector<TableData>& existing_tables,
    double page_width, double page_height);
std::vector<TableData> detect_text_tables(const PageCharCache& cache,
                                          const std::vector<TableData>& existing_tables,
                                          double page_width, double page_height,
                                          double col_boundary = 0.0);
std::string format_table(const TableData& table);
std::vector<ExtractedImage> extract_page_images(PdfDoc& doc, const PdfObj& resources,
                                                const ContentParseResult& parse_result,
                                                int page_num,
                                                const std::string& output_dir,
                                                unsigned min_image_size = 0,
                                                PageRenderDiag* diag = nullptr,
                                                const std::vector<size_t>* only = nullptr,
                                                int name_base = 0);
ImageData render_page_composite(PdfDoc& doc, const PdfObj& resources,
                                const ContentParseResult& parse_result,
                                int page_num, double page_w, double page_h,
                                const std::string& output_dir,
                                int img_idx = 0,
                                PageRenderDiag* diag = nullptr);
// Render only the given placements (indices into parse_result.images), plus
// every path intersecting the region, into a canvas cropped to region
// [x0,y0,x1,y1] in viewing coordinates. Used to composite one fragment
// cluster without losing the rest of the page.
ImageData render_region_composite(PdfDoc& doc, const PdfObj& resources,
                                  const ContentParseResult& parse_result,
                                  const std::vector<size_t>& members,
                                  int page_num, const double region[4],
                                  const std::string& output_dir,
                                  int img_idx,
                                  PageRenderDiag* diag = nullptr);
void collect_bookmarks(PdfDoc& doc, const PdfObj& node, int depth,
                       std::vector<BookmarkEntry>& out);
void collect_attachments(PdfDoc& doc, const PdfObj& root,
                         std::vector<AttachmentEntry>& out);
std::vector<AnnotEntry> extract_annotations(PdfDoc& doc, const PdfObj& page_obj, double page_h,
                                            const double* view_ctm = nullptr);
std::string result_to_markdown(ExtractResult& r, const ConvertOptions& opts);
std::vector<PageChunk> result_to_chunks(ExtractResult& r, const ConvertOptions& opts);
void stream_result_chunks(ExtractResult& r, const ConvertOptions& opts,
                          const PageSink& sink, bool release_per_page = true);

}} // namespace jdoc::pdf_detail
