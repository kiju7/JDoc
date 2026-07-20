// archive_walker.cpp - Recursive archive traversal without disk extraction
//
// Members are streamed (decompressed) one at a time into a memory buffer,
// parsed there, and the buffer is released before the next member. Nested
// archives recurse from that buffer. All size limits are enforced while
// the decompressor runs — header size fields are never trusted.
//
// License: MIT

#include "convert_internal.h"
#include "zip_reader.h"
#include "common/file_utils.h"
#include "archive/alz_reader.h"
#include "archive/egg_reader.h"
#include "archive/input_stream.h"
#include "archive/member_pipeline.h"
#include "archive/rar_reader.h"
#include "archive/seven_zip_reader.h"
#include "archive/tar_reader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace jdoc {
namespace {

// Media/binary extensions that are never parseable documents: skipped
// without spending time decompressing them (unless the caller asked for
// unsupported members to be reported).
bool has_skippable_ext(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    for (auto& c : ext) c = std::tolower(static_cast<unsigned char>(c));
    static const char* kSkip[] = {
        "png", "jpg", "jpeg", "gif", "bmp", "tif", "tiff", "webp", "ico",
        "mp3", "mp4", "avi", "mov", "mkv", "wav", "flac",
        "exe", "dll", "so", "dylib", "class", "o", "a",
        "ttf", "otf", "woff", "woff2",
    };
    for (auto* s : kSkip)
        if (ext == s) return true;
    return false;
}

// macOS archiving tools add AppleDouble sidecars ("._name", "__MACOSX/")
// and ".DS_Store" — metadata, not documents.
bool is_metadata_member(const std::string& name) {
    if (name.compare(0, 9, "__MACOSX/") == 0) return true;
    auto slash = name.rfind('/');
    std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    return base.compare(0, 2, "._") == 0 || base == ".DS_Store";
}

// Strip leading '/' and any "../" components so member paths are safe,
// display-friendly relative paths.
std::string normalize_member_name(const std::string& name) {
    std::string out;
    size_t pos = 0;
    while (pos < name.size()) {
        size_t slash = name.find('/', pos);
        size_t len = (slash == std::string::npos ? name.size() : slash) - pos;
        std::string comp = name.substr(pos, len);
        if (!comp.empty() && comp != "." && comp != "..") {
            if (!out.empty()) out += '/';
            out += comp;
        }
        pos = (slash == std::string::npos) ? name.size() : slash + 1;
    }
    return out;
}

std::string strip_gz_ext(const std::string& name) {
    if (name.size() > 3) {
        std::string tail = name.substr(name.size() - 3);
        for (auto& c : tail) c = std::tolower(static_cast<unsigned char>(c));
        if (tail == ".gz") return name.substr(0, name.size() - 3);
    }
    return name;
}

std::string strip_bz2_ext(const std::string& name) {
    if (name.size() > 4) {
        std::string tail = name.substr(name.size() - 4);
        for (auto& c : tail) c = std::tolower(static_cast<unsigned char>(c));
        if (tail == ".bz2") return name.substr(0, name.size() - 4);
    }
    return name;
}

// Sink that appends to a buffer while enforcing the per-member cap, the
// compression-ratio bound, and the cumulative budget during streaming
// decompression. The two per-member checks are separate flags so the
// error can name the option that actually tripped.
struct CapSink {
    std::vector<char>& out;
    uint64_t member_cap;   // from max_member_bytes
    uint64_t ratio_cap;    // compression-ratio bound; 0 = not applicable
    uint64_t total_cap;
    WalkBudget& budget;
    bool member_exceeded = false;
    bool ratio_exceeded = false;
    bool total_exceeded = false;

