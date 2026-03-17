#pragma once
// XLSB (Binary Excel) parser
// Parses ZIP-based .xlsb files containing binary records

#include "zip_reader.h"
#include "jdoc/types.h"
#include <string>
#include <vector>
#include <map>

namespace jdoc {

class XlsbParser {
public:
    explicit XlsbParser(ZipReader& zip);

    std::string to_markdown(const ConvertOptions& opts);
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

private:
    ZipReader& zip_;

    // Shared strings table
    std::vector<std::string> shared_strings_;

    // Sheet info
    struct SheetInfo {
        std::string name;
        std::string file_path;  // e.g. "xl/worksheets/sheet1.bin"
    };
    std::vector<SheetInfo> sheets_;

    // Number format: xf index -> numFmtId
    std::vector<int> xf_num_fmt_ids_;
    // Custom number formats: numFmtId -> formatCode
    std::map<int, std::string> custom_num_fmts_;

    void parse_shared_strings();
    void parse_workbook();
    void parse_styles();

    // XLSB variable-length integer reading
    static uint16_t read_record_header(const uint8_t* data, size_t& offset,
                                        uint32_t& out_size);
    static std::string read_xl_widestring(const uint8_t* data, size_t& offset,
                                            size_t end);

    // Number formatting (shared logic with XLSX)
    std::string format_number(const std::string& raw_value, int style_idx) const;
    static std::string serial_to_date(double serial);
    static std::string serial_to_time(double serial);
    static bool is_date_format(int fmt_id, const std::string& fmt_code);

    struct SheetData {
        std::string name;
        std::map<int, std::map<int, std::string>> cells;
        int max_row = 0;
        int max_col = 0;
    };

    SheetData parse_sheet(const SheetInfo& info);
    std::string format_sheet_as_table(const SheetData& sheet, int max_rows = 10000);
};

} // namespace jdoc
