#pragma once
// jbig2.h — internal: embedded (PDF) JBIG2 decoder, arithmetic subset.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace jdoc { namespace pdf_detail {

// Decode a PDF-embedded JBIG2 stream (optionally preceded by the segments of
// a /JBIG2Globals stream). Supports the arithmetic paths — generic regions,
// symbol dictionaries and text regions (scanner stamps, document masks and
// copier "compact PDF" glyph layers). Returns packed 1bpp rows (MSB first,
// 1 = foreground/black) sized from the page information segment; w/h report
// the page size. Empty on unsupported features (Huffman tables, MMR,
// refinement, halftones).
std::vector<uint8_t> jbig2_decode(const uint8_t* data, size_t len,
                                  const uint8_t* globals, size_t globals_len,
                                  int& w, int& h);

// Header-only probe: true when every segment stays inside the arithmetic
// subset above, i.e. jbig2_decode will not bail. Cheap enough to gate the
// composite-DPI estimate without decoding.
bool jbig2_supported(const uint8_t* data, size_t len,
                     const uint8_t* globals, size_t globals_len);

}} // namespace jdoc::pdf_detail
