// jdoc - Unified document converter
// Auto-detects format via magic bytes + extension fallback.

#include "jdoc/jdoc.h"
#include "jdoc/archive.h"
#include "jdoc/pdf.h"
#include "jdoc/office.h"
#include "jdoc/hwp.h"
#include "jdoc/hwpx.h"
#include "jdoc/eml.h"
#include "convert_internal.h"
#include "zip_reader.h"
#include "legacy/ole_reader.h"
#include "common/string_utils.h"
#include "common/image_utils.h"
#include "common/png_encode.h"
#include "common/emf_text.h"
#include "common/wmf_text.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace jdoc {
namespace {

// Check if a buffer looks like valid UTF-8/ASCII text (first 8KB sampled).
// Rejects buffers with NUL bytes or invalid UTF-8 sequences.
static bool is_likely_text_buf(const uint8_t* u, size_t n) {
    if (n == 0) return true; // empty is valid text
    if (n > 8192) n = 8192;

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

static bool is_likely_text(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    char buf[8192];
    f.read(buf, sizeof(buf));
    return is_likely_text_buf(reinterpret_cast<const uint8_t*>(buf),
                              static_cast<size_t>(f.gcount()));
}

// Extension → format, shared by path- and memory-based detection.
// Returns UNKNOWN when the extension decides nothing.
static FileFormat format_from_ext(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return FileFormat::UNKNOWN;
    std::string ext = name.substr(dot);
    for (auto& c : ext) c = std::tolower(static_cast<unsigned char>(c));

    if (ext == ".pdf") return FileFormat::PDF;
    if (ext == ".hwpx") return FileFormat::HWPX;
    if (ext == ".hwp") return FileFormat::HWP;
    if (ext == ".eml") return FileFormat::EML;
    if (ext == ".emf") return FileFormat::EMF;
    if (ext == ".wmf") return FileFormat::WMF;
    if (ext == ".docx" || ext == ".xlsx" || ext == ".pptx" ||
        ext == ".doc" || ext == ".xls" || ext == ".ppt" || ext == ".rtf" ||
        ext == ".html" || ext == ".htm" || ext == ".xlsb" ||
        ext == ".odt" || ext == ".ods" || ext == ".odp")
        return FileFormat::OFFICE;
    if (ext == ".zip") return FileFormat::ZIP;
    if (ext == ".gz" || ext == ".tgz") return FileFormat::GZIP;
    if (ext == ".bz2" || ext == ".tbz2") return FileFormat::BZIP2;
    if (ext == ".tar") return FileFormat::TAR;
    if (ext == ".7z") return FileFormat::SEVENZIP;
    if (ext == ".alz") return FileFormat::ALZ;
    if (ext == ".egg") return FileFormat::EGG;
    if (ext == ".rar") return FileFormat::RAR;
    if (ext == ".txt" || ext == ".text" || ext == ".log" || ext == ".csv" ||
        ext == ".tsv" || ext == ".md" || ext == ".json" || ext == ".xml" ||
        ext == ".yml" || ext == ".yaml" || ext == ".ini" || ext == ".cfg" ||
        ext == ".conf" || ext == ".sh" || ext == ".bat" || ext == ".ps1" ||
        ext == ".py" || ext == ".js" || ext == ".ts" || ext == ".cpp" ||
        ext == ".c" || ext == ".h" || ext == ".hpp" || ext == ".java" ||
        ext == ".rs" || ext == ".go" || ext == ".rb" || ext == ".php" ||
        ext == ".sql" || ext == ".css" || ext == ".scss" || ext == ".less")
        return FileFormat::TXT;

    return FileFormat::UNKNOWN;
}

// Magic-byte checks that need no container probing.
// probe must hold at least 8 bytes (or fewer at EOF); tar needs 262+.
static FileFormat format_from_magic(const unsigned char* magic, size_t n) {
    if (n >= 4 && magic[0] == '%' && magic[1] == 'P' && magic[2] == 'D' &&
        magic[3] == 'F')
        return FileFormat::PDF;
    if (n >= 2 && magic[0] == 0x1F && magic[1] == 0x8B)
        return FileFormat::GZIP;
    if (n >= 4 && magic[0] == 'B' && magic[1] == 'Z' && magic[2] == 'h' &&
        magic[3] >= '1' && magic[3] <= '9')
        return FileFormat::BZIP2;
    if (n >= 6 && magic[0] == 0x37 && magic[1] == 0x7A && magic[2] == 0xBC &&
        magic[3] == 0xAF && magic[4] == 0x27 && magic[5] == 0x1C)
        return FileFormat::SEVENZIP;
    if (n >= 4 && magic[0] == 'A' && magic[1] == 'L' && magic[2] == 'Z' &&
        magic[3] == 0x01)
        return FileFormat::ALZ;
    if (n >= 4 && magic[0] == 'E' && magic[1] == 'G' && magic[2] == 'G' &&
        magic[3] == 'A')
        return FileFormat::EGG;
    if (n >= 7 && memcmp(magic, "Rar!\x1A\x07", 6) == 0 &&
        (magic[6] == 0x00 || magic[6] == 0x01))
        return FileFormat::RAR;
    if (n >= 262 && memcmp(magic + 257, "ustar", 5) == 0)
        return FileFormat::TAR;
    if (n >= 5 && magic[0] == '{' && magic[1] == '\\' && magic[2] == 'r' &&
        magic[3] == 't' && magic[4] == 'f')
        return FileFormat::OFFICE;
    // HWP 2.x/3.x legacy binary ("HWP Document File V3.00 \x1A\x01\x02\x03\x04\x05")
    if (n >= 19 && memcmp(magic, "HWP Document File V", 19) == 0)
        return FileFormat::HWP;
    // EMF: EMR_HEADER record (0x01000000) + " EMF" signature at offset 40.
    if (n >= 44 && magic[0] == 0x01 && magic[1] == 0x00 && magic[2] == 0x00 &&
        magic[3] == 0x00 && memcmp(magic + 40, " EMF", 4) == 0)
        return FileFormat::EMF;
    // WMF with the Aldus placeable header (D7 CD C6 9A). A bare WMF header has no
    // reliable magic, so those resolve by extension instead.
    if (n >= 4 && magic[0] == 0xD7 && magic[1] == 0xCD && magic[2] == 0xC6 &&
        magic[3] == 0x9A)
        return FileFormat::WMF;
    return FileFormat::UNKNOWN;
}

// Classify an open zip container: OOXML/HWPX document package vs plain
// archive of files.
static FileFormat classify_zip(const ZipReader& zip) {
    if (zip.has_entry("Contents/section0.xml") ||
        zip.has_entry("Contents/content.hpf") ||
        zip.has_entry("META-INF/container.xml"))
        return FileFormat::HWPX;
    if (zip.has_entry("[Content_Types].xml"))
        return FileFormat::OFFICE;
    // ODF (odt/ods/odp): no [Content_Types].xml — instead a top-level mimetype
    // member plus META-INF/manifest.xml. The office layer splits the three
    // kinds by the mimetype string.
    if (zip.has_entry("mimetype") && zip.has_entry("META-INF/manifest.xml"))
        return FileFormat::OFFICE;
    return FileFormat::ZIP;
}

static FileFormat classify_ole_hwp(const OleReader& ole) {
    if (ole.has_stream("FileHeader") || ole.has_stream("BodyText/Section0"))
        return FileFormat::HWP;
    return FileFormat::OFFICE;
}

static std::string read_text_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open file: " + path);
    // Read the whole file into a single buffer, then move it through the
    // valid-UTF-8 fast path — no intermediate stringstream copy.
    std::string buf;
    std::streamoff size = f.tellg();
    if (size > 0) {
        buf.resize(static_cast<size_t>(size));
        f.seekg(0);
        f.read(buf.data(), static_cast<std::streamsize>(size));
        buf.resize(static_cast<size_t>(f.gcount()));  // guard short reads
    } else if (size < 0) {
        // Seek/tell unsupported (rare): read via streambuf directly into buf.
        buf.assign(std::istreambuf_iterator<char>(f),
                   std::istreambuf_iterator<char>());
    }
    return util::plain_text_to_utf8(std::move(buf));
}

// Read a whole file into a raw byte buffer (no text conversion). Throws if the
// file cannot be opened. Used for binary formats like EMF/WMF metafiles.
static std::string read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open file: " + path);
    std::string buf;
    std::streamoff size = f.tellg();
    if (size > 0) {
        buf.resize(static_cast<size_t>(size));
        f.seekg(0);
        f.read(buf.data(), static_cast<std::streamsize>(size));
        buf.resize(static_cast<size_t>(f.gcount()));
    } else if (size < 0) {
        buf.assign(std::istreambuf_iterator<char>(f),
                   std::istreambuf_iterator<char>());
    }
    return buf;
}

