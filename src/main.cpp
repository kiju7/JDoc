// jdoc CLI - Universal document to Markdown converter
// Usage: jdoc <input_file> [output.md] [--pages 0,1,2] [--no-tables] [--chunks] [--images DIR]
// Supports: PDF, DOCX, XLSX, PPTX, DOC, XLS, PPT, RTF, HWP, HWPX

#include "jdoc/jdoc.h"
#include "jdoc/archive.h"
#include <cstdint>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <input_file> [output.md] [options]\n"
              << "\nSupported formats:\n"
              << "  PDF, DOCX, XLSX, PPTX, DOC, XLS, PPT, RTF, HTML, HWP, HWPX\n"
              << "  Archives (parsed in memory, no extraction): ZIP, GZ, BZ2, TAR, TAR.GZ, TAR.BZ2, 7Z, ALZ, EGG, RAR\n"
              << "\nOptions:\n"
              << "  --pages N,N,N   Page numbers (0-based, comma-separated)\n"
              << "  --no-tables     Disable table detection\n"
              << "  --chunks        Output per-page/slide/sheet chunks\n"
              << "  --images DIR    Extract images to directory\n"
              << "  --min-image-size N  Skip images smaller than NxN pixels (default: 50, 0=no filter)\n"
              << "  --format F      Output format: markdown (default) or text\n"
              << "\nArchive options:\n"
              << "  --max-depth N        Max nested-archive depth (default: 3, -1 = unlimited)\n"
              << "  --max-member-mb N    Per-member uncompressed cap in MiB (default: 512, -1 = unlimited)\n"
              << "  --max-total-mb N     Cumulative uncompressed cap in MiB (default: 65536, -1 = unlimited)\n"
              << "  --max-entries N      Max members visited (default: 200000, -1 = unlimited)\n"
              << "  --max-ratio N        Bomb-suspect compression ratio (default: 1000, 0 = off)\n"
              << "  --include-unsupported  Report unsupported members as errors\n"
              << "  --help          Show this help\n";
}

std::vector<int> parse_pages(const std::string& s) {
    std::vector<int> pages;
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        try { pages.push_back(std::stoi(token)); }
        catch (...) {}
    }
    return pages;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string input_path;
    std::string output_path;
    jdoc::ConvertOptions opts;
    bool chunk_mode = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        else if (arg == "--pages" && i + 1 < argc) {
            opts.pages = parse_pages(argv[++i]);
        }
        else if (arg == "--no-tables") {
            opts.tables = false;
        }
        else if (arg == "--chunks") {
            chunk_mode = true;
        }
        else if (arg == "--images" && i + 1 < argc) {
            opts.images = true;
            opts.image_dir = argv[++i];
        }
        else if (arg == "--min-image-size" && i + 1 < argc) {
            opts.min_image_size = static_cast<unsigned>(std::stoi(argv[++i]));
        }
        else if (arg == "--format" && i + 1 < argc) {
            std::string f = argv[++i];
            if (f == "text" || f == "plaintext")
                opts.format = jdoc::OutputFormat::PLAINTEXT;
        }
        else if (arg == "--plaintext" || arg == "--text") {  // alias of --format text
            opts.format = jdoc::OutputFormat::PLAINTEXT;
        }
        else if (arg == "--max-depth" && i + 1 < argc) {
            opts.archive.max_depth = std::stoi(argv[++i]);  // negative = unlimited
        }
        else if (arg == "--max-member-mb" && i + 1 < argc) {
            long long mb = std::stoll(argv[++i]);
            opts.archive.max_member_bytes =
                mb < 0 ? UINT64_MAX : static_cast<uint64_t>(mb) << 20;
        }
        else if (arg == "--max-total-mb" && i + 1 < argc) {
            long long mb = std::stoll(argv[++i]);
            opts.archive.max_total_bytes =
                mb < 0 ? UINT64_MAX : static_cast<uint64_t>(mb) << 20;
        }
        else if (arg == "--max-entries" && i + 1 < argc) {
            long long n = std::stoll(argv[++i]);
            opts.archive.max_entries =
                n < 0 ? UINT32_MAX : static_cast<uint32_t>(n);
        }
        else if (arg == "--max-ratio" && i + 1 < argc) {
            long long n = std::stoll(argv[++i]);
            opts.archive.max_ratio = n <= 0 ? 0 : static_cast<uint32_t>(n);
        }
        else if (arg == "--include-unsupported") {
            opts.archive.include_unsupported = true;
        }
        else if (input_path.empty()) {
            input_path = arg;
        }
        else if (output_path.empty()) {
            output_path = arg;
        }
    }

    if (input_path.empty()) {
        std::cerr << "Error: No input file specified.\n";
        return 1;
    }

    // Compute image reference prefix: relative path from md file dir to image dir
    if (opts.images && !opts.image_dir.empty()) {
        namespace fs = std::filesystem;
        fs::path img_dir = fs::weakly_canonical(fs::absolute(opts.image_dir));
        fs::path md_dir = output_path.empty()
            ? fs::current_path()
            : fs::weakly_canonical(fs::absolute(output_path)).parent_path();
        fs::path rel = fs::relative(img_dir, md_dir);
        if (rel != ".") {
            opts.image_ref_prefix = rel.string() + "/";
        }
    }

    try {
        if (jdoc::is_archive_file(input_path)) {
            std::ofstream ofs;
            std::ostream* out = &std::cout;
            if (!output_path.empty()) {
                ofs.open(output_path);
                if (!ofs) {
                    std::cerr << "Error: Cannot write to " << output_path << "\n";
                    return 1;
                }
                out = &ofs;
            }

            int failed = 0;
            jdoc::convert_archive(input_path, [&](jdoc::MemberResult&& r) {
                if (r.ok()) {
                    *out << "=== " << r.member_path << " ===\n"
                         << r.markdown << "\n\n";
                } else {
                    failed++;
                    *out << "=== " << r.member_path << " — ERROR: "
                         << r.error << " ===\n\n";
                }
                return true;
            }, opts);
            return failed > 0 ? 2 : 0;
        }

        if (chunk_mode) {
            auto chunks = jdoc::convert_chunks(input_path, opts);

            for (auto& chunk : chunks) {
                std::cout << "=== Page " << chunk.page_number
                          << " (" << chunk.page_width << "x" << chunk.page_height
                          << ", body=" << chunk.body_font_size << "pt) ===\n";
                std::cout << chunk.text << "\n\n";
                for (auto& img : chunk.images) {
                    if (!img.saved_path.empty())
                        std::cerr << img.name << " -> " << img.saved_path << "\n";
                }
            }
        } else {
            std::string md = jdoc::convert(input_path, opts);

            if (output_path.empty()) {
                std::cout << md << std::endl;
            } else {
                std::ofstream ofs(output_path);
                if (!ofs) {
                    std::cerr << "Error: Cannot write to " << output_path << "\n";
                    return 1;
                }
                ofs << md;
                ofs.close();
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
