// bench_convert.cpp - Benchmark tool for archive vs per-file conversion
//
//   bench_convert archive <file.zip|tar.gz>   convert members in memory
//   bench_convert dir <directory>             convert every file in a directory
//
// Prints member/file count, total output bytes, error count, and wall time.
// Single process, single thread — used for the A/B/C comparison:
//   A: unzip to tmp + `bench_convert dir` + rm   (extract-then-parse)
//   B: `bench_convert archive`                    (direct in-memory parse)
//   C: `bench_convert dir` on pre-extracted files (pure parse baseline)
//
// `--hash` switches to output-equivalence mode: one line per member/file with
// a digest of the markdown plus the error code and message. The summary line's
// out_bytes total cannot catch a change that moves bytes between members, so
// optimization work diffs this instead:
//
//   diff <(old bench_convert dir CORPUS --hash) <(new bench_convert dir CORPUS --hash)
//
// License: MIT

#include "jdoc/jdoc.h"
#include "jdoc/archive.h"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static bool is_metadata_file(const fs::path& p) {
    auto name = p.filename().string();
    return name.rfind("._", 0) == 0 || name == ".DS_Store";
}

static bool has_skippable_ext(const fs::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);

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

// FNV-1a. Not cryptographic — this only has to catch an accidental change in
// converter output, and keeping it inline avoids a digest dependency.
static uint64_t digest(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

static const char* error_code_name(jdoc::MemberErrorCode c) {
    switch (c) {
        case jdoc::MemberErrorCode::OK:             return "OK";
        case jdoc::MemberErrorCode::MEMBER_LIMIT:   return "MEMBER_LIMIT";
        case jdoc::MemberErrorCode::RATIO_LIMIT:    return "RATIO_LIMIT";
        case jdoc::MemberErrorCode::TOTAL_LIMIT:    return "TOTAL_LIMIT";
        case jdoc::MemberErrorCode::ENTRY_LIMIT:    return "ENTRY_LIMIT";
        case jdoc::MemberErrorCode::DEPTH_LIMIT:    return "DEPTH_LIMIT";
        case jdoc::MemberErrorCode::ENCRYPTED:      return "ENCRYPTED";
        case jdoc::MemberErrorCode::UNSUPPORTED:    return "UNSUPPORTED";
        case jdoc::MemberErrorCode::CORRUPT:        return "CORRUPT";
        case jdoc::MemberErrorCode::CONVERT_FAILED: return "CONVERT_FAILED";
    }
    return "?";
}

// One line per member: everything a change could alter, including the failure
// classification and message.
static void print_hash_line(const std::string& path, const std::string& markdown,
                            jdoc::MemberErrorCode code, const std::string& err) {
    printf("%s\t%016llx\t%zu\t%s\t%s\n", path.c_str(),
           static_cast<unsigned long long>(digest(markdown)), markdown.size(),
           error_code_name(code), err.c_str());
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <archive|dir> <path> [--hash]\n", argv[0]);
        return 1;
    }
    std::string mode = argv[1];
    std::string path = argv[2];

    size_t count = 0, errors = 0;
    uint64_t out_bytes = 0;
    bool hash_mode = false;
    jdoc::ConvertOptions opts;
    opts.archive.max_total_bytes = 16ull << 30;  // benchmark corpus can exceed the library default.
    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--hash") hash_mode = true;
        // Other positional args (e.g. a legacy thread count) are ignored.
    }

    auto t0 = std::chrono::steady_clock::now();

    if (mode == "archive") {
        jdoc::convert_archive(path, [&](jdoc::MemberResult&& r) {
            count++;
            if (r.ok()) out_bytes += r.markdown.size();
            else errors++;
            if (hash_mode)
                print_hash_line(r.member_path, r.markdown, r.error_code, r.error);
            return true;
        }, opts);
    } else if (mode == "dir") {
        // Sorted: recursive_directory_iterator order is filesystem-dependent,
        // and --hash output is meant to be diffed across builds and machines.
        std::vector<fs::path> files;
        for (const auto& e : fs::recursive_directory_iterator(path)) {
            if (!e.is_regular_file()) continue;
            const auto& p = e.path();
            if (is_metadata_file(p) || has_skippable_ext(p))
                continue;
            files.push_back(p);
        }
        std::sort(files.begin(), files.end());
        for (const auto& p : files) {
            count++;
            std::string md;
            std::string err;
            try {
                md = jdoc::convert(p.string(), opts);
                out_bytes += md.size();
            } catch (const std::exception& e) {
                errors++;
                err = e.what();
            }
            if (hash_mode) {
                auto rel = fs::relative(p, path).generic_string();
                print_hash_line(rel, md,
                                err.empty() ? jdoc::MemberErrorCode::OK
                                            : jdoc::MemberErrorCode::CONVERT_FAILED,
                                err);
            }
        }
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode.c_str());
        return 1;
    }

    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    // The summary carries a wall time, so in --hash mode it goes to stderr to
    // keep stdout byte-identical across runs.
    fprintf(hash_mode ? stderr : stdout,
            "mode=%s files=%zu errors=%zu out_bytes=%llu elapsed=%.3fs\n",
            mode.c_str(), count, errors,
            static_cast<unsigned long long>(out_bytes), sec);
    return 0;
}
