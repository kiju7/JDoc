# JDoc

C++17 document-to-Markdown converter. No heavy dependencies — just zlib, libjpeg-turbo, and pugixml.

**Supported formats:** PDF, DOCX, XLSX, XLSB, PPTX, DOC, XLS, PPT, RTF, HTML, HWP, HWPX, TXT

## Key Features

- **Custom PDF parser** — no PDFium/Poppler, fully thread-safe with no global state
- **Heading detection** — font size ratio analysis (H1–H4)
- **Table extraction** — line-based grid + borderless text table detection
- **Image extraction** — JPEG passthrough, 200 DPI vector rendering, CCITTFax G3/G4, min-size filtering
- **Encrypted PDF** — RC4 Standard Security Handler (40/128-bit)
- **Malformed PDF recovery** — xref rebuild, stream length recovery
- **Korean document support** — HWP/HWPX with full table, image, heading support
- **CJK encodings** — CP949, CP932, CMap-based Unicode mapping
- **Page chunking** — per-page output with metadata for RAG pipelines
- **Multiple APIs** — CLI, Python (pybind11), C, C++

## Install

System deps (one-time, needed for both C++ and Python builds):

```bash
# Ubuntu/Debian
sudo apt install cmake build-essential zlib1g-dev libjpeg-dev
# macOS
brew install cmake libjpeg-turbo
# RHEL/Fedora
sudo dnf install cmake gcc-c++ zlib-devel libjpeg-turbo-devel
```

Then build:

```bash
# Python
pip install .

# C++
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

## Usage

### CLI

```bash
jdoc input.pdf                              # Markdown to stdout
jdoc input.pdf output.md                    # Save to file
jdoc input.docx --plaintext                 # Plain text output
jdoc input.pdf --pages 0,1,2                # Select pages (0-based)
jdoc input.pdf --chunks                     # Per-page output
jdoc input.pdf --images ./imgs              # Extract images
jdoc input.pdf --images ./imgs --min-image-size 100   # Skip images < 100px
jdoc input.pdf --images ./imgs --min-image-size 0     # Extract all images
```

### Python

```python
import jdoc

# Convert to Markdown
text = jdoc.convert("document.pdf")
text = jdoc.convert("report.docx", format="text", pages=[0, 1])

# Per-page chunks with images
pages = jdoc.convert_pages("document.pdf", extract_images=True)
for page in pages:
    print(page.text)
    for img in page.images:
        print(f"  {img.name} {img.width}x{img.height} {img.format}")
        # img.data  — JPEG/PNG bytes
        # img.pixels — raw RGB buffer (width * height * components)

# Image size filtering
text = jdoc.convert("doc.pdf", extract_images=True, min_image_size=100)  # skip < 100px
text = jdoc.convert("doc.pdf", extract_images=True, min_image_size=0)    # no filter

# ConvertOptions for full control
opts = jdoc.ConvertOptions()
opts.extract_images = True
opts.image_output_dir = "./images"
opts.min_image_size = 50
opts.pages = [0, 1, 2]
```

### C++

```cpp
#include <jdoc/jdoc.h>

// Auto-detects format (PDF, DOCX, XLSX, PPTX, HWP, etc.)
std::string md = jdoc::convert("input.pdf");
std::string md = jdoc::convert("report.docx");

// Extract images to directory
jdoc::ConvertOptions opts;
opts.pages = {0, 1, 2};
opts.extract_images = true;
opts.image_output_dir = "./images";  // saved to files
opts.min_image_size = 50;
std::string md = jdoc::convert("input.pdf", opts);

// Per-page chunks with images in memory
opts.image_output_dir = "";  // empty = keep in memory only
auto chunks = jdoc::convert_chunks("input.pdf", opts);
for (auto& chunk : chunks) {
    // chunk.text, chunk.tables
    // chunk.page_width, chunk.page_height, chunk.body_font_size
    for (auto& img : chunk.images) {
        // img.name, img.width, img.height, img.format
        // img.data — JPEG/PNG encoded bytes
    }
}
```

CMake:
```cmake
add_subdirectory(jdoc)
target_link_libraries(your_app PRIVATE jdoc_all)
```

### C API

```c
#include <jdoc/jdoc_c_api.h>

char err[256];

// Simple text conversion
char* text = jdoc_convert("input.pdf", NULL, err, sizeof(err));
// use text...
jdoc_free_string(text);

// Per-page chunks with images
JDocOptions opts = jdoc_default_options();
opts.extract_images = 1;
opts.image_output_dir = "./images";  // NULL = keep in memory only

int page_count;
JDocPage* pages = jdoc_convert_pages("input.pdf", &opts, &page_count, err, sizeof(err));
for (int i = 0; i < page_count; i++) {
    printf("Page %d: %s\n", pages[i].page_number, pages[i].text);
    for (int j = 0; j < pages[i].image_count; j++) {
        JDocImage* img = &pages[i].images[j];
        // img->name, img->width, img->height, img->format
        // img->data (raw bytes), img->data_size
        // img->saved_path (when image_output_dir is set)
    }
}
jdoc_free_pages(pages, page_count);
```

## Format Support

| Feature | PDF | DOCX | DOC | XLSX/XLSB | XLS | PPTX | PPT | HWP/HWPX | RTF | HTML | TXT |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Text | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Headings | ✓ | ✓ | ✓ | | | ✓ | ✓ | ✓ | | | |
| Bold/Italic | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | |
| Tables | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | | ✓ | ✓ | ✓ | |
| Images | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | |
| Lists | | ✓ | ✓ | | | | | | | ✓ | |
| Links | ✓ | ✓ | ✓ | | | | | | | ✓ | |
| Annotations | ✓ | | | | | | | | | | |
| Charts/SmartArt | | | | | | ✓ | | | | | |
| Speaker Notes | | | | | | ✓ | ✓ | | | | |

## Dependencies

| Library | License | Role |
|---|---|---|
| zlib | zlib | Compression (FlateDecode, PNG) |
| libjpeg-turbo | IJG/BSD | JPEG decode for PDF images |
| pugixml | MIT | XML parsing (bundled) |
| pybind11 | BSD-3 | Python bindings (optional) |

## Platforms

Linux (x64), macOS (arm64/x64), Windows (x64)

## License

MIT
