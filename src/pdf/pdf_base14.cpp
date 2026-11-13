// pdf_base14.cpp — standard 14 font metrics and font-name resolution.
#include "pdf_base14.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace jdoc { namespace pdf_detail {

#include "pdf_base14_data.inc"

namespace {

constexpr size_t kFontCount = sizeof(kBase14Fonts) / sizeof(kBase14Fonts[0]);

const Base14Font* font_named(std::string_view name) {
    for (const auto& f : kBase14Fonts)
        if (name == f.name) return &f;
    return nullptr;
}

bool ends_with(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Drops the PostScript vendor suffixes Monotype/Microsoft names carry
// ("ArialMT", "TimesNewRomanPSMT", "Arial-BoldItalicMT").
void strip_vendor_suffix(std::string& s) {
    for (bool again = true; again;) {
        again = false;
        for (std::string_view suf : {std::string_view("mt"), std::string_view("ps")}) {
            if (s.size() > suf.size() && ends_with(s, suf)) {
                s.resize(s.size() - suf.size());
                again = true;
            }
        }
    }
}

} // namespace

const Base14Font* find_base14_font(std::string_view base_font) {
    // Subset tag: six uppercase letters and '+' (PDF 32000-1 9.6.4).
    if (base_font.size() > 7 && base_font[6] == '+' &&
        std::all_of(base_font.begin(), base_font.begin() + 6,
                    [](char c) { return c >= 'A' && c <= 'Z'; }))
        base_font.remove_prefix(7);

    std::string s;
    s.reserve(base_font.size());
    for (char c : base_font) {
        if (c == ' ' || c == '_') continue;  // "Times New Roman", "Courier_New"
        s += static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    size_t cut = s.find_first_of(",-");
    std::string family = s.substr(0, cut);
    std::string style = cut == std::string::npos ? std::string() : s.substr(cut + 1);
    strip_vendor_suffix(family);
    strip_vendor_suffix(style);

    // The family may run straight into the style ("TimesNewRomanBold",
    // "ArialMT" already lost its suffix above): match the longest known
    // family prefix and read whatever follows as style.
    enum { kHelvetica, kTimes, kCourier, kSymbol, kDingbats, kNone } fam = kNone;
    static const struct { const char* name; int fam; } kFamilies[] = {
        {"timesnewroman", kTimes}, {"timesroman", kTimes}, {"times", kTimes},
        {"helvetica", kHelvetica}, {"arial", kHelvetica},
        {"couriernew", kCourier}, {"courier", kCourier},
        {"itczapfdingbats", kDingbats}, {"zapfdingbats", kDingbats}, {"dingbats", kDingbats},
        {"symbol", kSymbol},
    };
    for (const auto& f : kFamilies) {  // longer spellings listed first
        size_t n = std::strlen(f.name);
        if (family.compare(0, n, f.name) == 0) {
            std::string rest = family.substr(n);
            strip_vendor_suffix(rest);
            if (!rest.empty()) style = rest + (style.empty() ? "" : "-" + style);
            fam = static_cast<decltype(fam)>(f.fam);
            break;
        }
    }
    if (fam == kNone) return nullptr;

    bool bold = false, italic = false;
    size_t p = 0;
    while (p < style.size()) {
        if (style[p] == ',' || style[p] == '-') { p++; continue; }
        bool matched = false;
        for (std::string_view tok : {"bold", "italic", "oblique", "roman", "regular", "normal"}) {
            if (style.compare(p, tok.size(), tok) == 0) {
                if (tok == "bold") bold = true;
                else if (tok == "italic" || tok == "oblique") italic = true;
                p += tok.size();
                matched = true;
                break;
            }
        }
        if (!matched) return nullptr;  // Narrow, Light, Black, Condensed...
    }

    // The symbolic faces come in one style; a synthesized ",Bold" draws the
    // same glyphs with the same advances.
    if (fam == kSymbol) return font_named("Symbol");
    if (fam == kDingbats) return font_named("ZapfDingbats");

    static const char* const kNames[3][4] = {
        // regular, bold, italic, bold italic
        {"Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique"},
        {"Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic"},
        {"Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique"},
    };
    return font_named(kNames[fam][(bold ? 1 : 0) + (italic ? 2 : 0)]);
}

int base14_width_by_name(const Base14Font& font, std::string_view glyph) {
    const Base14Glyph* end = font.glyphs + font.count;
    const Base14Glyph* it = std::lower_bound(
        font.glyphs, end, glyph,
        [](const Base14Glyph& g, std::string_view n) { return std::string_view(g.name) < n; });
    if (it == end || glyph != it->name) return -1;
    return font.widths[it - font.glyphs];
}

int base14_width_by_unicode(const Base14Font& font, uint32_t unicode) {
    auto lookup = [&](uint32_t u) -> int {
        const uint16_t* end = font.by_unicode + font.unicode_count;
        const uint16_t* it = std::lower_bound(
            font.by_unicode, end, u,
            [&](uint16_t i, uint32_t v) { return font.glyphs[i].unicode < v; });
        if (it == end || font.glyphs[*it].unicode != u) return -1;
        return font.widths[*it];
    };
    int w = lookup(unicode);
    if (w >= 0) return w;
    // Encodings name a few characters the AFM glyph set spells differently:
    // WinAnsi's no-break space and soft hyphen draw the space and hyphen
    // glyphs.
    if (unicode == 0x00A0) return lookup(0x0020);
    if (unicode == 0x00AD) return lookup(0x002D);
    return -1;
}

int base14_width_by_code(const Base14Font& font, uint32_t code) {
    if (code == 0 || code > 0xFF) return -1;
    for (uint16_t i = 0; i < font.count; i++)
        if (font.glyphs[i].code == code) return font.widths[i];
    return -1;
}

const uint32_t* base14_builtin_encoding(const Base14Font& font) {
    static const auto tables = [] {
        std::array<std::array<uint32_t, 256>, kFontCount> t{};
        for (size_t f = 0; f < kFontCount; f++) {
            const Base14Font& bf = kBase14Fonts[f];
            for (uint16_t i = 0; i < bf.count; i++)
                if (bf.glyphs[i].code) t[f][bf.glyphs[i].code] = bf.glyphs[i].unicode;
        }
        return t;
    }();
    return tables[static_cast<size_t>(&font - kBase14Fonts)].data();
}

}} // namespace jdoc::pdf_detail
