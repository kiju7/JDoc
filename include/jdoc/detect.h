#pragma once
// jdoc - Public format detection API
//
// Identifies a file's format from magic bytes + container inspection WITHOUT
// running a full extraction, and returns a rich descriptor (name, category,
// canonical extension, MIME type, and whether jdoc can process it).
//
// This is the detect-only counterpart to convert(): use it to branch on a
// file's type before deciding whether/how to convert it.
// License: MIT

#include <cstddef>
#include <cstdint>
#include <string>

namespace jdoc {

// Coarse family a format belongs to. Useful for callers that only care about
// "is this a spreadsheet / an archive / an image" rather than the exact format.
enum class FormatCategory {
    Document,      // pdf, doc(x), rtf, html, odt, hwp, hwpx
    Spreadsheet,   // xls(x/b), ods
    Presentation,  // ppt(x), odp
    Archive,       // zip, gz, bz2, tar, 7z, alz, egg, rar
    Email,         // eml, msg
    Text,          // txt/csv/json/xml/source code …
    Image,         // png, jpeg, gif, bmp, tiff, webp, ico, psd (detect-only)
    Unknown
};

// Rich result of a detection. `format` is a stable uppercase token
// ("PDF", "DOCX", "PNG", "ZIP", "UNKNOWN"); prefer it over `category` for
// exact branching.
struct FormatInfo {
    std::string format;       // canonical name, e.g. "PDF", "DOCX", "PNG"
    FormatCategory category = FormatCategory::Unknown;
    std::string extension;    // canonical extension incl. dot, e.g. ".pdf"
    std::string mime;         // e.g. "application/pdf"; empty if unknown
    bool convertible = false; // jdoc can extract text (convert / convert_archive)
};

// Detect the format of a file on disk. Never throws; an unreadable or
// unrecognized file yields format "UNKNOWN".
FormatInfo detect(const std::string& file_path);

// Detect the format of a document held in memory. name_hint (e.g. the original
// filename) resolves extension-based ambiguity; may be empty.
FormatInfo detect(const void* data, size_t size, const std::string& name_hint = "");

// Lowercase category name ("document", "spreadsheet", …, "unknown").
const char* format_category_name(FormatCategory c);

} // namespace jdoc
