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
#include <vector>

namespace jdoc {

// Text and embedded bitmaps recovered from a metafile in one pass. `images`
// holds each embedded bitmap as a standalone BMP file; the char buffer type
// matches ImageData::data so callers can std::move it in without a copy.
struct MetafileContent {
    std::string text;
    std::vector<std::vector<char>> images;
};

// One text-out record of a metafile: its reference point, its right edge
// when the record carries character advances (else unknown), and its text.
struct MetafileRun {
    int32_t x = 0, y = 0;
    bool has_end = false;
    int32_t end_x = 0;
    double char_width = 0;   // average advance, with has_end
    std::string text;
};

// Runs in reading order (top-to-bottom, then left-to-right), a line per
// baseline. Runs on one line are joined with a space when a gap lies
// between them — or when, lacking advances, nothing says they touch — so
// table cells and labels drawn apart do not run together. Sorts `runs`.
std::string metafile_runs_text(std::vector<MetafileRun>& runs, size_t max_output);

// Single-pass EMF extraction: walk the record stream once, collecting text
// (when want_text) and/or embedded bitmaps (when want_images). This is the
// primary entry point — the convert path uses it so a metafile that carries
// both is parsed only once. Never throws; malformed input yields whatever was
// recovered before the defect.
//
// Text is ordered top-to-bottom then left-to-right by each run's reference
// point; W (Unicode) runs decode as UTF-16LE, A (ANSI) runs as UTF-8-or-CP949.
// Each image is one BMP file, one per bitmap record (EMR_BITBLT /
// EMR_STRETCHDIBITS / …) — many EMFs just wrap a single scanned bitmap.
MetafileContent emf_extract(const uint8_t* data, size_t size,
                            bool want_text = true, bool want_images = true);

// Convenience wrappers over emf_extract() for callers that want only one side.
std::string emf_extract_text(const uint8_t* data, size_t size);
std::vector<std::vector<char>> emf_extract_bitmaps(const uint8_t* data,
                                                   size_t size);

// Convenience for the embedded-image path: extract text from a metafile blob
// given its format label ("emf" or "wmf"). Returns "" for any other label or
// when there is no text. Never throws.
std::string metafile_extract_text(const char* format, const uint8_t* data,
                                  size_t size);

} // namespace jdoc
