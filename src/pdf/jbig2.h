#pragma once
// jbig2.h — internal: embedded (PDF) JBIG2 decoder, generic-region subset.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace jdoc { namespace pdf_detail {

// Decode a PDF-embedded JBIG2 stream (optionally preceded by the segments of
// a /JBIG2Globals stream). Supports arithmetic-coded generic regions only —
// the form scanner stamps and document masks use. Returns packed 1bpp rows
// (MSB first, 1 = foreground/black) sized from the page information segment;
// w/h report the page size. Empty on unsupported features (MMR, symbol
// dictionaries, refinement, halftones).
std::vector<uint8_t> jbig2_decode(const uint8_t* data, size_t len,
                                  const uint8_t* globals, size_t globals_len,
                                  int& w, int& h);

}} // namespace jdoc::pdf_detail
