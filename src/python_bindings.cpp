// jdoc Python bindings via pybind11
// Exposes document conversion as Python module: import jdoc
// License: MIT

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>

#include "jdoc/jdoc.h"
#include "jdoc/archive.h"
#include "jdoc/pdf.h"
#include "jdoc/office.h"
#include "jdoc/hwp.h"
#include "jdoc/hwpx.h"
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
        .value("XLSB", jdoc::DocFormat::XLSB)
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

    // ── Archive conversion ────────────────────────────────

    py::enum_<jdoc::MemberErrorCode>(m, "MemberErrorCode",
        "Machine-readable member failure classification. The *_LIMIT values "
        "name the convert_archive() limit argument to raise; the remaining "
        "values are input problems no limit change can fix.")
        .value("OK", jdoc::MemberErrorCode::OK)
        .value("MEMBER_LIMIT", jdoc::MemberErrorCode::MEMBER_LIMIT)
        .value("RATIO_LIMIT", jdoc::MemberErrorCode::RATIO_LIMIT)
        .value("TOTAL_LIMIT", jdoc::MemberErrorCode::TOTAL_LIMIT)
        .value("ENTRY_LIMIT", jdoc::MemberErrorCode::ENTRY_LIMIT)
        .value("DEPTH_LIMIT", jdoc::MemberErrorCode::DEPTH_LIMIT)
        .value("ENCRYPTED", jdoc::MemberErrorCode::ENCRYPTED)
        .value("UNSUPPORTED", jdoc::MemberErrorCode::UNSUPPORTED)
        .value("CORRUPT", jdoc::MemberErrorCode::CORRUPT)
        .value("CONVERT_FAILED", jdoc::MemberErrorCode::CONVERT_FAILED);

    py::class_<jdoc::MemberResult>(m, "MemberResult")
        .def(py::init<>())
        .def_readwrite("member_path", &jdoc::MemberResult::member_path)
        .def_readwrite("format", &jdoc::MemberResult::format)
        .def_readwrite("markdown", &jdoc::MemberResult::markdown)
        .def_readwrite("error", &jdoc::MemberResult::error)
        .def_readwrite("error_code", &jdoc::MemberResult::error_code)
        .def_readwrite("uncompressed_size", &jdoc::MemberResult::uncompressed_size)
        .def_property_readonly("ok", &jdoc::MemberResult::ok)
        .def("__repr__", [](const jdoc::MemberResult& r) {
            return "<MemberResult '" + r.member_path + "' (" + r.format + ") " +
                   (r.ok() ? "ok" : "error: " + r.error) + ">";
        });

    m.def("convert_archive", [](const std::string& file_path,
                                 const std::string& format,
                                 int max_depth,
                                 long long max_member_bytes,
                                 long long max_total_bytes,
                                 long long max_entries,
                                 bool include_unsupported,
                                 int threads)
                                 -> std::vector<jdoc::MemberResult> {
        jdoc::ConvertOptions opts;
        if (format == "text" || format == "plaintext" || format == "plain")
            opts.output_format = jdoc::OutputFormat::PLAINTEXT;
        // -1 (any negative) disables the corresponding guard.
        opts.archive.max_depth = max_depth;
        opts.archive.max_member_bytes =
            max_member_bytes < 0 ? UINT64_MAX : (uint64_t)max_member_bytes;
        opts.archive.max_total_bytes =
            max_total_bytes < 0 ? UINT64_MAX : (uint64_t)max_total_bytes;
        opts.archive.max_entries =
            max_entries < 0 ? UINT32_MAX : (uint32_t)max_entries;
        opts.archive.include_unsupported = include_unsupported;
        // 1 = single-threaded (default), 0 = all cores, N = N workers.
        opts.archive.threads = threads < 0 ? 0 : (uint32_t)threads;
        return jdoc::convert_archive(file_path, opts);
    },
    py::arg("file_path"),
    py::arg("format") = "markdown",
    py::arg("max_depth") = 3,
    py::arg("max_member_bytes") = (long long)(512) << 20,
    py::arg("max_total_bytes") = (long long)(64) << 30,
    py::arg("max_entries") = 200000,
    py::arg("include_unsupported") = false,
    py::arg("threads") = 1,
    R"doc(Convert every supported document inside an archive (zip/gz/tar/tar.gz/
7z/alz/egg) without extracting to disk. Members are decompressed into memory
one at a time; nested archives are walked recursively up to max_depth.

Limits: pass -1 to disable a limit (unlimited). Only do this for trusted
inputs — archive-bomb protection is disabled with it.

threads: conversion worker threads (default 1 = fully single-threaded,
0 = all cores). Decoding stays on the calling thread; results keep walk
order regardless of thread count.

Returns:
    List of MemberResult (member_path, format, markdown, error, ok)
)doc");

    m.def("is_archive", &jdoc::is_archive_file, py::arg("file_path"),
          "True if the file is an archive container rather than a document.");

    m.def("convert_bytes", [](py::bytes data, const std::string& name_hint,
                               const std::string& format) -> std::string {
        jdoc::ConvertOptions opts;
        if (format == "text" || format == "plaintext" || format == "plain")
            opts.output_format = jdoc::OutputFormat::PLAINTEXT;
        std::string buf = data;  // copy out of the Python object
        return jdoc::convert(buf.data(), buf.size(), name_hint, opts);
    },
    py::arg("data"), py::arg("name_hint") = "", py::arg("format") = "markdown",
    "Convert a document held in bytes (no file I/O).");

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
