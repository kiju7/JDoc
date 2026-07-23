#pragma once
// jdoc - HWP (Hangul Word Processor binary) to Markdown converter
// License: MIT

#include "jdoc/types.h"

namespace jdoc {

std::string hwp_to_markdown(const std::string& hwp_path,
                             ConvertOptions opts = {});

std::vector<PageChunk> hwp_to_markdown_chunks(const std::string& hwp_path,
                                               ConvertOptions opts = {});

// In-memory variant (used for archive members parsed without extraction)
std::string hwp_to_markdown_mem(const uint8_t* data, size_t size,
                                ConvertOptions opts = {});

// Streaming variant: emit one OLE BodyText section at a time; return false to stop.
void hwp_to_markdown_chunks_stream(const std::string& hwp_path,
                                   const ConvertOptions& opts, const PageSink& sink);

} // namespace jdoc
