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

}} // namespace jdoc::util
