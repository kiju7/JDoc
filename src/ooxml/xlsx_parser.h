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

    void parse_shared_strings();
    void parse_workbook();
    void parse_workbook_rels();

    // Parse a cell reference like "A1" -> (col_index, row_index) both 0-based
    static std::pair<int, int> parse_cell_ref(const std::string& ref);
    // Convert column letter(s) to 0-based index: A=0, B=1, ..., Z=25, AA=26
    static int column_to_index(const std::string& col);

    struct SheetData {
        std::string name;
        // Sparse grid: row -> col -> value
        std::map<int, std::map<int, std::string>> cells;
        int max_row = 0;
        int max_col = 0;
    };

    SheetData parse_sheet(const SheetInfo& info);

    std::string format_sheet_as_table(const SheetData& sheet,
                                       int max_rows = 10000);

    std::vector<ImageData> extract_images(
        const ConvertOptions& opts);
};

} // namespace jdoc
