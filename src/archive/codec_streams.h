#pragma once
// codec_streams.h - Streaming decompressors over an InputStream, shared by
// the alz/egg readers. Each function consumes EXACTLY comp_size bytes from
// src (draining leftovers on error or sink abort) so the caller's record
// framing survives a failed member and the walk can continue.
// License: MIT

#include "archive/input_stream.h"
#include <cstdint>
#include <functional>
#include <string>

namespace jdoc {

// Sink returns false to abort (size-cap enforcement); on abort the codec
// stops decompressing, drains the member's remaining compressed bytes, and
// returns false with *err untouched (caller distinguishes abort from
// corruption by its own sink-side flags, same contract as ZipReader).
using CodecSink = std::function<bool(const char* data, size_t len)>;

// method 0 everywhere: raw copy (comp_size == uncomp_size).
bool copy_stored_stream(InputStream& src, uint64_t comp_size,
                        const CodecSink& sink, std::string* err);

// Raw deflate (no zlib/gzip wrapper) — alz method 2, egg method 1.
bool inflate_raw_stream(InputStream& src, uint64_t comp_size,
                        const CodecSink& sink, std::string* err);

// bzip2 — alz method 1, egg method 2. Without JDOC_WITH_BZIP2 this drains
// the member and fails with a clear "built without bzip2" error.
bool bzip2_stream(InputStream& src, uint64_t comp_size,
                  const CodecSink& sink, std::string* err);

// egg method 4: block data = 4 reserved bytes + 5 LZMA prop bytes + raw
// LZMA stream. Decodes until uncomp_size bytes are produced (no end marker).
bool lzma_egg_stream(InputStream& src, uint64_t comp_size,
                     uint64_t uncomp_size, const CodecSink& sink,
                     std::string* err);

// Read-and-discard exactly len bytes; false if src ends early.
bool discard_stream(InputStream& src, uint64_t len);

} // namespace jdoc
