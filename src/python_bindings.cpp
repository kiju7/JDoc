// jdoc Python bindings via pybind11
// Exposes document conversion as Python module: import jdoc
// License: MIT

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "jdoc/types.h"
#include "jdoc/pdf.h"
#include "jdoc/office.h"
#include "jdoc/hwp.h"
#include "jdoc/hwpx.h"
#include "common/file_utils.h"
#include "zip_reader.h"
#include "legacy/ole_reader.h"

#include <fstream>
#include <algorithm>
#include <cstring>

namespace py = pybind11;

namespace {

// ── Format detection (same as main.cpp) ──────────────────

enum class FileFormat { PDF, OFFICE, HWP, HWPX, UNKNOWN };

static std::string get_ext(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot);
    for (auto& c : ext) c = std::tolower(static_cast<unsigned char>(c));
    return ext;
}

static FileFormat detect_format(const std::string& path) {
    std::string ext = get_ext(path);
    if (ext == ".pdf") return FileFormat::PDF;
    if (ext == ".hwpx") return FileFormat::HWPX;
    if (ext == ".hwp") return FileFormat::HWP;
    if (ext == ".docx" || ext == ".xlsx" || ext == ".pptx" ||
        ext == ".doc" || ext == ".xls" || ext == ".ppt" || ext == ".rtf" ||
        ext == ".html" || ext == ".htm")
        return FileFormat::OFFICE;

    unsigned char magic[8] = {};
    std::ifstream f(path, std::ios::binary);
    if (!f) return FileFormat::UNKNOWN;
    f.read(reinterpret_cast<char*>(magic), 8);

    if (magic[0] == '%' && magic[1] == 'P' && magic[2] == 'D' && magic[3] == 'F')
        return FileFormat::PDF;
    if (magic[0] == 'P' && magic[1] == 'K' && magic[2] == 0x03 && magic[3] == 0x04) {
        // ZIP: check for HWPX by inspecting internal structure
        jdoc::ZipReader zip(path);
        if (zip.is_open()) {
            if (zip.has_entry("Contents/section0.xml") ||
                zip.has_entry("META-INF/container.xml"))
                return FileFormat::HWPX;
        }
        return FileFormat::OFFICE;
    }
    if (magic[0] == 0xD0 && magic[1] == 0xCF && magic[2] == 0x11 && magic[3] == 0xE0) {
        // OLE2: check for HWP by inspecting internal streams
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

    return FileFormat::UNKNOWN;
}

// ── Unified convert function ─────────────────────────────

std::string convert_file(const std::string& path, jdoc::ConvertOptions& opts) {
    auto fmt = detect_format(path);
    switch (fmt) {
        case FileFormat::PDF:    return jdoc::pdf_to_markdown(path, opts);
        case FileFormat::HWPX:   return jdoc::hwpx_to_markdown(path, opts);
        case FileFormat::HWP:    return jdoc::hwp_to_markdown(path, opts);
        case FileFormat::OFFICE: return jdoc::office_to_markdown(path, opts);
        default:
            throw std::runtime_error("Unsupported file format: " + path);
    }
}

std::vector<jdoc::PageChunk> convert_file_chunks(const std::string& path,
                                                   jdoc::ConvertOptions& opts) {
    auto fmt = detect_format(path);
    switch (fmt) {
        case FileFormat::PDF:    return jdoc::pdf_to_markdown_chunks(path, opts);
        case FileFormat::HWPX:   return jdoc::hwpx_to_markdown_chunks(path, opts);
        case FileFormat::HWP:    return jdoc::hwp_to_markdown_chunks(path, opts);
        case FileFormat::OFFICE: return jdoc::office_to_markdown_chunks(path, opts);
        default:
            throw std::runtime_error("Unsupported file format: " + path);
    }
}

} // anonymous namespace

// ── Python module definition ─────────────────────────────

