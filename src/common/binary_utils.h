#pragma once
// Binary reading utilities (little-endian)
// License: MIT

#include <cstdint>

namespace jdoc { namespace util {

inline uint16_t read_u16_le(const void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return uint16_t(b[0]) | (uint16_t(b[1]) << 8);
}

inline uint32_t read_u32_le(const void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8)
         | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}

inline uint64_t read_u64_le(const void* p) {
    return uint64_t(read_u32_le(p))
         | (uint64_t(read_u32_le(static_cast<const uint8_t*>(p) + 4)) << 32);
}

inline int16_t read_i16_le(const void* p) {
    return static_cast<int16_t>(read_u16_le(p));
}

inline int32_t read_i32_le(const void* p) {
    return static_cast<int32_t>(read_u32_le(p));
}

}} // namespace jdoc::util
