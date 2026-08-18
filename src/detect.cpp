// jdoc - Public format detection API implementation.
//
// Thin layer over the internal detect_format() / detect_office_format():
//   1. Sniff standalone image signatures first (png/jpeg/gif/… — formats jdoc
//      recognizes but cannot convert). Magic-byte driven (offset/mask table).
//   2. Otherwise defer to the internal container/magic detector, then refine
//      the OFFICE bucket into the exact office sub-format.
//   3. Map the resulting fine-grained format to a rich FormatInfo descriptor.
// License: MIT

#include "jdoc/detect.h"
#include "jdoc/office.h"
#include "convert_internal.h"
#include "common/image_magic.h"

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
    // Shared magic table (common/image_magic.h). This is the standalone-raster
    // sniff: metafiles (WMF/EMF) are vector documents, not raster images, and
    // stay on the container/magic path below, so exclude them here to preserve
    // detect()'s classification flow.
    util::ImageFormat f = util::image_magic(b, n);
    if (f == util::ImageFormat::None ||
        f == util::ImageFormat::Wmf || f == util::ImageFormat::Emf)
        return nullptr;
    return util::image_type(f);
}

// ── CAD drawings ────────────────────────────────────────
// detect_format() lumps every drawing into FileFormat::CAD because that is all
// the routing/skipping paths need. detect() reports the exact one, so the
// signature is re-read here — the same split image_from_magic already makes.
//
// Only DWG, DWF and binary DXF are self-identifying; an ASCII DXF is plain
// text whose extension is the only evidence.
static const char* cad_from_magic(const unsigned char* b, size_t n,
                                  const std::string& name) {
    if (b) {
        if (n >= 7 && std::memcmp(b, "AC10", 4) == 0 && b[6] == 0x00 &&
            b[4] >= '0' && b[4] <= '9' && b[5] >= '0' && b[5] <= '9')
            return "DWG";
        if (n >= 6 && std::memcmp(b, "(DWF V", 6) == 0) return "DWF";
        if (n >= 18 && std::memcmp(b, "AutoCAD Binary DXF", 18) == 0) return "DXF";
        // DWFx is an XPS zip; detect_format already opened the container.
        if (n >= 4 && std::memcmp(b, "PK\x03\x04", 4) == 0) return "DWFX";
    }
    auto dot = name.rfind('.');
    if (dot != std::string::npos) {
        std::string ext = name.substr(dot);
        for (auto& c : ext) c = static_cast<char>(std::tolower(
            static_cast<unsigned char>(c)));
        // No ".dxf" here: an ASCII DXF never reaches FileFormat::CAD, and a
        // binary one is already claimed by its signature above.
        if (ext == ".dwg")  return "DWG";
        if (ext == ".dwfx") return "DWFX";
        if (ext == ".dwf")  return "DWF";
    }
    return "CAD";
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
    // Metafiles: vector records carrying text — jdoc extracts that text, so
    // unlike raster images these are convertible.
    static const Entry EMF  {FormatCategory::Image, ".emf",  "image/emf", true};
    static const Entry WMF  {FormatCategory::Image, ".wmf",  "image/wmf", true};

    // CAD drawings (detect-only). Grouped with the metafiles as vector
    // pictures, but nothing is extracted: a DWG keeps its text inside a
    // compressed object section, and a DWF/DWFx would need a WHIP! opcode
    // walker. Recognizing them lets callers route or skip instead of
    // decompressing a hundred-megabyte drawing only to discard it.
    static const Entry DWG  {FormatCategory::Image, ".dwg",  "image/vnd.dwg", false};
    static const Entry DXF  {FormatCategory::Image, ".dxf",  "image/vnd.dxf", false};
    static const Entry DWF  {FormatCategory::Image, ".dwf",  "model/vnd.dwf", false};
    static const Entry DWFX {FormatCategory::Image, ".dwfx", "model/vnd.dwfx+xps", false};
    static const Entry CAD  {FormatCategory::Image, "", "", false};

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
    if (fmt == "EMF")  return &EMF;
    if (fmt == "WMF")  return &WMF;
    if (fmt == "DWG")  return &DWG;
    if (fmt == "DXF")  return &DXF;
    if (fmt == "DWF")  return &DWF;
    if (fmt == "DWFX") return &DWFX;
    if (fmt == "CAD")  return &CAD;
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
    if (ff == FileFormat::CAD)
        return make_info(cad_from_magic(hdr, n, file_path));
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
    if (ff == FileFormat::CAD)
        return make_info(cad_from_magic(b, size, name_hint));
    return make_info(file_format_name(ff));
}

} // namespace jdoc
