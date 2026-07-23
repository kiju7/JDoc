// jdoc - Public format detection API implementation.
//
// Thin layer over the internal detect_format() / detect_office_format():
//   1. Sniff standalone image signatures first (png/jpeg/gif/… — formats jdoc
//      recognizes but cannot convert). Magic-byte driven, table-driven.
//   2. Otherwise defer to the internal container/magic detector, then refine
//      the OFFICE bucket into the exact office sub-format.
//   3. Map the resulting fine-grained format to a rich FormatInfo descriptor.
// License: MIT

#include "jdoc/detect.h"
#include "jdoc/office.h"
#include "convert_internal.h"

#include <cstring>
#include <fstream>

namespace jdoc {
namespace {

// ── Image signature sniffing ────────────────────────────
// Standalone raster images. jdoc has no text to extract from these, so they
// come back convertible=false — but classifying them lets callers route or
// skip them instead of getting a bare "UNKNOWN".
//
// Returns a canonical name ("PNG", "JPEG", …) or nullptr when no image
// signature matches. `n` is the number of valid header bytes in `b`.
static const char* image_from_magic(const unsigned char* b, size_t n) {
    if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return "PNG";
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "JPEG";
    if (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0))
        return "GIF";
    if (n >= 4 && memcmp(b, "8BPS", 4) == 0) return "PSD";
    // TIFF little-endian (II*\0) and big-endian (MM\0*).
    if (n >= 4 && (memcmp(b, "II\x2a\x00", 4) == 0 ||
                   memcmp(b, "MM\x00\x2a", 4) == 0))
        return "TIFF";
    // WebP: "RIFF"????"WEBP".
    if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0)
        return "WEBP";
    // ICO: reserved=0, type=1 (icon).
    if (n >= 4 && b[0] == 0x00 && b[1] == 0x00 && b[2] == 0x01 && b[3] == 0x00)
        return "ICO";
    // BMP: "BM". Guard against text that happens to start with "BM" by also
    // requiring the 4 reserved header bytes (offset 6-9) to be zero.
    if (n >= 10 && b[0] == 'B' && b[1] == 'M' &&
        b[6] == 0 && b[7] == 0 && b[8] == 0 && b[9] == 0)
        return "BMP";
    return nullptr;
}

struct Entry {
    FormatCategory category;
    const char* extension;
    const char* mime;
    bool convertible;
};

