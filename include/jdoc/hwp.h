#pragma once
// jdoc - HWP (Hangul Word Processor binary) to Markdown converter
// License: MIT

#include "jdoc/types.h"

namespace jdoc {

std::string hwp_to_markdown(const std::string& hwp_path,
                             ConvertOptions opts = {});

std::vector<PageChunk> hwp_to_markdown_chunks(const std::string& hwp_path,
                                               ConvertOptions opts = {});

} // namespace jdoc
