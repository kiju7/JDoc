#include "jdoc/jdoc_c_api.h"
#include "jdoc/jdoc.h"
#include "jdoc/archive.h"
#include "jdoc/detect.h"

#include <cstdint>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

static void set_error(char* buf, int buf_size, const char* msg) {
    if (buf && buf_size > 0) {
        const size_t size = static_cast<size_t>(buf_size);
        strncpy(buf, msg, size - 1);
        buf[size - 1] = '\0';
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
    o.tables = (opts->tables != 0);
    o.images = (opts->images != 0);
    if (opts->image_dir)
        o.image_dir = opts->image_dir;
    if (opts->image_ref_prefix)
        o.image_ref_prefix = opts->image_ref_prefix;
    o.min_image_size = opts->min_image_size;
    if (opts->pages && opts->page_count > 0)
        o.pages.assign(opts->pages, opts->pages + opts->page_count);
    if (opts->format && (strcmp(opts->format, "text") == 0 ||
                         strcmp(opts->format, "plaintext") == 0))
        o.format = jdoc::OutputFormat::PLAINTEXT;
    // Archive limits: 0 keeps the library default; negative disables the
    // guard.
    if (opts->max_depth > 0)
        o.archive.max_depth = opts->max_depth;
    else if (opts->max_depth < 0)
        o.archive.max_depth = -1;  // unlimited nesting
    if (opts->max_member_bytes > 0)
        o.archive.max_member_bytes = (uint64_t)opts->max_member_bytes;
    else if (opts->max_member_bytes < 0)
        o.archive.max_member_bytes = UINT64_MAX;
    if (opts->max_total_bytes > 0)
        o.archive.max_total_bytes = (uint64_t)opts->max_total_bytes;
    else if (opts->max_total_bytes < 0)
        o.archive.max_total_bytes = UINT64_MAX;
    if (opts->max_entries > 0)
        o.archive.max_entries = (uint32_t)opts->max_entries;
    else if (opts->max_entries < 0)
        o.archive.max_entries = UINT32_MAX;
    if (opts->max_ratio > 0)
        o.archive.max_ratio = (uint32_t)opts->max_ratio;
    else if (opts->max_ratio < 0)
        o.archive.max_ratio = 0;  // ratio check off
    o.archive.include_unsupported = (opts->include_unsupported != 0);
    return o;
}

// Marshal one C++ PageChunk into a caller-owned JDocPage (deep copy). Inner
// allocations are released by free_page_inner / jdoc_free_pages.
// Release a JDocPage's inner allocations (not the page struct itself).
static void free_page_inner(JDocPage& page) {
    free(page.text);
    for (int i = 0; i < page.image_count; ++i) {
        free(page.images[i].name);
        free(page.images[i].data);
        free(page.images[i].format);
        free(page.images[i].saved_path);
    }
    free(page.images);
    page = {};
}

static void free_pages(JDocPage* pages, size_t count) {
    if (!pages) return;
    for (size_t i = 0; i < count; ++i)
        free_page_inner(pages[i]);
    free(pages);
}

static void free_members(JDocMember* members, size_t count) {
    if (!members) return;
    for (size_t i = 0; i < count; ++i) {
        free(members[i].member_path);
        free(members[i].format);
        free(members[i].markdown);
        free(members[i].error);
    }
    free(members);
}

static bool fill_page(JDocPage& dst, const jdoc::PageChunk& src) {
    dst = {};
    dst.page_number = src.page_number;
    dst.text = strdup_c(src.text);
    if (!dst.text) return false;

    size_t n = src.images.size();
    if (n == 0) return true;
    if (n > INT_MAX) {
        free_page_inner(dst);
        return false;
    }

    dst.images = (JDocImage*)calloc(n, sizeof(JDocImage));
    if (!dst.images) {
        free_page_inner(dst);
        return false;
    }
    dst.image_count = (int)n;

    for (size_t j = 0; j < n; j++) {
        auto& si = src.images[j];
        auto& di = dst.images[j];
        di.page_number = si.page_number;
        di.name = strdup_c(si.name);
        di.width = si.width;
        di.height = si.height;
        di.format = strdup_c(si.format);
        di.saved_path = strdup_c(si.saved_path);
        if (!di.name || !di.format || !di.saved_path ||
            si.data.size() > INT_MAX) {
            free_page_inner(dst);
            return false;
        }
        di.data_size = (int)si.data.size();
        if (!si.data.empty()) {
            di.data = (char*)malloc(si.data.size());
            if (!di.data) {
                free_page_inner(dst);
                return false;
            }
            memcpy(di.data, si.data.data(), si.data.size());
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

extern "C" {

JDocOptions jdoc_default_options(void) {
    JDocOptions o = {};
    o.tables = 1;
    o.min_image_size = 50;
    return o;
}

char* jdoc_convert(const char* file_path, const JDocOptions* opts,
                   char* err_buf, int err_buf_size) {
    set_error(err_buf, err_buf_size, "");
    if (!file_path) {
        set_error(err_buf, err_buf_size, "file_path is NULL");
        return nullptr;
    }
    try {
        auto cpp_opts = to_cpp_opts(opts);
        char* result = strdup_c(jdoc::convert(file_path, cpp_opts));
        if (!result)
            set_error(err_buf, err_buf_size, "memory allocation failed");
        return result;
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
    set_error(err_buf, err_buf_size, "");
    if (!file_path || !out_count) {
        set_error(err_buf, err_buf_size, "file_path or out_count is NULL");
        return nullptr;
    }
    *out_count = 0;
    try {
        auto cpp_opts = to_cpp_opts(opts);
        auto chunks = jdoc::convert_chunks(file_path, cpp_opts);
        if (chunks.empty()) return nullptr;
        if (chunks.size() > INT_MAX) {
            set_error(err_buf, err_buf_size, "too many pages");
            return nullptr;
        }

        JDocPage* pages = (JDocPage*)calloc(chunks.size(), sizeof(JDocPage));
        if (!pages) {
            set_error(err_buf, err_buf_size, "memory allocation failed");
            return nullptr;
        }

        for (size_t i = 0; i < chunks.size(); i++) {
            if (!fill_page(pages[i], chunks[i])) {
                free_pages(pages, i + 1);
                set_error(err_buf, err_buf_size, "memory allocation failed");
                return nullptr;
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

int jdoc_convert_pages_stream(const char* file_path, const JDocOptions* opts,
                              JDocPageCallback cb, void* userdata,
                              char* err_buf, int err_buf_size) {
    set_error(err_buf, err_buf_size, "");
    if (!file_path || !cb) {
        set_error(err_buf, err_buf_size, "file_path or callback is NULL");
        return -1;
    }
    try {
        auto cpp_opts = to_cpp_opts(opts);
        bool allocation_failed = false;
        jdoc::for_each_chunk(file_path, cpp_opts, [&](jdoc::PageChunk&& chunk) {
            JDocPage page = {};
            if (!fill_page(page, chunk)) {
                allocation_failed = true;
                return false;
            }
            int keep_going = cb(&page, userdata);
            free_page_inner(page);
            return keep_going != 0;
        });
        if (allocation_failed) {
            set_error(err_buf, err_buf_size, "memory allocation failed");
            return -1;
        }
        return 0;
    } catch (const std::exception& e) {
        set_error(err_buf, err_buf_size, e.what());
        return -1;
    } catch (...) {
        set_error(err_buf, err_buf_size, "unknown error");
        return -1;
    }
}

JDocMember* jdoc_convert_archive(const char* file_path, const JDocOptions* opts,
                                 int* out_count,
                                 char* err_buf, int err_buf_size) {
    set_error(err_buf, err_buf_size, "");
    if (!file_path || !out_count) {
        set_error(err_buf, err_buf_size, "file_path or out_count is NULL");
        return nullptr;
    }
    *out_count = 0;
    try {
        auto results = jdoc::convert_archive(file_path, to_cpp_opts(opts));
        if (results.empty()) return nullptr;
        if (results.size() > INT_MAX) {
            set_error(err_buf, err_buf_size, "too many archive members");
            return nullptr;
        }

        JDocMember* members = (JDocMember*)calloc(results.size(), sizeof(JDocMember));
        if (!members) {
            set_error(err_buf, err_buf_size, "memory allocation failed");
            return nullptr;
        }
        for (size_t i = 0; i < results.size(); i++) {
            auto& src = results[i];
            members[i].member_path = strdup_c(src.member_path);
            members[i].format = strdup_c(src.format);
            members[i].error_code = (int)src.error_code;
            members[i].uncompressed_size = (long long)src.uncompressed_size;
            if (src.ok())
                members[i].markdown = strdup_c(src.markdown);
            else
                members[i].error = strdup_c(src.error);
            if (!members[i].member_path || !members[i].format ||
                (src.ok() ? !members[i].markdown : !members[i].error)) {
                free_members(members, i + 1);
                set_error(err_buf, err_buf_size, "memory allocation failed");
                return nullptr;
            }
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
    set_error(err_buf, err_buf_size, "");
    if (!data || size <= 0) {
        set_error(err_buf, err_buf_size, "data is NULL or empty");
        return nullptr;
    }
    try {
        auto cpp_opts = to_cpp_opts(opts);
        char* result = strdup_c(jdoc::convert(
            data, (size_t)size, name_hint ? name_hint : "", cpp_opts));
        if (!result)
            set_error(err_buf, err_buf_size, "memory allocation failed");
        return result;
    } catch (const std::exception& e) {
        set_error(err_buf, err_buf_size, e.what());
        return nullptr;
    } catch (...) {
        set_error(err_buf, err_buf_size, "unknown error");
        return nullptr;
    }
}

static int fill_format_info(const jdoc::FormatInfo& src, JDocFormatInfo* out) {
    *out = {};
    out->format = strdup_c(src.format);
    out->extension = strdup_c(src.extension);
    out->mime = strdup_c(src.mime);
    if (!out->format || !out->extension || !out->mime) {
        free(out->format);
        free(out->extension);
        free(out->mime);
        *out = {};
        return -1;
    }
    out->category = (int)src.category;
    out->convertible = src.convertible ? 1 : 0;
    return 0;
}

int jdoc_detect(const char* file_path, JDocFormatInfo* out,
                char* err_buf, int err_buf_size) {
    set_error(err_buf, err_buf_size, "");
    if (!file_path || !out) {
        set_error(err_buf, err_buf_size, "file_path or out is NULL");
        return -1;
    }
    try {
        int result = fill_format_info(jdoc::detect(file_path), out);
        if (result != 0)
            set_error(err_buf, err_buf_size, "memory allocation failed");
        return result;
    } catch (const std::exception& e) {
        set_error(err_buf, err_buf_size, e.what());
        return -1;
    } catch (...) {
        set_error(err_buf, err_buf_size, "unknown error");
        return -1;
    }
}

int jdoc_detect_mem(const void* data, int size, const char* name_hint,
                    JDocFormatInfo* out, char* err_buf, int err_buf_size) {
    set_error(err_buf, err_buf_size, "");
    if (!data || size <= 0 || !out) {
        set_error(err_buf, err_buf_size, "data is NULL/empty or out is NULL");
        return -1;
    }
    try {
        int result = fill_format_info(
            jdoc::detect(data, (size_t)size, name_hint ? name_hint : ""), out);
        if (result != 0)
            set_error(err_buf, err_buf_size, "memory allocation failed");
        return result;
    } catch (const std::exception& e) {
        set_error(err_buf, err_buf_size, e.what());
        return -1;
    } catch (...) {
        set_error(err_buf, err_buf_size, "unknown error");
        return -1;
    }
}

void jdoc_free_string(char* str) { free(str); }

void jdoc_free_format_info(JDocFormatInfo* info) {
    if (!info) return;
    free(info->format);
    free(info->extension);
    free(info->mime);
    info->format = nullptr;
    info->extension = nullptr;
    info->mime = nullptr;
}

void jdoc_free_members(JDocMember* members, int count) {
    free_members(members, count > 0 ? static_cast<size_t>(count) : 0);
}

void jdoc_free_pages(JDocPage* pages, int count) {
    free_pages(pages, count > 0 ? static_cast<size_t>(count) : 0);
}

} // extern "C"
