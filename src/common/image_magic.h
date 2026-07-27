#pragma once
// Single source of truth for magic-byte image-format detection.
// License: MIT
//
// Every place that sniffs image bytes — detect()'s file classifier, the office
// image saver, the .doc/.hwp embedded-image scanners — used to carry its own
// copy of the magic table, with different coverage and different spellings
// ("PNG" vs "png"). They now all go through image_magic(); the two adapters
// render the result in whichever spelling the caller needs.
//
// The .doc scanner probes this at every byte offset of a multi-megabyte stream,
// so the primitive returns an enum and never constructs a std::string on the
// reject path. Guards require the full signature length (a strict superset of
// what any single caller demanded before), so unifying can only reduce
// false positives, never add them.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace jdoc { namespace util {

enum class ImageFormat {
    None, Jpeg, Png, Gif, Bmp, Tiff, Webp, Ico, Psd, Wmf, Emf
};

inline ImageFormat image_magic(const void* data, size_t n) {
    const auto* b = static_cast<const uint8_t*>(data);
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        return ImageFormat::Jpeg;
    if (n >= 8 && std::memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0)
        return ImageFormat::Png;
    if (n >= 6 && (std::memcmp(b, "GIF87a", 6) == 0 ||
                   std::memcmp(b, "GIF89a", 6) == 0))
        return ImageFormat::Gif;
    if (n >= 4 && std::memcmp(b, "8BPS", 4) == 0)
        return ImageFormat::Psd;
    // TIFF little-endian (II*\0) and big-endian (MM\0*).
    if (n >= 4 && (std::memcmp(b, "II\x2a\x00", 4) == 0 ||
                   std::memcmp(b, "MM\x00\x2a", 4) == 0))
        return ImageFormat::Tiff;
    // WebP: "RIFF"????"WEBP".
    if (n >= 12 && std::memcmp(b, "RIFF", 4) == 0 &&
        std::memcmp(b + 8, "WEBP", 4) == 0)
        return ImageFormat::Webp;
    // ICO: reserved=0, type=1 (icon).
    if (n >= 4 && b[0] == 0x00 && b[1] == 0x00 && b[2] == 0x01 && b[3] == 0x00)
        return ImageFormat::Ico;
    // BMP: "BM" plus the 4 reserved header bytes (offset 6-9) zero, so text that
    // happens to start with "BM" is not mistaken for a bitmap.
    if (n >= 10 && b[0] == 'B' && b[1] == 'M' &&
        b[6] == 0 && b[7] == 0 && b[8] == 0 && b[9] == 0)
        return ImageFormat::Bmp;
    // Placeable WMF.
    if (n >= 4 && b[0] == 0xD7 && b[1] == 0xCD && b[2] == 0xC6 && b[3] == 0x9A)
        return ImageFormat::Wmf;
    // EMF: record type 1 (EMR_HEADER) with the " EMF" signature at offset 40.
    if (n >= 44 && b[0] == 0x01 && b[1] == 0x00 && b[2] == 0x00 && b[3] == 0x00 &&
        b[40] == ' ' && b[41] == 'E' && b[42] == 'M' && b[43] == 'F')
        return ImageFormat::Emf;
    return ImageFormat::None;
}

// Lowercase extension/name for saved files and ImageData.format ("" for None).
inline const char* image_ext(ImageFormat f) {
    switch (f) {
        case ImageFormat::Jpeg: return "jpeg";
        case ImageFormat::Png:  return "png";
        case ImageFormat::Gif:  return "gif";
        case ImageFormat::Bmp:  return "bmp";
        case ImageFormat::Tiff: return "tiff";
        case ImageFormat::Webp: return "webp";
        case ImageFormat::Ico:  return "ico";
        case ImageFormat::Psd:  return "psd";
        case ImageFormat::Wmf:  return "wmf";
        case ImageFormat::Emf:  return "emf";
        case ImageFormat::None: return "";
    }
    return "";
}

// Uppercase canonical name for detect()'s type classification (nullptr for None).
inline const char* image_type(ImageFormat f) {
    switch (f) {
        case ImageFormat::Jpeg: return "JPEG";
        case ImageFormat::Png:  return "PNG";
        case ImageFormat::Gif:  return "GIF";
        case ImageFormat::Bmp:  return "BMP";
        case ImageFormat::Tiff: return "TIFF";
        case ImageFormat::Webp: return "WEBP";
        case ImageFormat::Ico:  return "ICO";
        case ImageFormat::Psd:  return "PSD";
        case ImageFormat::Wmf:  return "WMF";
        case ImageFormat::Emf:  return "EMF";
        case ImageFormat::None: return nullptr;
    }
    return nullptr;
}

// Convenience: lowercase format string straight from bytes ("" if unrecognized).
// Backs the office image saver's extension-repair path.
inline const char* image_magic_ext(const void* data, size_t n) {
    return image_ext(image_magic(data, n));
}

}} // namespace jdoc::util
