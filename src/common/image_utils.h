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

// Same, reading from an external buffer — used when the bytes are written
// straight to disk and ImageData.data is left empty (no userspace copy).
inline void populate_image_dimensions(ImageData& img, const uint8_t* data,
                                      size_t size) {
    if (img.width > 0 && img.height > 0) return;
    if (!data || size == 0) return;
    auto [w, h] = image_dimensions_from_data(
        reinterpret_cast<const char*>(data), size);
    if (w > 0 && h > 0) { img.width = w; img.height = h; }
}

// Check if image is below minimum size threshold.
// Returns true if image should be skipped (either dimension < min_size).
inline bool is_image_too_small(const ImageData& img, unsigned min_size) {
    if (min_size == 0) return false;
    if (img.width == 0 && img.height == 0) return false;
    return img.width < min_size || img.height < min_size;
}

// DeviceCMYK → sRGB. Real inks cross-absorb (magenta also dims blue), so the
// naive per-channel subtraction reads noticeably wrong next to Acrobat or a
// color-managed renderer. This is the polynomial fit used by pdf.js to match
// Acrobat's US Web Coated rendering; inputs are ink fractions in [0,1].
inline void cmyk_to_rgb(double c, double m, double y, double k,
                        uint8_t& r_out, uint8_t& g_out, uint8_t& b_out) {
    auto clamp8 = [](double v) {
        return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5));
    };
    double r =
        255 +
        c * (-4.387332384609988 * c + 54.48615194189176 * m +
             18.82290502165302 * y + 212.25662451639585 * k -
             285.2331026137004) +
        m * (1.7149763477362134 * m - 5.6096736904047315 * y -
             17.873870861415444 * k - 5.497006427196366) +
        y * (-2.5217340131683033 * y - 21.248923337353073 * k +
             17.5119270841813) +
        k * (-21.86122147463605 * k - 189.48180835922747);
    double g =
        255 +
        c * (8.841041422036149 * c + 60.118027045597366 * m +
             6.871425592049007 * y + 31.159100130055922 * k -
             79.2970844816548) +
        m * (-15.310361306967817 * m + 17.575251261109482 * y +
             131.35250912493976 * k - 190.9453302588951) +
        y * (4.444339102852739 * y + 9.8632861493405 * k -
             24.86741582555878) +
        k * (-20.737325471181034 * k - 187.80453709719578);
    double b =
        255 +
        c * (0.8842522430003296 * c + 8.078677503112928 * m +
             30.89978309703729 * y - 0.23883238689178934 * k -
             14.183576799673286) +
        m * (10.49593273432072 * m + 63.02378494754052 * y +
             50.606957656360734 * k - 112.23884253719248) +
        y * (0.03296041114873217 * y + 115.60384449646641 * k -
             193.58209356861505) +
        k * (-22.33816807309886 * k - 180.12613974708367);
    r_out = clamp8(r);
    g_out = clamp8(g);
    b_out = clamp8(b);
}

}} // namespace jdoc::util