PYBIND11_MODULE(_jdoc, m) {
    m.doc() = "JDoc - Universal document converter (PDF, Office, HTML, HWP/HWPX)";

    // OutputFormat enum
    py::enum_<jdoc::OutputFormat>(m, "OutputFormat")
        .value("MARKDOWN", jdoc::OutputFormat::MARKDOWN)
        .value("PLAINTEXT", jdoc::OutputFormat::PLAINTEXT)
        .export_values();

    // ImageData
    py::class_<jdoc::ImageData>(m, "ImageData")
        .def(py::init<>())
        .def_readwrite("page_number", &jdoc::ImageData::page_number)
        .def_readwrite("name", &jdoc::ImageData::name)
        .def_readwrite("width", &jdoc::ImageData::width)
        .def_readwrite("height", &jdoc::ImageData::height)
        .def_readwrite("format", &jdoc::ImageData::format)
        .def_readwrite("saved_path", &jdoc::ImageData::saved_path)
        .def_property("data",
            // getter: return image data as Python bytes
            [](const jdoc::ImageData& img) -> py::bytes {
                return py::bytes(img.data.data(), img.data.size());
            },
            // setter: accept Python bytes
            [](jdoc::ImageData& img, const std::string& val) {
                img.data.assign(val.begin(), val.end());
            })
        .def("__repr__", [](const jdoc::ImageData& img) {
            return "<ImageData '" + img.name + "' " +
                   std::to_string(img.width) + "x" + std::to_string(img.height) +
                   " " + img.format + ">";
        });

    // PageChunk
    py::class_<jdoc::PageChunk>(m, "PageChunk")
        .def(py::init<>())
        .def_readwrite("page_number", &jdoc::PageChunk::page_number)
        .def_readwrite("text", &jdoc::PageChunk::text)
        .def_readwrite("page_width", &jdoc::PageChunk::page_width)
        .def_readwrite("page_height", &jdoc::PageChunk::page_height)
        .def_readwrite("body_font_size", &jdoc::PageChunk::body_font_size)
        .def_readwrite("tables", &jdoc::PageChunk::tables)
        .def_readwrite("images", &jdoc::PageChunk::images)
        .def("__repr__", [](const jdoc::PageChunk& c) {
            return "<PageChunk page=" + std::to_string(c.page_number) +
                   " text_len=" + std::to_string(c.text.size()) +
                   " images=" + std::to_string(c.images.size()) + ">";
        });

    // ConvertOptions
    py::class_<jdoc::ConvertOptions>(m, "ConvertOptions")
        .def(py::init<>())
        .def_readwrite("pages", &jdoc::ConvertOptions::pages)
        .def_readwrite("extract_tables", &jdoc::ConvertOptions::extract_tables)
        .def_readwrite("extract_images", &jdoc::ConvertOptions::extract_images)
        .def_readwrite("image_output_dir", &jdoc::ConvertOptions::image_output_dir)
        .def_readwrite("output_format", &jdoc::ConvertOptions::output_format);

    // DocFormat enum
    py::enum_<jdoc::DocFormat>(m, "DocFormat")
        .value("UNKNOWN", jdoc::DocFormat::UNKNOWN)
        .value("DOCX", jdoc::DocFormat::DOCX)
        .value("XLSX", jdoc::DocFormat::XLSX)
        .value("PPTX", jdoc::DocFormat::PPTX)
        .value("DOC", jdoc::DocFormat::DOC)
        .value("XLS", jdoc::DocFormat::XLS)
        .value("PPT", jdoc::DocFormat::PPT)
        .value("RTF", jdoc::DocFormat::RTF)
        .value("HTML", jdoc::DocFormat::HTML)
        .export_values();

    // ── Top-level convenience functions ──────────────────

    m.def("convert", [](const std::string& file_path,
                         const std::string& format,
                         const std::vector<int>& pages,
                         bool extract_images,
                         const std::string& image_output_dir) -> std::string {
        jdoc::ConvertOptions opts;
        opts.pages = pages;
        opts.extract_images = extract_images;
        opts.image_output_dir = image_output_dir;
        if (format == "text" || format == "plaintext" || format == "plain")
            opts.output_format = jdoc::OutputFormat::PLAINTEXT;
        else
            opts.output_format = jdoc::OutputFormat::MARKDOWN;
        return convert_file(file_path, opts);
    },
    py::arg("file_path"),
    py::arg("format") = "markdown",
    py::arg("pages") = std::vector<int>{},
    py::arg("extract_images") = false,
    py::arg("image_output_dir") = "",
    R"doc(Convert a document file to text.

Args:
    file_path: Path to the document file (PDF, DOCX, XLSX, PPTX, DOC, XLS, PPT, RTF, HWP, HWPX)
    format: Output format - "markdown" (default) or "text"/"plaintext"
    pages: List of page numbers (0-based). Empty = all pages.
    extract_images: Whether to extract images
    image_output_dir: Directory to save extracted images (if empty, images kept in memory only)

Returns:
    Converted text as string
)doc");

    m.def("convert_pages", [](const std::string& file_path,
                               const std::string& format,
                               const std::vector<int>& pages,
                               bool extract_images,
                               const std::string& image_output_dir)
                               -> std::vector<jdoc::PageChunk> {
        jdoc::ConvertOptions opts;
        opts.pages = pages;
        opts.extract_images = extract_images;
        opts.image_output_dir = image_output_dir;
        if (format == "text" || format == "plaintext" || format == "plain")
            opts.output_format = jdoc::OutputFormat::PLAINTEXT;
        else
            opts.output_format = jdoc::OutputFormat::MARKDOWN;
        return convert_file_chunks(file_path, opts);
    },
    py::arg("file_path"),
    py::arg("format") = "markdown",
    py::arg("pages") = std::vector<int>{},
    py::arg("extract_images") = false,
    py::arg("image_output_dir") = "",
    R"doc(Convert a document file to per-page chunks.

Args:
    file_path: Path to the document file
    format: Output format - "markdown" (default) or "text"/"plaintext"
    pages: List of page numbers (0-based). Empty = all pages.
    extract_images: Whether to extract images (accessible via chunk.images)
    image_output_dir: Directory to save images (if empty, images available via ImageData.data bytes)

Returns:
    List of PageChunk objects, one per page/slide/sheet
)doc");

    // ── Per-format functions (for advanced usage) ────────

    m.def("pdf_to_markdown", &jdoc::pdf_to_markdown,
          py::arg("pdf_path"), py::arg("opts") = jdoc::ConvertOptions{});
    m.def("pdf_to_markdown_chunks", &jdoc::pdf_to_markdown_chunks,
          py::arg("pdf_path"), py::arg("opts") = jdoc::ConvertOptions{});

    m.def("office_to_markdown", &jdoc::office_to_markdown,
          py::arg("file_path"), py::arg("opts") = jdoc::ConvertOptions{});
    m.def("office_to_markdown_chunks", &jdoc::office_to_markdown_chunks,
          py::arg("file_path"), py::arg("opts") = jdoc::ConvertOptions{});

    m.def("hwp_to_markdown", &jdoc::hwp_to_markdown,
          py::arg("hwp_path"), py::arg("opts") = jdoc::ConvertOptions{});
    m.def("hwp_to_markdown_chunks", &jdoc::hwp_to_markdown_chunks,
          py::arg("hwp_path"), py::arg("opts") = jdoc::ConvertOptions{});

    m.def("hwpx_to_markdown", &jdoc::hwpx_to_markdown,
          py::arg("hwpx_path"), py::arg("opts") = jdoc::ConvertOptions{});
    m.def("hwpx_to_markdown_chunks", &jdoc::hwpx_to_markdown_chunks,
          py::arg("hwpx_path"), py::arg("opts") = jdoc::ConvertOptions{});

    m.def("detect_office_format", &jdoc::detect_office_format,
          py::arg("file_path"));
    m.def("format_name", &jdoc::format_name,
          py::arg("fmt"));
}
