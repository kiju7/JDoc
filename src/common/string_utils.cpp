// String conversion utilities implementation
// License: MIT

#include "common/string_utils.h"

namespace jdoc { namespace util {

const uint16_t kCp1252Table[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, // 80-87
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F, // 88-8F
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 90-97
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178, // 98-9F
};

std::string utf16le_to_utf8(const char* data, size_t byte_len) {
    std::string out;
    out.reserve(byte_len);
    for (size_t i = 0; i + 1 < byte_len; i += 2) {
        uint16_t code = static_cast<uint8_t>(data[i])
                      | (static_cast<uint16_t>(static_cast<uint8_t>(data[i + 1])) << 8);
        if (code >= 0xD800 && code <= 0xDBFF && i + 3 < byte_len) {
            uint16_t low = static_cast<uint8_t>(data[i + 2])
                         | (static_cast<uint16_t>(static_cast<uint8_t>(data[i + 3])) << 8);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                uint32_t cp = 0x10000
                    + ((static_cast<uint32_t>(code - 0xD800) << 10) | (low - 0xDC00));
                append_utf8(out, cp);
                i += 2;
                continue;
            }
        }
        append_utf8(out, code);
    }
    return out;
}

std::string cp1252_to_utf8(uint8_t ch) {
    std::string out;
    if (ch >= 0x80 && ch <= 0x9F) {
        append_utf8(out, kCp1252Table[ch - 0x80]);
    } else {
        append_utf8(out, ch);
    }
    return out;
}

}} // namespace jdoc::util
