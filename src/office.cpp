// doc2md.cpp - Office document to Markdown converter (format dispatch)
// License: MIT

#include "jdoc/office.h"
#include "common/file_utils.h"
#include "zip_reader.h"
#include "ooxml/docx_parser.h"
#include "ooxml/pptx_parser.h"
#include "ooxml/xlsx_parser.h"
#include "ooxml/xlsb_parser.h"
#include "legacy/ole_reader.h"
#include "legacy/doc_parser.h"
#include "legacy/xls_parser.h"
#include "legacy/ppt_parser.h"
#include "legacy/rtf_parser.h"
#include "html/html_parser.h"

#include <fstream>
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace jdoc {

// ── Format Detection ────────────────────────────────────

static std::string lowercase_ext(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot);
    for (auto& c : ext) c = std::tolower(static_cast<unsigned char>(c));
    return ext;
}

DocFormat detect_office_format(const std::string& file_path) {
    // Read first 8 bytes for magic detection
    unsigned char magic[8] = {};
    {
        std::ifstream f(file_path, std::ios::binary);
        if (!f) return DocFormat::UNKNOWN;
        f.read(reinterpret_cast<char*>(magic), 8);
    }

    // Check extension first
    std::string ext = lowercase_ext(file_path);

    // ZIP magic: PK\x03\x04
    static const unsigned char ZIP_MAGIC[] = {0x50, 0x4B, 0x03, 0x04};
    if (memcmp(magic, ZIP_MAGIC, 4) == 0) {
        // OOXML — determine specific type from extension or internal content
        if (ext == ".docx") return DocFormat::DOCX;
        if (ext == ".xlsx") return DocFormat::XLSX;
        if (ext == ".xlsb") return DocFormat::XLSB;
        if (ext == ".pptx") return DocFormat::PPTX;

        // Fallback: inspect [Content_Types].xml
        ZipReader zip(file_path);
        if (zip.is_open()) {
            auto ct = zip.read_entry("[Content_Types].xml");
            if (!ct.empty()) {
                std::string content(ct.begin(), ct.end());
                if (content.find("wordprocessingml") != std::string::npos)
                    return DocFormat::DOCX;
                if (content.find("spreadsheetml") != std::string::npos) {
                    // Check if XLSB by looking for .bin worksheets
                    if (content.find("binaryFormat") != std::string::npos ||
                        zip.has_entry("xl/workbook.bin"))
                        return DocFormat::XLSB;
                    return DocFormat::XLSX;
                }
                if (content.find("presentationml") != std::string::npos)
                    return DocFormat::PPTX;
            }
        }
        return DocFormat::UNKNOWN;
    }

    // OLE magic: D0 CF 11 E0 A1 B1 1A E1
    static const unsigned char OLE_MAGIC[] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    if (memcmp(magic, OLE_MAGIC, 8) == 0) {
        // Determine type from extension first (skip .hwp — handled by main)
        if (ext == ".doc") return DocFormat::DOC;
        if (ext == ".xls") return DocFormat::XLS;
        if (ext == ".ppt") return DocFormat::PPT;

        // Fallback: inspect internal streams
        OleReader ole(file_path);
        if (ole.is_open()) {
            // HWP check: skip it here, let the main dispatcher handle HWP
            if (ole.has_stream("FileHeader")) return DocFormat::UNKNOWN;
            if (ole.has_stream("WordDocument")) return DocFormat::DOC;
            if (ole.has_stream("Workbook") || ole.has_stream("Book"))
                return DocFormat::XLS;
            if (ole.has_stream("PowerPoint Document")) return DocFormat::PPT;
        }
        return DocFormat::UNKNOWN;
    }

    // RTF magic: {\rtf
    if (magic[0] == '{' && magic[1] == '\\' && magic[2] == 'r' &&
        magic[3] == 't' && magic[4] == 'f') {
        return DocFormat::RTF;
    }

    // HTML detection: check extension or content heuristic
    if (ext == ".html" || ext == ".htm") return DocFormat::HTML;

    // Content-based HTML detection: look for <html or <!DOCTYPE
    if (magic[0] == '<') {
        // Read more bytes for detection
        std::ifstream f(file_path, std::ios::binary);
        if (f) {
            char buf[256] = {};
            f.read(buf, sizeof(buf));
            std::string head(buf, f.gcount());
            // Lowercase for comparison
            std::string lower;
            for (char c : head)
                lower += std::tolower(static_cast<unsigned char>(c));
            if (lower.find("<!doctype html") != std::string::npos ||
                lower.find("<html") != std::string::npos)
                return DocFormat::HTML;
        }
    }

    // Extension-only fallback
    if (ext == ".docx") return DocFormat::DOCX;
    if (ext == ".xlsx") return DocFormat::XLSX;
    if (ext == ".xlsb") return DocFormat::XLSB;
    if (ext == ".pptx") return DocFormat::PPTX;
    if (ext == ".doc")  return DocFormat::DOC;
    if (ext == ".xls")  return DocFormat::XLS;
    if (ext == ".ppt")  return DocFormat::PPT;
    if (ext == ".rtf")  return DocFormat::RTF;

    return DocFormat::UNKNOWN;
}

