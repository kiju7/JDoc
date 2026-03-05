"""JDoc - Universal document converter (PDF, Office, HTML, HWP/HWPX)

Convert documents to Markdown or plain text from Python.

Usage:
    import jdoc

    # Simple conversion (returns string)
    text = jdoc.convert("document.pdf")
    text = jdoc.convert("document.pdf", format="text")  # plain text

    # Per-page conversion (returns list of PageChunk)
    pages = jdoc.convert_pages("document.pdf")
    for page in pages:
        print(f"Page {page.page_number}: {page.text[:100]}")

    # With images
    pages = jdoc.convert_pages("document.pdf", extract_images=True)
    for page in pages:
        for img in page.images:
            print(f"  Image: {img.name} ({img.width}x{img.height})")
            raw_bytes = img.data  # bytes object
"""

from _jdoc import (
    # Enums
    OutputFormat,
    DocFormat,
    # Data classes
    ImageData,
    PageChunk,
    ConvertOptions,
    # High-level API
    convert,
    convert_pages,
    # Per-format functions
    pdf_to_markdown,
    pdf_to_markdown_chunks,
    office_to_markdown,
    office_to_markdown_chunks,
    hwp_to_markdown,
    hwp_to_markdown_chunks,
    hwpx_to_markdown,
    hwpx_to_markdown_chunks,
    detect_office_format,
    format_name,
)

__version__ = "2.0.0"
__all__ = [
    "OutputFormat",
    "DocFormat",
    "ImageData",
    "PageChunk",
    "ConvertOptions",
    "convert",
    "convert_pages",
    "pdf_to_markdown",
    "pdf_to_markdown_chunks",
    "office_to_markdown",
    "office_to_markdown_chunks",
    "hwp_to_markdown",
    "hwp_to_markdown_chunks",
    "hwpx_to_markdown",
    "hwpx_to_markdown_chunks",
    "detect_office_format",
    "format_name",
]
