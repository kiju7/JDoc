#pragma once
// egg_reader.h - Sequential EGG (ESTsoft) reader over an InputStream
// Record stream: EGG header → per-file header sections → data blocks.
// Member data is streamed through the shared codec helpers
// (store/deflate/bzip2/lzma); a file may span multiple blocks.
//
// Solid archives (all file headers first, then one shared block stream)
// are supported via read_solid_stream(): the caller demultiplexes the
// decoded byte stream across the collected members in order.
// Unsupported by design: AZO (proprietary codec), split volumes,
// encrypted members/archives.
// Format reference: ESTsoft EGG Format Specification v1.0 (public);
// notes in docs/alz-egg-format-notes.md.
// License: MIT

#include "archive/codec_streams.h"
#include "archive/input_stream.h"
#include <cstdint>
#include <string>

namespace jdoc {

class EggReader {
public:
    struct Member {
        std::string name;                // UTF-8 (area-code names converted)
        uint64_t uncompressed_size = 0;
        bool encrypted = false;          // encrypt header or encrypted filename
    };

    // src must outlive the reader. Parses the global EGGA header.
    explicit EggReader(InputStream& src);

    bool is_open() const { return open_; }

    // Archive-level features; split/global-encryption cannot be walked.
    bool is_solid() const { return solid_; }
    bool is_split() const { return split_; }
    bool is_global_encrypted() const { return global_encrypted_; }

    // Advance to the next file header section. Returns false at the
    // archive end marker, on EOF, or on an unrecognized record.
    // In solid archives, returns false once the shared block stream is
    // reached (at_solid_data() then reports true).
    bool next(Member& out);

    // Non-solid: stream the current member's data (all of its blocks) to
    // sink. Consumes the blocks even on failure so the walk can continue.
    // Verifies each block's CRC32.
    bool read_data(const CodecSink& sink, std::string* err = nullptr);

    // Non-solid: skip the current member's blocks without decompressing.
    bool skip_data();

    // Solid: true when next() stopped because the shared data area starts.
    bool at_solid_data() const { return at_solid_data_; }

    // Solid: decode the entire shared block stream to sink (per-block CRC
    // verified). The concatenated output covers the collected members in
    // order; the caller splits it by each member's uncompressed_size.
    bool read_solid_stream(const CodecSink& sink, std::string* err = nullptr);

private:
    InputStream& src_;
    bool open_ = false;
    bool solid_ = false;
    bool split_ = false;
    bool global_encrypted_ = false;
    Member cur_;
    bool data_pending_ = false;
    bool block_magic_consumed_ = false;  // a block magic was read ahead of
                                         // its consumer (no EOFARC before it)
    bool at_solid_data_ = false;

    bool read_full(void* buf, size_t len);
    // Generic extra field: bit_flag(1) + size(2 or 4, by flag bit0) + data.
    bool skip_extra_field();
    bool parse_filename(Member& m);
    // Decode blocks to sink until `remaining` output bytes are produced
    // (per-member for normal archives); nullptr sink skips without decoding.
    bool consume_blocks(uint64_t remaining, const CodecSink* sink,
                        std::string* err);
    // Decode a single block; used by both member and solid paths.
    // remaining caps the expected output; pass UINT64_MAX for solid streams.
    bool decode_block(uint64_t remaining, uint64_t& produced_out,
                      const CodecSink* sink, std::string* err);
};

} // namespace jdoc
