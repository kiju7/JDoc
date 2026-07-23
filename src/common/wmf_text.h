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

#include <cstddef>
#include <cstdint>
#include <string>

namespace jdoc {

// Extract the text drawn inside a WMF metafile, ordered top-to-bottom then
// left-to-right. Strings decode as UTF-8-or-CP949 (Korean legacy fallback).
// Returns "" for non-WMF input or a WMF with no text. Never throws.
std::string wmf_extract_text(const uint8_t* data, size_t size);

} // namespace jdoc
