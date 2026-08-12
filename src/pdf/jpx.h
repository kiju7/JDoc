#pragma once
// jpx.h — internal: JPEG 2000 (ITU-T T.800) baseline decoder for JPXDecode.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace jdoc { namespace pdf_detail {

struct JpxImage {
    int width = 0, height = 0, components = 0;
    // Largest component bit depth declared by the codestream. Samples above
    // 8 bits are flattened into the 8-bit pixels below — fine for canvas
    // compositing, lossy as an extraction deliverable, so extraction checks
    // this to prefer the byte-exact passthrough.
    int src_depth = 8;
    std::vector<uint8_t> pixels; // interleaved samples, 8 bits per component
};

// Decode a JPXDecode stream: either a raw J2K codestream (SOC..EOC) or a JP2
// container (the jp2c box is located first). Baseline profile: 5/3 and 9/7
// wavelets, RCT/ICT, LRCP/RLCP progressions (others only in their
// single-precinct degenerate form), scalar quantization, multiple tiles and
// layers. Unsupported features (packed headers, coding-style bypass/termall,
// subsampling) return an empty image. Stateless; safe from parallel page
// workers.
JpxImage jpx_decode(const uint8_t* data, size_t len);

}} // namespace jdoc::pdf_detail
