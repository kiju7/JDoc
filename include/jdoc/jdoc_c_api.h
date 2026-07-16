#ifndef JDOC_C_API_H
#define JDOC_C_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── Options ──────────────────────────────────────────────── */

typedef struct {
    int extract_images;              /* 0 = skip images, 1 = extract */
    const char* image_output_dir;    /* NULL = keep in memory only */
    unsigned int min_image_size;     /* skip images smaller than NxN (0 = no filter) */
    const int* pages;                /* page numbers to extract (NULL = all) */
    int page_count;                  /* length of pages array */
    int plaintext;                   /* 0 = markdown, 1 = plaintext */
    /* Archive limits (jdoc_convert_archive).
     * 0 = library default, -1 = unlimited (disables that guard —
     * only for trusted inputs; archive-bomb protection goes with it). */
    int max_archive_depth;               /* default 3 */
    long long max_member_bytes;          /* per-member uncompressed cap; default 512 MiB */
    long long max_total_bytes;           /* cumulative cap per call; default 64 GiB */
    int max_archive_entries;             /* default 200000 */
    int include_unsupported;             /* 1 = report unsupported members */
} JDocOptions;

/* Returns default options: no images, markdown, all pages, min_size=50. */
JDocOptions jdoc_default_options(void);

/* ── Image ────────────────────────────────────────────────── */

typedef struct {
    int page_number;
    char* name;                      /* e.g. "page1_img0" */
    unsigned int width;
    unsigned int height;
    char* data;                      /* raw image bytes (jpeg/png/bmp) */
    int data_size;
    char* format;                    /* "jpeg", "png", "bmp", ... */
    char* saved_path;                /* disk path if image_output_dir was set */
} JDocImage;

/* ── Page ─────────────────────────────────────────────────── */

typedef struct {
    int page_number;
    char* text;                      /* markdown or plaintext for this page */
    JDocImage* images;               /* images belonging to this page */
    int image_count;
} JDocPage;

/* ── API ──────────────────────────────────────────────────── */

/* Convert document to text. Returns malloc'd string.
 * opts may be NULL for defaults.
 * Returns NULL on error; message written to err_buf. */
char* jdoc_convert(const char* file_path, const JDocOptions* opts,
                   char* err_buf, int err_buf_size);

/* Convert document to per-page chunks with images.
 * opts may be NULL for defaults.
 * Returns malloc'd array of JDocPage; count written to *out_count.
 * Returns NULL on error. */
JDocPage* jdoc_convert_pages(const char* file_path, const JDocOptions* opts,
                              int* out_count,
                              char* err_buf, int err_buf_size);

/* ── Archive ──────────────────────────────────────────────── */

typedef struct {
    char* member_path;               /* "outer.zip/dir/report.hwp" (UTF-8) */
    char* format;                    /* "PDF", "HWP", "ZIP", ... */
    char* markdown;                  /* NULL on error */
    char* error;                     /* NULL on success */
    long long uncompressed_size;
} JDocMember;

/* Convert every supported document inside an archive without extracting
 * to disk. Per-member failures are recorded in that member's `error`.
 * Returns malloc'd array of JDocMember; count written to *out_count.
 * Returns NULL when the file cannot be opened (message in err_buf). */
JDocMember* jdoc_convert_archive(const char* file_path, const JDocOptions* opts,
                                 int* out_count,
                                 char* err_buf, int err_buf_size);

/* Convert a document held in memory. name_hint (e.g. original filename)
 * resolves extension-based format ambiguity; may be NULL. */
char* jdoc_convert_mem(const void* data, int size, const char* name_hint,
                       const JDocOptions* opts,
                       char* err_buf, int err_buf_size);

/* ── Free ─────────────────────────────────────────────────── */

void jdoc_free_string(char* str);
void jdoc_free_pages(JDocPage* pages, int count);
void jdoc_free_members(JDocMember* members, int count);

#ifdef __cplusplus
}
#endif

#endif /* JDOC_C_API_H */
