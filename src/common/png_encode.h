#pragma once
// Minimal PNG encoder from raw pixel data / BMP using zlib
// License: MIT

#include "common/binary_utils.h"
#include "common/file_utils.h"
#include <zlib.h>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace jdoc { namespace util {

inline void png_put32(std::vector<char>& v, uint32_t val) {
    char b[4] = {static_cast<char>((val >> 24) & 0xFF),
                 static_cast<char>((val >> 16) & 0xFF),
                 static_cast<char>((val >> 8) & 0xFF),
                 static_cast<char>(val & 0xFF)};
    v.insert(v.end(), b, b + 4);
}

inline void png_write_chunk(std::vector<char>& out, const char type[4],
                             const uint8_t* data, uint32_t len) {
    png_put32(out, len);
    size_t type_pos = out.size();
    out.insert(out.end(), type, type + 4);
    if (data && len > 0)
        out.insert(out.end(), reinterpret_cast<const char*>(data),
                   reinterpret_cast<const char*>(data) + len);
    uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(&out[type_pos]),
                         4 + len);
    png_put32(out, crc);
}

inline std::vector<char> pixels_to_png(const uint8_t* pixels, int w, int h,
                                        int components, int level = Z_BEST_SPEED) {
    if (!pixels || w <= 0 || h <= 0) return {};

    size_t row_bytes = 1 + static_cast<size_t>(w) * 3;
    std::vector<uint8_t> raw(row_bytes * h);
    for (int y = 0; y < h; y++) {
        const uint8_t* sr = pixels + y * w * components;
        uint8_t* dr = raw.data() + y * row_bytes;
        dr[0] = 0; // filter: none
        for (int x = 0; x < w; x++) {
            if (components >= 3) {
                dr[1 + x*3]     = sr[x*components];     // R
                dr[1 + x*3 + 1] = sr[x*components + 1]; // G
                dr[1 + x*3 + 2] = sr[x*components + 2]; // B
            } else if (components == 1) {
                dr[1 + x*3] = dr[1 + x*3 + 1] = dr[1 + x*3 + 2] = sr[x];
            } else if (components == 4) {
                // CMYK to RGB
                int c = sr[x*4], m = sr[x*4+1], yy = sr[x*4+2], k = sr[x*4+3];
                dr[1 + x*3]     = static_cast<uint8_t>(255 - std::min(255, c + k));
                dr[1 + x*3 + 1] = static_cast<uint8_t>(255 - std::min(255, m + k));
                dr[1 + x*3 + 2] = static_cast<uint8_t>(255 - std::min(255, yy + k));
            }
        }
    }

    uLong bound = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> deflated(bound);
    uLong deflated_size = bound;
    if (compress2(deflated.data(), &deflated_size, raw.data(),
                  static_cast<uLong>(raw.size()), level) != Z_OK)
        return {};

    raw.clear(); raw.shrink_to_fit();

    std::vector<char> png;
    png.reserve(8 + 25 + deflated_size + 24);

    const uint8_t sig[] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    png.insert(png.end(), sig, sig + 8);

    uint8_t ihdr[13] = {};
    ihdr[0] = (w >> 24); ihdr[1] = (w >> 16); ihdr[2] = (w >> 8); ihdr[3] = w;
    ihdr[4] = (h >> 24); ihdr[5] = (h >> 16); ihdr[6] = (h >> 8); ihdr[7] = h;
    ihdr[8] = 8; ihdr[9] = 2; // 8-bit RGB
    png_write_chunk(png, "IHDR", ihdr, 13);
    png_write_chunk(png, "IDAT", deflated.data(), static_cast<uint32_t>(deflated_size));
    png_write_chunk(png, "IEND", nullptr, 0);

    return png;
}

// Convert BMP data to PNG. Returns empty vector on failure or non-BMP input.
// Input: raw BMP file bytes (with BM header). Supports 24-bit and 32-bit BMPs.
inline std::vector<char> bmp_to_png(const void* data, size_t size) {
    if (size <= 54) return {};
    auto* d = static_cast<const uint8_t*>(data);
    if (d[0] != 'B' || d[1] != 'M') return {};

    uint32_t data_offset = read_u32_le(d + 10);
    int32_t w = static_cast<int32_t>(read_u32_le(d + 18));
    int32_t h_raw = static_cast<int32_t>(read_u32_le(d + 22));
    uint16_t bpp = read_u16_le(d + 28);
    bool top_down = (h_raw < 0);
    int32_t abs_h = top_down ? -h_raw : h_raw;

    if (w <= 0 || abs_h <= 0 || (bpp != 24 && bpp != 32)) return {};
    int components = bpp / 8;
    int src_stride = (w * components + 3) & ~3;
    if (data_offset + static_cast<size_t>(abs_h) * src_stride > size) return {};

    std::vector<uint8_t> pixels(w * abs_h * 3);
    for (int y = 0; y < abs_h; y++) {
        int src_y = top_down ? y : (abs_h - 1 - y);
        const uint8_t* row = d + data_offset + src_y * src_stride;
        uint8_t* dst = pixels.data() + y * w * 3;
        for (int x = 0; x < w; x++) {
            dst[x*3]     = row[x*components + 2]; // B→R
            dst[x*3 + 1] = row[x*components + 1]; // G
            dst[x*3 + 2] = row[x*components];     // R→B
        }
    }
    return pixels_to_png(pixels.data(), w, abs_h, 3);
}

// Detect actual image format from magic bytes. Returns format string or empty.
inline std::string detect_image_format(const void* data, size_t size) {
    if (size < 4) return "";
    auto* d = static_cast<const uint8_t*>(data);
    if (d[0] == 0xFF && d[1] == 0xD8) return "jpeg";
    if (d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return "png";
    if (d[0] == 'G' && d[1] == 'I' && d[2] == 'F') return "gif";
    if (d[0] == 'B' && d[1] == 'M') return "bmp";
    return "";
}

// Save image to disk. BMP is converted to PNG; other formats written as-is.
// Returns saved path, or empty string on failure.
inline std::string save_image_to_file(const std::string& dir,
                                       const std::string& name,
                                       const std::string& format,
                                       const void* data, size_t size) {
    if (dir.empty() || !data || size == 0) return "";
    ensure_dir(dir);

    // Use magic bytes when extension claims BMP but data disagrees
    std::string actual = format;
    if (format == "bmp") {
        std::string detected = detect_image_format(data, size);
        if (!detected.empty() && detected != "bmp")
            actual = detected;
    }

    std::vector<char> png;
    if (actual == "bmp")
        png = bmp_to_png(data, size);

    std::string ext = !png.empty() ? "png" : (actual == "jpeg" ? "jpg" : actual);
    std::string path = dir + "/" + name + "." + ext;
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return "";
    if (!png.empty())
        ofs.write(png.data(), png.size());
    else
        ofs.write(static_cast<const char*>(data), size);
    return path;
}

}} // namespace jdoc::util
