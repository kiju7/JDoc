#pragma once
// XLSX (Office Open XML Spreadsheet) parser
// Extracts sheet data as markdown tables from .xlsx files

#include "zip_reader.h"
#include "jdoc/types.h"
#include <string>
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

private:
    ZipReader& zip_;

    // Shared strings table (index -> string)
    std::vector<std::string> shared_strings_;

    // Sheet info
    struct SheetInfo {
        std::string name;       // display name
        std::string r_id;       // relationship ID (e.g. "rId1")
        std::string file_path;  // resolved path (e.g. "xl/worksheets/sheet1.xml")
    };
    std::vector<SheetInfo> sheets_;

    // Number format: style index (xf) -> numFmtId
    std::vector<int> xf_num_fmt_ids_;
    // Custom number formats: numFmtId -> formatCode
    std::map<int, std::string> custom_num_fmts_;

    void parse_shared_strings();
    void parse_workbook();
    void parse_workbook_rels();
    void parse_styles();

    // Parse a cell reference like "A1" -> (col_index, row_index) both 0-based
    static std::pair<int, int> parse_cell_ref(const std::string& ref);
    // Convert column letter(s) to 0-based index: A=0, B=1, ..., Z=25, AA=26
    static int column_to_index(const std::string& col);

    // Format a numeric cell value based on style index
    std::string format_number(const std::string& raw_value, int style_idx) const;

    // Excel serial date -> "YYYY-MM-DD"
    static std::string serial_to_date(double serial);
    // Excel serial time -> "HH:MM:SS"
    static std::string serial_to_time(double serial);
    // Detect if a numFmtId is a date/time format
    static bool is_date_format(int fmt_id, const std::string& fmt_code);

    struct SheetData {
        std::string name;
        // Sparse grid: row -> col -> value
        std::map<int, std::map<int, std::string>> cells;
        int max_row = 0;
        int max_col = 0;
    };

    SheetData parse_sheet(const SheetInfo& info);

    // Parse comments for a sheet, return cell_ref -> comment_text
    std::map<std::string, std::string> parse_comments(const SheetInfo& info);

    std::string format_sheet_as_table(const SheetData& sheet,
                                       int max_rows = 10000);

    std::vector<ImageData> extract_images(
        const ConvertOptions& opts);
};

} // namespace jdoc
