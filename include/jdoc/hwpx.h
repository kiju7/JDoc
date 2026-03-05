#pragma once
// jdoc - HWPX (Hangul Word Processor XML) to Markdown converter
// License: MIT

#include "jdoc/types.h"

namespace jdoc {

std::string hwpx_to_markdown(const std::string& hwpx_path,
                              ConvertOptions opts = {});

std::vector<PageChunk> hwpx_to_markdown_chunks(const std::string& hwpx_path,
                                                ConvertOptions opts = {});

} // namespace jdoc
