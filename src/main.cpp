// jdoc CLI - Universal document to Markdown converter
// Usage: jdoc <input_file> [output.md] [--pages 0,1,2] [--no-tables] [--chunks] [--images DIR]
// Supports: PDF, DOCX, XLSX, PPTX, DOC, XLS, PPT, RTF, HWP, HWPX

#include "jdoc/jdoc.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <input_file> [output.md] [options]\n"
              << "\nSupported formats:\n"
              << "  PDF, DOCX, XLSX, PPTX, DOC, XLS, PPT, RTF, HTML, HWP, HWPX\n"
              << "\nOptions:\n"
              << "  --pages N,N,N   Page numbers (0-based, comma-separated)\n"
              << "  --no-tables     Disable table detection\n"
              << "  --chunks        Output per-page/slide/sheet chunks\n"
              << "  --images DIR    Extract images to directory\n"
              << "  --min-image-size N  Skip images smaller than NxN pixels (default: 50, 0=no filter)\n"
              << "  --plaintext     Output plain text instead of Markdown\n"
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
            opts.extract_tables = false;
        }
        else if (arg == "--chunks") {
            chunk_mode = true;
        }
        else if (arg == "--images" && i + 1 < argc) {
            opts.extract_images = true;
            opts.image_output_dir = argv[++i];
        }
        else if (arg == "--min-image-size" && i + 1 < argc) {
            opts.min_image_size = static_cast<unsigned>(std::stoi(argv[++i]));
        }
        else if (arg == "--plaintext" || arg == "--text") {
            opts.output_format = jdoc::OutputFormat::PLAINTEXT;
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
    if (opts.extract_images && !opts.image_output_dir.empty()) {
        namespace fs = std::filesystem;
        fs::path img_dir = fs::weakly_canonical(fs::absolute(opts.image_output_dir));
        fs::path md_dir = output_path.empty()
            ? fs::current_path()
            : fs::weakly_canonical(fs::absolute(output_path)).parent_path();
        fs::path rel = fs::relative(img_dir, md_dir);
        if (rel != ".") {
            opts.image_ref_prefix = rel.string() + "/";
        }
    }

    try {
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
