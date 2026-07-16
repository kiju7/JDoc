#pragma once
// egg_reader.h - Sequential EGG (ESTsoft) reader over an InputStream
// Record stream: EGG header → per-file header sections → data blocks.
// Member data is streamed through the shared codec helpers
// (store/deflate/bzip2/lzma); a file may span multiple blocks.
// Unsupported by design: AZO (proprietary codec), solid, split volumes.
// Format notes: docs/alz-egg-format-notes.md (clean-room, no GPL sources).
// License: MIT

#include "archive/codec_streams.h"
#include "archive/input_stream.h"
#include <cstdint>
#include <string>

namespace jdoc {

class EggReader {
public:
    struct Member {
        std::string name;                // UTF-8 (egg stores UTF-8 natively)
        uint64_t uncompressed_size = 0;
        bool encrypted = false;
    };

    // src must outlive the reader. Parses the global EGGA header.
    explicit EggReader(InputStream& src);

    bool is_open() const { return open_; }

    // Archive-level features this reader does not support; when set, the
    // caller should report one archive-level error instead of walking.
    bool is_solid() const { return solid_; }
    bool is_split() const { return split_; }

    // Advance to the next file header section. Returns false at the
    // archive end marker, on EOF, or on an unrecognized record.
    bool next(Member& out);

    // Stream the current member's data (all of its blocks) to sink.
    // Consumes the blocks even on failure so the walk can continue.
    // Verifies each block's CRC32.
    bool read_data(const CodecSink& sink, std::string* err = nullptr);

    // Skip the current member's blocks without decompressing.
    bool skip_data();

private:
    InputStream& src_;
    bool open_ = false;
    bool solid_ = false;
    bool split_ = false;
    Member cur_;
    bool data_pending_ = false;
    bool block_magic_consumed_ = false;  // header section ran into the first
                                         // block magic (no EOFARC before it)

    bool read_full(void* buf, size_t len);
    bool skip_extra_field(uint64_t max_len);
    // Iterate the member's blocks; decode is false for skip_data.
    bool consume_blocks(const CodecSink* sink, std::string* err);
};

} // namespace jdoc
