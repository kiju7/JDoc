#pragma once
// Minimal PNG encoder from raw pixel data / BMP using zlib
// License: MIT

#include "common/binary_utils.h"
#include "common/file_utils.h"
#include "common/image_magic.h"
#include "common/image_utils.h"
#include <zlib.h>          // crc32 for PNG chunk checksums
#include <libdeflate.h>    // faster DEFLATE compression for the IDAT payload
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <limits>

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
    uint32_t crc = static_cast<uint32_t>(
        crc32(0, reinterpret_cast<const Bytef*>(&out[type_pos]), 4 + len));
    png_put32(out, crc);
}

// components==4 defaults to CMYK input (converted to RGB); rgba=true makes a
// 4-component input encode as a true RGBA PNG (color type 6) instead.
inline std::vector<char> pixels_to_png(const uint8_t* pixels, size_t pixel_size,
                                       unsigned w, unsigned h, int components,
                                       int level = Z_BEST_SPEED,
                                       bool rgba = false) {
    if (!pixels || w == 0 || h == 0 ||
        (components != 1 && components != 3 && components != 4))
        return {};
    if (rgba && components != 4) rgba = false;

    const size_t width = static_cast<size_t>(w);
    const size_t height = static_cast<size_t>(h);
    const size_t component_count = static_cast<size_t>(components);
    if (width > std::numeric_limits<size_t>::max() / component_count)
        return {};
    const size_t source_row_bytes = width * component_count;
    if (height > std::numeric_limits<size_t>::max() / source_row_bytes ||
        pixel_size < height * source_row_bytes)
        return {};

    // Preserve grayscale input as grayscale instead of expanding every sample
    // to three identical RGB bytes. Page-rendered line art also commonly
    // arrives as RGB with R == G == B throughout, so detect that case before
    // allocating and compressing three times as much scanline data.
    bool grayscale = components == 1;
    if (rgba) grayscale = false;
    if (components == 3) {
        grayscale = true;
        for (size_t i = 0; i < height * source_row_bytes; i += 3) {
            if (pixels[i] != pixels[i + 1] || pixels[i] != pixels[i + 2]) {
                grayscale = false;
                break;
            }
        }
    }

    const size_t output_components = rgba ? 4 : (grayscale ? 1 : 3);
    if (width > (std::numeric_limits<size_t>::max() - 1) /
                    output_components)
        return {};

    size_t row_bytes = 1 + width * output_components;
    if (height > std::numeric_limits<size_t>::max() / row_bytes)
        return {};
    std::vector<uint8_t> raw(row_bytes * height);
    for (unsigned y = 0; y < h; y++) {
        const uint8_t* sr = pixels + static_cast<size_t>(y) * source_row_bytes;
        uint8_t* dr = raw.data() + static_cast<size_t>(y) * row_bytes;
        dr[0] = 0; // filter: none
        if (grayscale) {
            if (components == 1) {
                std::memcpy(dr + 1, sr, width);
            } else {
                for (unsigned x = 0; x < w; x++)
                    dr[1 + x] = sr[x * 3];
            }
            continue;
        }
        if (rgba) {
            std::memcpy(dr + 1, sr, width * 4);
            continue;
        }
        for (unsigned x = 0; x < w; x++) {
            if (components == 4) {
                // CMYK to RGB
                jdoc::util::cmyk_to_rgb(sr[x*4] / 255.0, sr[x*4+1] / 255.0,
                                        sr[x*4+2] / 255.0, sr[x*4+3] / 255.0,
                                        dr[1 + x*3], dr[1 + x*3 + 1],
                                        dr[1 + x*3 + 2]);
            } else if (components == 3) {
                dr[1 + x*3]     = sr[x*3];     // R
                dr[1 + x*3 + 1] = sr[x*3 + 1]; // G
                dr[1 + x*3 + 2] = sr[x*3 + 2]; // B
            } else {
                dr[1 + x*3] = dr[1 + x*3 + 1] = dr[1 + x*3 + 2] = sr[x];
            }
        }
    }

    // libdeflate compresses the IDAT payload 2-3x faster than zlib. The bytes
    // differ from zlib's (different encoder) but the decoded pixels are
    // identical — PNG is lossless — and the files are typically smaller.
    // Map the zlib level (Z_BEST_SPEED default) onto libdeflate's 1-12 scale.
    int ld_level = (level <= 0) ? 6 : (level > 12 ? 12 : level);
    libdeflate_compressor* comp = libdeflate_alloc_compressor(ld_level);
    if (!comp) return {};
    size_t bound = libdeflate_zlib_compress_bound(comp, raw.size());
    std::vector<uint8_t> deflated(bound);
    size_t deflated_size = libdeflate_zlib_compress(
        comp, raw.data(), raw.size(), deflated.data(), bound);
    libdeflate_free_compressor(comp);
    if (deflated_size == 0) return {};

    raw.clear(); raw.shrink_to_fit();

    std::vector<char> png;
    png.reserve(8 + 25 + deflated_size + 24);

    const uint8_t sig[] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    png.insert(png.end(), sig, sig + 8);

    uint8_t ihdr[13] = {};
    const uint32_t png_width = w;
    const uint32_t png_height = h;
    ihdr[0] = static_cast<uint8_t>(png_width >> 24);
    ihdr[1] = static_cast<uint8_t>(png_width >> 16);
    ihdr[2] = static_cast<uint8_t>(png_width >> 8);
    ihdr[3] = static_cast<uint8_t>(png_width);
    ihdr[4] = static_cast<uint8_t>(png_height >> 24);
    ihdr[5] = static_cast<uint8_t>(png_height >> 16);
    ihdr[6] = static_cast<uint8_t>(png_height >> 8);
    ihdr[7] = static_cast<uint8_t>(png_height);
    ihdr[8] = 8;
    ihdr[9] = rgba ? 6 : (grayscale ? 0 : 2); // grayscale, RGB, or RGBA
    png_write_chunk(png, "IHDR", ihdr, 13);
    png_write_chunk(png, "IDAT", deflated.data(), static_cast<uint32_t>(deflated_size));
    png_write_chunk(png, "IEND", nullptr, 0);

    return png;
}

