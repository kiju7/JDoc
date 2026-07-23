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

// Streaming variant: an EML is a single page, so this emits exactly one chunk.
void eml_to_markdown_chunks_stream(const std::string& file_path,
                                   const ConvertOptions& opts, const PageSink& sink);
void eml_to_markdown_chunks_mem_stream(const uint8_t* data, size_t size,
                                       const ConvertOptions& opts,
                                       const PageSink& sink);

} // namespace jdoc
