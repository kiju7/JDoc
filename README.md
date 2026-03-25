# JDoc

C++17 document converter with Python bindings. Converts PDF, Office (DOCX/XLSX/PPTX/DOC/XLS/PPT), HTML, and Korean HWP/HWPX to Markdown or plain text.

## Features

- **Multi-format** — PDF, DOCX, XLSX, XLSB, PPTX, DOC, XLS, PPT, RTF, HTML, HWP, HWPX
- **Thread-safe PDF** — custom parser, no global state, fully reentrant
- **Heading detection** — font size ratio analysis (H1–H4)
- **Table extraction** — line-based grid + borderless table detection
- **Image extraction** — JPEG passthrough, vector rendering (200 DPI), CCITTFax G3/G4
- **Page chunking** — per-page output for RAG pipelines
- **Python bindings** — pybind11 module

## Supported Platforms

Linux (x64), macOS (arm64/x64), Windows (x64)

## Dependencies

| Library | License | Role |
|---------|---------|------|
| zlib | zlib | Compression (FlateDecode, PNG) |
| libjpeg-turbo | IJG/BSD | JPEG decode for PDF images |
| pugixml | MIT | XML parsing (bundled) |
| pybind11 | BSD-3 | Python bindings (optional) |

## Install

```bash
# Ubuntu/Debian
sudo apt install cmake build-essential zlib1g-dev libjpeg-dev

# macOS
brew install cmake libjpeg-turbo

# RHEL/Fedora
sudo dnf install cmake gcc-c++ zlib-devel libjpeg-turbo-devel

# Windows
winget install Kitware.CMake
```

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
```

Python bindings:
```bash
pip install pybind11 && pip install .
```

## CLI

```bash
./jdoc input.pdf                    # Markdown to stdout
./jdoc input.pdf output.md          # Save to file (Markdown)
./jdoc input.pdf output.txt --plaintext  # Plain text
./jdoc input.pdf --pages 0,1,2      # Select pages
./jdoc input.pdf --chunks           # Per-page output
./jdoc input.pdf --images ./imgs/   # Extract images
```

## Python

```python
import jdoc

text = jdoc.convert("document.pdf")
text = jdoc.convert("document.pdf", format="text", pages=[0, 1])

pages = jdoc.convert_pages("document.pdf", extract_images=True)
for page in pages:
    print(page.text)
    for img in page.images:
        # img.pixels: raw RGB bytes (zero-copy, width * height * components)
        # img.data: JPEG original bytes (passthrough, no re-encoding)
        pass
```

## C++

```cpp
#include <jdoc/pdf.h>
#include <jdoc/office.h>

// Simple conversion
std::string md = jdoc::pdf_to_markdown("input.pdf");
std::string md = jdoc::office_to_markdown("input.docx");

// With options
jdoc::ConvertOptions opts;
opts.pages = {0, 1, 2};
opts.extract_images = true;
std::string md = jdoc::pdf_to_markdown("input.pdf", opts);

// Per-page chunks
auto chunks = jdoc::pdf_to_markdown_chunks("input.pdf");
for (auto& chunk : chunks) {
    std::cout << chunk.text << "\n";
    for (auto& img : chunk.images) {
        // img.pixels: raw RGB/gray (width * height * components)
        // img.data: JPEG/JP2 original bytes
    }
}
```

CMake:
```cmake
add_subdirectory(jdoc)
target_link_libraries(your_app PRIVATE jdoc_pdf jdoc_office jdoc_hwpx jdoc_hwp)
```

## License

MIT
