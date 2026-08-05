#pragma once
// XLSX (Office Open XML Spreadsheet) parser
// Extracts sheet data as markdown tables from .xlsx files

#include "zip_reader.h"
#include "jdoc/types.h"
#include <string>
#include <string_view>
#include <vector>
#include <map>

namespace jdoc {

class XlsxParser {
public:
    explicit XlsxParser(ZipReader& zip);

    /// Convert entire workbook to a single markdown string.
    std::string to_markdown(const ConvertOptions& opts);

    /// Convert workbook to per-sheet chunks.
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

    /// Streaming variant: emit one sheet chunk at a time (parsing each sheet on
    /// demand), so the working set tracks a single sheet's table rather than the
    /// whole workbook, and the first sheet is available before the rest are
    /// parsed. Images are still enumerated up front (needed for text refs), so
    /// image bytes are resident regardless. Byte-identical to to_chunks().
    /// Returns false if the sink stopped early.
    bool to_chunks(const ConvertOptions& opts, const PageSink& sink);

private:
    ZipReader& zip_;

    // Shared strings table (index -> string), stored as one text arena plus
    // an offset per entry. A workbook can carry tens of millions of shared
    // strings; per-entry std::string would mean that many separate heap
    // allocations, while the arena keeps it to two.
    class SharedStringStore {
    public:
        void reserve(size_t count, size_t bytes) {
            offs_.reserve(count + 1);
            arena_.reserve(bytes);
        }
        void push(std::string_view s) {
            arena_.append(s.data(), s.size());
            offs_.push_back(arena_.size());
        }
        size_t size() const { return offs_.size() - 1; }
        std::string_view get(size_t i) const {
            return {arena_.data() + offs_[i], offs_[i + 1] - offs_[i]};
        }
    private:
        std::string arena_;
        std::vector<uint64_t> offs_ = {0};
    };
    SharedStringStore shared_strings_;

    // Sheet info
    struct SheetInfo {
        std::string name;       // display name
        std::string r_id;       // relationship ID (e.g. "rId1")
        std::string file_path;  // resolved path (e.g. "xl/worksheets/sheet1.xml")
    };
    std::vector<SheetInfo> sheets_;

    // Number format: style index (xf) -> numFmtId
    std::vector<int> xf_num_fmt_ids_;
    // Font index per xf entry (for bold/italic lookup)
    std::vector<int> xf_font_ids_;
    // Per-font bold flag
    std::vector<bool> font_bold_;
    // Custom number formats: numFmtId -> formatCode
    std::map<int, std::string> custom_num_fmts_;

    // Threaded-comment authors: personId (GUID) -> display name
    std::map<std::string, std::string> persons_;

    void parse_shared_strings();
    void parse_workbook();
    void parse_workbook_rels();
    void parse_persons();
    void parse_styles();

    // ── Streaming (SAX) path for parts too large for the DOM ─────────
    // Threshold above which a part is scanned in streaming mode instead of
    // being loaded into pugixml (env JDOC_XLSX_STREAM_THRESHOLD overrides,
    // for tests / tuning).
    static uint64_t stream_threshold();
    // True when this sheet's XML part exceeds the streaming threshold.
    bool sheet_is_streamed(const SheetInfo& info) const;
    // Load xl/sharedStrings.xml through the chunked scanner into the arena.
    // Throws std::runtime_error on a corrupt or oversized part.
    void parse_shared_strings_streamed(const ZipReader::Entry& entry);
    // Stream one sheet's XML directly into `out` as a markdown table,
    // without materializing SheetData. Grid semantics match parse_sheet +
    // format_sheet_as_table. Returns the number of nonempty cells emitted.
    // Throws std::runtime_error on a corrupt or oversized part.
    uint64_t stream_sheet_markdown(const SheetInfo& info, std::string& out);

    // Parse a cell reference like "A1" -> (col_index, row_index) both 0-based
    static std::pair<int, int> parse_cell_ref(const std::string& ref);
    // Convert column letter(s) to 0-based index: A=0, B=1, ..., Z=25, AA=26
    static int column_to_index(const std::string& col);

    // Format a numeric cell value based on style index
    std::string format_number(const std::string& raw_value, int style_idx) const;

    // Check if a style index references a bold font
    bool is_bold_style(int style_idx) const;

    // Excel serial date -> "YYYY-MM-DD"
    static std::string serial_to_date(double serial);
    // Excel serial time -> "HH:MM:SS"
    static std::string serial_to_time(double serial);
    // Detect if a numFmtId is a date/time format
    static bool is_date_format(int fmt_id, const std::string& fmt_code);

    struct CellInfo {
        std::string value;
        bool bold = false;
    };

    struct Cell {
        int row;
        int col;
        CellInfo info;
    };

    struct SheetData {
        std::string name;
        // Cells in row-major (row, then col) order — see parse_sheet, which
        // sorts once after building. A flat vector rather than a nested
        // std::map: building is push_back (no per-cell red-black-tree insert or
        // teardown), and the table renderer walks it with a single advancing
        // cursor instead of an O(log n) lookup per cell. Stays sparse (no dense
        // grid) so a huge but mostly-empty sheet cannot blow up memory.
        std::vector<Cell> cells;
        int max_row = 0;
        int max_col = 0;
    };

    SheetData parse_sheet(const SheetInfo& info);

    // Per-sheet chunk builder (base text + tables, no images) shared by the
    // eager and streaming to_chunks.
    PageChunk build_sheet_chunk(size_t sheet_index, const ConvertOptions& opts);
    static bool sheet_wanted(int sheet_num, const ConvertOptions& opts);

    // Parse comments for a sheet, return cell_ref -> comment_text
    // (merges legacy xl/comments*.xml and modern threaded comments).
    std::map<std::string, std::string> parse_comments(const SheetInfo& info);
    void parse_legacy_comments(const std::string& path,
                               std::map<std::string, std::string>& out);
    void parse_threaded_comments(const std::string& path,
                                 std::map<std::string, std::string>& out);

    std::string format_sheet_as_table(const SheetData& sheet,
                                       int max_rows = 0);  // 0 = no limit

    std::vector<ImageData> extract_images(
        const ConvertOptions& opts);
};

} // namespace jdoc
