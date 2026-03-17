#pragma once
// .doc (Word Binary / BIFF) parser.
// Extracts text and images from legacy Word documents via OLE streams.
// Supports: Word 6/95, Word 97-2003, encrypted document detection,
// field markers, smart quotes, CJK encodings (CP949/CP932).

#include "ole_reader.h"
#include "jdoc/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jdoc {

class DocParser {
public:
    explicit DocParser(OleReader& ole);

    // Convert the entire document to a markdown string.
    std::string to_markdown(const ConvertOptions& opts);

    // Convert the document to page chunks (one chunk for the whole doc).
    std::vector<PageChunk> to_chunks(const ConvertOptions& opts);

private:
    OleReader& ole_;

    // Word version detected from FIB.
    enum class WordVersion { UNKNOWN, WORD6, WORD7, WORD8_PLUS };
    WordVersion version_ = WordVersion::UNKNOWN;

    // Language ID from FIB (used for DBCS detection).
    uint16_t lid_ = 0;

    // Text extraction from FIB / Clx / piece table.
    std::string extract_text();

    // Word 6/95 piece table extraction (simplified CLX layout).
    std::string extract_text_word6(const std::vector<char>& word_doc,
                                    const std::vector<char>& table_stream);

    // Word 97+ piece table extraction (full CLX/PlcPcd layout).
    std::string extract_text_word8(const std::vector<char>& word_doc,
                                    const std::vector<char>& table_stream);

    // Process a single character from a piece, handling field markers,
    // smart quotes, and control characters. Returns false to skip.
    bool process_char(uint32_t ch, std::string& result);

    // Image extraction: scan Data stream and WordDocument for embedded images.
    std::vector<ImageData> extract_images();

    // Convert raw extracted text to markdown with heuristic formatting.
    std::string text_to_markdown(const std::string& raw_text);

    // Determine image format from magic bytes; returns format string or empty.
    static std::string detect_image_format(const char* data, size_t len);

    // Find end of JPEG image starting at data.
    static size_t find_jpeg_end(const char* data, size_t len);

    // Find end of PNG image starting at data.
    static size_t find_png_end(const char* data, size_t len);

    // Field nesting state.
    int field_depth_ = 0;
    bool field_show_result_ = false;
};

} // namespace jdoc
