// jdoc Python bindings via pybind11
// Exposes document conversion as Python module: import jdoc
// License: MIT

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>

#include "jdoc/jdoc.h"
#include "jdoc/archive.h"
#include "jdoc/detect.h"
#include "jdoc/pdf.h"
#include "jdoc/office.h"
#include "jdoc/hwp.h"
#include "jdoc/hwpx.h"
#include "common/file_utils.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace py = pybind11;

// Lazy page iterator backing convert_pages_stream(). A background thread runs
// the C++ core's for_each_chunk with the GIL released, pushing each page into a
// bounded queue; __next__ pops from it (releasing the GIL while it waits). The
// bound caps producer/consumer skew so peak memory tracks a few pages, not the
// whole document. The sink stays in pure C++ (no Python), so no GIL juggling is
// needed on the producer side.
class PageStream {
public:
    PageStream(std::string path, jdoc::ConvertOptions opts, size_t capacity)
        : path_(std::move(path)), opts_(std::move(opts)), capacity_(capacity) {
        worker_ = std::thread([this] { run(); });
    }

    ~PageStream() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stop_ = true;
        }
        not_full_.notify_all();
        not_empty_.notify_all();
        if (worker_.joinable()) {
            py::gil_scoped_release release;  // producer needs no GIL; avoid any stall
            worker_.join();
        }
    }

    // Returns the next page, or raises StopIteration when exhausted.
    jdoc::PageChunk next() {
        jdoc::PageChunk item;
        bool have_item = false;
        std::string error;
        {
            py::gil_scoped_release release;
            std::unique_lock<std::mutex> lk(mutex_);
            not_empty_.wait(lk, [this] { return !queue_.empty() || done_; });
            if (!queue_.empty()) {
                item = std::move(queue_.front());
                queue_.pop_front();
                have_item = true;
                lk.unlock();
                not_full_.notify_one();
            } else {
                error = error_;  // done_ and queue drained
            }
        }
        if (have_item) return item;
        if (!error.empty()) throw std::runtime_error(error);
        throw py::stop_iteration();
    }

