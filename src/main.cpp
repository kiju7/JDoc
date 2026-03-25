// jdoc CLI - Universal document to Markdown converter
// Usage: jdoc <input_file> [output.md] [--pages 0,1,2] [--no-tables] [--chunks] [--images DIR]
// Supports: PDF, DOCX, XLSX, PPTX, DOC, XLS, PPT, RTF, HWP, HWPX

#include "jdoc/pdf.h"
#include "jdoc/office.h"
#include "jdoc/hwpx.h"
#include "jdoc/hwp.h"
#include "zip_reader.h"
#include "legacy/ole_reader.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <input_file> [output.md] [options]\n"
              << "\nSupported formats:\n"
              << "  PDF, DOCX, XLSX, PPTX, DOC, XLS, PPT, RTF, HTML, HWP, HWPX\n"
              << "\nOptions:\n"
              << "  --pages N,N,N   Page numbers (0-based, comma-separated)\n"
              << "  --no-tables     Disable table detection\n"
              << "  --chunks        Output per-page/slide/sheet chunks\n"
              << "  --images DIR    Extract images to directory\n"
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

// ── Format detection by extension + magic bytes ─────────────

static std::string get_ext(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot);
    for (auto& c : ext) c = std::tolower(static_cast<unsigned char>(c));
    return ext;
}

enum class FileFormat { PDF, OFFICE, HWP, HWPX, UNKNOWN };

static FileFormat detect_format(const std::string& path) {
    // Magic bytes first — reliable regardless of file extension
    unsigned char magic[8] = {};
    std::ifstream f(path, std::ios::binary);
    if (f) {
        f.read(reinterpret_cast<char*>(magic), 8);
        f.close();

        if (magic[0] == '%' && magic[1] == 'P' && magic[2] == 'D' && magic[3] == 'F')
            return FileFormat::PDF;

        if (magic[0] == 'P' && magic[1] == 'K' && magic[2] == 0x03 && magic[3] == 0x04) {
            jdoc::ZipReader zip(path);
            if (zip.is_open()) {
                if (zip.has_entry("Contents/section0.xml") ||
                    zip.has_entry("META-INF/container.xml"))
                    return FileFormat::HWPX;
            }
            return FileFormat::OFFICE;
        }

        if (magic[0] == 0xD0 && magic[1] == 0xCF && magic[2] == 0x11 && magic[3] == 0xE0) {
            jdoc::OleReader ole(path);
            if (ole.is_open()) {
                if (ole.has_stream("FileHeader") || ole.has_stream("BodyText/Section0"))
                    return FileFormat::HWP;
            }
            return FileFormat::OFFICE;
        }

        if (magic[0] == '{' && magic[1] == '\\' && magic[2] == 'r' &&
            magic[3] == 't' && magic[4] == 'f')
            return FileFormat::OFFICE;

        if (magic[0] == '<')
            return FileFormat::OFFICE; // HTML
    }

    // Extension fallback — for files where magic bytes are ambiguous
    std::string ext = get_ext(path);
    if (ext == ".pdf") return FileFormat::PDF;
    if (ext == ".hwpx") return FileFormat::HWPX;
    if (ext == ".hwp") return FileFormat::HWP;
    if (ext == ".docx" || ext == ".xlsx" || ext == ".pptx" ||
        ext == ".doc" || ext == ".xls" || ext == ".ppt" || ext == ".rtf" ||
        ext == ".html" || ext == ".htm" || ext == ".xlsb")
        return FileFormat::OFFICE;

    return FileFormat::UNKNOWN;
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

    try {
        auto fmt = detect_format(input_path);
        if (fmt == FileFormat::UNKNOWN) {
            std::cerr << "Error: Unsupported file format: " << input_path << "\n";
            return 1;
        }

        if (chunk_mode) {
            std::vector<jdoc::PageChunk> chunks;

            switch (fmt) {
                case FileFormat::PDF:
                    chunks = jdoc::pdf_to_markdown_chunks(input_path, opts);
                    break;
                case FileFormat::HWPX:
                    chunks = jdoc::hwpx_to_markdown_chunks(input_path, opts);
                    break;
                case FileFormat::HWP:
                    chunks = jdoc::hwp_to_markdown_chunks(input_path, opts);
                    break;
                case FileFormat::OFFICE:
                    chunks = jdoc::office_to_markdown_chunks(input_path, opts);
                    break;
                default: break;
            }

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
            std::string md;

            switch (fmt) {
                case FileFormat::PDF:
                    md = jdoc::pdf_to_markdown(input_path, opts);
                    break;
                case FileFormat::HWPX:
                    md = jdoc::hwpx_to_markdown(input_path, opts);
                    break;
                case FileFormat::HWP:
                    md = jdoc::hwp_to_markdown(input_path, opts);
                    break;
                case FileFormat::OFFICE:
                    md = jdoc::office_to_markdown(input_path, opts);
                    break;
                default: break;
            }

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
