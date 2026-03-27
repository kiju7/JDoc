// jdoc - Unified document converter
// Auto-detects format via magic bytes + extension fallback.

#include "jdoc/jdoc.h"
#include "jdoc/pdf.h"
#include "jdoc/office.h"
#include "jdoc/hwp.h"
#include "jdoc/hwpx.h"
#include "zip_reader.h"
#include "legacy/ole_reader.h"

#include <fstream>
#include <sstream>

namespace jdoc {
namespace {

enum class FileFormat { PDF, OFFICE, HWP, HWPX, TXT, UNKNOWN };

// Check if a file looks like valid UTF-8/ASCII text by sampling the first 8KB.
// Rejects files with NUL bytes or invalid UTF-8 sequences.
static bool is_likely_text(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    char buf[8192];
    f.read(buf, sizeof(buf));
    size_t n = static_cast<size_t>(f.gcount());
    if (n == 0) return true; // empty file is valid text

    auto u = reinterpret_cast<const uint8_t*>(buf);
    for (size_t i = 0; i < n; ) {
        uint8_t c = u[i];
        if (c == 0) return false; // NUL → binary
        if (c < 0x80) { i++; continue; } // ASCII

        // UTF-8 multibyte: validate continuation bytes
        int expect = (c < 0xE0) ? 1 : (c < 0xF0) ? 2 : (c < 0xF8) ? 3 : -1;
        if (expect < 0) return false;
        if (i + expect >= n) break; // truncated at buffer boundary, allow
        for (int j = 1; j <= expect; j++) {
            if ((u[i + j] & 0xC0) != 0x80) return false;
        }
        i += 1 + expect;
    }
    return true;
}

static FileFormat detect_format(const std::string& path) {
    // Magic bytes first
    unsigned char magic[8] = {};
    std::ifstream f(path, std::ios::binary);
    if (f) {
        f.read(reinterpret_cast<char*>(magic), 8);
        f.close();

        if (magic[0] == '%' && magic[1] == 'P' && magic[2] == 'D' && magic[3] == 'F')
            return FileFormat::PDF;

        if (magic[0] == 'P' && magic[1] == 'K' && magic[2] == 0x03 && magic[3] == 0x04) {
            ZipReader zip(path);
            if (zip.is_open()) {
                if (zip.has_entry("Contents/section0.xml") ||
                    zip.has_entry("META-INF/container.xml"))
                    return FileFormat::HWPX;
            }
            return FileFormat::OFFICE;
        }

        if (magic[0] == 0xD0 && magic[1] == 0xCF && magic[2] == 0x11 && magic[3] == 0xE0) {
            OleReader ole(path);
            if (ole.is_open()) {
                if (ole.has_stream("FileHeader") || ole.has_stream("BodyText/Section0"))
                    return FileFormat::HWP;
            }
            return FileFormat::OFFICE;
        }

        if (magic[0] == '{' && magic[1] == '\\' && magic[2] == 'r' &&
            magic[3] == 't' && magic[4] == 'f')
            return FileFormat::OFFICE;
    }

    // Extension — resolves ambiguous cases (e.g. .xml starts with '<' but is TXT)
    auto dot = path.rfind('.');
    if (dot != std::string::npos) {
        std::string ext = path.substr(dot);
        for (auto& c : ext) c = std::tolower(static_cast<unsigned char>(c));
        if (ext == ".pdf") return FileFormat::PDF;
        if (ext == ".hwpx") return FileFormat::HWPX;
        if (ext == ".hwp") return FileFormat::HWP;
        if (ext == ".docx" || ext == ".xlsx" || ext == ".pptx" ||
            ext == ".doc" || ext == ".xls" || ext == ".ppt" || ext == ".rtf" ||
            ext == ".html" || ext == ".htm" || ext == ".xlsb")
            return FileFormat::OFFICE;
        if (ext == ".txt" || ext == ".text" || ext == ".log" || ext == ".csv" ||
            ext == ".tsv" || ext == ".md" || ext == ".json" || ext == ".xml" ||
            ext == ".yml" || ext == ".yaml" || ext == ".ini" || ext == ".cfg" ||
            ext == ".conf" || ext == ".sh" || ext == ".bat" || ext == ".ps1" ||
            ext == ".py" || ext == ".js" || ext == ".ts" || ext == ".cpp" ||
            ext == ".c" || ext == ".h" || ext == ".hpp" || ext == ".java" ||
            ext == ".rs" || ext == ".go" || ext == ".rb" || ext == ".php" ||
            ext == ".sql" || ext == ".css" || ext == ".scss" || ext == ".less")
            return FileFormat::TXT;
    }

    // '<' fallback — extensionless HTML
    if (magic[0] == '<')
        return FileFormat::OFFICE;

    // UTF-8/ASCII heuristic — if content looks like valid text, treat as TXT
    if (is_likely_text(path))
        return FileFormat::TXT;

    return FileFormat::UNKNOWN;
}

static std::string read_text_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // anonymous namespace

std::string convert(const std::string& file_path, ConvertOptions opts) {
    auto fmt = detect_format(file_path);
    switch (fmt) {
        case FileFormat::PDF:    return pdf_to_markdown(file_path, opts);
        case FileFormat::OFFICE: return office_to_markdown(file_path, opts);
        case FileFormat::HWPX:   return hwpx_to_markdown(file_path, opts);
        case FileFormat::HWP:    return hwp_to_markdown(file_path, opts);
        case FileFormat::TXT:    return read_text_file(file_path);
        default:
            throw std::runtime_error("Unsupported file format: " + file_path);
    }
}

std::vector<PageChunk> convert_chunks(const std::string& file_path,
                                       ConvertOptions opts) {
    auto fmt = detect_format(file_path);
    switch (fmt) {
        case FileFormat::PDF:    return pdf_to_markdown_chunks(file_path, opts);
        case FileFormat::OFFICE: return office_to_markdown_chunks(file_path, opts);
        case FileFormat::HWPX:   return hwpx_to_markdown_chunks(file_path, opts);
        case FileFormat::HWP:    return hwp_to_markdown_chunks(file_path, opts);
        case FileFormat::TXT: {
            PageChunk chunk;
            chunk.page_number = 0;
            chunk.text = read_text_file(file_path);
            return {chunk};
        }
        default:
            throw std::runtime_error("Unsupported file format: " + file_path);
    }
}

} // namespace jdoc
