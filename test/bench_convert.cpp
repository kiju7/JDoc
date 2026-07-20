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
// License: MIT

#include "jdoc/jdoc.h"
#include "jdoc/archive.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>

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

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <archive|dir> <path> [threads]\n", argv[0]);
        return 1;
    }
    std::string mode = argv[1];
    std::string path = argv[2];

    size_t count = 0, errors = 0;
    uint64_t out_bytes = 0;
    jdoc::ConvertOptions opts;
    opts.archive.max_total_bytes = 16ull << 30;  // benchmark corpus can exceed the library default.
    if (argc > 3) opts.archive.threads = static_cast<uint32_t>(atoi(argv[3]));

    auto t0 = std::chrono::steady_clock::now();

    if (mode == "archive") {
        jdoc::convert_archive(path, [&](jdoc::MemberResult&& r) {
            count++;
            if (r.ok()) out_bytes += r.markdown.size();
            else errors++;
            return true;
        }, opts);
    } else if (mode == "dir") {
        for (const auto& e : fs::recursive_directory_iterator(path)) {
            if (!e.is_regular_file()) continue;
            const auto& p = e.path();
            if (is_metadata_file(p) || has_skippable_ext(p))
                continue;
            count++;
            try {
                out_bytes += jdoc::convert(p.string(), opts).size();
            } catch (const std::exception&) {
                errors++;
            }
        }
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode.c_str());
        return 1;
    }

    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    printf("mode=%s files=%zu errors=%zu out_bytes=%llu elapsed=%.3fs\n",
           mode.c_str(), count, errors,
           static_cast<unsigned long long>(out_bytes), sec);
    return 0;
}