    bool operator()(const char* p, size_t n) {
        uint64_t grown = out.size() + n;
        if (grown > member_cap) { member_exceeded = true; return false; }
        if (ratio_cap != 0 && grown > ratio_cap) {
            ratio_exceeded = true;
            return false;
        }
        budget.total_out += n;
        if (budget.total_out > total_cap) { total_exceeded = true; return false; }
        out.insert(out.end(), p, p + n);
        return true;
    }
};

// Compression-ratio bound for a member whose compressed size is known
// (0 = not applicable). Small members are allowed to expand freely up to
// 16 MiB so tiny highly-compressible files don't trip the ratio check.
uint64_t ratio_cap_for(const ArchiveLimits& lim, uint64_t compressed_size) {
    if (compressed_size == 0 || lim.max_ratio == 0) return 0;
    return std::max<uint64_t>(16ull << 20, compressed_size * lim.max_ratio);
}

std::string human_size(uint64_t bytes) {
    char buf[32];
    if (bytes >= (1ull << 30))
        snprintf(buf, sizeof buf, "%.4g GiB", bytes / double(1ull << 30));
    else if (bytes >= (1ull << 20))
        snprintf(buf, sizeof buf, "%.4g MiB", bytes / double(1ull << 20));
    else
        snprintf(buf, sizeof buf, "%llu bytes", (unsigned long long)bytes);
    return buf;
}

// Limit-failure messages name the ArchiveLimits option to raise, its
// current value, and (when known) the size the member actually needs.
std::string member_limit_msg(const ArchiveLimits& lim, uint64_t needed) {
    std::string s = "member exceeds max_member_bytes limit (" +
                    human_size(lim.max_member_bytes);
    if (needed > lim.max_member_bytes)
        s += ", member is at least " + human_size(needed);
    s += "); skipped — raise max_member_bytes (--max-member-mb)";
    return s;
}

std::string ratio_limit_msg(const ArchiveLimits& lim) {
    return "member expansion exceeds max_ratio limit (" +
           std::to_string(lim.max_ratio) +
           "x compression ratio, suspected archive bomb); skipped — raise "
           "max_ratio only for trusted input";
}

std::string total_limit_msg(const ArchiveLimits& lim) {
    return "total uncompressed output exceeds max_total_bytes limit (" +
           human_size(lim.max_total_bytes) +
           "); walk stopped — raise max_total_bytes";
}

// Per-member ConvertOptions: members reuse document-relative image names
// (page1_img0), so each member gets its own subdirectory to prevent
// collisions; markdown refs follow via the ref prefix.
ConvertOptions member_convert_opts(const ConvertOptions& opts,
                                   const std::string& member_path) {
    ConvertOptions member_opts = opts;
    member_opts.image_output_dir = opts.image_output_dir + "/" + member_path;
    member_opts.image_ref_prefix = opts.image_ref_prefix + member_path + "/";
    util::ensure_dirs(member_opts.image_output_dir);
    return member_opts;
}

// Process one materialized member: detect, recurse or convert, emit.
// `bytes` need only stay valid for the duration of the call (zero-copy
// views into a parent buffer or the 7z solid-block cache are fine);
// `owned` is non-null when the caller can hand over ownership of the same
// bytes, sparing the parallel pipeline a copy.
// Returns false to stop the whole walk (budget exhausted or cb abort).
bool handle_member_bytes(const std::string& member_path, const uint8_t* bytes,
                         size_t size, std::vector<char>* owned, int depth,
                         WalkBudget& budget, const ConvertOptions& opts,
                         const MemberCallback& cb) {
    FileFormat fmt = detect_format_mem(bytes, size, member_path);

    MemberResult r;
    r.member_path = member_path;
    r.format = file_format_name(fmt);
    r.uncompressed_size = size;

    if (is_archive_format(fmt)) {
        // Negative max_depth means unlimited nesting (caller's explicit choice).
        if (opts.archive.max_depth >= 0 && depth + 1 > opts.archive.max_depth) {
            r.error_code = MemberErrorCode::DEPTH_LIMIT;
            r.error = "nested archive exceeds max_depth limit (" +
                      std::to_string(opts.archive.max_depth) +
                      "); raise max_depth (--max-depth) to descend";
            return cb(std::move(r));
        }
        return walk_archive_mem(bytes, size, fmt, member_path + "/",
                                depth + 1, budget, opts, cb);
    }

    if (fmt == FileFormat::UNKNOWN) {
        if (!opts.archive.include_unsupported) return true;
        r.error_code = MemberErrorCode::UNSUPPORTED;
        r.error = "unsupported format";
        return cb(std::move(r));
    }

    bool wants_images = opts.extract_images && !opts.image_output_dir.empty();

    if (budget.pipeline) {
        // Leaf documents convert on the pipeline workers while this thread
        // keeps decoding. Zero-copy views must be copied here — the buffer
        // they point into may be released before a worker runs.
        std::vector<char> data = owned
            ? std::move(*owned)
            : std::vector<char>(bytes, bytes + size);
        return budget.pipeline->submit(
            std::move(r), fmt, std::move(data),
            wants_images ? member_convert_opts(opts, member_path) : opts);
    }

    try {
        if (wants_images) {
            r.markdown = convert_from_memory_as(
                fmt, bytes, size, member_path,
                member_convert_opts(opts, member_path));
        } else {
            r.markdown = convert_from_memory_as(fmt, bytes, size,
                                                member_path, opts);
        }
    } catch (const std::exception& e) {
        r.error_code = MemberErrorCode::CONVERT_FAILED;
        r.error = e.what();
    }
    return cb(std::move(r));
}

bool handle_member(const std::string& member_path, std::vector<char>&& data,
                   int depth, WalkBudget& budget, const ConvertOptions& opts,
                   const MemberCallback& cb) {
    return handle_member_bytes(member_path,
                               reinterpret_cast<const uint8_t*>(data.data()),
                               data.size(), &data, depth, budget, opts, cb);
}

// Count a member against the entry budget; emits a final error result and
// returns false when the limit is hit.
bool count_entry(const std::string& member_path, WalkBudget& budget,
                 const ConvertOptions& opts, const MemberCallback& cb) {
    if (++budget.entries > opts.archive.max_entries) {
        MemberResult r;
        r.member_path = member_path;
        r.error_code = MemberErrorCode::ENTRY_LIMIT;
        r.error = "archive entry count exceeds max_entries limit (" +
                  std::to_string(opts.archive.max_entries) +
                  "); walk stopped — raise max_entries";
        cb(std::move(r));
        return false;
    }
    return true;
}

// Result of pre-checking a zero-copy view member against the caps.
enum class ViewCheck { PROCEED, SKIP, STOP };

// A member whose bytes are already resident (zero-copy view) skips CapSink;
// this applies the same member/total caps up front. Emits the limit result
// itself on SKIP/STOP.
ViewCheck precheck_view_member(const std::string& member_path, uint64_t size,
                               WalkBudget& budget, const ConvertOptions& opts,
                               const MemberCallback& cb) {
    if (size > opts.archive.max_member_bytes) {
        MemberResult r;
        r.member_path = member_path;
        r.uncompressed_size = size;
        r.error_code = MemberErrorCode::MEMBER_LIMIT;
        r.error = member_limit_msg(opts.archive, size);
        return cb(std::move(r)) ? ViewCheck::SKIP : ViewCheck::STOP;
    }
    budget.total_out += size;
    if (budget.total_out > opts.archive.max_total_bytes) {
        MemberResult r;
        r.member_path = member_path;
        r.uncompressed_size = size;
        r.error_code = MemberErrorCode::TOTAL_LIMIT;
        r.error = total_limit_msg(opts.archive);
        cb(std::move(r));
        return ViewCheck::STOP;  // cumulative budget gone — stop everything
    }
    return ViewCheck::PROCEED;
}

// Emit the appropriate error result after a failed materialization.
// declared_size is the header's uncompressed-size claim (0 = unknown);
// combined with the bytes actually produced it gives the caller a lower
// bound on how far max_member_bytes must be raised.
// Returns false if the whole walk must stop.
bool emit_stream_failure(const std::string& member_path, const CapSink& sink,
                         const std::string& stream_err, const char* fmt_name,
                         uint64_t got, uint64_t declared_size,
                         const ArchiveLimits& lim, const MemberCallback& cb) {
    MemberResult r;
    r.member_path = member_path;
    r.format = fmt_name;
    r.uncompressed_size = got;
    if (sink.total_exceeded) {
        r.error_code = MemberErrorCode::TOTAL_LIMIT;
        r.error = total_limit_msg(lim);
        cb(std::move(r));
        return false;  // cumulative budget gone — stop everything
    }
    if (sink.member_exceeded) {
        r.error_code = MemberErrorCode::MEMBER_LIMIT;
        r.error = member_limit_msg(lim, std::max(declared_size, got));
    } else if (sink.ratio_exceeded) {
        r.error_code = MemberErrorCode::RATIO_LIMIT;
        r.error = ratio_limit_msg(lim);
    } else {
        // Readers report unsupported coders and encrypted members through
        // the same error string channel as corruption; classify them so the
        // machine-readable code stays truthful.
        if (stream_err.find("encrypted") != std::string::npos)
            r.error_code = MemberErrorCode::ENCRYPTED;
        else if (stream_err.find("unsupported") != std::string::npos)
            r.error_code = MemberErrorCode::UNSUPPORTED;
        else
            r.error_code = MemberErrorCode::CORRUPT;
        r.error = stream_err.empty() ? "corrupt member data" : stream_err;
    }
    return cb(std::move(r));
}

// ── zip ─────────────────────────────────────────────────

bool walk_zip(const ZipReader& zip, const std::string& prefix, int depth,
              WalkBudget& budget, const ConvertOptions& opts,
              const MemberCallback& cb) {
    for (const auto& e : zip.entries()) {
        if (e.name.empty() || e.name.back() == '/') continue;  // directory
        if (is_metadata_member(e.name)) continue;
        std::string member_path = prefix + normalize_member_name(e.name);

        if (!count_entry(member_path, budget, opts, cb)) return false;

        if (has_skippable_ext(e.name)) {
            if (opts.archive.include_unsupported) {
                MemberResult r;
                r.member_path = member_path;
                r.error_code = MemberErrorCode::UNSUPPORTED;
                r.error = "unsupported format";
                r.uncompressed_size = e.uncompressed_size;
                if (!cb(std::move(r))) return false;
            }
            continue;
        }

        // Zero-copy: a STORE entry of a memory-backed zip (nested archive)
        // is already resident in the parent buffer — hand it to the parser
        // without materializing a copy.
        if (const uint8_t* view = zip.stored_view(e)) {
            switch (precheck_view_member(member_path, e.uncompressed_size,
                                         budget, opts, cb)) {
                case ViewCheck::SKIP: continue;
                case ViewCheck::STOP: return false;
                case ViewCheck::PROCEED: break;
            }
            if (!handle_member_bytes(member_path, view, e.uncompressed_size,
                                     nullptr, depth, budget, opts, cb))
                return false;
            continue;
        }

        std::vector<char> data;
        data.reserve(std::min<uint64_t>(e.uncompressed_size,
                                        opts.archive.max_member_bytes));
        CapSink sink{data, opts.archive.max_member_bytes,
                     ratio_cap_for(opts.archive, e.compressed_size),
                     opts.archive.max_total_bytes, budget};
        std::string err;
        if (!zip.read_entry_streamed(e, std::ref(sink), &err)) {
            if (!emit_stream_failure(member_path, sink, err, "UNKNOWN",
                                     data.size(), e.uncompressed_size,
                                     opts.archive, cb))
                return false;
            continue;
        }

        if (!handle_member(member_path, std::move(data), depth, budget, opts, cb))
            return false;
    }
    return true;
}

// ── 7z ──────────────────────────────────────────────────

bool walk_7z(const SevenZipReader& sz, const std::string& prefix, int depth,
             WalkBudget& budget, const ConvertOptions& opts,
             const MemberCallback& cb) {
    const auto& entries = sz.entries();
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (e.name.empty() || is_metadata_member(e.name)) continue;
        std::string member_path = prefix + normalize_member_name(e.name);

        if (!count_entry(member_path, budget, opts, cb)) return false;

        if (has_skippable_ext(e.name)) {
            if (opts.archive.include_unsupported) {
                MemberResult r;
                r.member_path = member_path;
                r.error_code = MemberErrorCode::UNSUPPORTED;
                r.error = "unsupported format";
                r.uncompressed_size = e.uncompressed_size;
                if (!cb(std::move(r))) return false;
            }
            continue;
        }

        // The SDK decodes a whole solid block (folder) into one buffer, so
        // the size check must happen BEFORE decoding: reject the folder while
        // its size is still just a header claim, never allocate it. No ratio
        // cap here — members of a solid block share one compressed stream, so
        // no meaningful per-member compressed size exists.
        if (e.folder_unpack_size > opts.archive.max_member_bytes) {
            MemberResult r;
            r.member_path = member_path;
            r.uncompressed_size = e.uncompressed_size;
            r.error_code = MemberErrorCode::MEMBER_LIMIT;
            r.error = "solid block exceeds max_member_bytes limit (" +
                      human_size(opts.archive.max_member_bytes) +
                      ", block is " + human_size(e.folder_unpack_size) +
                      "); skipped — raise max_member_bytes (--max-member-mb)";
            if (!cb(std::move(r))) return false;
            continue;
        }

        // Zero-copy: the SDK decodes the whole solid block into the cache
        // buffer and the member is a slice of it — hand the slice to the
        // parser directly. Peak memory drops from (block + member copy)
        // to the block alone.
        const uint8_t* view = nullptr;
        size_t view_size = 0;
        std::string err;
        if (!sz.read_entry_view(e, &view, &view_size, &err)) {
            sz.release_cache();
            std::vector<char> none;
            CapSink no_sink{none, 0, 0, 0, budget};  // flags stay false:
            if (!emit_stream_failure(member_path, no_sink, err, "UNKNOWN",
                                     0, e.uncompressed_size,
                                     opts.archive, cb))
                return false;
            continue;
        }

        ViewCheck vc = precheck_view_member(member_path, view_size, budget,
                                            opts, cb);
        if (vc == ViewCheck::STOP) return false;

        bool ok = true;
        if (vc == ViewCheck::PROCEED)
            ok = handle_member_bytes(member_path, view, view_size, nullptr,
                                     depth, budget, opts, cb);

        // The view points into the block cache, so release only after the
        // member is fully processed; keep the cache when upcoming members
        // share the folder.
        bool keep_cache = false;
        if (e.folder_index != SevenZipReader::kNoFolder) {
            for (size_t j = i + 1; j < entries.size(); ++j) {
                const auto& next = entries[j];
                if (next.folder_index != e.folder_index) break;
                if (!next.name.empty() && !is_metadata_member(next.name) &&
                    !has_skippable_ext(next.name)) {
                    keep_cache = true;
                    break;
                }
            }
        }
        if (!keep_cache) sz.release_cache();

        if (!ok) return false;
    }
    return true;
}

