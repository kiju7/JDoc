#pragma once
// jdoc - Archive (zip/gz/tar/...) conversion: parse documents inside an
// archive without extracting to disk. Members are decompressed one at a
// time into memory and parsed there; nested archives are walked
// recursively up to ConvertOptions::archive.max_depth.
// License: MIT

#include "jdoc/types.h"
#include <functional>
#include <string>
#include <vector>

namespace jdoc {

// Machine-readable classification of a member failure. The *_LIMIT codes
// name the ArchiveLimits field to raise; the remaining codes are input
// problems no limit change can fix.
enum class MemberErrorCode {
    OK = 0,
    MEMBER_LIMIT,    // member larger than max_member_bytes
    RATIO_LIMIT,     // expansion above max_ratio (suspected archive bomb)
    TOTAL_LIMIT,     // cumulative output hit max_total_bytes; walk stopped
    ENTRY_LIMIT,     // member count hit max_entries; walk stopped
    DEPTH_LIMIT,     // nested archive deeper than max_depth
    ENCRYPTED,       // encrypted member or archive
    UNSUPPORTED,     // format jdoc cannot convert
    CORRUPT,         // container or member data unreadable
    CONVERT_FAILED,  // recognized document, but the parser rejected it
};

struct MemberResult {
    std::string member_path;   // nested path, '/'-joined: "outer.zip/dir/report.hwp"
    std::string format;        // detected format: "PDF", "HWP", "ZIP", "UNKNOWN", ...
    std::string markdown;      // conversion output; empty on error
    std::string error;         // empty on success
    MemberErrorCode error_code = MemberErrorCode::OK;
    uint64_t uncompressed_size = 0;

    bool ok() const { return error.empty(); }
};

// Convert every supported document inside an archive.
// Per-member failures are recorded in that member's `error` and the walk
// continues; only failure to open the top-level file throws.
// Calling this on a non-archive document returns a single-element result.
std::vector<MemberResult> convert_archive(const std::string& file_path,
                                          ConvertOptions opts = {});

// Streaming variant: results are delivered one at a time and not
// accumulated (constant memory for archives with many members).
// Return false from the callback to stop the walk early.
using MemberCallback = std::function<bool(MemberResult&&)>;
void convert_archive(const std::string& file_path, const MemberCallback& cb,
                     ConvertOptions opts = {});

// True if the file is an archive container (zip/gz/tar/7z/alz/egg/rar) rather
// than a single document. OOXML/HWPX packages are documents, not archives.
bool is_archive_file(const std::string& file_path);

} // namespace jdoc
