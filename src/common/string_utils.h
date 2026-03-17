#pragma once
// String conversion utilities: UTF-16LE, CP1252, Unicode codepoint → UTF-8
// License: MIT

#include <cstddef>
#include <cstdint>
#include <string>

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

// Sanitize a UTF-8 string: replace invalid sequences with U+FFFD.
std::string sanitize_utf8(const char* data, size_t len);

inline std::string sanitize_utf8(const std::string& s) {
    return sanitize_utf8(s.data(), s.size());
}

}} // namespace jdoc::util
