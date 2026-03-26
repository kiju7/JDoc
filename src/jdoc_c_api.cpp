#include "jdoc/jdoc_c_api.h"
#include "jdoc/pdf.h"
#include "jdoc/office.h"
#include "jdoc/hwpx.h"
#include "jdoc/hwp.h"
#include "jdoc/types.h"
#include "zip_reader.h"
#include "legacy/ole_reader.h"

#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

enum class FileFormat { PDF, OFFICE, HWP, HWPX, UNKNOWN };


static std::string get_ext(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot);
    for (auto& c : ext) c = std::tolower(static_cast<unsigned char>(c));
    return ext;
}

static FileFormat detect_format(const std::string& path) {
    // Magic bytes first — reliable regardless of file extension
    unsigned char magic[8] = {};
    std::ifstream f(path, std::ios::binary);
    if (f) {
        f.read(reinterpret_cast<char*>(magic), 8);
        f.close();

        if (magic[0] == '%' && magic[1] == 'P' && magic[2] == 'D' && magic[3] == 'F')
            return FileFormat::PDF;

        if (magic[0] == 'P' && magic[1] == 'K' && magic[2] == 0x03 && magic[3] == 0x04) {
            jdoc::ZipReader zip(path);
            if (zip.is_open()) {
                if (zip.has_entry("Contents/section0.xml") ||
                    zip.has_entry("META-INF/container.xml"))
                    return FileFormat::HWPX;
            }
            return FileFormat::OFFICE;
        }

        if (magic[0] == 0xD0 && magic[1] == 0xCF && magic[2] == 0x11 && magic[3] == 0xE0) {
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

        if (magic[0] == '<')
            return FileFormat::OFFICE; // HTML
    }

    // Extension fallback
    std::string ext = get_ext(path);
    if (ext == ".pdf") return FileFormat::PDF;
    if (ext == ".hwpx") return FileFormat::HWPX;
    if (ext == ".hwp") return FileFormat::HWP;
    if (ext == ".docx" || ext == ".xlsx" || ext == ".pptx" ||
        ext == ".doc" || ext == ".xls" || ext == ".ppt" || ext == ".rtf" ||
        ext == ".html" || ext == ".htm" || ext == ".xlsb")
        return FileFormat::OFFICE;

    return FileFormat::UNKNOWN;
}

static void set_error(char* buf, int buf_size, const char* msg) {
    if (buf && buf_size > 0) {
        strncpy(buf, msg, buf_size - 1);
        buf[buf_size - 1] = '\0';
    }
}

static char* strdup_cpp(const std::string& s) {
    char* p = (char*)malloc(s.size() + 1);
    if (!p) {
        p = (char*)malloc(1);
        if (p) p[0] = '\0';
        return p;
    }
    memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

static std::vector<jdoc::PageChunk> parse_chunks(const std::string& path,
                                                   FileFormat fmt,
                                                   const jdoc::ConvertOptions& opts) {
    switch (fmt) {
        case FileFormat::PDF:    return jdoc::pdf_to_markdown_chunks(path, opts);
        case FileFormat::HWPX:   return jdoc::hwpx_to_markdown_chunks(path, opts);
        case FileFormat::HWP:    return jdoc::hwp_to_markdown_chunks(path, opts);
        case FileFormat::OFFICE: return jdoc::office_to_markdown_chunks(path, opts);
        default:                 return {};
    }
}

// Concatenate chunk texts with pre-reserved buffer
static std::string concat_text(const std::vector<jdoc::PageChunk>& chunks) {
    size_t total = 0;
    for (auto& c : chunks) total += c.text.size() + 1;

    std::string text;
    text.reserve(total);
    for (auto& c : chunks) {
        if (!text.empty() && !c.text.empty()) text += '\n';
        text += c.text;
    }
    return text;
}

// Build C image array from chunks. Returns count, -1 on alloc failure.
static int build_images(const std::vector<jdoc::PageChunk>& chunks,
                        JDocImage** out, char* err_buf, int err_buf_size) {
    size_t n = 0;
    for (auto& c : chunks) n += c.images.size();

    if (n == 0) {
        *out = nullptr;
        return 0;
    }

    JDocImage* arr = (JDocImage*)calloc(n, sizeof(JDocImage));
    if (!arr) {
        set_error(err_buf, err_buf_size, "memory allocation failed");
        return -1;
    }

    size_t idx = 0;
    for (auto& c : chunks) {
        for (auto& src : c.images) {
            arr[idx].page_number = src.page_number;
            arr[idx].name = strdup_cpp(src.name);
            arr[idx].width = src.width;
            arr[idx].height = src.height;
            arr[idx].format = strdup_cpp(src.format);
            arr[idx].data_size = (int)src.data.size();
            if (!src.data.empty()) {
                arr[idx].data = (char*)malloc(src.data.size());
                if (arr[idx].data)
                    memcpy(arr[idx].data, src.data.data(), src.data.size());
            }
            idx++;
        }
    }

    *out = arr;
    return (int)n;
}

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

extern "C" {

char* jdoc_extract_text(const char* file_path, char* err_buf, int err_buf_size) {
    if (!file_path) {
        set_error(err_buf, err_buf_size, "file_path is NULL");
        return nullptr;
    }
    try {
        std::string path(file_path);
        auto fmt = detect_format(path);
        if (fmt == FileFormat::UNKNOWN) {
            set_error(err_buf, err_buf_size, "unsupported file format");
            return nullptr;
        }

        jdoc::ConvertOptions opts;
        opts.output_format = jdoc::OutputFormat::PLAINTEXT;
        opts.extract_tables = true;

        std::string text;
        switch (fmt) {
            case FileFormat::PDF:    text = jdoc::pdf_to_markdown(path, opts); break;
            case FileFormat::HWPX:   text = jdoc::hwpx_to_markdown(path, opts); break;
            case FileFormat::HWP:    text = jdoc::hwp_to_markdown(path, opts); break;
            case FileFormat::OFFICE: text = jdoc::office_to_markdown(path, opts); break;
            default: break;
        }
        return strdup_cpp(text);
    } catch (const std::exception& e) {
        set_error(err_buf, err_buf_size, e.what());
        return nullptr;
    } catch (...) {
        set_error(err_buf, err_buf_size, "unknown error");
        return nullptr;
    }
}

int jdoc_extract_images(const char* file_path, JDocImage** out_images,
                        char* err_buf, int err_buf_size) {
    if (!file_path) {
        set_error(err_buf, err_buf_size, "file_path is NULL");
        return -1;
    }
    if (!out_images) {
        set_error(err_buf, err_buf_size, "out_images is NULL");
        return -1;
    }
    try {
        std::string path(file_path);
        auto fmt = detect_format(path);
        if (fmt == FileFormat::UNKNOWN) {
            set_error(err_buf, err_buf_size, "unsupported file format");
            return -1;
        }

        jdoc::ConvertOptions opts;
        opts.extract_images = true;
        opts.page_chunks = true;

        auto chunks = parse_chunks(path, fmt, opts);
        return build_images(chunks, out_images, err_buf, err_buf_size);
    } catch (const std::exception& e) {
        set_error(err_buf, err_buf_size, e.what());
        return -1;
    } catch (...) {
        set_error(err_buf, err_buf_size, "unknown error");
        return -1;
    }
}

char* jdoc_extract_all(const char* file_path,
                       JDocImage** out_images, int* out_image_count,
                       char* err_buf, int err_buf_size) {
    if (!file_path) {
        set_error(err_buf, err_buf_size, "file_path is NULL");
        return nullptr;
    }
    if (!out_images || !out_image_count) {
        set_error(err_buf, err_buf_size, "output pointer is NULL");
        return nullptr;
    }
    try {
        std::string path(file_path);
        auto fmt = detect_format(path);
        if (fmt == FileFormat::UNKNOWN) {
            set_error(err_buf, err_buf_size, "unsupported file format");
            return nullptr;
        }

        jdoc::ConvertOptions opts;
        opts.extract_images = true;
        opts.extract_tables = true;
        opts.page_chunks = true;

        auto chunks = parse_chunks(path, fmt, opts);

        std::string text = concat_text(chunks);

        int img_count = build_images(chunks, out_images, err_buf, err_buf_size);
        if (img_count < 0) return nullptr;
        *out_image_count = img_count;

        return strdup_cpp(text);
    } catch (const std::exception& e) {
        set_error(err_buf, err_buf_size, e.what());
        return nullptr;
    } catch (...) {
        set_error(err_buf, err_buf_size, "unknown error");
        return nullptr;
    }
}

char* jdoc_extract_all_paged(const char* file_path,
                              JDocImage** out_images, int* out_image_count,
                              JDocPageText** out_pages, int* out_page_count,
                              char* err_buf, int err_buf_size) {
    if (!file_path) {
        set_error(err_buf, err_buf_size, "file_path is NULL");
        return nullptr;
    }
    if (!out_images || !out_image_count || !out_pages || !out_page_count) {
        set_error(err_buf, err_buf_size, "output pointer is NULL");
        return nullptr;
    }
    try {
        std::string path(file_path);
        auto fmt = detect_format(path);
        if (fmt == FileFormat::UNKNOWN) {
            set_error(err_buf, err_buf_size, "unsupported file format");
            return nullptr;
        }

        jdoc::ConvertOptions opts;
        opts.extract_images = true;
        opts.extract_tables = true;
        opts.page_chunks = true;

        auto chunks = parse_chunks(path, fmt, opts);

        std::string text = concat_text(chunks);

        // Build per-page text
        if (chunks.empty()) {
            *out_pages = nullptr;
            *out_page_count = 0;
        } else {
            JDocPageText* pages = (JDocPageText*)calloc(chunks.size(), sizeof(JDocPageText));
            if (!pages) {
                set_error(err_buf, err_buf_size, "memory allocation failed");
                return nullptr;
            }
            for (size_t i = 0; i < chunks.size(); i++) {
                pages[i].page_number = chunks[i].page_number;
                pages[i].text = strdup_cpp(chunks[i].text);
            }
            *out_pages = pages;
            *out_page_count = (int)chunks.size();
        }

        // Build images
        int img_count = build_images(chunks, out_images, err_buf, err_buf_size);
        if (img_count < 0) {
            jdoc_free_page_texts(*out_pages, *out_page_count);
            *out_pages = nullptr;
            *out_page_count = 0;
            return nullptr;
        }
        *out_image_count = img_count;

        return strdup_cpp(text);
    } catch (const std::exception& e) {
        set_error(err_buf, err_buf_size, e.what());
        return nullptr;
    } catch (...) {
        set_error(err_buf, err_buf_size, "unknown error");
        return nullptr;
    }
}

void jdoc_free_page_texts(JDocPageText* pages, int count) {
    if (!pages) return;
    for (int i = 0; i < count; i++) free(pages[i].text);
    free(pages);
}

void jdoc_free_string(char* str) { free(str); }

void jdoc_free_images(JDocImage* images, int count) {
    if (!images) return;
    for (int i = 0; i < count; i++) {
        free(images[i].name);
        free(images[i].data);
        free(images[i].format);
    }
    free(images);
}

} // extern "C"