// One-pass EMF/WMF extraction (text and/or bitmaps) dispatched by format, so a
// metafile carrying both is parsed only once.
static MetafileContent metafile_extract(FileFormat fmt, const uint8_t* data,
                                        size_t size, bool want_text,
                                        bool want_images) {
    return fmt == FileFormat::EMF
               ? emf_extract(data, size, want_text, want_images)
               : wmf_extract(data, size, want_text, want_images);
}

// Markdown for a standalone EMF/WMF: recovered text plus, when opts.images,
// its embedded bitmaps saved to image_dir and referenced inline. Image naming
// ("page1_img{N}") and the "![name](ref)\n\n" reference format match every
// other parser so downstream output stays uniform.
static std::string metafile_to_markdown(FileFormat fmt, const uint8_t* data,
                                        size_t size, const ConvertOptions& opts) {
    MetafileContent c = metafile_extract(fmt, data, size, true, opts.images);
    std::string md = std::move(c.text);
    int i = 0;
    for (auto& bmp : c.images) {
        std::string name = "page1_img" + std::to_string(i++);
        std::string filename = name + ".bmp";
        if (!opts.image_dir.empty())
            util::save_image_to_file(opts.image_dir, name, "bmp",
                                     bmp.data(), bmp.size());
        if (!md.empty() && md.back() != '\n') md += "\n\n";
        md += "![" + filename + "](" + opts.image_ref_prefix + filename + ")\n\n";
    }
    return md;
}

