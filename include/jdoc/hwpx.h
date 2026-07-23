#pragma once
// jdoc - HWPX (Hangul Word Processor XML) to Markdown converter
// License: MIT

#include "jdoc/types.h"

namespace jdoc {

std::string hwpx_to_markdown(const std::string& hwpx_path,
                              ConvertOptions opts = {});

std::vector<PageChunk> hwpx_to_markdown_chunks(const std::string& hwpx_path,
                                                ConvertOptions opts = {});

// In-memory variant (used for archive members parsed without extraction)
std::string hwpx_to_markdown_mem(const uint8_t* data, size_t size,
                                 ConvertOptions opts = {});

// Streaming variant: emit one section XML part at a time; return false to stop.
void hwpx_to_markdown_chunks_stream(const std::string& hwpx_path,
                                    const ConvertOptions& opts, const PageSink& sink);

} // namespace jdoc
