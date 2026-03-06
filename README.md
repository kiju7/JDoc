# JDoc — Universal Document to Markdown/Text Converter

C++17 document converter with Python bindings. Converts PDF, Office (DOCX/XLSX/PPTX/DOC/XLS/PPT/RTF), HTML, and Korean HWP/HWPX formats to Markdown or plain text.

## Features

- **Multi-format** — PDF, DOCX, XLSX, PPTX, DOC, XLS, PPT, RTF, HTML, HWP, HWPX
- **Output options** — Markdown (with headings, bold, tables) or plain text (with page separators)
- **Heading detection** — font size ratio analysis (H1–H4)
- **Bold / Italic** — font name pattern matching + inline formatting
- **Table extraction** — line-based grid detection + text-based borderless table detection
- **Image extraction** — returns image bytes in Python (no file save required)
- **Page chunking** — per-page output for RAG pipelines
- **Python bindings** — pybind11 module, returns text/data directly (no file I/O needed)

## Dependencies

| Library | License | Role |
|---------|---------|------|
| PDFium | BSD-3 | PDF backend |
| pugixml | MIT | XML parsing (HWPX/OOXML) |
| POLE | BSD | OLE2 container (HWP/legacy Office) |
| ZLIB | zlib | ZIP decompression |
| pybind11 | BSD | Python bindings (optional) |
| CMake ≥ 3.16 | BSD | Build system |
| C++17 compiler | — | GCC 8+ / Clang 7+ / MSVC 2019+ |

## Build

### C++ CLI only

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### With Python bindings

```bash
# Option 1: pip install (recommended)
pip install pybind11
pip install .

# Option 2: CMake direct
mkdir build && cd build
cmake .. -DBUILD_PYTHON=ON
make -j$(nproc)
```

## Usage

### CLI

```bash
# Convert to Markdown (stdout)
./jdoc input.pdf

# Convert to file
./jdoc input.pdf output.md

# Plain text output (page separators: --- Page N ---)
./jdoc input.pdf --plaintext

# Select specific pages (0-based)
./jdoc input.pdf --pages 0,1,2

# Per-page chunk output
./jdoc input.pdf --chunks

# Extract images
./jdoc input.pdf output.md --images ./img_output/

# Disable table detection
./jdoc input.pdf --no-tables

# All formats supported
./jdoc document.docx
./jdoc spreadsheet.xlsx
./jdoc presentation.pptx
./jdoc page.html
./jdoc document.hwp
./jdoc document.hwpx
```

### Python API

```python
import jdoc

# Simple conversion — returns string directly
text = jdoc.convert("document.pdf")

# Plain text output (strips markdown formatting, adds page separators)
text = jdoc.convert("document.pdf", format="text")

# Markdown output (default)
md = jdoc.convert("document.pdf", format="markdown")

# Specific pages (0-based)
md = jdoc.convert("document.pdf", pages=[0, 1, 2])

# Per-page conversion — returns list of PageChunk
pages = jdoc.convert_pages("document.pdf")
for page in pages:
    print(f"Page {page.page_number}: {len(page.text)} chars")
    print(page.text[:200])

# Plain text per-page
pages = jdoc.convert_pages("document.pdf", format="text")

# With image extraction (images returned as bytes, no file save needed)
pages = jdoc.convert_pages("document.pdf", extract_images=True)
for page in pages:
    for img in page.images:
        print(f"  {img.name} ({img.width}x{img.height}, {img.format})")
        raw_bytes = img.data  # Python bytes object

# Save images to directory (optional)
pages = jdoc.convert_pages("document.pdf",
                           extract_images=True,
                           image_output_dir="./images/")

# All formats work the same way
md = jdoc.convert("report.docx")
md = jdoc.convert("spreadsheet.xlsx")
md = jdoc.convert("presentation.pptx")
md = jdoc.convert("page.html")
md = jdoc.convert("document.hwp")
md = jdoc.convert("document.hwpx")
md = jdoc.convert("legacy.doc")
```

#### Advanced Python API

