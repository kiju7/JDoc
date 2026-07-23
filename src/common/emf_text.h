#pragma once
// EMF (Enhanced Metafile) text extraction.
//
// EMF is not a raster image: it is a stream of GDI drawing records, and text is
// drawn via text-out records (EMR_EXTTEXTOUT{W,A}, EMR_POLYTEXTOUT{W,A},
// EMR_SMALLTEXTOUT). Documents (esp. HWP/Office) frequently embed text — labels,
// equations, whole regions — as EMF blobs, so pulling that text out recovers
// content a raster-only path would lose.
//
// This is a self-contained, portable byte parser — no Windows GDI, no rendering.
// Spec: https://learn.microsoft.com/openspecs/windows_protocols/ms-emf/
// License: MIT

#include <cstddef>
#include <cstdint>
#include <string>

namespace jdoc {

// Extract the text drawn inside an EMF metafile, ordered top-to-bottom then
// left-to-right by each run's reference point. W (Unicode) runs decode as
// UTF-16LE; A (ANSI) runs decode as UTF-8-or-CP949 (Korean legacy fallback).
//
// Returns "" when `data` is not a valid EMF or contains no text. Never throws;
// malformed input yields whatever text was recovered before the defect.
std::string emf_extract_text(const uint8_t* data, size_t size);

// Convenience for the embedded-image path: extract text from a metafile blob
// given its format label ("emf" or "wmf"). Returns "" for any other label or
// when there is no text. Never throws.
std::string metafile_extract_text(const char* format, const uint8_t* data,
                                  size_t size);

} // namespace jdoc
