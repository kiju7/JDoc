#pragma once
// jdoc - PDF to Markdown converter
// Backend: Custom parser (thread-safe, no PDFium dependency)
// License: MIT

#include "jdoc/types.h"

namespace jdoc {

std::string pdf_to_markdown(const std::string& pdf_path,
                             ConvertOptions opts = {});

std::vector<PageChunk> pdf_to_markdown_chunks(const std::string& pdf_path,
                                               ConvertOptions opts = {});

// In-memory variants (used for archive members parsed without extraction)
std::string pdf_to_markdown_mem(const uint8_t* data, size_t size,
                                ConvertOptions opts = {});

std::vector<PageChunk> pdf_to_markdown_chunks_mem(const uint8_t* data, size_t size,
                                                  ConvertOptions opts = {});

// Streaming variants: emit one page at a time to `sink` (return false to stop).
// The document's shared structure (xref, fonts, page tree) is parsed once; only
// the per-page image/render/emit loop streams.
void pdf_to_markdown_chunks_stream(const std::string& pdf_path,
                                   const ConvertOptions& opts, const PageSink& sink);
void pdf_to_markdown_chunks_mem_stream(const uint8_t* data, size_t size,
                                       const ConvertOptions& opts, const PageSink& sink);

} // namespace jdoc