// Build a single PageChunk for a standalone EMF/WMF, matching the parsers: the
// recovered text with inline image references, and (when opts.images) the
// bitmaps as ImageData — the BMP buffer is moved in (no copy), then saved to
// disk when image_dir is set, else held in memory.
static PageChunk metafile_to_chunk(FileFormat fmt, const uint8_t* data,
                                   size_t size, const ConvertOptions& opts) {
    MetafileContent c = metafile_extract(fmt, data, size, true, opts.images);
    PageChunk chunk;
    chunk.page_number = 1;
    chunk.text = std::move(c.text);
    int i = 0;
    for (auto& bmp : c.images) {
        std::string name = "page1_img" + std::to_string(i++);
        std::string filename = name + ".bmp";
        ImageData img;
        img.page_number = 1;
        img.name = name;
        img.format = "bmp";
        img.data = std::move(bmp);           // BMP buffer moved in, no copy
        util::populate_image_dimensions(img);
        if (!opts.image_dir.empty()) {
            img.saved_path = util::save_image_to_file(
                opts.image_dir, name, "bmp", img.data.data(), img.data.size());
            if (!img.saved_path.empty()) {
                img.data.clear();
                img.data.shrink_to_fit();
            }
        }
        if (!chunk.text.empty() && chunk.text.back() != '\n')
            chunk.text += "\n\n";
        chunk.text += "![" + filename + "](" + opts.image_ref_prefix +
                      filename + ")\n\n";
        chunk.images.push_back(std::move(img));
    }
    return chunk;
}

} // anonymous namespace

