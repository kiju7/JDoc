#pragma once
// jdoc - Shared data types for all document converters
// License: MIT

#include <cstdint>
#include <string>
#include <vector>

namespace jdoc {

enum class OutputFormat {
    MARKDOWN,   // Default: Markdown with headings, bold, tables, image refs
    PLAINTEXT   // Plain text with page separators (--- Page N ---)
};

struct ImageData {
    int page_number = 0;
    std::string name;
    unsigned width = 0;
    unsigned height = 0;
    int components = 3;             // 1=gray, 3=RGB, 4=CMYK
    std::vector<char> data;         // encoded bytes (jpeg/png) or raw pixels
    std::vector<uint8_t> pixels;    // raw pixel buffer (width * height * components)
    std::string format;             // "jpeg", "png", "raw"
    std::string saved_path;
};

struct PageChunk {
    int page_number = 0;
    std::string text;
    double page_width = 0;
    double page_height = 0;
    double body_font_size = 0;
    std::vector<std::vector<std::vector<std::string>>> tables;
    std::vector<ImageData> images;
};

// Limits for archive conversion (convert_archive). Sizes are enforced while
// streaming decompression runs — header size fields are never trusted, so a
// crafted archive (zip bomb) cannot cause unbounded allocation.
//
// Disabling a limit: max_depth < 0 means unlimited nesting; assigning -1 to
// the unsigned fields yields their max value, which is effectively unlimited
// (the C API and CLI accept -1 directly). max_ratio 0 disables the ratio
// check. Only disable these for trusted inputs — every guard against
// archive bombs goes with them.
struct ArchiveLimits {
    int      max_depth = 3;                     // nesting: top-level archive = depth 1
    uint64_t max_member_bytes = 512ull << 20;   // per-member uncompressed cap (512 MiB);
                                                // this is the actual memory ceiling —
                                                // one member is resident at a time
    uint64_t max_total_bytes  = 64ull << 30;    // cumulative uncompressed per call (64 GiB);
                                                // CPU-time guard, not memory — bounds the
                                                // damage of ratio-limit-evading bombs
    uint32_t max_entries = 200000;              // members visited incl. nested archives
    uint32_t max_ratio = 10000;                 // suspected-bomb compression ratio.
                                                // Above DEFLATE's ~1032:1 ceiling so
                                                // legitimate maximally-compressible
                                                // members never trip; genuine bombs
                                                // (nested / high-ratio codecs) are
                                                // still caught, and max_member_bytes/
                                                // max_total_bytes remain the hard caps.
    bool     include_unsupported = false;       // emit results for unsupported members
};

struct ConvertOptions {
    std::vector<int> pages;
    bool tables = true;
    bool page_chunks = false;
    bool images = false;
    std::string image_dir;
    std::string image_ref_prefix;   // prepended to image filenames in markdown refs
    unsigned min_image_size = 50;   // skip images smaller than NxN pixels (0 = no filter)
    OutputFormat format = OutputFormat::MARKDOWN;
    ArchiveLimits archive;
};

} // namespace jdoc
