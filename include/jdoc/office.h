#pragma once
// jdoc - Office document to Markdown converter
// Supports: DOCX, XLSX, XLSB, PPTX, DOC, XLS, PPT, RTF, HTML
// License: MIT

#include "jdoc/types.h"
#include <string>
#include <vector>

namespace jdoc {

enum class DocFormat {
    UNKNOWN, DOCX, XLSX, XLSB, PPTX, DOC, XLS, PPT, RTF, HTML
};

DocFormat detect_office_format(const std::string& file_path);
const char* format_name(DocFormat fmt);

std::string office_to_markdown(const std::string& file_path,
                                ConvertOptions opts = {});

std::vector<PageChunk> office_to_markdown_chunks(
    const std::string& file_path,
    ConvertOptions opts = {});

// In-memory variants (used for archive members parsed without extraction).
// name_hint (e.g. the member filename) resolves extension-based ambiguity.
DocFormat detect_office_format_mem(const uint8_t* data, size_t size,
                                   const std::string& name_hint);

std::string office_to_markdown_mem(const uint8_t* data, size_t size,
                                   const std::string& name_hint,
                                   ConvertOptions opts = {});

std::vector<PageChunk> office_to_markdown_chunks_mem(const uint8_t* data, size_t size,
                                                     const std::string& name_hint,
                                                     ConvertOptions opts = {});

} // namespace jdoc
