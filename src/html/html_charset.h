#pragma once
// HTML character-encoding detection (a practical subset of the WHATWG
// "encoding sniffing algorithm"): BOM > caller hint > <meta> > UTF-8 heuristic.
// Used to normalize non-UTF-8 HTML (e.g. EUC-KR) to UTF-8 before tokenization.

#include <cstddef>
#include <string>

namespace jdoc {
namespace html {

enum class Charset {
    UTF8,     // utf-8 (with or without BOM)
    UTF16LE,  // utf-16, utf-16le
    UTF16BE,  // utf-16be
    EUCKR,    // euc-kr, ks_c_5601-1987, ksc5601, cp949, windows-949, uhc
    LATIN1,   // iso-8859-1, latin1, windows-1252 (approximated)
    UNKNOWN
};

// Normalize a charset label (case/alias-insensitive) to a Charset. UNKNOWN if
// unrecognized.
Charset charset_from_label(const std::string& label);

// Scan the head region (first sniff_limit bytes, read as ASCII) for a <meta>
// charset declaration. Returns the raw label, or "" if none found.
std::string sniff_meta_charset(const char* data, size_t size,
                               size_t sniff_limit = 2048);

// Detect a byte-order mark. On match returns the Charset and sets bom_len to
// the BOM byte count (2 or 3); otherwise UNKNOWN and bom_len = 0.
Charset detect_bom(const char* data, size_t size, size_t& bom_len);

// Final decision: BOM > hint > meta > heuristic. Returns the resolved Charset
// and the body start offset (past any BOM).
struct CharsetDecision {
    Charset charset;
    size_t body_offset;
};
CharsetDecision decide_charset(const char* data, size_t size,
                               const std::string& hint_label);

// Normalize [data, data+size) to UTF-8 for the given charset.
std::string to_utf8(const char* data, size_t size, Charset cs);

} // namespace html
} // namespace jdoc
