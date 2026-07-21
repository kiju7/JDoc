#pragma once
// String conversion utilities: UTF-16LE, CP1252, Unicode codepoint → UTF-8
// License: MIT

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace jdoc { namespace util {

// Append a Unicode codepoint as UTF-8 bytes.
inline void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// CP1252 0x80-0x9F → Unicode lookup table.
extern const uint16_t kCp1252Table[32];

// Convert a UTF-16LE byte buffer to UTF-8 string.
// Handles surrogate pairs correctly.
std::string utf16le_to_utf8(const char* data, size_t byte_len);

inline std::string utf16le_to_utf8(const uint8_t* data, size_t byte_len) {
    return utf16le_to_utf8(reinterpret_cast<const char*>(data), byte_len);
}

// Convert a single CP1252 byte to UTF-8 string.
std::string cp1252_to_utf8(uint8_t ch);

// Convert CP949 (Korean Windows codepage) byte pair to UTF-8.
// lead: first byte (0x81-0xFE), trail: second byte.
// Returns empty string if the pair is not a valid CP949 sequence.
std::string cp949_to_utf8(uint8_t lead, uint8_t trail);

// Convert CP932 (Japanese Windows codepage / Shift-JIS) byte pair to UTF-8.
// lead: first byte (0x81-0x9F or 0xE0-0xFC), trail: second byte.
// Returns empty string if the pair is not a valid CP932 sequence.
std::string cp932_to_utf8(uint8_t lead, uint8_t trail);

// Check if a byte is a CP949 lead byte.
inline bool is_cp949_lead(uint8_t ch) {
    return ch >= 0x81 && ch <= 0xFE;
}

// Check if a byte is a CP932 (Shift-JIS) lead byte.
inline bool is_cp932_lead(uint8_t ch) {
    return (ch >= 0x81 && ch <= 0x9F) || (ch >= 0xE0 && ch <= 0xFC);
}

// Validate a UTF-8 byte sequence. Returns the decoded codepoint and advances
// pos past the sequence. Returns 0xFFFD on invalid/overlong sequences.
uint32_t decode_utf8(const char* data, size_t len, size_t& pos);

// True if the whole string is well-formed UTF-8.
bool is_valid_utf8(const std::string& s);

// Convert a CP949 byte string to UTF-8 (legacy Korean archive member names).
// Invalid sequences become U+FFFD.
std::string cp949_string_to_utf8(const std::string& s);

bool is_valid_utf8(const std::string& s);  // defined below with decode_utf8

// Normalize a legacy archive member name to UTF-8: already-valid UTF-8 is
// kept as-is, anything else is decoded as CP949 (the encoding Korean
// archivers used before UTF-8 flags/records existed). Every archive reader
// shares this so all member paths leave the library as UTF-8.
inline std::string legacy_name_to_utf8(const std::string& name) {
    return is_valid_utf8(name) ? name : cp949_string_to_utf8(name);
}
// rvalue overload: when the caller passes a temporary, move the already-valid
// UTF-8 through instead of copying it.
inline std::string legacy_name_to_utf8(std::string&& name) {
    return is_valid_utf8(name) ? std::move(name) : cp949_string_to_utf8(name);
}

// Normalize plain-text bytes to UTF-8: already-valid UTF-8 (including pure
// ASCII) is kept verbatim, anything else is decoded as CP949 — the encoding
// legacy Korean .txt files and archive members use. Shared by the .txt reader
// (path and in-memory) so plain text always leaves the library as UTF-8.
inline std::string plain_text_to_utf8(const std::string& s) {
    return is_valid_utf8(s) ? s : cp949_string_to_utf8(s);
}
// rvalue overload: the .txt readers pass the whole-file buffer as a temporary
// (ss.str() / std::string(data, size)); move it through the valid-UTF-8 fast
// path instead of copying the entire document.
inline std::string plain_text_to_utf8(std::string&& s) {
    return is_valid_utf8(s) ? std::move(s) : cp949_string_to_utf8(s);
}

// Sanitize a UTF-8 string: replace invalid sequences with U+FFFD.
std::string sanitize_utf8(const char* data, size_t len);

inline std::string sanitize_utf8(const std::string& s) {
    return sanitize_utf8(s.data(), s.size());
}

// ── MIME / mail codecs ──────────────────────────────────────
// Base64 decode. Whitespace and '=' padding are skipped; other non-alphabet
// bytes are ignored, so malformed input decodes as much as possible without
// throwing.
std::string decode_base64(const std::string& in);

// Quoted-printable decode: "=\r\n"/"=\n" soft breaks are removed and "=XX" hex
// escapes restored. When q_encoding is true (RFC 2047 'Q'), '_' maps to space.
std::string decode_quoted_printable(const std::string& in, bool q_encoding = false);

// Convert a byte string to UTF-8 for the named charset (case-insensitive):
//   utf-8 / us-ascii            -> sanitized as UTF-8
//   euc-kr / ks_c_5601-1987 / cp949 / uhc / korean -> cp949_string_to_utf8
//   iso-8859-1 / latin1         -> byte value as code point
//   otherwise / empty           -> valid UTF-8 kept, else CP949 fallback
std::string charset_to_utf8(const std::string& bytes, const std::string& charset);

}} // namespace jdoc::util