// ── tar ─────────────────────────────────────────────────

bool walk_tar(InputStream& src, const std::string& prefix, int depth,
              WalkBudget& budget, const ConvertOptions& opts,
              const MemberCallback& cb) {
    TarReader tar(src);
    TarReader::Member m;
    while (tar.next(m)) {
        if (!m.is_file || m.size == 0 || is_metadata_member(m.name)) {
            if (!tar.skip_data()) break;
            continue;
        }
        std::string member_path = prefix + normalize_member_name(m.name);

        if (!count_entry(member_path, budget, opts, cb)) return false;

        if (has_skippable_ext(m.name)) {
            if (opts.archive.include_unsupported) {
                MemberResult r;
                r.member_path = member_path;
                r.error_code = MemberErrorCode::UNSUPPORTED;
                r.error = "unsupported format";
                r.uncompressed_size = m.size;
                if (!cb(std::move(r))) return false;
            }
            if (!tar.skip_data()) break;
            continue;
        }

        // Zero-copy: a memory-backed tar (nested archive) exposes the
        // member's bytes in place — no materialization needed.
        if (const uint8_t* view = tar.view_data()) {
            switch (precheck_view_member(member_path, m.size, budget, opts,
                                         cb)) {
                case ViewCheck::SKIP: continue;
                case ViewCheck::STOP: return false;
                case ViewCheck::PROCEED: break;
            }
            if (!handle_member_bytes(member_path, view, m.size, nullptr,
                                     depth, budget, opts, cb))
                return false;
            continue;
        }

        // tar stores members uncompressed; the size field is bounded by the
        // container itself, but caps still apply (and guard the total budget).
        std::vector<char> data;
        data.reserve(std::min<uint64_t>(m.size, opts.archive.max_member_bytes));
        CapSink sink{data, opts.archive.max_member_bytes, 0,
                     opts.archive.max_total_bytes, budget};
        if (!tar.read_data(std::ref(sink))) {
            if (!emit_stream_failure(member_path, sink, "truncated tar member",
                                     "UNKNOWN", data.size(), m.size,
                                     opts.archive, cb))
                return false;
            // A partially consumed member breaks tar framing — stop this tar.
            break;
        }

        if (!handle_member(member_path, std::move(data), depth, budget, opts, cb))
            return false;
    }
    return true;
}