const char* format_name(DocFormat fmt) {
    switch (fmt) {
        case DocFormat::DOCX: return "DOCX";
        case DocFormat::XLSX: return "XLSX";
        case DocFormat::XLSB: return "XLSB";
        case DocFormat::PPTX: return "PPTX";
        case DocFormat::DOC:  return "DOC";
        case DocFormat::XLS:  return "XLS";
        case DocFormat::PPT:  return "PPT";
        case DocFormat::RTF:  return "RTF";
        case DocFormat::HTML: return "HTML";
        default: return "UNKNOWN";
    }
}

// ── Public API ──────────────────────────────────────────

// Section separator label for each format
static const char* section_label(DocFormat fmt) {
    switch (fmt) {
    case DocFormat::PPTX: case DocFormat::PPT: return "Slide";
    case DocFormat::XLSX: case DocFormat::XLS: case DocFormat::XLSB: return "Sheet";
    default: return "Page";
    }
}

std::string office_to_markdown(const std::string& file_path, ConvertOptions opts) {
    auto format = detect_office_format(file_path);

    if (opts.output_format == OutputFormat::PLAINTEXT) {
        auto chunks = office_to_markdown_chunks(file_path, opts);
        if (chunks.size() <= 1 && !chunks.empty())
            return chunks[0].text;
        const char* label = section_label(format);
        std::string result;
        for (size_t i = 0; i < chunks.size(); ++i) {
            if (i > 0)
                result += "\n--- " + std::string(label) + " " + std::to_string(chunks[i].page_number) + " ---\n\n";
            result += chunks[i].text;
        }
        return result;
    }

    std::string md;
    switch (format) {
    case DocFormat::DOCX: {
        ZipReader zip(file_path);
        if (!zip.is_open()) throw std::runtime_error("Cannot open DOCX file: " + file_path);
        DocxParser parser(zip);
        md = parser.to_markdown(opts);
        break;
    }
    case DocFormat::PPTX: {
        ZipReader zip(file_path);
        if (!zip.is_open()) throw std::runtime_error("Cannot open PPTX file: " + file_path);
        PptxParser parser(zip);
        md = parser.to_markdown(opts);
        break;
    }
    case DocFormat::XLSX: {
        ZipReader zip(file_path);
        if (!zip.is_open()) throw std::runtime_error("Cannot open XLSX file: " + file_path);
        XlsxParser parser(zip);
        md = parser.to_markdown(opts);
        break;
    }
    case DocFormat::XLSB: {
        ZipReader zip(file_path);
        if (!zip.is_open()) throw std::runtime_error("Cannot open XLSB file: " + file_path);
        XlsbParser parser(zip);
        md = parser.to_markdown(opts);
        break;
    }
    case DocFormat::DOC: {
        OleReader ole(file_path);
        if (!ole.is_open()) throw std::runtime_error("Cannot open DOC file: " + file_path);
        DocParser parser(ole);
        md = parser.to_markdown(opts);
        break;
    }
    case DocFormat::XLS: {
        OleReader ole(file_path);
        if (!ole.is_open()) throw std::runtime_error("Cannot open XLS file: " + file_path);
        XlsParser parser(ole);
        md = parser.to_markdown(opts);
        break;
    }
    case DocFormat::PPT: {
        OleReader ole(file_path);
        if (!ole.is_open()) throw std::runtime_error("Cannot open PPT file: " + file_path);
        PptParser parser(ole);
        md = parser.to_markdown(opts);
        break;
    }
    case DocFormat::RTF: {
        RtfParser parser(file_path);
        md = parser.to_markdown(opts);
        break;
    }
    case DocFormat::HTML: {
        HtmlParser parser(file_path);
        md = parser.to_markdown(opts);
        break;
    }
    default:
        throw std::runtime_error("Unsupported document format: " + file_path);
    }
    return md;
}

