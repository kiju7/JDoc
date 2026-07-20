#pragma once
// tar_reader.h - Sequential tar (ustar/pax/GNU) reader over an InputStream
// tar has no compression of its own; pairing with GzInflateStream handles
// .tar.gz. The container is consumed front-to-back and never buffered whole.
// License: MIT

#include "archive/input_stream.h"
#include <cstdint>
#include <functional>
#include <string>

namespace jdoc {

class TarReader {
public:
    struct Member {
        std::string name;      // UTF-8 path within the archive
        uint64_t size = 0;
        bool is_file = false;  // regular file (typeflag '0' or '\0')
    };

    // src must outlive the reader.
    explicit TarReader(InputStream& src) : src_(src) {}

    // Advance to the next header. Returns false at end of archive or on
    // corruption. Directories and special entries are reported with
    // is_file == false; callers typically skip_data() those.
    bool next(Member& out);

    // Stream the current member's data to sink in chunks.
    // Sink returns false to abort; reader stays positioned consistently
    // only if the stream is then discarded (abort ends the walk of this tar).
    using WriteFn = std::function<bool(const char* data, size_t len)>;
    bool read_data(const WriteFn& sink);

    // Zero-copy view of the current member's data when the underlying
    // stream is memory-backed and the member (with padding) is fully
    // available. On success the member is consumed; nullptr means fall
    // back to read_data. Valid for the stream's backing buffer lifetime.
    const uint8_t* view_data();

    // Skip the current member's data without buffering it.
    bool skip_data();

private:
    InputStream& src_;
    uint64_t data_remaining_ = 0;  // unread bytes of current member (incl. padding)
    uint64_t pad_remaining_ = 0;
    std::string pending_longname_;  // from pax 'x' or GNU 'L' records

    bool read_full(void* buf, size_t len);
    bool discard(uint64_t len);
};

} // namespace jdoc
