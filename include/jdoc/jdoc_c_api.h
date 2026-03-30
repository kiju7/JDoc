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

/* ── Free ─────────────────────────────────────────────────── */

void jdoc_free_string(char* str);
void jdoc_free_pages(JDocPage* pages, int count);

#ifdef __cplusplus
}
#endif

#endif /* JDOC_C_API_H */
