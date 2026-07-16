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
#include "archive/alz_reader.h"
#include "archive/egg_reader.h"
#include "archive/input_stream.h"
#include "archive/seven_zip_reader.h"
#include "archive/tar_reader.h"

#include <algorithm>
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

// Sink that appends to a buffer while enforcing the per-member cap and the
// cumulative budget during streaming decompression.
struct CapSink {
    std::vector<char>& out;
    uint64_t member_cap;
    uint64_t total_cap;
    WalkBudget& budget;
    bool member_exceeded = false;
    bool total_exceeded = false;

    bool operator()(const char* p, size_t n) {
        if (out.size() + n > member_cap) { member_exceeded = true; return false; }
        budget.total_out += n;
        if (budget.total_out > total_cap) { total_exceeded = true; return false; }
        out.insert(out.end(), p, p + n);
        return true;
    }
};

// Per-member cap: the smaller of max_member_bytes and the compression-ratio
// bound (when the compressed size is known). Small members are allowed to
// expand freely up to 16 MiB so tiny highly-compressible files don't trip
// the ratio check.
uint64_t effective_member_cap(const ArchiveLimits& lim, uint64_t compressed_size) {
    uint64_t cap = lim.max_member_bytes;
    if (compressed_size > 0 && lim.max_ratio > 0) {
        uint64_t ratio_cap = std::max<uint64_t>(16ull << 20,
                                                compressed_size * lim.max_ratio);
        cap = std::min(cap, ratio_cap);
    }
    return cap;
}

// Process one fully materialized member: detect, recurse or convert, emit.
// Returns false to stop the whole walk (budget exhausted or cb abort).
bool handle_member(const std::string& member_path, std::vector<char>&& data,
                   int depth, WalkBudget& budget, const ConvertOptions& opts,
                   const MemberCallback& cb) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    FileFormat fmt = detect_format_mem(bytes, data.size(), member_path);

    MemberResult r;
    r.member_path = member_path;
    r.format = file_format_name(fmt);
    r.uncompressed_size = data.size();

    if (is_archive_format(fmt)) {
        if (depth + 1 > opts.archive.max_depth) {
            r.error = "max archive depth exceeded";
            return cb(std::move(r));
        }
        return walk_archive_mem(bytes, data.size(), fmt, member_path + "/",
                                depth + 1, budget, opts, cb);
    }

    if (fmt == FileFormat::UNKNOWN) {
        if (!opts.archive.include_unsupported) return true;
        r.error = "unsupported format";
        return cb(std::move(r));
    }

    try {
        r.markdown = convert_from_memory(bytes, data.size(), member_path, opts);
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return cb(std::move(r));
}

// Count a member against the entry budget; emits a final error result and
// returns false when the limit is hit.
bool count_entry(const std::string& member_path, WalkBudget& budget,
                 const ConvertOptions& opts, const MemberCallback& cb) {
    if (++budget.entries > opts.archive.max_entries) {
        MemberResult r;
        r.member_path = member_path;
        r.error = "max archive entry count exceeded; walk stopped";
        cb(std::move(r));
        return false;
    }
    return true;
}

