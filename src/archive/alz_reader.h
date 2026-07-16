#pragma once
// alz_reader.h - Sequential ALZ (ALZip legacy) reader over an InputStream
// Local file headers are walked front-to-back like tar; member data is
// streamed through the shared codec helpers (store/deflate/bzip2), so the
// container is never buffered whole.
// Format notes: docs/alz-egg-format-notes.md (clean-room, no GPL sources).
// License: MIT

#include "archive/codec_streams.h"
#include "archive/input_stream.h"
#include <cstdint>
#include <string>

namespace jdoc {

class AlzReader {
public:
    struct Member {
        std::string name;               // UTF-8 (CP949 legacy names converted)
        uint64_t compressed_size = 0;
        uint64_t uncompressed_size = 0;
        uint32_t crc32 = 0;
        uint8_t method = 0;             // 0=store, 1=bzip2, 2=deflate
        bool encrypted = false;
        bool has_data_fields = false;   // false: directory-style entry (no size/method)
    };

    // src must outlive the reader. Verifies the global "ALZ\x01" header.
    explicit AlzReader(InputStream& src);

    bool is_open() const { return open_; }

    // Advance to the next local file header. Returns false at the end
    // marker, on EOF, or on an unrecognized record (lenient stop).
    bool next(Member& out);

    // Stream the current member's decompressed data to sink. Consumes the
    // member's compressed bytes even on failure, so the walk can continue.
    // Verifies the header CRC32; returns false on mismatch/corruption/abort.
    bool read_data(const CodecSink& sink, std::string* err = nullptr);

    // Skip the current member's data without decompressing.
    bool skip_data();

private:
    InputStream& src_;
    bool open_ = false;
    Member cur_;
    bool data_pending_ = false;

    bool read_full(void* buf, size_t len);
};

} // namespace jdoc
