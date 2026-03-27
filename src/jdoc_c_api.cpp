#include "jdoc/jdoc_c_api.h"
#include "jdoc/jdoc.h"

#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

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
        jdoc::ConvertOptions opts;
        opts.output_format = jdoc::OutputFormat::PLAINTEXT;
        opts.extract_tables = true;
        return strdup_cpp(jdoc::convert(file_path, opts));
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
        jdoc::ConvertOptions opts;
        opts.extract_images = true;
        auto chunks = jdoc::convert_chunks(file_path, opts);
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
        jdoc::ConvertOptions opts;
        opts.extract_images = true;
        opts.extract_tables = true;

        auto chunks = jdoc::convert_chunks(file_path, opts);

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
        jdoc::ConvertOptions opts;
        opts.extract_images = true;
        opts.extract_tables = true;

        auto chunks = jdoc::convert_chunks(file_path, opts);

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
