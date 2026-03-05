#pragma once
// jdoc - Shared data types for all document converters
// License: MIT

#include <string>
#include <vector>

namespace jdoc {

enum class OutputFormat {
    MARKDOWN,   // Default: Markdown with headings, bold, tables, image refs
    PLAINTEXT   // Plain text with page separators (--- Page N ---)
};

struct ImageData {
    int page_number = 0;
    std::string name;
    unsigned width = 0;
    unsigned height = 0;
    std::vector<char> data;
    std::string format;
    std::string saved_path;
};

struct PageChunk {
    int page_number = 0;
    std::string text;
    double page_width = 0;
    double page_height = 0;
    double body_font_size = 0;
    std::vector<std::vector<std::vector<std::string>>> tables;
    std::vector<ImageData> images;
};

struct ConvertOptions {
    std::vector<int> pages;
    bool extract_tables = true;
    bool page_chunks = false;
    bool extract_images = false;
    std::string image_output_dir;
    OutputFormat output_format = OutputFormat::MARKDOWN;
};

} // namespace jdoc
