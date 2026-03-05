#pragma once
// jdoc - Office document to Markdown converter
// Supports: DOCX, XLSX, PPTX, DOC, XLS, PPT, RTF, HTML
// License: MIT

#include "jdoc/types.h"
#include <string>
#include <vector>

namespace jdoc {

enum class DocFormat {
    UNKNOWN, DOCX, XLSX, PPTX, DOC, XLS, PPT, RTF, HTML
};

DocFormat detect_office_format(const std::string& file_path);
const char* format_name(DocFormat fmt);

std::string office_to_markdown(const std::string& file_path,
                                ConvertOptions opts = {});

std::vector<PageChunk> office_to_markdown_chunks(
    const std::string& file_path,
    ConvertOptions opts = {});

} // namespace jdoc