// Encode an RGB buffer that already carries the PNG row layout — one leading
// filter byte (0) per row, then w*3 samples — straight to PNG. Skips the
// intermediate raw-buffer copy pixels_to_png() makes (26 MB per A4 page at
// 300 DPI). Grayscale pages are still repacked to 1-channel rows first: the
// smaller DEFLATE input more than pays for that copy.
inline std::vector<char> prefiltered_to_png(const uint8_t* rows, size_t size,
                                            unsigned w, unsigned h,
                                            int level = Z_BEST_SPEED) {
    if (!rows || w == 0 || h == 0) return {};
    const size_t stride = static_cast<size_t>(w) * 3 + 1;
    if (size < stride * h) return {};

    bool grayscale = true;
    for (size_t y = 0; y < h && grayscale; y++) {
        const uint8_t* r = rows + y * stride + 1;
        for (size_t x = 0; x < static_cast<size_t>(w) * 3; x += 3) {
            if (r[x] != r[x + 1] || r[x] != r[x + 2]) { grayscale = false; break; }
        }
    }

    std::vector<uint8_t> gray;
    const uint8_t* payload = rows;
    size_t payload_size = stride * h;
    if (grayscale) {
        gray.resize((static_cast<size_t>(w) + 1) * h);
        for (size_t y = 0; y < h; y++) {
            uint8_t* dr = gray.data() + y * (static_cast<size_t>(w) + 1);
            const uint8_t* sr = rows + y * stride;
            dr[0] = 0;
            for (size_t x = 0; x < w; x++) dr[1 + x] = sr[1 + x * 3];
        }
        payload = gray.data();
        payload_size = gray.size();
    }

    int ld_level = (level <= 0) ? 6 : (level > 12 ? 12 : level);
    libdeflate_compressor* comp = libdeflate_alloc_compressor(ld_level);
    if (!comp) return {};
    size_t bound = libdeflate_zlib_compress_bound(comp, payload_size);
    std::vector<uint8_t> deflated(bound);
    size_t deflated_size = libdeflate_zlib_compress(
        comp, payload, payload_size, deflated.data(), bound);
    libdeflate_free_compressor(comp);
    if (deflated_size == 0) return {};

    std::vector<char> png;
    png.reserve(8 + 25 + deflated_size + 24);
    const uint8_t sig[] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    png.insert(png.end(), sig, sig + 8);
    uint8_t ihdr[13] = {};
    ihdr[0] = static_cast<uint8_t>(w >> 24);
    ihdr[1] = static_cast<uint8_t>(w >> 16);
    ihdr[2] = static_cast<uint8_t>(w >> 8);
    ihdr[3] = static_cast<uint8_t>(w);
    ihdr[4] = static_cast<uint8_t>(h >> 24);
    ihdr[5] = static_cast<uint8_t>(h >> 16);
    ihdr[6] = static_cast<uint8_t>(h >> 8);
    ihdr[7] = static_cast<uint8_t>(h);
    ihdr[8] = 8;
    ihdr[9] = grayscale ? 0 : 2;
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
    if (h_raw == std::numeric_limits<int32_t>::min()) return {};
    int32_t abs_h = top_down ? -h_raw : h_raw;

    if (w <= 0 || abs_h <= 0 || (bpp != 24 && bpp != 32)) return {};
    int components = bpp / 8;
    size_t width = static_cast<size_t>(w);
    size_t height = static_cast<size_t>(abs_h);
    size_t src_stride = (width * static_cast<size_t>(components) + 3) & ~size_t{3};
    if (height > std::numeric_limits<size_t>::max() / src_stride ||
        data_offset > size || height * src_stride > size - data_offset)
        return {};

    if (width > std::numeric_limits<size_t>::max() / height / 3)
        return {};
    std::vector<uint8_t> pixels(width * height * 3);
    for (int y = 0; y < abs_h; y++) {
        int src_y = top_down ? y : (abs_h - 1 - y);
        const uint8_t* row = d + data_offset +
                             static_cast<size_t>(src_y) * src_stride;
        uint8_t* dst = pixels.data() +
                       static_cast<size_t>(y) * width * 3;
        for (int x = 0; x < w; x++) {
            dst[x*3]     = row[x*components + 2]; // B→R
            dst[x*3 + 1] = row[x*components + 1]; // G
            dst[x*3 + 2] = row[x*components];     // R→B
        }
    }
    return pixels_to_png(pixels.data(), pixels.size(),
                         static_cast<unsigned>(w),
                         static_cast<unsigned>(abs_h), 3);
}