FileFormat detect_format(const std::string& path) {
    // Magic bytes first (262+ so the tar "ustar" field is visible)
    unsigned char magic[262] = {};
    size_t n = 0;
    {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            f.read(reinterpret_cast<char*>(magic), sizeof(magic));
            n = static_cast<size_t>(f.gcount());
        }
    }

    if (n >= 4 && magic[0] == 'P' && magic[1] == 'K' && magic[2] == 0x03 &&
        magic[3] == 0x04) {
        ZipReader zip(path);
        if (zip.is_open()) return classify_zip(zip);
        return FileFormat::ZIP;  // zip magic but unreadable directory
    }

    if (n >= 4 && magic[0] == 0xD0 && magic[1] == 0xCF && magic[2] == 0x11 &&
        magic[3] == 0xE0) {
        OleReader ole(path);
        if (ole.is_open()) return classify_ole_hwp(ole);
        return FileFormat::OFFICE;
    }

    FileFormat fmt = format_from_magic(magic, n);
    if (fmt != FileFormat::UNKNOWN) return fmt;

    // Extension — resolves ambiguous cases (e.g. .xml starts with '<' but is TXT)
    fmt = format_from_ext(path);
    if (fmt != FileFormat::UNKNOWN) return fmt;

    // '<' fallback — extensionless HTML
    if (n >= 1 && magic[0] == '<')
        return FileFormat::OFFICE;

    // UTF-8/ASCII heuristic — if content looks like valid text, treat as TXT
    if (is_likely_text(path))
        return FileFormat::TXT;

    return FileFormat::UNKNOWN;
}

FileFormat detect_format_mem(const uint8_t* data, size_t size,
                             const std::string& name_hint) {
    if (!data || size == 0) return FileFormat::UNKNOWN;

    if (size >= 4 && data[0] == 'P' && data[1] == 'K' && data[2] == 0x03 &&
        data[3] == 0x04) {
        ZipReader zip(data, size);
        if (zip.is_open()) return classify_zip(zip);
        return FileFormat::ZIP;
    }

    if (size >= 4 && data[0] == 0xD0 && data[1] == 0xCF && data[2] == 0x11 &&
        data[3] == 0xE0) {
        OleReader ole(data, size);
        if (ole.is_open()) return classify_ole_hwp(ole);
        return FileFormat::OFFICE;
    }

    FileFormat fmt = format_from_magic(data, size);
    if (fmt != FileFormat::UNKNOWN) return fmt;

    fmt = format_from_ext(name_hint);
    if (fmt != FileFormat::UNKNOWN) return fmt;

    if (data[0] == '<')
        return FileFormat::OFFICE;

    if (is_likely_text_buf(data, size))
        return FileFormat::TXT;

    return FileFormat::UNKNOWN;
}

const char* file_format_name(FileFormat fmt) {
    switch (fmt) {
        case FileFormat::PDF:      return "PDF";
        case FileFormat::OFFICE:   return "OFFICE";
        case FileFormat::HWP:      return "HWP";
        case FileFormat::HWPX:     return "HWPX";
        case FileFormat::TXT:      return "TXT";
        case FileFormat::EML:      return "EML";
        case FileFormat::EMF:      return "EMF";
        case FileFormat::WMF:      return "WMF";
        case FileFormat::ZIP:      return "ZIP";
        case FileFormat::GZIP:     return "GZIP";
        case FileFormat::BZIP2:    return "BZIP2";
        case FileFormat::TAR:      return "TAR";
        case FileFormat::SEVENZIP: return "7Z";
        case FileFormat::ALZ:      return "ALZ";
        case FileFormat::EGG:      return "EGG";
        case FileFormat::RAR:      return "RAR";
        default:                   return "UNKNOWN";
    }
}

