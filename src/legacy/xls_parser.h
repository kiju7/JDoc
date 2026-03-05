#pragma once
// .xls (BIFF8) parser.
// Extracts cell data and images from legacy Excel spreadsheets via OLE streams.

#include "ole_reader.h"
#include "jdoc/types.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace jdoc {

class XlsParser {
public:
    explicit XlsParser(OleReader& ole);

    // Convert all sheets to markdown string (tables).
    std::string to_markdown(const ConvertOptions& opts);

    // Convert to page chunks (one chunk per sheet).
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

private:
    OleReader& ole_;

    // A single cell value.
    struct Cell {
        uint16_t row;
        uint16_t col;
        std::string value;
    };

    // A parsed sheet with name and cell data.
    struct Sheet {
        std::string name;
        std::vector<Cell> cells;
    };

    // Shared String Table.
    std::vector<std::string> sst_;

    // Parsed sheets.
    std::vector<Sheet> sheets_;

    // Parse the Workbook (or Book) stream.
    void parse_workbook();

    // Parse the SST record (and CONTINUE records that follow).
    void parse_sst(const char* data, size_t len,
                   const std::vector<std::vector<char>>& continues);

    // Parse a single XLUnicodeRichExtendedString from a buffer.
    // Returns the string and advances pos past the string data.
    std::string parse_xl_string(const char* data, size_t len, size_t& pos,
                                bool* crossed_continue = nullptr) const;

    // Decode an RK value to a double.
    static double decode_rk(uint32_t rk);

    // Format a double as a string (strip trailing zeros).
    static std::string format_number(double val);

    // Build a markdown table from sheet cells.
    static std::string sheet_to_markdown(const Sheet& sheet);

    // Extract images from MSODRAWING records.
    std::vector<ImageData> extract_images();

};

} // namespace jdoc
