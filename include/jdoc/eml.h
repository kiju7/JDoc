#pragma once
// jdoc - EML (RFC 5322 / MIME) email to Markdown
// License: MIT

#include "jdoc/types.h"
#include <string>
#include <vector>

namespace jdoc {

std::string eml_to_markdown(const std::string& file_path, ConvertOptions opts = {});
std::string eml_to_markdown_mem(const uint8_t* data, size_t size,
                                ConvertOptions opts = {});
std::vector<PageChunk> eml_to_markdown_chunks(const std::string& file_path,
                                              ConvertOptions opts = {});

} // namespace jdoc