std::string convert_from_memory(const uint8_t* data, size_t size,
                                const std::string& name_hint,
                                const ConvertOptions& opts) {
    auto fmt = detect_format_mem(data, size, name_hint);
    return convert_from_memory_as(fmt, data, size, name_hint, opts);
}

std::string convert_from_memory_as(FileFormat fmt,
                                   const uint8_t* data, size_t size,
                                   const std::string& name_hint,
                                   const ConvertOptions& opts) {
    switch (fmt) {
        case FileFormat::PDF:    return pdf_to_markdown_mem(data, size, opts);
        case FileFormat::OFFICE: return office_to_markdown_mem(data, size, name_hint, opts);
        case FileFormat::HWPX:   return hwpx_to_markdown_mem(data, size, opts);
        case FileFormat::HWP:    return hwp_to_markdown_mem(data, size, opts);
        case FileFormat::TXT:
            return util::plain_text_to_utf8(
                std::string(reinterpret_cast<const char*>(data), size));
        case FileFormat::EML:    return eml_to_markdown_mem(data, size, opts);
        case FileFormat::EMF:
        case FileFormat::WMF:    return metafile_to_markdown(fmt, data, size, opts);
        default:
            if (is_archive_format(fmt))
                throw std::runtime_error("Nested archive must be walked, not converted: " + name_hint);
            throw std::runtime_error("Unsupported file format: " + name_hint);
    }
}

std::string convert(const std::string& file_path, ConvertOptions opts) {
    auto fmt = detect_format(file_path);
    switch (fmt) {
        case FileFormat::PDF:    return pdf_to_markdown(file_path, opts);
        case FileFormat::OFFICE: return office_to_markdown(file_path, opts);
        case FileFormat::HWPX:   return hwpx_to_markdown(file_path, opts);
        case FileFormat::HWP:    return hwp_to_markdown(file_path, opts);
        case FileFormat::TXT:    return read_text_file(file_path);
        case FileFormat::EML:    return eml_to_markdown(file_path, opts);
        case FileFormat::EMF:
        case FileFormat::WMF: {
            auto b = read_file_bytes(file_path);
            return metafile_to_markdown(
                fmt, reinterpret_cast<const uint8_t*>(b.data()), b.size(), opts);
        }
        default:
            if (is_archive_format(fmt))
                throw std::runtime_error("File is an archive; use convert_archive(): " + file_path);
            throw std::runtime_error("Unsupported file format: " + file_path);
    }
}

std::string convert(const void* data, size_t size, const std::string& name_hint,
                    ConvertOptions opts) {
    return convert_from_memory(static_cast<const uint8_t*>(data), size,
                               name_hint, opts);
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
        case FileFormat::EML:    return eml_to_markdown_chunks(file_path, opts);
        case FileFormat::EMF:
        case FileFormat::WMF: {
            auto b = read_file_bytes(file_path);
            return {metafile_to_chunk(
                fmt, reinterpret_cast<const uint8_t*>(b.data()), b.size(), opts)};
        }
        default:
            if (is_archive_format(fmt))
                throw std::runtime_error("File is an archive; use convert_archive(): " + file_path);
            throw std::runtime_error("Unsupported file format: " + file_path);
    }
}

