#include "jdoc/jdoc_c_api.h"
#include "jdoc/jdoc.h"
#include "jdoc/archive.h"

#include <cstdint>
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

static char* strdup_c(const std::string& s) {
    char* p = (char*)malloc(s.size() + 1);
    if (!p) return nullptr;
    memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

static jdoc::ConvertOptions to_cpp_opts(const JDocOptions* opts) {
    jdoc::ConvertOptions o;
    if (!opts) return o;
    o.extract_images = (opts->extract_images != 0);
    if (opts->image_output_dir)
        o.image_output_dir = opts->image_output_dir;
    o.min_image_size = opts->min_image_size;
    if (opts->pages && opts->page_count > 0)
        o.pages.assign(opts->pages, opts->pages + opts->page_count);
    if (opts->plaintext)
        o.output_format = jdoc::OutputFormat::PLAINTEXT;
    // 0 keeps the library default; negative disables the guard entirely.
    if (opts->max_archive_depth > 0)
        o.archive.max_depth = opts->max_archive_depth;
    else if (opts->max_archive_depth < 0)
        o.archive.max_depth = -1;  // unlimited nesting
    if (opts->max_member_bytes > 0)
        o.archive.max_member_bytes = (uint64_t)opts->max_member_bytes;
    else if (opts->max_member_bytes < 0)
        o.archive.max_member_bytes = UINT64_MAX;
    if (opts->max_total_bytes > 0)
        o.archive.max_total_bytes = (uint64_t)opts->max_total_bytes;
    else if (opts->max_total_bytes < 0)
        o.archive.max_total_bytes = UINT64_MAX;
    if (opts->max_archive_entries > 0)
        o.archive.max_entries = (uint32_t)opts->max_archive_entries;
    else if (opts->max_archive_entries < 0)
        o.archive.max_entries = UINT32_MAX;
    o.archive.include_unsupported = (opts->include_unsupported != 0);
    return o;
}

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

extern "C" {

JDocOptions jdoc_default_options(void) {
    JDocOptions o = {};
    o.min_image_size = 50;
    return o;
}

char* jdoc_convert(const char* file_path, const JDocOptions* opts,
                   char* err_buf, int err_buf_size) {
    if (!file_path) {
        set_error(err_buf, err_buf_size, "file_path is NULL");
        return nullptr;
    }
    try {
        auto cpp_opts = to_cpp_opts(opts);
        return strdup_c(jdoc::convert(file_path, cpp_opts));
    } catch (const std::exception& e) {
        set_error(err_buf, err_buf_size, e.what());
        return nullptr;
    } catch (...) {
        set_error(err_buf, err_buf_size, "unknown error");
        return nullptr;
    }
}

JDocPage* jdoc_convert_pages(const char* file_path, const JDocOptions* opts,
                              int* out_count,
                              char* err_buf, int err_buf_size) {
    if (!file_path || !out_count) {
        set_error(err_buf, err_buf_size, "file_path or out_count is NULL");
        return nullptr;
    }
    *out_count = 0;
    try {
        auto cpp_opts = to_cpp_opts(opts);
        auto chunks = jdoc::convert_chunks(file_path, cpp_opts);
        if (chunks.empty()) return nullptr;

        JDocPage* pages = (JDocPage*)calloc(chunks.size(), sizeof(JDocPage));
        if (!pages) {
            set_error(err_buf, err_buf_size, "memory allocation failed");
            return nullptr;
        }

        for (size_t i = 0; i < chunks.size(); i++) {
            auto& src = chunks[i];
            pages[i].page_number = src.page_number;
            pages[i].text = strdup_c(src.text);

            size_t n = src.images.size();
            if (n == 0) continue;

            pages[i].images = (JDocImage*)calloc(n, sizeof(JDocImage));
            if (!pages[i].images) continue;
            pages[i].image_count = (int)n;

            for (size_t j = 0; j < n; j++) {
                auto& si = src.images[j];
                auto& di = pages[i].images[j];
                di.page_number = si.page_number;
                di.name = strdup_c(si.name);
                di.width = si.width;
                di.height = si.height;
                di.format = strdup_c(si.format);
                di.saved_path = strdup_c(si.saved_path);
                di.data_size = (int)si.data.size();
                if (!si.data.empty()) {
                    di.data = (char*)malloc(si.data.size());
                    if (di.data)
                        memcpy(di.data, si.data.data(), si.data.size());
                }
            }
        }

        *out_count = (int)chunks.size();
        return pages;
    } catch (const std::exception& e) {
        set_error(err_buf, err_buf_size, e.what());
        return nullptr;
    } catch (...) {
        set_error(err_buf, err_buf_size, "unknown error");
        return nullptr;
    }
}

JDocMember* jdoc_convert_archive(const char* file_path, const JDocOptions* opts,
                                 int* out_count,
                                 char* err_buf, int err_buf_size) {
    if (!file_path || !out_count) {
        set_error(err_buf, err_buf_size, "file_path or out_count is NULL");
        return nullptr;
    }
    *out_count = 0;
    try {
        auto results = jdoc::convert_archive(file_path, to_cpp_opts(opts));
        if (results.empty()) return nullptr;

        JDocMember* members = (JDocMember*)calloc(results.size(), sizeof(JDocMember));
        if (!members) {
            set_error(err_buf, err_buf_size, "memory allocation failed");
            return nullptr;
        }
        for (size_t i = 0; i < results.size(); i++) {
            auto& src = results[i];
            members[i].member_path = strdup_c(src.member_path);
            members[i].format = strdup_c(src.format);
            members[i].uncompressed_size = (long long)src.uncompressed_size;
            if (src.ok())
                members[i].markdown = strdup_c(src.markdown);
            else
                members[i].error = strdup_c(src.error);
        }
        *out_count = (int)results.size();
        return members;
    } catch (const std::exception& e) {
        set_error(err_buf, err_buf_size, e.what());
        return nullptr;
    } catch (...) {
        set_error(err_buf, err_buf_size, "unknown error");
        return nullptr;
    }
}

char* jdoc_convert_mem(const void* data, int size, const char* name_hint,
                       const JDocOptions* opts,
                       char* err_buf, int err_buf_size) {
    if (!data || size <= 0) {
        set_error(err_buf, err_buf_size, "data is NULL or empty");
        return nullptr;
    }
    try {
        auto cpp_opts = to_cpp_opts(opts);
        return strdup_c(jdoc::convert(data, (size_t)size,
                                      name_hint ? name_hint : "", cpp_opts));
    } catch (const std::exception& e) {
        set_error(err_buf, err_buf_size, e.what());
        return nullptr;
    } catch (...) {
        set_error(err_buf, err_buf_size, "unknown error");
        return nullptr;
    }
}

void jdoc_free_string(char* str) { free(str); }

void jdoc_free_members(JDocMember* members, int count) {
    if (!members) return;
    for (int i = 0; i < count; i++) {
        free(members[i].member_path);
        free(members[i].format);
        free(members[i].markdown);
        free(members[i].error);
    }
    free(members);
}

void jdoc_free_pages(JDocPage* pages, int count) {
    if (!pages) return;
    for (int i = 0; i < count; i++) {
        free(pages[i].text);
        for (int j = 0; j < pages[i].image_count; j++) {
            free(pages[i].images[j].name);
            free(pages[i].images[j].data);
            free(pages[i].images[j].format);
            free(pages[i].images[j].saved_path);
        }
        free(pages[i].images);
    }
    free(pages);
}

} // extern "C"
