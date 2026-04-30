#pragma once
// jdoc - Shared data types for all document converters
// License: MIT

#include <cstdint>
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
    int components = 3;             // 1=gray, 3=RGB, 4=CMYK
    std::vector<char> data;         // encoded bytes (jpeg/png) or raw pixels
    std::vector<uint8_t> pixels;    // raw pixel buffer (width * height * components)
    std::string format;             // "jpeg", "png", "raw"
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
    std::string image_ref_prefix;   // prepended to image filenames in markdown refs
    unsigned min_image_size = 50;   // skip images smaller than NxN pixels (0 = no filter)
    OutputFormat output_format = OutputFormat::MARKDOWN;
};

} // namespace jdoc