// Detect actual image format from magic bytes. Returns format string or empty.
// Thin wrapper over the shared detector (common/image_magic.h) so the office
// image saver and the standalone-image path agree on the magic table.
inline std::string detect_image_format(const void* data, size_t size) {
    return image_magic_ext(data, size);
}

// Save image to disk as-is (no format conversion).
// Returns saved path, or empty string on failure.
inline std::string save_image_to_file(const std::string& dir,
                                       const std::string& name,
                                       const std::string& format,
                                       const void* data, size_t size) {
    if (dir.empty() || !data || size == 0) return "";
    ensure_dir(dir);

    // Keep the extension the container declared (zip entry name, BLIP record
    // type, OLE stream) rather than second-guessing it with a magic-byte sniff:
    // the extracted file should carry its real, source-declared extension.
    std::string ext = (format == "jpeg") ? "jpg" : format;
    if (ext.empty()) ext = "bin";
    std::string path = dir + "/" + name + "." + ext;
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return "";
    if (size > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
        return "";
    ofs.write(static_cast<const char*>(data),
              static_cast<std::streamsize>(size));
    return path;
}

// Save bytes to "<dir>/<filename>" verbatim — the filename (including its
// original extension) is preserved exactly, unlike save_image_to_file which
// derives the extension from the format. Creates dir and any missing parents.
// Returns the saved path, or empty on failure.
inline std::string save_named_file(const std::string& dir,
                                   const std::string& filename,
                                   const void* data, size_t size) {
    if (dir.empty() || filename.empty() || !data || size == 0) return "";
    ensure_dirs(dir);
    std::string path = dir + "/" + filename;
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return "";
    if (size > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
        return "";
    ofs.write(static_cast<const char*>(data),
              static_cast<std::streamsize>(size));
    return path;
}

}} // namespace jdoc::util