private:
    void run() {
        try {
            jdoc::for_each_chunk(path_, opts_, [this](jdoc::PageChunk&& chunk) {
                std::unique_lock<std::mutex> lk(mutex_);
                not_full_.wait(lk, [this] { return queue_.size() < capacity_ || stop_; });
                if (stop_) return false;
                queue_.push_back(std::move(chunk));
                lk.unlock();
                not_empty_.notify_one();
                return true;
            });
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(mutex_);
            error_ = e.what();
        } catch (...) {
            std::lock_guard<std::mutex> lk(mutex_);
            error_ = "unknown error";
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            done_ = true;
        }
        not_empty_.notify_all();
    }

    std::string path_;
    jdoc::ConvertOptions opts_;
    size_t capacity_;

    std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<jdoc::PageChunk> queue_;
    std::string error_;
    bool done_ = false;
    bool stop_ = false;
    std::thread worker_;
};

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
        .def_readwrite("embedded_text", &jdoc::ImageData::embedded_text)
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
        .def_readwrite("tables", &jdoc::ConvertOptions::tables)
        .def_readwrite("images", &jdoc::ConvertOptions::images)
        .def_readwrite("image_dir", &jdoc::ConvertOptions::image_dir)
        .def_readwrite("min_image_size", &jdoc::ConvertOptions::min_image_size)
        .def_readwrite("format", &jdoc::ConvertOptions::format);

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
        .value("ENCRYPTED_PASSWORD", jdoc::DocFormat::ENCRYPTED_PASSWORD)
        .value("ENCRYPTED_RIGHTS", jdoc::DocFormat::ENCRYPTED_RIGHTS)
        .export_values();

    // FormatCategory enum (detect API)
    py::enum_<jdoc::FormatCategory>(m, "FormatCategory")
        .value("DOCUMENT", jdoc::FormatCategory::Document)
        .value("SPREADSHEET", jdoc::FormatCategory::Spreadsheet)
        .value("PRESENTATION", jdoc::FormatCategory::Presentation)
        .value("ARCHIVE", jdoc::FormatCategory::Archive)
        .value("EMAIL", jdoc::FormatCategory::Email)
        .value("TEXT", jdoc::FormatCategory::Text)
        .value("IMAGE", jdoc::FormatCategory::Image)
        .value("UNKNOWN", jdoc::FormatCategory::Unknown)
        .export_values();

    // FormatInfo (detect API result)
    py::class_<jdoc::FormatInfo>(m, "FormatInfo")
        .def(py::init<>())
        .def_readwrite("format", &jdoc::FormatInfo::format)
        .def_readwrite("category", &jdoc::FormatInfo::category)
        .def_readwrite("extension", &jdoc::FormatInfo::extension)
        .def_readwrite("mime", &jdoc::FormatInfo::mime)
        .def_readwrite("convertible", &jdoc::FormatInfo::convertible)
        .def_property_readonly("category_name", [](const jdoc::FormatInfo& i) {
            return std::string(jdoc::format_category_name(i.category));
        })
        .def("__repr__", [](const jdoc::FormatInfo& i) {
            return "<FormatInfo format='" + i.format + "' category='" +
                   jdoc::format_category_name(i.category) + "' ext='" +
                   i.extension + "' convertible=" +
                   (i.convertible ? "True" : "False") + ">";
        });

    // ── Top-level convenience functions ──────────────────

    m.def("convert", [](const std::string& file_path,
                         const std::string& format,
                         const std::vector<int>& pages,
                         bool tables,
                         bool images,
                         const std::string& image_dir,
                         const std::string& image_ref_prefix,
                         unsigned min_image_size) -> std::string {
        jdoc::ConvertOptions opts;
        opts.pages = pages;
        opts.tables = tables;
        opts.images = images;
        opts.image_dir = image_dir;
        opts.image_ref_prefix = image_ref_prefix;
        opts.min_image_size = min_image_size;
        if (format == "text" || format == "plaintext" || format == "plain")
            opts.format = jdoc::OutputFormat::PLAINTEXT;
        else
            opts.format = jdoc::OutputFormat::MARKDOWN;
        return jdoc::convert(file_path, opts);
    },
    py::arg("file_path"),
    py::arg("format") = "markdown",
    py::arg("pages") = std::vector<int>{},
    py::arg("tables") = true,
    py::arg("images") = false,
    py::arg("image_dir") = "",
    py::arg("image_ref_prefix") = "",
    py::arg("min_image_size") = 50,
    R"doc(Convert a document file to text.

Args:
    file_path: Path to the document file (PDF, DOCX, XLSX, PPTX, DOC, XLS, PPT, RTF, HWP, HWPX)
    format: Output format - "markdown" (default) or "text"/"plaintext"
    pages: List of page numbers (0-based). Empty = all pages.
    images: Whether to extract images
    image_dir: Directory to save extracted images (if empty, images kept in memory only)
    min_image_size: Skip images smaller than NxN pixels (default: 50, 0=no filter)

Returns:
    Converted text as string
)doc");

    m.def("convert_pages", [](const std::string& file_path,
                               const std::string& format,
                               const std::vector<int>& pages,
                               bool tables,
                               bool images,
                               const std::string& image_dir,
                               const std::string& image_ref_prefix,
                               unsigned min_image_size)
                               -> std::vector<jdoc::PageChunk> {
        jdoc::ConvertOptions opts;
        opts.pages = pages;
        opts.tables = tables;
        opts.images = images;
        opts.image_dir = image_dir;
        opts.image_ref_prefix = image_ref_prefix;
        opts.min_image_size = min_image_size;
        if (format == "text" || format == "plaintext" || format == "plain")
            opts.format = jdoc::OutputFormat::PLAINTEXT;
        else
            opts.format = jdoc::OutputFormat::MARKDOWN;
        return jdoc::convert_chunks(file_path, opts);
    },
    py::arg("file_path"),
    py::arg("format") = "markdown",
    py::arg("pages") = std::vector<int>{},
    py::arg("tables") = true,
    py::arg("images") = false,
    py::arg("image_dir") = "",
    py::arg("image_ref_prefix") = "",
    py::arg("min_image_size") = 50,
    R"doc(Convert a document file to per-page chunks.

Args:
    file_path: Path to the document file
    format: Output format - "markdown" (default) or "text"/"plaintext"
    pages: List of page numbers (0-based). Empty = all pages.
    images: Whether to extract images (accessible via chunk.images)
    image_dir: Directory to save images (if empty, images available via ImageData.data bytes)
    min_image_size: Skip images smaller than NxN pixels (default: 50, 0=no filter)

Returns:
    List of PageChunk objects, one per page/slide/sheet
)doc");

    // Lazy streaming iterator over pages. Yields one PageChunk at a time as the
    // document is converted, so peak memory tracks a few pages rather than the
    // whole document and the first page arrives before the rest are parsed.
    py::class_<PageStream>(m, "_PageStream")
        .def("__iter__", [](PageStream& self) -> PageStream& { return self; })
        .def("__next__", &PageStream::next);

    m.def("convert_pages_stream", [](const std::string& file_path,
                                     const std::string& format,
                                     const std::vector<int>& pages,
                                     bool tables,
                                     bool images,
                                     const std::string& image_dir,
                                     const std::string& image_ref_prefix,
                                     unsigned min_image_size,
                                     unsigned buffer_size) {
        jdoc::ConvertOptions opts;
        opts.pages = pages;
        opts.tables = tables;
        opts.images = images;
        opts.image_dir = image_dir;
        opts.image_ref_prefix = image_ref_prefix;
        opts.min_image_size = min_image_size;
        if (format == "text" || format == "plaintext" || format == "plain")
            opts.format = jdoc::OutputFormat::PLAINTEXT;
        else
            opts.format = jdoc::OutputFormat::MARKDOWN;
        size_t cap = buffer_size > 0 ? buffer_size : 1;
        return std::unique_ptr<PageStream>(
            new PageStream(file_path, std::move(opts), cap));
    },
    py::arg("file_path"),
    py::arg("format") = "markdown",
    py::arg("pages") = std::vector<int>{},
    py::arg("tables") = true,
    py::arg("images") = false,
    py::arg("image_dir") = "",
    py::arg("image_ref_prefix") = "",
    py::arg("min_image_size") = 50,
    py::arg("buffer_size") = 4,
    R"doc(Convert a document to per-page chunks, lazily.

Returns an iterator that yields one PageChunk at a time as the document is
converted. Compared to convert_pages(), peak memory tracks only a few pages
(see buffer_size) instead of the whole document, and the first page is
available before the rest are parsed. Output is identical to convert_pages().

A background thread does the conversion; abandoning the iterator (or breaking
out of the loop) stops it. Same arguments as convert_pages(), plus:
    buffer_size: max pages held in the internal queue (default 4)

Yields:
    PageChunk objects, one per page/slide/sheet
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
                                 long long max_ratio,
                                 bool include_unsupported)
                                 -> std::vector<jdoc::MemberResult> {
        jdoc::ConvertOptions opts;
        if (format == "text" || format == "plaintext" || format == "plain")
            opts.format = jdoc::OutputFormat::PLAINTEXT;
        // -1 (any negative) disables the corresponding guard.
        opts.archive.max_depth = max_depth;
        opts.archive.max_member_bytes =
            max_member_bytes < 0 ? UINT64_MAX : (uint64_t)max_member_bytes;
        opts.archive.max_total_bytes =
            max_total_bytes < 0 ? UINT64_MAX : (uint64_t)max_total_bytes;
        opts.archive.max_entries =
            max_entries < 0 ? UINT32_MAX : (uint32_t)max_entries;
        opts.archive.max_ratio =
            max_ratio < 0 ? 0 : (uint32_t)max_ratio;
        opts.archive.include_unsupported = include_unsupported;
        return jdoc::convert_archive(file_path, opts);
    },
    py::arg("file_path"),
    py::arg("format") = "markdown",
    py::arg("max_depth") = 3,
    py::arg("max_member_bytes") = (long long)(512) << 20,
    py::arg("max_total_bytes") = (long long)(64) << 30,
    py::arg("max_entries") = 200000,
    py::arg("max_ratio") = 10000,
    py::arg("include_unsupported") = false,
    R"doc(Convert every supported document inside an archive (zip/gz/tar/tar.gz/