// ── alz ─────────────────────────────────────────────────

bool walk_alz(InputStream& src, const std::string& name_hint,
              const std::string& prefix, int depth, WalkBudget& budget,
              const ConvertOptions& opts, const MemberCallback& cb) {
    AlzReader alz(src);
    if (!alz.is_open()) {
        MemberResult r;
        r.member_path = prefix.empty() ? name_hint : prefix;
        r.error_code = MemberErrorCode::CORRUPT;
        r.error = "cannot open alz archive";
        return cb(std::move(r));
    }

    AlzReader::Member m;
    while (alz.next(m)) {
        // Directory-style entries carry no size/method fields.
        if (!m.has_data_fields || m.uncompressed_size == 0 ||
            is_metadata_member(m.name)) {
            if (!alz.skip_data()) break;
            continue;
        }
        std::string member_path = prefix + normalize_member_name(m.name);

        if (!count_entry(member_path, budget, opts, cb)) return false;

        if (has_skippable_ext(m.name)) {
            if (opts.archive.include_unsupported) {
                MemberResult r;
                r.member_path = member_path;
                r.error_code = MemberErrorCode::UNSUPPORTED;
                r.error = "unsupported format";
                r.uncompressed_size = m.uncompressed_size;
                if (!cb(std::move(r))) return false;
            }
            if (!alz.skip_data()) break;
            continue;
        }

        std::vector<char> data;
        data.reserve(std::min<uint64_t>(m.uncompressed_size,
                                        opts.archive.max_member_bytes));
        CapSink sink{data, opts.archive.max_member_bytes,
                     ratio_cap_for(opts.archive, m.compressed_size),
                     opts.archive.max_total_bytes, budget};
        std::string err;
        if (!alz.read_data(std::ref(sink), &err)) {
            if (!emit_stream_failure(member_path, sink, err, "UNKNOWN",
                                     data.size(), m.uncompressed_size,
                                     opts.archive, cb))
                return false;
            continue;  // failed reads still consume the member: framing holds
        }

        if (!handle_member(member_path, std::move(data), depth, budget, opts, cb))
            return false;
    }
    return true;
}

