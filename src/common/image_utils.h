#pragma once
// Image dimension utilities for min-size filtering
// License: MIT

#include "jdoc/types.h"
#include "common/file_utils.h"

namespace jdoc { namespace util {

// Populate width/height on ImageData from its raw data if not already set.
inline void populate_image_dimensions(ImageData& img) {
    if (img.width > 0 && img.height > 0) return;
    if (img.data.empty()) return;
    auto [w, h] = image_dimensions_from_data(img.data.data(), img.data.size());
    if (w > 0 && h > 0) { img.width = w; img.height = h; }
}

// Check if image is below minimum size threshold.
// Returns true if image should be skipped (either dimension < min_size).
inline bool is_image_too_small(const ImageData& img, unsigned min_size) {
    if (min_size == 0) return false;
    if (img.width == 0 && img.height == 0) return false;
    return img.width < min_size || img.height < min_size;
}

}} // namespace jdoc::util