```python
import jdoc

# Direct per-format functions
md = jdoc.pdf_to_markdown("input.pdf")
md = jdoc.office_to_markdown("input.docx")
md = jdoc.hwp_to_markdown("input.hwp")
md = jdoc.hwpx_to_markdown("input.hwpx")

# Full control with ConvertOptions
opts = jdoc.ConvertOptions()
opts.pages = [0, 1]
opts.extract_tables = True
opts.extract_images = True
opts.output_format = jdoc.OutputFormat.PLAINTEXT

text = jdoc.pdf_to_markdown("input.pdf", opts)
chunks = jdoc.pdf_to_markdown_chunks("input.pdf", opts)

# Detect Office format
fmt = jdoc.detect_office_format("document.docx")
print(jdoc.format_name(fmt))  # "DOCX"
```

### C++ Library API

```cpp
#include <jdoc/pdf.h>
#include <jdoc/office.h>
#include <jdoc/hwp.h>
#include <jdoc/hwpx.h>

// Simple conversion
std::string md = jdoc::pdf_to_markdown("input.pdf");

// With options
jdoc::ConvertOptions opts;
opts.pages = {0, 1, 2};
opts.extract_tables = true;
opts.extract_images = true;
opts.output_format = jdoc::OutputFormat::PLAINTEXT;
std::string text = jdoc::pdf_to_markdown("input.pdf", opts);

// Per-page chunks
auto chunks = jdoc::pdf_to_markdown_chunks("input.pdf");
for (auto& chunk : chunks) {
    std::cout << "Page " << chunk.page_number
              << ": " << chunk.text.size() << " bytes\n";
    for (auto& img : chunk.images) {
        // img.data contains raw image bytes
    }
}
```

### C API (Shared Library)

```c
#include <jdoc/jdoc_c_api.h>

char err[256];

// Extract text only
char* text = jdoc_extract_text("input.pdf", err, sizeof(err));
// use text...
jdoc_free_string(text);

// Extract images only
JDocImage* images = NULL;
int count = jdoc_extract_images("input.pdf", &images, err, sizeof(err));
for (int i = 0; i < count; i++) {
    // images[i].name, .data, .data_size, .format, .width, .height
}
jdoc_free_images(images, count);

// Extract text + images in a single pass
JDocImage* imgs = NULL;
int img_count = 0;
char* md = jdoc_extract_all("input.pdf", &imgs, &img_count, err, sizeof(err));
// use md and imgs...
jdoc_free_string(md);
jdoc_free_images(imgs, img_count);
```

Link with `-ljdoc` (builds as `libjdoc.so`).

### CMake Integration

```cmake
add_subdirectory(jdoc)
target_link_libraries(your_app PRIVATE jdoc_pdf jdoc_office jdoc_hwpx jdoc_hwp)
```

## Output Format Comparison

### Markdown (default)

```markdown
# Document Title

## Section 1

This is **bold** and *italic* text.

| Name | Value |
| ---- | ----- |
| A    | 100   |
| B    | 200   |
```

### Plain Text (`--plaintext` / `format="text"`)

```
Document Title

Section 1

This is bold and italic text.

Name	Value
A	100
B	200

--- Page 2 ---

Next page content...
```

## Architecture

```
Document file (PDF/Office/HTML/HWP/HWPX)
  │
  ▼
┌──────────────────────┐
│  Format Detection    │  Extension + magic bytes
└──────────┬───────────┘
           │
  ┌────────┼────────┬────────┬────────┐
  ▼        ▼        ▼        ▼        ▼
┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐
│PDFium│ │OOXML │ │ HTML │ │ HWP  │ │ HWPX │
│Parser│ │Parser│ │Parser│ │ OLE2 │ │ XML  │
└──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘
   └────────┴────────┴────────┴────────┘
                │
                ▼
      ┌──────────────────┐
      │ Markdown / Text  │  OutputFormat option
      │ Formatter        │
      └──────────────────┘
                │
      ┌────────┼────────┐
      ▼        ▼        ▼
  Python    CLI /    C API
   API     C++ API  (libjdoc.so)
```

## License

MIT — use freely in commercial and open-source projects.
