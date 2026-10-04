#pragma once
// pdf_base14.h — internal: built-in metrics of the PDF standard 14 fonts.
//
// A simple font that names one of the standard 14 faces (Helvetica, Times,
// Courier and their bold/italic variants, Symbol, ZapfDingbats) may omit
// /Widths and the font program (PDF 32000-1 9.6.2.2): every conforming
// reader is expected to carry their metrics. These tables are those metrics,
// compiled in from Adobe's Core14 AFM files, so the result never depends on
// fonts installed on the host system.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace jdoc { namespace pdf_detail {

struct Base14Glyph {
    const char* name;  // PostScript glyph name
    uint32_t unicode;  // Adobe Glyph List / vendor mapping value
    uint8_t code;      // code in the face's built-in encoding, 0 = unencoded
};

struct Base14Font {
    const char* name;            // canonical face name ("Helvetica-Bold")
    bool symbolic;               // Symbol / ZapfDingbats: own built-in encoding
    const Base14Glyph* glyphs;   // sorted by name
    const uint16_t* widths;      // parallel to glyphs, 1/1000 em
    uint16_t count;
    const uint16_t* by_unicode;  // glyph indices sorted by Unicode value
    uint16_t unicode_count;
};

// The standard face a /BaseFont resolves to, or nullptr. Applies the naming
// conventions of PDF 32000-1 9.6.2.2 / Annex H and of common producers:
// a subset tag ("ABCDEF+") is dropped, the style follows a ',' or '-'
// ("Arial,BoldItalic", "Helvetica-Oblique"), the metric-compatible families
// map onto the standard ones (Arial -> Helvetica, Times New Roman -> Times,
// Courier New -> Courier) and a PostScript "MT"/"PS"/"PSMT" suffix is
// ignored. A family or style token outside these (Narrow, Light, Black...)
// yields nullptr: its metrics are not those of any standard face.
const Base14Font* find_base14_font(std::string_view base_font);

// Advance width in 1/1000 em, or -1 when the face has no such glyph.
int base14_width_by_name(const Base14Font& font, std::string_view glyph);
int base14_width_by_unicode(const Base14Font& font, uint32_t unicode);
int base14_width_by_code(const Base14Font& font, uint32_t code);

// The face's built-in encoding as a 256-entry code -> Unicode table (0 for
// unencoded codes). Only meaningful for the symbolic faces; the Latin faces'
// built-in encoding is StandardEncoding.
const uint32_t* base14_builtin_encoding(const Base14Font& font);

}} // namespace jdoc::pdf_detail
