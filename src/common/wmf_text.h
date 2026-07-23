#pragma once
// WMF (Windows Metafile, 16-bit) text extraction.
//
// The older sibling of EMF: a stream of 16-bit GDI records. Text is drawn via
// META_TEXTOUT / META_EXTTEXTOUT, whose strings are ANSI (code-page) bytes —
// for Korean documents that means CP949, which we decode via the shared tables.
//
// Self-contained portable byte parser — no Windows GDI. Handles both the raw
// WMF header and the Aldus placeable header wrapper.
// Spec: https://learn.microsoft.com/openspecs/windows_protocols/ms-wmf/
// License: MIT

#include "common/emf_text.h"   // MetafileContent

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jdoc {

// Single-pass WMF extraction: walk the record stream once, collecting text
// (when want_text) and/or embedded bitmaps (when want_images). Text decodes as
// UTF-8-or-CP949; each image is one BMP file, one per DIB-carrying record
// (META_DIBBITBLT / META_DIBSTRETCHBLT / META_STRETCHDIB / …). Never throws.
MetafileContent wmf_extract(const uint8_t* data, size_t size,
                            bool want_text = true, bool want_images = true);

// Convenience wrappers over wmf_extract() for callers that want only one side.
std::string wmf_extract_text(const uint8_t* data, size_t size);
std::vector<std::vector<char>> wmf_extract_bitmaps(const uint8_t* data,
                                                   size_t size);

} // namespace jdoc
