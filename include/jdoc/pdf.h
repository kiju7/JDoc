#pragma once
// jdoc - PDF to Markdown converter
// Backend: PDFium
// License: MIT

#include "jdoc/types.h"

namespace jdoc {

std::string pdf_to_markdown(const std::string& pdf_path,
                             ConvertOptions opts = {});

std::vector<PageChunk> pdf_to_markdown_chunks(const std::string& pdf_path,
                                               ConvertOptions opts = {});

} // namespace jdoc
