#ifndef JDOC_C_API_H
#define JDOC_C_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* Extract plain text from a document file.
 * Returns a malloc'd string (caller must call jdoc_free_string).
 * Returns NULL on error; error message written to err_buf if non-NULL. */
char* jdoc_extract_text(const char* file_path, char* err_buf, int err_buf_size);

/* Extract images from a document file.
 * Images are returned as a contiguous buffer of JDocImage structs.
 * Returns the number of images, or -1 on error.
 * Caller must call jdoc_free_images to release memory. */
typedef struct {
    int page_number;
    char* name;
    unsigned int width;
    unsigned int height;
    char* data;
    int data_size;
    char* format;
} JDocImage;

int jdoc_extract_images(const char* file_path, JDocImage** out_images,
                        char* err_buf, int err_buf_size);

/* Extract text and images in a single parse pass.
 * Returns malloc'd text (caller must call jdoc_free_string).
 * Images are written to *out_images, count to *out_image_count.
 * Returns NULL on error. */
char* jdoc_extract_all(const char* file_path,
                       JDocImage** out_images, int* out_image_count,
                       char* err_buf, int err_buf_size);

/* Per-page text chunk returned by jdoc_extract_all_paged. */
typedef struct {
    int page_number;
    char* text;
} JDocPageText;

/* Extract text (per-page), images in a single parse pass.
 * Returns malloc'd concatenated text. Per-page texts are written to *out_pages,
 * count to *out_page_count. Caller must call jdoc_free_page_texts to release.
 * Returns NULL on error. */
char* jdoc_extract_all_paged(const char* file_path,
                              JDocImage** out_images, int* out_image_count,
                              JDocPageText** out_pages, int* out_page_count,
                              char* err_buf, int err_buf_size);

void jdoc_free_page_texts(JDocPageText* pages, int count);
void jdoc_free_string(char* str);
void jdoc_free_images(JDocImage* images, int count);

#ifdef __cplusplus
}
#endif

#endif /* JDOC_C_API_H */