void for_each_chunk(const std::string& file_path, const ConvertOptions& opts,
                    const PageSink& sink) {
    auto fmt = detect_format(file_path);
    switch (fmt) {
        case FileFormat::PDF:    pdf_to_markdown_chunks_stream(file_path, opts, sink); return;
        case FileFormat::OFFICE: office_to_markdown_chunks_stream(file_path, opts, sink); return;
        case FileFormat::HWPX:   hwpx_to_markdown_chunks_stream(file_path, opts, sink); return;
        case FileFormat::HWP:    hwp_to_markdown_chunks_stream(file_path, opts, sink); return;
        case FileFormat::TXT: {
            PageChunk chunk;
            chunk.page_number = 0;
            chunk.text = read_text_file(file_path);
            sink(std::move(chunk));
            return;
        }
        case FileFormat::EML:    eml_to_markdown_chunks_stream(file_path, opts, sink); return;
        case FileFormat::EMF:
        case FileFormat::WMF: {
            auto b = read_file_bytes(file_path);
            sink(metafile_to_chunk(
                fmt, reinterpret_cast<const uint8_t*>(b.data()), b.size(), opts));
            return;
        }
        default:
            if (is_archive_format(fmt))
                throw std::runtime_error("File is an archive; use convert_archive(): " + file_path);
            throw std::runtime_error("Unsupported file format: " + file_path);
    }
}

void for_each_chunk(const void* data, size_t size, const std::string& name_hint,
                    const ConvertOptions& opts, const PageSink& sink) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    auto fmt = detect_format_mem(bytes, size, name_hint);
    switch (fmt) {
        case FileFormat::PDF:
            pdf_to_markdown_chunks_mem_stream(bytes, size, opts, sink);
            return;
        case FileFormat::OFFICE:
            office_to_markdown_chunks_mem_stream(bytes, size, name_hint, opts, sink);
            return;
        case FileFormat::HWPX:
            hwpx_to_markdown_chunks_mem_stream(bytes, size, opts, sink);
            return;
        case FileFormat::HWP:
            hwp_to_markdown_chunks_mem_stream(bytes, size, opts, sink);
            return;
        case FileFormat::TXT: {
            PageChunk chunk;
            chunk.page_number = 0;
            chunk.text = util::plain_text_to_utf8(
                std::string(reinterpret_cast<const char*>(bytes), size));
            sink(std::move(chunk));
            return;
        }
        case FileFormat::EMF:
        case FileFormat::WMF:
            sink(metafile_to_chunk(fmt, bytes, size, opts));
            return;
        case FileFormat::EML:
            eml_to_markdown_chunks_mem_stream(bytes, size, opts, sink);
            return;
        default:
            if (is_archive_format(fmt))
                throw std::runtime_error("Nested archive must be walked, not converted: " + name_hint);
            throw std::runtime_error(
                "In-memory page streaming is not supported for this format: " + name_hint);
    }
}

// ── Archive conversion ──────────────────────────────────

bool is_archive_file(const std::string& file_path) {
    return is_archive_format(detect_format(file_path));
}

void convert_archive(const std::string& file_path, const MemberCallback& cb,
                     ConvertOptions opts) {
    auto fmt = detect_format(file_path);

    if (!is_archive_format(fmt)) {
        // Non-archive input: return a single-element result for the file
        std::string name = file_path;
        auto slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name = name.substr(slash + 1);

        MemberResult r;
        r.member_path = name;
        r.format = file_format_name(fmt);
        try {
            r.markdown = convert(file_path, opts);
        } catch (const std::exception& e) {
            r.error_code = fmt == FileFormat::UNKNOWN
                ? MemberErrorCode::UNSUPPORTED
                : MemberErrorCode::CONVERT_FAILED;
            r.error = e.what();
        }
        cb(std::move(r));
        return;
    }

    // Verify the file is openable so callers get a throw (not a silent
    // empty result) for a missing path.
    {
        std::ifstream f(file_path, std::ios::binary);
        if (!f) throw std::runtime_error("Cannot open file: " + file_path);
    }

    WalkBudget budget;
    walk_archive_path(file_path, fmt, "", 1, budget, opts, cb);
}

std::vector<MemberResult> convert_archive(const std::string& file_path,
                                          ConvertOptions opts) {
    std::vector<MemberResult> results;
    convert_archive(file_path, [&](MemberResult&& r) {
        results.push_back(std::move(r));
        return true;
    }, opts);
    return results;
}

} // namespace jdoc