// ── rar ─────────────────────────────────────────────────

bool walk_rar(InputStream& src, const std::string& name_hint,
              const std::string& prefix, int depth, WalkBudget& budget,
              const ConvertOptions& opts, const MemberCallback& cb) {
    RarReader rar(src);
    if (!rar.is_open()) {
        MemberResult r;
        r.member_path = prefix.empty() ? name_hint : prefix;
        r.error_code = MemberErrorCode::CORRUPT;
        r.error = "cannot open rar archive";
        return cb(std::move(r));
    }
    RarReader::Member m;
    while (rar.next(m)) {
        if (m.directory || m.uncompressed_size == 0 ||
            is_metadata_member(m.name)) {
            if (!rar.skip_data()) break;
            continue;
        }
        std::string member_path = prefix + normalize_member_name(m.name);

        if (!count_entry(member_path, budget, opts, cb)) return false;

        if (has_skippable_ext(m.name)) {
            if (opts.archive.include_unsupported) {
                MemberResult r;
                r.member_path = member_path;
                r.error_code = MemberErrorCode::UNSUPPORTED;
                r.error = "unsupported format";
                r.uncompressed_size = m.uncompressed_size;
                if (!cb(std::move(r))) return false;
            }
            if (!rar.skip_data()) break;
            continue;
        }

        std::vector<char> data;
        data.reserve(std::min<uint64_t>(m.uncompressed_size,
                                        opts.archive.max_member_bytes));
        CapSink sink{data, opts.archive.max_member_bytes,
                     ratio_cap_for(opts.archive, m.compressed_size),
                     opts.archive.max_total_bytes, budget};
        std::string err;
        if (!rar.read_data(std::ref(sink), &err)) {
            if (!emit_stream_failure(member_path, sink, err, "UNKNOWN",
                                     data.size(), m.uncompressed_size,
                                     opts.archive, cb))
                return false;
            continue;  // failed reads still consume the member: framing holds
        }

        if (!handle_member(member_path, std::move(data), depth, budget, opts, cb))
            return false;
    }
    // rar4 flags this in the constructor, rar5 on the first header block.
    if (rar.headers_encrypted()) {
        MemberResult r;
        r.member_path = prefix.empty() ? name_hint : prefix;
        r.error_code = MemberErrorCode::ENCRYPTED;
        r.error = "encrypted rar archive unsupported";
        return cb(std::move(r));
    }
    return true;
}

// ── egg ─────────────────────────────────────────────────

