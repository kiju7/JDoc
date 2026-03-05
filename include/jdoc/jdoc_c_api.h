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

void jdoc_free_string(char* str);
void jdoc_free_images(JDocImage* images, int count);

#ifdef __cplusplus
}
#endif

#endif /* JDOC_C_API_H */
