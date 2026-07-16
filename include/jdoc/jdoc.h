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

} // namespace jdoc
