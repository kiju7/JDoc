#pragma once
// Glyph outlines from embedded font programs (TrueType, CFF, Type1), so the
// compositor can draw text that sits inside a figure.

#include "pdf_content.h"
#include <memory>
#include <vector>

namespace jdoc { namespace pdf_detail {

class GlyphSource {
public:
    // Parses the font's embedded program once. ok() is false when the font
    // has none or it cannot be read; outline() then always fails.
    GlyphSource(PdfDoc& doc, const PdfFont& font);
    ~GlyphSource();
    bool ok() const;
    // Outline of the glyph shown for `code`, in text space units (1 = 1 em,
    // y up, origin at the glyph origin). False when the glyph is missing or
    // empty (a space).
    bool outline(uint32_t code, std::vector<PathPoint>& out) const;

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

}} // namespace jdoc::pdf_detail