// Solid egg: all file headers precede one shared block stream whose
// decoded bytes cover the members in order. Members are demultiplexed
// while the stream decodes, so only one member is resident at a time.
bool walk_egg_solid(EggReader& egg, const std::string& prefix, int depth,
                    WalkBudget& budget, const ConvertOptions& opts,
                    const MemberCallback& cb) {
    std::vector<EggReader::Member> members;
    EggReader::Member m;
    while (egg.next(m)) members.push_back(std::move(m));
    if (!egg.at_solid_data()) {
        // No data area (all members empty) — nothing to emit.
        return true;
    }

    struct Demux {
        std::vector<EggReader::Member>& members;
        const std::string& prefix;
        int depth;
        WalkBudget& budget;
        const ConvertOptions& opts;
        const MemberCallback& cb;

        size_t idx = 0;              // next member to start
        uint64_t member_left = 0;    // undelivered bytes of current member
        std::vector<char> data;
        bool draining = false;       // consume current member without keeping
        bool cap_hit = false;        // draining because the member cap tripped
        bool stopped = false;        // walk aborted (budget/cb)

        // Position on the next member that has stream bytes; decides
        // whether it will be materialized or just drained.
        bool next_member() {
            while (idx < members.size()) {
                auto& mem = members[idx++];
                if (mem.uncompressed_size == 0) continue;  // no stream bytes
                member_left = mem.uncompressed_size;
                draining = true;
                cap_hit = false;

                if (mem.name.empty() || is_metadata_member(mem.name))
                    return true;  // silent drain

                std::string path = prefix + normalize_member_name(mem.name);
                if (!count_entry(path, budget, opts, cb)) {
                    stopped = true;
                    return false;
                }
                if (has_skippable_ext(mem.name) || mem.encrypted) {
                    if (mem.encrypted || opts.archive.include_unsupported) {
                        MemberResult r;
                        r.member_path = path;
                        r.uncompressed_size = mem.uncompressed_size;
                        r.error_code = mem.encrypted
                            ? MemberErrorCode::ENCRYPTED
                            : MemberErrorCode::UNSUPPORTED;
                        r.error = mem.encrypted ? "encrypted member unsupported"
                                                : "unsupported format";
                        if (!cb(std::move(r))) { stopped = true; return false; }
                    }
                    return true;  // drain
                }

                draining = false;
                data.clear();
                data.reserve(std::min<uint64_t>(mem.uncompressed_size,
                                                opts.archive.max_member_bytes));
                return true;
            }
            return false;  // stream longer than declared members: corrupt
        }

        bool finish_member() {
            auto& mem = members[idx - 1];
            std::string path = prefix + normalize_member_name(mem.name);
            if (draining) {
                if (cap_hit) {
                    MemberResult r;
                    r.member_path = path;
                    r.uncompressed_size = mem.uncompressed_size;
                    r.error_code = MemberErrorCode::MEMBER_LIMIT;
                    r.error = member_limit_msg(opts.archive,
                                               mem.uncompressed_size);
                    if (!cb(std::move(r))) { stopped = true; return false; }
                }
                return true;
            }
            if (!handle_member(path, std::move(data), depth, budget, opts, cb)) {
                stopped = true;
                return false;
            }
            data = std::vector<char>();
            return true;
        }

        bool operator()(const char* p, size_t n) {
            while (n > 0) {
                if (member_left == 0 && !next_member()) return false;
                size_t take = static_cast<size_t>(
                    std::min<uint64_t>(n, member_left));
                if (!draining) {
                    if (data.size() + take > opts.archive.max_member_bytes) {
                        draining = true;  // cap hit: drain the rest
                        cap_hit = true;
                        data = std::vector<char>();
                    } else {
                        budget.total_out += take;
                        if (budget.total_out > opts.archive.max_total_bytes) {
                            MemberResult r;
                            r.member_path = prefix + normalize_member_name(
                                members[idx - 1].name);
                            r.error_code = MemberErrorCode::TOTAL_LIMIT;
                            r.error = total_limit_msg(opts.archive);
                            cb(std::move(r));
                            stopped = true;
                            return false;
                        }
                        data.insert(data.end(), p, p + take);
                    }
                }
                p += take;
                n -= take;
                member_left -= take;
                if (member_left == 0 && !finish_member()) return false;
            }
            return true;
        }
    } demux{members, prefix, depth, budget, opts, cb};

    std::string err;
    if (!egg.read_solid_stream(std::ref(demux), &err)) {
        if (demux.stopped) return false;  // budget/cb abort already reported
        MemberResult r;
        r.member_path = demux.idx > 0 && demux.idx <= members.size()
            ? prefix + normalize_member_name(members[demux.idx - 1].name)
            : prefix;
        r.error_code = MemberErrorCode::CORRUPT;
        r.error = err.empty() ? "corrupt solid egg data" : err;
        return cb(std::move(r));
    }
    return true;
}

bool walk_egg(InputStream& src, const std::string& name_hint,
              const std::string& prefix, int depth, WalkBudget& budget,
              const ConvertOptions& opts, const MemberCallback& cb) {
    EggReader egg(src);
    if (!egg.is_open()) {
        MemberResult r;
        r.member_path = prefix.empty() ? name_hint : prefix;
        r.error_code = MemberErrorCode::CORRUPT;
        r.error = "cannot open egg archive";
        return cb(std::move(r));
    }
    if (egg.is_split() || egg.is_global_encrypted()) {
        MemberResult r;
        r.member_path = prefix.empty() ? name_hint : prefix;
        r.error_code = egg.is_split() ? MemberErrorCode::UNSUPPORTED
                                      : MemberErrorCode::ENCRYPTED;
        r.error = egg.is_split() ? "split egg volume unsupported"
                                 : "encrypted egg archive unsupported";
        return cb(std::move(r));
    }
    if (egg.is_solid())
        return walk_egg_solid(egg, prefix, depth, budget, opts, cb);

    EggReader::Member m;
    while (egg.next(m)) {
        if (m.name.empty() || m.uncompressed_size == 0 ||
            is_metadata_member(m.name)) {
            if (!egg.skip_data()) break;
            continue;
        }
        std::string member_path = prefix + normalize_member_name(m.name);

        if (!count_entry(member_path, budget, opts, cb)) return false;

        if (has_skippable_ext(m.name)) {
            if (opts.archive.include_unsupported) {
                MemberResult r;
                r.member_path = member_path;
                r.error_code = MemberErrorCode::UNSUPPORTED;
                r.error = "unsupported format";
                r.uncompressed_size = m.uncompressed_size;
                if (!cb(std::move(r))) return false;
            }
            if (!egg.skip_data()) break;
            continue;
        }

        // No per-member compressed size up front (data is block-framed), so
        // the ratio cap is skipped; the member and total caps still bound
        // memory and cumulative output.
        std::vector<char> data;
        data.reserve(std::min<uint64_t>(m.uncompressed_size,
                                        opts.archive.max_member_bytes));
        CapSink sink{data, opts.archive.max_member_bytes, 0,
                     opts.archive.max_total_bytes, budget};
        std::string err;
        if (!egg.read_data(std::ref(sink), &err)) {
            if (!emit_stream_failure(member_path, sink, err, "UNKNOWN",
                                     data.size(), m.uncompressed_size,
                                     opts.archive, cb))
                return false;
            continue;
        }

        if (!handle_member(member_path, std::move(data), depth, budget, opts, cb))
            return false;
    }
    return true;
}

