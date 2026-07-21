// HTML character-encoding detection and UTF-8 normalization.
// License: MIT

#include "html_charset.h"
#include "common/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace jdoc {
namespace html {

Charset charset_from_label(const std::string& label) {
    // Lower-case and strip separators so aliases like "ks_c_5601-1987" and
    // "ksc5601" collapse to the same key.
    std::string k;
    k.reserve(label.size());
    for (char c : label) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u == '-' || u == '_' || u == ' ') continue;
        k.push_back(static_cast<char>(std::tolower(u)));
    }

    if (k == "utf8") return Charset::UTF8;
    if (k == "euckr" || k == "ksc56011987" || k == "ksc5601" ||
        k == "cp949" || k == "windows949" || k == "ms949" || k == "uhc" ||
        k == "korean" || k == "xwindows949")
        return Charset::EUCKR;
    if (k == "utf16be") return Charset::UTF16BE;
    if (k == "utf16" || k == "utf16le" || k == "unicode") return Charset::UTF16LE;
    if (k == "iso88591" || k == "latin1" || k == "windows1252" || k == "cp1252")
        return Charset::LATIN1;
    return Charset::UNKNOWN;
}

std::string sniff_meta_charset(const char* data, size_t size, size_t sniff_limit) {
    size_t n = std::min(size, sniff_limit);
    std::string lower(data, n);
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    size_t p = 0;
    while ((p = lower.find("<meta", p)) != std::string::npos) {
        size_t end = lower.find('>', p);
        if (end == std::string::npos) end = n;
        std::string tag = lower.substr(p, end - p);
        size_t cs = tag.find("charset");
        if (cs != std::string::npos) {
            size_t eq = tag.find('=', cs);
            if (eq != std::string::npos) {
                size_t v = eq + 1;
                while (v < tag.size() &&
                       (tag[v] == ' ' || tag[v] == '"' || tag[v] == '\''))
                    v++;
                size_t e = v;
                while (e < tag.size() && tag[e] != '"' && tag[e] != '\'' &&
                       tag[e] != ' ' && tag[e] != ';' && tag[e] != '>')
                    e++;
                if (e > v) return tag.substr(v, e - v);  // already lower-case
            }
        }
        p = end;
    }
    return "";
}

Charset detect_bom(const char* d, size_t n, size_t& bom_len) {
    bom_len = 0;
    auto b = [&](size_t i) { return static_cast<uint8_t>(d[i]); };
    if (n >= 3 && b(0) == 0xEF && b(1) == 0xBB && b(2) == 0xBF) {
        bom_len = 3;
        return Charset::UTF8;
    }
    if (n >= 2 && b(0) == 0xFF && b(1) == 0xFE) {
        bom_len = 2;
        return Charset::UTF16LE;
    }
    if (n >= 2 && b(0) == 0xFE && b(1) == 0xFF) {
        bom_len = 2;
        return Charset::UTF16BE;
    }
    return Charset::UNKNOWN;
}

CharsetDecision decide_charset(const char* d, size_t n, const std::string& hint) {
    size_t bom = 0;
    Charset b = detect_bom(d, n, bom);
    if (b != Charset::UNKNOWN) return {b, bom};  // 1) BOM wins outright

    if (!hint.empty()) {                          // 2) caller (transport) hint
        Charset h = charset_from_label(hint);
        if (h != Charset::UNKNOWN) return {h, 0};
    }

    std::string label = sniff_meta_charset(d, n);  // 3) <meta> declaration
    if (!label.empty()) {
        Charset m = charset_from_label(label);
        if (m != Charset::UNKNOWN) return {m, 0};
    }

    // 4) heuristic: valid UTF-8 stays UTF-8, otherwise assume EUC-KR (CP949).
    return {util::is_valid_utf8(std::string(d, n)) ? Charset::UTF8
                                                   : Charset::EUCKR,
            0};
}

std::string to_utf8(const char* data, size_t size, Charset cs) {
    switch (cs) {
        case Charset::UTF8:
            // Sanitize any stray invalid sequences to U+FFFD.
            return util::sanitize_utf8(data, size);
        case Charset::EUCKR: {
            // Guard against a mislabeled document that is actually valid UTF-8
            // (e.g. a UTF-8 page carrying a stale charset=euc-kr meta): don't
            // double-decode it.
            std::string s(data, size);
            if (util::is_valid_utf8(s)) return s;
            return util::cp949_string_to_utf8(s);
        }
        case Charset::UTF16LE:
            return util::utf16le_to_utf8(data, size);
        case Charset::UTF16BE: {
            // Byte-swap to LE and reuse the LE converter (UTF-16BE is rare).
            std::vector<char> swapped(size);
            for (size_t i = 0; i + 1 < size; i += 2) {
                swapped[i] = data[i + 1];
                swapped[i + 1] = data[i];
            }
            if (size & 1) swapped[size - 1] = data[size - 1];
            return util::utf16le_to_utf8(swapped.data(), swapped.size());
        }
        case Charset::LATIN1: {
            std::string out;
            out.reserve(size);
            for (size_t i = 0; i < size; i++) {
                uint8_t c = static_cast<uint8_t>(data[i]);
                if (c < 0x80)
                    out.push_back(static_cast<char>(c));
                else
                    out += util::cp1252_to_utf8(c);
            }
            return out;
        }
        case Charset::UNKNOWN:
        default:
            return util::plain_text_to_utf8(std::string(data, size));
    }
}

} // namespace html
} // namespace jdoc