7z/alz/egg) without extracting to disk. Members are decompressed into memory
one at a time; nested archives are walked recursively up to max_depth.

Limits: pass -1 to disable a limit (unlimited). Only do this for trusted
inputs — archive-bomb protection is disabled with it.

Returns:
    List of MemberResult (member_path, format, markdown, error, ok)
)doc");

    m.def("is_archive", &jdoc::is_archive_file, py::arg("file_path"),
          "True if the file is an archive container rather than a document.");

    m.def("convert_bytes", [](py::bytes data, const std::string& name_hint,
                               const std::string& format) -> std::string {
        jdoc::ConvertOptions opts;
        if (format == "text" || format == "plaintext" || format == "plain")
            opts.format = jdoc::OutputFormat::PLAINTEXT;
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

    // ── Format detection (detect-only, no extraction) ────
    m.def("detect",
          [](const std::string& file_path) { return jdoc::detect(file_path); },
          py::arg("file_path"),
          R"doc(Detect a file's format without running a full extraction.

Returns:
    FormatInfo(format, category, extension, mime, convertible)
)doc");

    m.def("detect_bytes",
          [](py::bytes data, const std::string& name_hint) {
              std::string buf = data;  // copy out of the Python object
              return jdoc::detect(buf.data(), buf.size(), name_hint);
          },
          py::arg("data"), py::arg("name_hint") = "",
          "Detect the format of a document held in bytes (no file I/O).");
}
