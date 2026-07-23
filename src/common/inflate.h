#pragma once
// inflate.h - whole-buffer DEFLATE/zlib decompression via libdeflate.
//
// libdeflate decodes a complete compressed buffer in one shot, ~1.3-1.6x faster
// than zlib/zlib-ng for inflate. It has no streaming API, so the pull-based
// gzip/tar.gz path (GzInflateStream) stays on zlib; every "read the whole
// member/stream then inflate" site (zip members, PDF FlateDecode, HWP sections,
// office metafiles) uses these helpers. Output is bit-identical to zlib.
// License: MIT
#include <cstddef>
#include <cstdint>
#include <vector>

namespace jdoc {

// Raw DEFLATE (no zlib/gzip wrapper) into a caller buffer of exactly out_size
// bytes — for members whose uncompressed size is known (zip central directory).
// Returns true iff the stream decoded to exactly out_size bytes.
bool inflate_raw_known(const uint8_t* in, size_t in_size,
                       uint8_t* out, size_t out_size);

// Raw DEFLATE with unknown output size; the buffer grows from `hint` (0 = auto).
// Returns the decompressed bytes, or empty on corrupt/short input.
std::vector<uint8_t> inflate_raw(const uint8_t* in, size_t in_size, size_t hint);

// zlib-wrapped (RFC 1950) stream with unknown output size; grows from `hint`.
// Used by PDF FlateDecode, whose streams carry the 2-byte zlib header.
std::vector<uint8_t> inflate_zlib(const uint8_t* in, size_t in_size, size_t hint);

} // namespace jdoc