std::vector<PageChunk> office_to_markdown_chunks(const std::string& file_path,
                                                    ConvertOptions opts) {
    auto format = detect_office_format(file_path);
    std::vector<PageChunk> chunks;

    switch (format) {
    case DocFormat::DOCX: {
        ZipReader zip(file_path);
        if (!zip.is_open()) throw std::runtime_error("Cannot open DOCX file: " + file_path);
        DocxParser parser(zip);
        chunks = parser.to_chunks(opts);
        break;
    }
    case DocFormat::PPTX: {
        ZipReader zip(file_path);
        if (!zip.is_open()) throw std::runtime_error("Cannot open PPTX file: " + file_path);
        PptxParser parser(zip);
        chunks = parser.to_chunks(opts);
        break;
    }
    case DocFormat::XLSX: {
        ZipReader zip(file_path);
        if (!zip.is_open()) throw std::runtime_error("Cannot open XLSX file: " + file_path);
        XlsxParser parser(zip);
        chunks = parser.to_chunks(opts);
        break;
    }
    case DocFormat::XLSB: {
        ZipReader zip(file_path);
        if (!zip.is_open()) throw std::runtime_error("Cannot open XLSB file: " + file_path);
        XlsbParser parser(zip);
        chunks = parser.to_chunks(opts);
        break;
    }
    case DocFormat::DOC: {
        OleReader ole(file_path);
        if (!ole.is_open()) throw std::runtime_error("Cannot open DOC file: " + file_path);
        DocParser parser(ole);
        chunks = parser.to_chunks(opts);
        break;
    }
    case DocFormat::XLS: {
        OleReader ole(file_path);
        if (!ole.is_open()) throw std::runtime_error("Cannot open XLS file: " + file_path);
        XlsParser parser(ole);
        chunks = parser.to_chunks(opts);
        break;
    }
    case DocFormat::PPT: {
        OleReader ole(file_path);
        if (!ole.is_open()) throw std::runtime_error("Cannot open PPT file: " + file_path);
        PptParser parser(ole);
        chunks = parser.to_chunks(opts);
        break;
    }
    case DocFormat::RTF: {
        RtfParser parser(file_path);
        chunks = parser.to_chunks(opts);
        break;
    }
    case DocFormat::HTML: {
        HtmlParser parser(file_path);
        chunks = parser.to_chunks(opts);
        break;
    }
    default:
        throw std::runtime_error("Unsupported document format: " + file_path);
    }

    if (opts.output_format == OutputFormat::PLAINTEXT) {
        for (auto& chunk : chunks)
            chunk.text = util::strip_markdown(chunk.text);
    }
    return chunks;
}

} // namespace jdoc
