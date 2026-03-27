// jdoc Python bindings via pybind11
// Exposes document conversion as Python module: import jdoc
// License: MIT

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "jdoc/jdoc.h"
#include "common/file_utils.h"

#include <cstring>

namespace py = pybind11;

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
        .def_readwrite("min_image_size", &jdoc::ConvertOptions::min_image_size)
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
                         const std::string& image_output_dir,
                         unsigned min_image_size) -> std::string {
        jdoc::ConvertOptions opts;
        opts.pages = pages;
        opts.extract_images = extract_images;
        opts.image_output_dir = image_output_dir;
        opts.min_image_size = min_image_size;
        if (format == "text" || format == "plaintext" || format == "plain")
            opts.output_format = jdoc::OutputFormat::PLAINTEXT;
        else
            opts.output_format = jdoc::OutputFormat::MARKDOWN;
        return jdoc::convert(file_path, opts);
    },
    py::arg("file_path"),
    py::arg("format") = "markdown",
    py::arg("pages") = std::vector<int>{},
    py::arg("extract_images") = false,
    py::arg("image_output_dir") = "",
    py::arg("min_image_size") = 50,
    R"doc(Convert a document file to text.

Args:
    file_path: Path to the document file (PDF, DOCX, XLSX, PPTX, DOC, XLS, PPT, RTF, HWP, HWPX)
    format: Output format - "markdown" (default) or "text"/"plaintext"
    pages: List of page numbers (0-based). Empty = all pages.
    extract_images: Whether to extract images
    image_output_dir: Directory to save extracted images (if empty, images kept in memory only)
    min_image_size: Skip images smaller than NxN pixels (default: 50, 0=no filter)

Returns:
    Converted text as string
)doc");

    m.def("convert_pages", [](const std::string& file_path,
                               const std::string& format,
                               const std::vector<int>& pages,
                               bool extract_images,
                               const std::string& image_output_dir,
                               unsigned min_image_size)
                               -> std::vector<jdoc::PageChunk> {
        jdoc::ConvertOptions opts;
        opts.pages = pages;
        opts.extract_images = extract_images;
        opts.image_output_dir = image_output_dir;
        opts.min_image_size = min_image_size;
        if (format == "text" || format == "plaintext" || format == "plain")
            opts.output_format = jdoc::OutputFormat::PLAINTEXT;
        else
            opts.output_format = jdoc::OutputFormat::MARKDOWN;
        return jdoc::convert_chunks(file_path, opts);
    },
    py::arg("file_path"),
    py::arg("format") = "markdown",
    py::arg("pages") = std::vector<int>{},
    py::arg("extract_images") = false,
    py::arg("image_output_dir") = "",
    py::arg("min_image_size") = 50,
    R"doc(Convert a document file to per-page chunks.

Args:
    file_path: Path to the document file
    format: Output format - "markdown" (default) or "text"/"plaintext"
    pages: List of page numbers (0-based). Empty = all pages.
    extract_images: Whether to extract images (accessible via chunk.images)
    image_output_dir: Directory to save images (if empty, images available via ImageData.data bytes)
    min_image_size: Skip images smaller than NxN pixels (default: 50, 0=no filter)

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
