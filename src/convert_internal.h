#pragma once
// convert_internal.h - Internal format detection / dispatch shared between
// the unified converter (jdoc.cpp) and the archive walker (archive_walker.cpp).
// License: MIT

#include "jdoc/types.h"
#include "jdoc/archive.h"
#include <cstdint>
#include <string>
#include <vector>

namespace jdoc {

enum class FileFormat {
    PDF, OFFICE, HWP, HWPX, TXT,
    ZIP, GZIP, BZIP2, TAR, SEVENZIP, ALZ, EGG, RAR,
    UNKNOWN
};

FileFormat detect_format(const std::string& path);
FileFormat detect_format_mem(const uint8_t* data, size_t size,
                             const std::string& name_hint);
const char* file_format_name(FileFormat fmt);

inline bool is_archive_format(FileFormat f) {
    return f == FileFormat::ZIP || f == FileFormat::GZIP ||
           f == FileFormat::BZIP2 || f == FileFormat::TAR ||
           f == FileFormat::SEVENZIP || f == FileFormat::ALZ ||
           f == FileFormat::EGG || f == FileFormat::RAR;
}

// Convert a single non-archive document held in memory.
// Throws on unsupported/unparseable input.
std::string convert_from_memory(const uint8_t* data, size_t size,
                                const std::string& name_hint,
                                const ConvertOptions& opts);
std::string convert_from_memory_as(FileFormat fmt,
                                   const uint8_t* data, size_t size,
                                   const std::string& name_hint,
                                   const ConvertOptions& opts);

// ── Archive walker (archive_walker.cpp) ─────────────────

class MemberPipeline;

// Recycles member buffers across a walk.
//
// Materializing each member into a fresh vector means faulting in its pages
// every time, and that — not the file read — is what bounds a walk over stored
// members (docs/decode-profile.md section 4). Handing buffers back here keeps
// the pages resident for the next member.
//
// Buffers are borrowed rather than kept in a free list keyed by size: a nested
// archive borrows its own while the outer walk still holds one, so the pool
// naturally grows to the recursion depth and no deeper.
class BufferPool {
public:
    std::vector<char> acquire() {
        if (free_.empty()) return {};
        std::vector<char> buf = std::move(free_.back());
        free_.pop_back();
        buf.clear();  // keeps capacity
        return buf;
    }

    void release(std::vector<char>&& buf) {
        // Nothing to recycle: the parallel pipeline moves the buffer out to a
        // worker, which leaves this one empty.
        if (buf.capacity() == 0) return;
        // Don't let one outsized member pin its capacity for the rest of the
        // walk, and don't hoard buffers past what the recursion needs.
        if (buf.capacity() > kMaxRetainedBytes || free_.size() >= kMaxBuffers)
            return;
        free_.push_back(std::move(buf));
    }

private:
    static constexpr size_t kMaxRetainedBytes = 64u << 20;  // 64 MiB
    static constexpr size_t kMaxBuffers = 8;
    std::vector<std::vector<char>> free_;
};

// Cumulative accounting shared across the whole convert_archive call,
// including nested archives.
struct WalkBudget {
    uint64_t total_out = 0;   // uncompressed bytes materialized so far
    uint32_t entries = 0;     // members visited so far
    // Non-null when ArchiveLimits::threads > 1: leaf-document conversion is
    // handed to this pipeline instead of running inline (archive.h docs).
    MemberPipeline* pipeline = nullptr;
    BufferPool buffers;
};

// Walk an archive and emit one MemberResult per member via cb.
// prefix is prepended to member paths ("inner.zip/" for nested walks).
// depth of the archive being walked; top level = 1.
// Returns false if the walk stopped early (budget exhausted or cb abort).
bool walk_archive_path(const std::string& file_path, FileFormat fmt,
                       const std::string& prefix, int depth, WalkBudget& budget,
                       const ConvertOptions& opts, const MemberCallback& cb);

bool walk_archive_mem(const uint8_t* data, size_t size, FileFormat fmt,
                      const std::string& prefix, int depth, WalkBudget& budget,
                      const ConvertOptions& opts, const MemberCallback& cb);

} // namespace jdoc
