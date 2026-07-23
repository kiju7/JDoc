// test_stream.cpp — Verify the streaming page iterator (for_each_chunk) is
// byte-identical to the eager convert_chunks path, and that early-stop works.
//
// Runs against test/fixtures/pdf/sample.pdf by default; pass any number of
// document paths as arguments to check additional formats (docx/xlsx/hwp/…).
#include "jdoc/jdoc.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

// Compare two PageChunks field-by-field. Returns "" on match, else a reason.
std::string diff_chunk(const jdoc::PageChunk& a, const jdoc::PageChunk& b) {
    if (a.page_number != b.page_number) return "page_number";
    if (a.text != b.text) return "text";
    if (a.page_width != b.page_width) return "page_width";
    if (a.page_height != b.page_height) return "page_height";
    if (a.body_font_size != b.body_font_size) return "body_font_size";
    if (a.tables != b.tables) return "tables";
    if (a.images.size() != b.images.size()) return "images.size";
    for (size_t i = 0; i < a.images.size(); ++i) {
        const auto& ia = a.images[i];
        const auto& ib = b.images[i];
        if (ia.name != ib.name || ia.width != ib.width || ia.height != ib.height ||
            ia.format != ib.format || ia.saved_path != ib.saved_path ||
            ia.data != ib.data || ia.pixels != ib.pixels ||
            ia.embedded_text != ib.embedded_text)
            return "image[" + std::to_string(i) + "]";
    }
    return "";
}

// Run the eager vs streaming comparison for one file with the given options.
bool check_equivalence(const std::string& path, const jdoc::ConvertOptions& opts,
                       const char* label) {
    std::vector<jdoc::PageChunk> eager = jdoc::convert_chunks(path, opts);

    std::vector<jdoc::PageChunk> streamed;
    jdoc::for_each_chunk(path, opts, [&](jdoc::PageChunk&& c) {
        streamed.push_back(std::move(c));
        return true;
    });

    if (eager.size() != streamed.size()) {
        std::cerr << "    FAIL [" << label << "]: page count eager="
                  << eager.size() << " streamed=" << streamed.size() << "\n";
        return false;
    }
    for (size_t i = 0; i < eager.size(); ++i) {
        std::string reason = diff_chunk(eager[i], streamed[i]);
        if (!reason.empty()) {
            std::cerr << "    FAIL [" << label << "]: page " << i
                      << " differs in " << reason << "\n";
            return false;
        }
    }
    std::cout << "    OK [" << label << "]: " << eager.size()
              << " pages byte-identical\n";
    return true;
}

// Verify that returning false from the sink stops the walk early.
bool check_early_stop(const std::string& path, const jdoc::ConvertOptions& opts) {
    size_t total = jdoc::convert_chunks(path, opts).size();
    if (total < 2) {
        std::cout << "    SKIP early-stop: only " << total << " page(s)\n";
        return true;
    }
    size_t seen = 0;
    jdoc::for_each_chunk(path, opts, [&](jdoc::PageChunk&&) {
        ++seen;
        return seen < 1;  // stop after the first page
    });
    if (seen != 1) {
        std::cerr << "    FAIL early-stop: expected 1 page, saw " << seen << "\n";
        return false;
    }
    std::cout << "    OK early-stop: stopped after 1 of " << total << " pages\n";
    return true;
}

bool run_file(const std::string& path) {
    std::cout << "[*] " << path << "\n";
    bool ok = true;

    jdoc::ConvertOptions md;  // defaults: markdown, tables on, images off
    ok &= check_equivalence(path, md, "markdown");

    jdoc::ConvertOptions plain;
    plain.format = jdoc::OutputFormat::PLAINTEXT;
    ok &= check_equivalence(path, plain, "plaintext");

    jdoc::ConvertOptions imgs;
    imgs.images = true;
    ok &= check_equivalence(path, imgs, "images");

    ok &= check_early_stop(path, md);
    return ok;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::cout << "=== jdoc Streaming Equivalence Test ===\n\n";

    std::vector<std::string> files;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) files.emplace_back(argv[i]);
    } else {
        files.emplace_back("test/fixtures/pdf/sample.pdf");
    }

    bool all_ok = true;
    bool ran_any = false;
    for (const auto& f : files) {
        if (!file_exists(f)) {
            std::cerr << "    SKIP (not found): " << f << "\n";
            continue;
        }
        ran_any = true;
        all_ok &= run_file(f);
    }

    if (!ran_any) {
        std::cerr << "No test fixtures found.\n";
        return 1;
    }
    if (!all_ok) {
        std::cerr << "\nFAILED\n";
        return 1;
    }
    std::cout << "\nAll streaming equivalence checks passed.\n";
    return 0;
}