// ── gzip (single member or tar.gz) ──────────────────────

// Replays a peeked prefix before continuing with the live stream.
class PrefixedStream final : public InputStream {
public:
    PrefixedStream(std::vector<char> prefix, InputStream& rest)
        : prefix_(std::move(prefix)), rest_(rest) {}

    size_t read(void* buf, size_t len) override {
        if (pos_ < prefix_.size()) {
            size_t n = std::min(len, prefix_.size() - pos_);
            memcpy(buf, prefix_.data() + pos_, n);
            pos_ += n;
            return n;
        }
        return rest_.read(buf, len);
    }

private:
    std::vector<char> prefix_;
    size_t pos_ = 0;
    InputStream& rest_;
};

bool walk_gz(InputStream& raw, const std::string& name_hint,
             const std::string& prefix, int depth, WalkBudget& budget,
             const ConvertOptions& opts, const MemberCallback& cb) {
    GzInflateStream gz(raw);
    if (!gz.is_open()) {
        MemberResult r;
        r.member_path = prefix + normalize_member_name(name_hint);
        r.error_code = MemberErrorCode::CORRUPT;
        r.error = "cannot initialize gzip decompressor";
        return cb(std::move(r));
    }

    // Peek the first 512 bytes: "ustar" at offset 257 means tar.gz.
    std::vector<char> head(512);
    size_t got = 0;
    while (got < head.size()) {
        size_t n = gz.read(head.data() + got, head.size() - got);
        if (n == 0) break;
        got += n;
    }
    head.resize(got);

    bool is_tar = got >= 262 && memcmp(head.data() + 257, "ustar", 5) == 0;
    PrefixedStream stream(std::move(head), gz);

    if (is_tar)
        return walk_tar(stream, prefix, depth, budget, opts, cb);

    // Single gzipped member (e.g. report.pdf.gz) — inner name is the
    // basename with .gz stripped
    std::string base = name_hint;
    auto slash = base.rfind('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    std::string inner_name = strip_gz_ext(base);
    std::string member_path = prefix + normalize_member_name(inner_name);
    if (!count_entry(member_path, budget, opts, cb)) return false;

    std::vector<char> data;
    CapSink sink{data, opts.archive.max_member_bytes, 0,
                 opts.archive.max_total_bytes, budget};
    char buf[65536];
    bool aborted = false;
    for (;;) {
        size_t n = stream.read(buf, sizeof(buf));
        if (n == 0) break;
        if (!sink(buf, n)) { aborted = true; break; }
        // Ratio check with exact stream counters
        if (data.size() > (16ull << 20) && opts.archive.max_ratio > 0 &&
            gz.compressed_in() > 0 &&
            data.size() / gz.compressed_in() > opts.archive.max_ratio) {
            sink.ratio_exceeded = true;
            aborted = true;
            break;
        }
    }
    if (aborted || gz.error()) {
        return emit_stream_failure(member_path, sink,
                                   gz.error() ? "corrupt gzip stream" : "",
                                   "UNKNOWN", data.size(), 0,
                                   opts.archive, cb);
    }

    return handle_member(member_path, std::move(data), depth, budget, opts, cb);
}

// ── bzip2 (single member or tar.bz2) ────────────────────

bool walk_bz2(InputStream& raw, const std::string& name_hint,
              const std::string& prefix, int depth, WalkBudget& budget,
              const ConvertOptions& opts, const MemberCallback& cb) {
    BzInflateStream bz(raw);
    if (!bz.is_open()) {
        MemberResult r;
        r.member_path = prefix + normalize_member_name(name_hint);
        r.error_code = BzInflateStream::supported()
            ? MemberErrorCode::CORRUPT
            : MemberErrorCode::UNSUPPORTED;
        r.error = BzInflateStream::supported()
            ? "cannot initialize bzip2 decompressor"
            : "bzip2 support not built (JDOC_WITH_BZIP2)";
        return cb(std::move(r));
    }

    // Peek the first 512 bytes: "ustar" at offset 257 means tar.bz2.
    std::vector<char> head(512);
    size_t got = 0;
    while (got < head.size()) {
        size_t n = bz.read(head.data() + got, head.size() - got);
        if (n == 0) break;
        got += n;
    }
    head.resize(got);

    bool is_tar = got >= 262 && memcmp(head.data() + 257, "ustar", 5) == 0;
    PrefixedStream stream(std::move(head), bz);

    if (is_tar)
        return walk_tar(stream, prefix, depth, budget, opts, cb);

    // Single bzipped member (e.g. report.pdf.bz2) — inner name is the
    // basename with .bz2 stripped
    std::string base = name_hint;
    auto slash = base.rfind('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    std::string inner_name = strip_bz2_ext(base);
    std::string member_path = prefix + normalize_member_name(inner_name);
    if (!count_entry(member_path, budget, opts, cb)) return false;

    std::vector<char> data;
    CapSink sink{data, opts.archive.max_member_bytes, 0,
                 opts.archive.max_total_bytes, budget};
    char buf[65536];
    bool aborted = false;
    for (;;) {
        size_t n = stream.read(buf, sizeof(buf));
        if (n == 0) break;
        if (!sink(buf, n)) { aborted = true; break; }
        // Ratio check with exact stream counters
        if (data.size() > (16ull << 20) && opts.archive.max_ratio > 0 &&
            bz.compressed_in() > 0 &&
            data.size() / bz.compressed_in() > opts.archive.max_ratio) {
            sink.ratio_exceeded = true;
            aborted = true;
            break;
        }
    }
    if (aborted || bz.error()) {
        return emit_stream_failure(member_path, sink,
                                   bz.error() ? "corrupt bzip2 stream" : "",
                                   "UNKNOWN", data.size(), 0,
                                   opts.archive, cb);
    }

    return handle_member(member_path, std::move(data), depth, budget, opts, cb);
}

} // anonymous namespace

// ── Entry points ────────────────────────────────────────

static bool walk_dispatch(FileFormat fmt,
                          std::unique_ptr<InputStream> stream,
                          const ZipReader* zip,
                          const SevenZipReader* sevenzip,
                          const std::string& name_hint,
                          const std::string& prefix, int depth,
                          WalkBudget& budget, const ConvertOptions& opts,
                          const MemberCallback& cb) {
    switch (fmt) {
        case FileFormat::ZIP:
            return walk_zip(*zip, prefix, depth, budget, opts, cb);
        case FileFormat::SEVENZIP:
            return walk_7z(*sevenzip, prefix, depth, budget, opts, cb);
        case FileFormat::GZIP:
            return walk_gz(*stream, name_hint, prefix, depth, budget, opts, cb);
        case FileFormat::BZIP2:
            return walk_bz2(*stream, name_hint, prefix, depth, budget, opts, cb);
        case FileFormat::TAR:
            return walk_tar(*stream, prefix, depth, budget, opts, cb);
        case FileFormat::ALZ:
            return walk_alz(*stream, name_hint, prefix, depth, budget, opts, cb);
        case FileFormat::EGG:
            return walk_egg(*stream, name_hint, prefix, depth, budget, opts, cb);
        case FileFormat::RAR:
            return walk_rar(*stream, name_hint, prefix, depth, budget, opts, cb);
        default: {
            MemberResult r;
            r.member_path = prefix.empty() ? name_hint : prefix;
            r.format = file_format_name(fmt);
            r.error_code = MemberErrorCode::UNSUPPORTED;
            r.error = "archive format not yet supported";
            cb(std::move(r));
            return true;
        }
    }
}

bool walk_archive_path(const std::string& file_path, FileFormat fmt,
                       const std::string& prefix, int depth, WalkBudget& budget,
                       const ConvertOptions& opts, const MemberCallback& cb) {
    if (fmt == FileFormat::ZIP) {
        ZipReader zip(file_path);
        if (!zip.is_open()) {
            MemberResult r;
            r.member_path = prefix.empty() ? file_path : prefix;
            r.error_code = MemberErrorCode::CORRUPT;
            r.error = "cannot open zip archive";
            return cb(std::move(r));
        }
        return walk_dispatch(fmt, nullptr, &zip, nullptr, file_path, prefix,
                             depth, budget, opts, cb);
    }
    if (fmt == FileFormat::SEVENZIP) {
        SevenZipReader sz(file_path);
        if (!sz.is_open()) {
            MemberResult r;
            r.member_path = prefix.empty() ? file_path : prefix;
            r.error_code = MemberErrorCode::CORRUPT;
            r.error = "cannot open 7z archive";
            return cb(std::move(r));
        }
        return walk_dispatch(fmt, nullptr, nullptr, &sz, file_path, prefix,
                             depth, budget, opts, cb);
    }
    auto stream = std::make_unique<FileStream>(file_path);
    if (!stream->is_open()) {
        MemberResult r;
        r.member_path = prefix.empty() ? file_path : prefix;
        r.error_code = MemberErrorCode::CORRUPT;
        r.error = "cannot open archive file";
        return cb(std::move(r));
    }
    return walk_dispatch(fmt, std::move(stream), nullptr, nullptr, file_path,
                         prefix, depth, budget, opts, cb);
}

bool walk_archive_mem(const uint8_t* data, size_t size, FileFormat fmt,
                      const std::string& prefix, int depth, WalkBudget& budget,
                      const ConvertOptions& opts, const MemberCallback& cb) {
    if (fmt == FileFormat::ZIP) {
        ZipReader zip(data, size);
        if (!zip.is_open()) {
            MemberResult r;
            r.member_path = prefix;
            r.error_code = MemberErrorCode::CORRUPT;
            r.error = "cannot open nested zip archive";
            return cb(std::move(r));
        }
        return walk_dispatch(fmt, nullptr, &zip, nullptr, prefix, prefix,
                             depth, budget, opts, cb);
    }
    if (fmt == FileFormat::SEVENZIP) {
        SevenZipReader sz(data, size);
        if (!sz.is_open()) {
            MemberResult r;
            r.member_path = prefix;
            r.error_code = MemberErrorCode::CORRUPT;
            r.error = "cannot open nested 7z archive";
            return cb(std::move(r));
        }
        return walk_dispatch(fmt, nullptr, nullptr, &sz, prefix, prefix,
                             depth, budget, opts, cb);
    }
    auto stream = std::make_unique<MemoryStream>(data, size);
    // name_hint for a nested gz is the member path itself (prefix ends with '/')
    std::string hint = prefix;
    if (!hint.empty() && hint.back() == '/') hint.pop_back();
    return walk_dispatch(fmt, std::move(stream), nullptr, nullptr, hint,
                         prefix, depth, budget, opts, cb);
}

} // namespace jdoc
