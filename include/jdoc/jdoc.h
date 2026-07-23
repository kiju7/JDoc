#pragma once
// jdoc - Unified document converter API
// Auto-detects format and dispatches to the appropriate parser.
// License: MIT

#include "jdoc/types.h"

#include <string>
#include <vector>

namespace jdoc {

// Convert any supported document to Markdown/plaintext.
// Automatically detects format from magic bytes and extension.
std::string convert(const std::string& file_path, ConvertOptions opts = {});

// Convert a document held in memory (no file I/O). name_hint (e.g. the
// original filename) resolves extension-based format ambiguity.
std::string convert(const void* data, size_t size, const std::string& name_hint,
                    ConvertOptions opts = {});

// Convert any supported document to per-page chunks.
std::vector<PageChunk> convert_chunks(const std::string& file_path,
                                       ConvertOptions opts = {});

// Streaming variant: pages are delivered one at a time to `sink` and not
// accumulated, so peak memory tracks a single page rather than the whole
// document and the first page is available before the rest are parsed.
// Return false from the sink to stop early. `convert_chunks` above is a thin
// collecting wrapper over this. Common document setup (e.g. a PDF's xref/font/
// page tree) is still parsed once; only the per-page loop streams.
void for_each_chunk(const std::string& file_path, const ConvertOptions& opts,
                    const PageSink& sink);

// In-memory streaming variant. name_hint (e.g. the original filename) resolves
// extension-based format ambiguity.
void for_each_chunk(const void* data, size_t size, const std::string& name_hint,
                    const ConvertOptions& opts, const PageSink& sink);

} // namespace jdoc
