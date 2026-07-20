#pragma once
// convert_internal.h - Internal format detection / dispatch shared between
// the unified converter (jdoc.cpp) and the archive walker (archive_walker.cpp).
// License: MIT

#include "jdoc/types.h"
#include "jdoc/archive.h"
#include <cstdint>
#include <string>

namespace jdoc {

enum class FileFormat {
    PDF, OFFICE, HWP, HWPX, TXT,
    ZIP, GZIP, TAR, SEVENZIP, ALZ, EGG, RAR,
    UNKNOWN
};

FileFormat detect_format(const std::string& path);
FileFormat detect_format_mem(const uint8_t* data, size_t size,
                             const std::string& name_hint);
const char* file_format_name(FileFormat fmt);

inline bool is_archive_format(FileFormat f) {
    return f == FileFormat::ZIP || f == FileFormat::GZIP ||
           f == FileFormat::TAR || f == FileFormat::SEVENZIP ||
           f == FileFormat::ALZ || f == FileFormat::EGG ||
           f == FileFormat::RAR;
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

// Cumulative accounting shared across the whole convert_archive call,
// including nested archives.
struct WalkBudget {
    uint64_t total_out = 0;   // uncompressed bytes materialized so far
    uint32_t entries = 0;     // members visited so far
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
