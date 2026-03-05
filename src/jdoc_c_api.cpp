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
#include <algorithm>

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

    return FileFormat::UNKNOWN;
}

static void set_error(char* err_buf, int err_buf_size, const char* msg) {
    if (err_buf && err_buf_size > 0) {
        strncpy(err_buf, msg, err_buf_size - 1);
        err_buf[err_buf_size - 1] = '\0';
    }
}

static char* strdup_cpp(const std::string& s) {
    char* p = (char*)malloc(s.size() + 1);
    if (p) {
        memcpy(p, s.c_str(), s.size() + 1);
    }
    return p;
}

extern "C" {

char* jdoc_extract_text(const char* file_path, char* err_buf, int err_buf_size) {
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

        std::vector<jdoc::PageChunk> chunks;
        switch (fmt) {
            case FileFormat::PDF:    chunks = jdoc::pdf_to_markdown_chunks(path, opts); break;
            case FileFormat::HWPX:   chunks = jdoc::hwpx_to_markdown_chunks(path, opts); break;
            case FileFormat::HWP:    chunks = jdoc::hwp_to_markdown_chunks(path, opts); break;
            case FileFormat::OFFICE: chunks = jdoc::office_to_markdown_chunks(path, opts); break;
            default: break;
        }

        // Collect all images
        std::vector<jdoc::ImageData> all_images;
        for (auto& chunk : chunks) {
            for (auto& img : chunk.images) {
                all_images.push_back(std::move(img));
            }
        }

        if (all_images.empty()) {
            *out_images = nullptr;
            return 0;
        }

        JDocImage* images = (JDocImage*)calloc(all_images.size(), sizeof(JDocImage));
        if (!images) {
            set_error(err_buf, err_buf_size, "memory allocation failed");
            return -1;
        }

        for (size_t i = 0; i < all_images.size(); i++) {
            auto& src = all_images[i];
            images[i].page_number = src.page_number;
            images[i].name = strdup_cpp(src.name);
            images[i].width = src.width;
            images[i].height = src.height;
            images[i].format = strdup_cpp(src.format);
            images[i].data_size = (int)src.data.size();
            if (!src.data.empty()) {
                images[i].data = (char*)malloc(src.data.size());
                if (images[i].data) {
                    memcpy(images[i].data, src.data.data(), src.data.size());
                }
            }
        }

        *out_images = images;
        return (int)all_images.size();
    } catch (const std::exception& e) {
        set_error(err_buf, err_buf_size, e.what());
        return -1;
    } catch (...) {
        set_error(err_buf, err_buf_size, "unknown error");
        return -1;
    }
}

void jdoc_free_string(char* str) {
    free(str);
}

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
