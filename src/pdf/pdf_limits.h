#pragma once

#include <cstddef>
#include <cstdint>

namespace jdoc { namespace pdf_detail { namespace limits {

// Shared ceilings for attacker-controlled raster dimensions and transient
// page rendering. Keep these together so decoders and the compositor cannot
// silently drift to incompatible memory policies.
inline constexpr uint64_t kMaxDecodedPixels = 64ull << 20;
inline constexpr uint64_t kMaxDecodedSamples = 64ull << 20;
inline constexpr size_t kCompositeMemoryBudget = 256ull << 20;
inline constexpr size_t kCompositeFixedBytes = 8ull << 20;
// Canvas (3 B/px) plus direct-to-IDAT compression output and decoded-image
// scratch. The PNG encoder no longer holds a separate compressed copy.
inline constexpr size_t kCompositeBytesPerPixel = 6;

}}} // namespace jdoc::pdf_detail::limits
