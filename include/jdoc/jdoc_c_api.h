#ifndef JDOC_C_API_H
#define JDOC_C_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── Options ──────────────────────────────────────────────── */

/* Field names match the C++ ConvertOptions / Python keywords one-to-one.
 * Initialize with jdoc_default_options(); a zero-filled struct is NOT a
 * valid default (tables would be off). */
typedef struct {
    /* conversion */
    int tables;              /* 1 = tables as markdown tables (default 1) */
    int images;              /* 0 = skip images, 1 = extract */
    const char* image_dir;           /* image output directory; NULL = keep in memory */
    const char* image_ref_prefix;    /* prepended to image refs in markdown; NULL = none */
    unsigned int min_image_size;     /* skip images smaller than NxN (0 = no filter) */
    const int* pages;                /* page numbers to extract (NULL = all) */
    int page_count;                  /* length of pages array */
    const char* format;              /* NULL or "markdown" (default), "text" */
    /* Archive limits (jdoc_convert_archive).
     * 0 = library default, -1 = unlimited (disables that guard —
     * only for trusted inputs; archive-bomb protection goes with it). */
    int max_depth;                   /* nesting depth; default 3 */
    long long max_member_bytes;      /* per-member uncompressed cap; default 512 MiB */
    long long max_total_bytes;       /* cumulative cap per call; default 64 GiB */
    int max_entries;                 /* members visited; default 200000 */
    int max_ratio;                   /* bomb-suspect compression ratio; default 10000 */
    int include_unsupported;         /* 1 = report unsupported members */
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
    char* saved_path;                /* disk path if image_dir was set */
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

/* Streaming page callback. Invoked once per page as the document is converted.
 * `page` is borrowed and valid only for the duration of the call — copy out
 * what you need; do NOT free it. Return nonzero to continue, 0 to stop early.
 * `userdata` is passed through from jdoc_convert_pages_stream. */
typedef int (*JDocPageCallback)(const JDocPage* page, void* userdata);

/* Streaming variant of jdoc_convert_pages: pages are delivered one at a time to
 * `cb` and never accumulated, so peak memory tracks a single page. The bindings
 * wrap this as their native lazy iterator. opts may be NULL for defaults.
 * Returns 0 on success, -1 on error (message in err_buf). */
int jdoc_convert_pages_stream(const char* file_path, const JDocOptions* opts,
                              JDocPageCallback cb, void* userdata,
                              char* err_buf, int err_buf_size);

/* ── Archive ──────────────────────────────────────────────── */

/* Machine-readable member failure classification (JDocMember.error_code).
 * The *_LIMIT codes name the JDocOptions field to raise; the remaining
 * codes are input problems no limit change can fix. Values mirror the
 * C++ jdoc::MemberErrorCode enum. */
typedef enum {
    JDOC_MEMBER_OK = 0,
    JDOC_MEMBER_ERR_MEMBER_LIMIT = 1,   /* raise max_member_bytes */
    JDOC_MEMBER_ERR_RATIO_LIMIT = 2,    /* suspected archive bomb (max_ratio) */
    JDOC_MEMBER_ERR_TOTAL_LIMIT = 3,    /* raise max_total_bytes; walk stopped */
    JDOC_MEMBER_ERR_ENTRY_LIMIT = 4,    /* raise max_entries; walk stopped */
    JDOC_MEMBER_ERR_DEPTH_LIMIT = 5,    /* raise max_depth */
    JDOC_MEMBER_ERR_ENCRYPTED = 6,      /* encrypted member or archive */
    JDOC_MEMBER_ERR_UNSUPPORTED = 7,    /* format jdoc cannot convert */
    JDOC_MEMBER_ERR_CORRUPT = 8,        /* container/member data unreadable */
    JDOC_MEMBER_ERR_CONVERT_FAILED = 9, /* parser rejected the document */
} JDocMemberErrorCode;

typedef struct {
    char* member_path;               /* "outer.zip/dir/report.hwp" (UTF-8) */
    char* format;                    /* "PDF", "HWP", "ZIP", ... */
    char* markdown;                  /* NULL on error */
    char* error;                     /* NULL on success */
    int error_code;                  /* JDocMemberErrorCode */
    long long uncompressed_size;
} JDocMember;

/* Convert every supported document inside an archive without extracting
 * to disk. Per-member failures are recorded in that member's `error`.
 * Returns malloc'd array of JDocMember; count written to *out_count.
 * Returns NULL when the file cannot be opened (message in err_buf).
 * err_buf is cleared on entry: a NULL return with an EMPTY err_buf means
 * the archive simply had no reportable members, not an error. */
JDocMember* jdoc_convert_archive(const char* file_path, const JDocOptions* opts,
                                 int* out_count,
                                 char* err_buf, int err_buf_size);

/* Convert a document held in memory. name_hint (e.g. original filename)
 * resolves extension-based format ambiguity; may be NULL. */
char* jdoc_convert_mem(const void* data, int size, const char* name_hint,
                       const JDocOptions* opts,
                       char* err_buf, int err_buf_size);

/* ── Detect ───────────────────────────────────────────────── */

/* Coarse format family (JDocFormatInfo.category). Mirrors the C++
 * jdoc::FormatCategory enum. */
typedef enum {
    JDOC_CAT_DOCUMENT = 0,
    JDOC_CAT_SPREADSHEET = 1,
    JDOC_CAT_PRESENTATION = 2,
    JDOC_CAT_ARCHIVE = 3,
    JDOC_CAT_EMAIL = 4,
    JDOC_CAT_TEXT = 5,
    JDOC_CAT_IMAGE = 6,
    JDOC_CAT_UNKNOWN = 7,
} JDocFormatCategory;

typedef struct {
    char* format;      /* canonical name: "PDF", "DOCX", "PNG", "UNKNOWN" */
    int   category;    /* JDocFormatCategory */
    char* extension;   /* canonical extension incl. dot, e.g. ".pdf" */
    char* mime;        /* e.g. "application/pdf"; "" if unknown */
    int   convertible; /* 1 = jdoc can extract text (convert/convert_archive) */
} JDocFormatInfo;

/* Detect a file's format without running a full extraction.
 * Fills *out (caller owns the strings; release with jdoc_free_format_info).
 * Returns 0 on success, -1 on error (message in err_buf). An unrecognized
 * file still succeeds with format "UNKNOWN". */
int jdoc_detect(const char* file_path, JDocFormatInfo* out,
                char* err_buf, int err_buf_size);

/* In-memory variant. name_hint (may be NULL) resolves extension ambiguity. */
int jdoc_detect_mem(const void* data, int size, const char* name_hint,
                    JDocFormatInfo* out, char* err_buf, int err_buf_size);

/* ── Free ─────────────────────────────────────────────────── */

void jdoc_free_string(char* str);
void jdoc_free_format_info(JDocFormatInfo* info);
void jdoc_free_pages(JDocPage* pages, int count);
void jdoc_free_members(JDocMember* members, int count);

#ifdef __cplusplus
}
#endif

#endif /* JDOC_C_API_H */