// Metadata for every canonical format name detect() can emit.
static const Entry* lookup(const std::string& fmt) {
    // Images (detect-only: recognized but not convertible).
    static const Entry PNG  {FormatCategory::Image, ".png",  "image/png",  false};
    static const Entry JPEG {FormatCategory::Image, ".jpg",  "image/jpeg", false};
    static const Entry GIF  {FormatCategory::Image, ".gif",  "image/gif",  false};
    static const Entry BMP  {FormatCategory::Image, ".bmp",  "image/bmp",  false};
    static const Entry TIFF {FormatCategory::Image, ".tiff", "image/tiff", false};
    static const Entry WEBP {FormatCategory::Image, ".webp", "image/webp", false};
    static const Entry ICO  {FormatCategory::Image, ".ico",  "image/x-icon", false};
    static const Entry PSD  {FormatCategory::Image, ".psd",  "image/vnd.adobe.photoshop", false};

    // Documents.
    static const Entry PDF  {FormatCategory::Document, ".pdf",  "application/pdf", true};
    static const Entry HWP  {FormatCategory::Document, ".hwp",  "application/x-hwp", true};
    static const Entry HWPX {FormatCategory::Document, ".hwpx", "application/hwp+zip", true};
    static const Entry DOC  {FormatCategory::Document, ".doc",  "application/msword", true};
    static const Entry DOCX {FormatCategory::Document, ".docx",
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document", true};
    static const Entry RTF  {FormatCategory::Document, ".rtf",  "application/rtf", true};
    static const Entry HTML {FormatCategory::Document, ".html", "text/html", true};
    static const Entry ODT  {FormatCategory::Document, ".odt",  "application/vnd.oasis.opendocument.text", true};

    // Spreadsheets.
    static const Entry XLS  {FormatCategory::Spreadsheet, ".xls",  "application/vnd.ms-excel", true};
    static const Entry XLSX {FormatCategory::Spreadsheet, ".xlsx",
        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", true};
    static const Entry XLSB {FormatCategory::Spreadsheet, ".xlsb",
        "application/vnd.ms-excel.sheet.binary.macroEnabled.12", true};
    static const Entry ODS  {FormatCategory::Spreadsheet, ".ods",  "application/vnd.oasis.opendocument.spreadsheet", true};

    // Presentations.
    static const Entry PPT  {FormatCategory::Presentation, ".ppt",  "application/vnd.ms-powerpoint", true};
    static const Entry PPTX {FormatCategory::Presentation, ".pptx",
        "application/vnd.openxmlformats-officedocument.presentationml.presentation", true};
    static const Entry ODP  {FormatCategory::Presentation, ".odp",  "application/vnd.oasis.opendocument.presentation", true};

    // Email.
    static const Entry EML  {FormatCategory::Email, ".eml", "message/rfc822", true};
    static const Entry MSG  {FormatCategory::Email, ".msg", "application/vnd.ms-outlook", true};

    // Text.
    static const Entry TXT  {FormatCategory::Text, ".txt", "text/plain", true};

    // Archives (convertible via convert_archive()).
    static const Entry ZIP  {FormatCategory::Archive, ".zip",  "application/zip", true};
    static const Entry GZIP {FormatCategory::Archive, ".gz",   "application/gzip", true};
    static const Entry BZIP2{FormatCategory::Archive, ".bz2",  "application/x-bzip2", true};
    static const Entry TAR  {FormatCategory::Archive, ".tar",  "application/x-tar", true};
    static const Entry SEVENZIP{FormatCategory::Archive, ".7z", "application/x-7z-compressed", true};
    static const Entry ALZ  {FormatCategory::Archive, ".alz",  "application/x-alz-compressed", true};
    static const Entry EGG  {FormatCategory::Archive, ".egg",  "application/x-egg", true};
    static const Entry RAR  {FormatCategory::Archive, ".rar",  "application/vnd.rar", true};

    // Generic office fallback: recognized as an office/OLE/ZIP document but not
    // sub-classified. convert() still dispatches it, so convertible=true.
    static const Entry OFFICE {FormatCategory::Document, "", "", true};

    // Encrypted office (recognized, but not convertible).
    static const Entry ENC  {FormatCategory::Document, "", "", false};

    if (fmt == "PNG")  return &PNG;
    if (fmt == "JPEG") return &JPEG;
    if (fmt == "GIF")  return &GIF;
    if (fmt == "BMP")  return &BMP;
    if (fmt == "TIFF") return &TIFF;
    if (fmt == "WEBP") return &WEBP;
    if (fmt == "ICO")  return &ICO;
    if (fmt == "PSD")  return &PSD;
    if (fmt == "PDF")  return &PDF;
    if (fmt == "HWP")  return &HWP;
    if (fmt == "HWPX") return &HWPX;
    if (fmt == "DOC")  return &DOC;
    if (fmt == "DOCX") return &DOCX;
    if (fmt == "RTF")  return &RTF;
    if (fmt == "HTML") return &HTML;
    if (fmt == "ODT")  return &ODT;
    if (fmt == "XLS")  return &XLS;
    if (fmt == "XLSX") return &XLSX;
    if (fmt == "XLSB") return &XLSB;
    if (fmt == "ODS")  return &ODS;
    if (fmt == "PPT")  return &PPT;
    if (fmt == "PPTX") return &PPTX;
    if (fmt == "ODP")  return &ODP;
    if (fmt == "EML")  return &EML;
    if (fmt == "MSG")  return &MSG;
    if (fmt == "TXT")  return &TXT;
    if (fmt == "ZIP")  return &ZIP;
    if (fmt == "GZIP") return &GZIP;
    if (fmt == "BZIP2")return &BZIP2;
    if (fmt == "TAR")  return &TAR;
    if (fmt == "7Z")   return &SEVENZIP;
    if (fmt == "ALZ")  return &ALZ;
    if (fmt == "EGG")  return &EGG;
    if (fmt == "RAR")  return &RAR;
    if (fmt == "OFFICE") return &OFFICE;
    if (fmt == "ENCRYPTED_PASSWORD" || fmt == "ENCRYPTED_RIGHTS") return &ENC;
    return nullptr;
}

static FormatInfo make_info(std::string fmt) {
    FormatInfo info;
    info.format = std::move(fmt);
    if (const Entry* e = lookup(info.format)) {
        info.category = e->category;
        info.extension = e->extension;
        info.mime = e->mime;
        info.convertible = e->convertible;
    }
    return info;  // UNKNOWN / unmapped → category=Unknown, convertible=false
}

// Resolve the OFFICE bucket into an exact office sub-format name, given the
// DocFormat the office layer reports.
static std::string office_name(DocFormat df) {
    // format_name() already maps DocFormat → the canonical token we use.
    if (df == DocFormat::UNKNOWN) return "OFFICE";
    return format_name(df);
}

} // namespace

const char* format_category_name(FormatCategory c) {
    switch (c) {
        case FormatCategory::Document:     return "document";
        case FormatCategory::Spreadsheet:  return "spreadsheet";
        case FormatCategory::Presentation: return "presentation";
        case FormatCategory::Archive:      return "archive";
        case FormatCategory::Email:        return "email";
        case FormatCategory::Text:         return "text";
        case FormatCategory::Image:        return "image";
        default:                           return "unknown";
    }
}

FormatInfo detect(const std::string& file_path) {
    // Read a header once for the image sniff (512B covers every image magic).
    unsigned char hdr[512] = {};
    size_t n = 0;
    {
        std::ifstream f(file_path, std::ios::binary);
        if (f) {
            f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
            n = static_cast<size_t>(f.gcount());
        }
    }
    if (const char* img = image_from_magic(hdr, n))
        return make_info(img);

    FileFormat ff = detect_format(file_path);
    if (ff == FileFormat::OFFICE)
        return make_info(office_name(detect_office_format(file_path)));
    return make_info(file_format_name(ff));
}

FormatInfo detect(const void* data, size_t size, const std::string& name_hint) {
    const unsigned char* b = static_cast<const unsigned char*>(data);
    if (b && size) {
        if (const char* img = image_from_magic(b, size))
            return make_info(img);
    }

    FileFormat ff = detect_format_mem(b, size, name_hint);
    if (ff == FileFormat::OFFICE)
        return make_info(office_name(detect_office_format_mem(b, size, name_hint)));
    return make_info(file_format_name(ff));
}

} // namespace jdoc
