#pragma once
// rar_reader.h - Sequential RAR 4.x / 5.x reader over an InputStream
// Header blocks are walked front-to-back; only stored (uncompressed)
// members are extracted. Compressed members (RAR's proprietary
// LZSS/Huffman/PPMd codec) are skipped with a per-member error so the
// walk can continue — see docs/rar-feasibility.md for why the codec is
// not implemented (unrar license is incompatible with MIT).
// Format facts from public sources only: RARLAB technotes, libarchive
// (BSD) archive_read_support_format_rar{,5}.c. No unrar-derived code.
// License: MIT

#include "archive/codec_streams.h"
#include "archive/input_stream.h"
#include <cstdint>
#include <string>

namespace jdoc {

class RarReader {
public:
    struct Member {
        std::string name;               // UTF-8
        uint64_t compressed_size = 0;   // bytes stored in the archive
        uint64_t uncompressed_size = 0; // 0 when unknown (rar5 optional)
        uint32_t crc32 = 0;
        bool has_crc = false;
        bool stored = false;            // method is store (extractable)
        bool encrypted = false;
        bool split = false;             // continued from/into another volume
        bool directory = false;
    };

    // src must outlive the reader. Verifies the RAR4 (7-byte) or RAR5
    // (8-byte) signature and, for RAR4, the main archive header.
    explicit RarReader(InputStream& src);

    bool is_open() const { return open_; }

    // True when the archive headers themselves are encrypted (RAR4
    // MHD_PASSWORD / RAR5 encryption header): nothing can be listed.
    bool headers_encrypted() const { return headers_encrypted_; }

    // Advance to the next file header. Returns false at end of archive,
    // on EOF, or on a malformed block (lenient stop).
    bool next(Member& out);

    // Stream the current member's data to sink (store members only).
    // Consumes the member's bytes even on failure so the walk continues.
    // Verifies the header CRC32 when present.
    bool read_data(const CodecSink& sink, std::string* err = nullptr);

    // Skip the current member's data.
    bool skip_data();

private:
    InputStream& src_;
    bool open_ = false;
    int version_ = 0;                   // 4 or 5
    bool headers_encrypted_ = false;
    Member cur_;
    uint64_t data_size_ = 0;            // bytes following the current header
    bool data_pending_ = false;

    bool read_full(void* buf, size_t len);
    bool next_v4(Member& out);
    bool next_v5(Member& out);
};

} // namespace jdoc