// Emit the appropriate error result after a failed materialization.
// Returns false if the whole walk must stop.
bool emit_stream_failure(const std::string& member_path, const CapSink& sink,
                         const std::string& stream_err, const char* fmt_name,
                         uint64_t got, const MemberCallback& cb) {
    MemberResult r;
    r.member_path = member_path;
    r.format = fmt_name;
    r.uncompressed_size = got;
    if (sink.total_exceeded) {
        r.error = "total uncompressed size limit exceeded; walk stopped";
        cb(std::move(r));
        return false;  // cumulative budget gone — stop everything
    }
    if (sink.member_exceeded)
        r.error = "member exceeds size/ratio limit, skipped";
    else
        r.error = stream_err.empty() ? "corrupt member data" : stream_err;
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
                r.error = "unsupported format";
                r.uncompressed_size = e.uncompressed_size;
                if (!cb(std::move(r))) return false;
            }
            continue;
        }

        std::vector<char> data;
        data.reserve(std::min<uint64_t>(e.uncompressed_size,
                                        opts.archive.max_member_bytes));
        CapSink sink{data, effective_member_cap(opts.archive, e.compressed_size),
                     opts.archive.max_total_bytes, budget};
        std::string err;
        if (!zip.read_entry_streamed(e, std::ref(sink), &err)) {
            if (!emit_stream_failure(member_path, sink, err, "UNKNOWN",
                                     data.size(), cb))
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
    for (const auto& e : sz.entries()) {
        if (e.name.empty() || is_metadata_member(e.name)) continue;
        std::string member_path = prefix + normalize_member_name(e.name);

        if (!count_entry(member_path, budget, opts, cb)) return false;

        if (has_skippable_ext(e.name)) {
            if (opts.archive.include_unsupported) {
                MemberResult r;
                r.member_path = member_path;
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
            r.error = "solid block exceeds size limit, skipped";
            if (!cb(std::move(r))) return false;
            continue;
        }

        std::vector<char> data;
        data.reserve(std::min<uint64_t>(e.uncompressed_size,
                                        opts.archive.max_member_bytes));
        CapSink sink{data, opts.archive.max_member_bytes,
                     opts.archive.max_total_bytes, budget};
        std::string err;
        if (!sz.read_entry_streamed(e, std::ref(sink), &err)) {
            if (!emit_stream_failure(member_path, sink, err, "UNKNOWN",
                                     data.size(), cb))
                return false;
            continue;
        }

        if (!handle_member(member_path, std::move(data), depth, budget, opts, cb))
            return false;
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
                r.error = "unsupported format";
                r.uncompressed_size = m.size;
                if (!cb(std::move(r))) return false;
            }
            if (!tar.skip_data()) break;
            continue;
        }

        // tar stores members uncompressed; the size field is bounded by the
        // container itself, but caps still apply (and guard the total budget).
        std::vector<char> data;
        data.reserve(std::min<uint64_t>(m.size, opts.archive.max_member_bytes));
        CapSink sink{data, opts.archive.max_member_bytes,
                     opts.archive.max_total_bytes, budget};
        if (!tar.read_data(std::ref(sink))) {
            if (!emit_stream_failure(member_path, sink, "truncated tar member",
                                     "UNKNOWN", data.size(), cb))
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
        CapSink sink{data, effective_member_cap(opts.archive, m.compressed_size),
                     opts.archive.max_total_bytes, budget};
        std::string err;
        if (!alz.read_data(std::ref(sink), &err)) {
            if (!emit_stream_failure(member_path, sink, err, "UNKNOWN",
                                     data.size(), cb))
                return false;
            continue;  // failed reads still consume the member: framing holds
        }

        if (!handle_member(member_path, std::move(data), depth, budget, opts, cb))
            return false;
    }
    return true;
}

// ── egg ─────────────────────────────────────────────────

bool walk_egg(InputStream& src, const std::string& name_hint,
              const std::string& prefix, int depth, WalkBudget& budget,
              const ConvertOptions& opts, const MemberCallback& cb) {
    EggReader egg(src);
    if (!egg.is_open()) {
        MemberResult r;
        r.member_path = prefix.empty() ? name_hint : prefix;
        r.error = "cannot open egg archive";
        return cb(std::move(r));
    }
    if (egg.is_solid() || egg.is_split()) {
        MemberResult r;
        r.member_path = prefix.empty() ? name_hint : prefix;
        r.error = egg.is_solid() ? "solid egg archive unsupported"
                                 : "split egg volume unsupported";
        return cb(std::move(r));
    }

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
        CapSink sink{data, opts.archive.max_member_bytes,
                     opts.archive.max_total_bytes, budget};
        std::string err;
        if (!egg.read_data(std::ref(sink), &err)) {
            if (!emit_stream_failure(member_path, sink, err, "UNKNOWN",
                                     data.size(), cb))
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
    CapSink sink{data, opts.archive.max_member_bytes,
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
            sink.member_exceeded = true;
            aborted = true;
            break;
        }
    }
    if (aborted || gz.error()) {
        return emit_stream_failure(member_path, sink,
                                   gz.error() ? "corrupt gzip stream" : "",
                                   "UNKNOWN", data.size(), cb);
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
        case FileFormat::TAR:
            return walk_tar(*stream, prefix, depth, budget, opts, cb);
        case FileFormat::ALZ:
            return walk_alz(*stream, name_hint, prefix, depth, budget, opts, cb);
        case FileFormat::EGG:
            return walk_egg(*stream, name_hint, prefix, depth, budget, opts, cb);
        default: {
            MemberResult r;
            r.member_path = prefix.empty() ? name_hint : prefix;
            r.format = file_format_name(fmt);
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
