"""Regenerate the CropBox scan fixtures used by test_pdf ([42] a whole-page
composite covers the visible page only).

A scanned page stored as two raster halves that abut, on an A4 MediaBox
whose CropBox shows only a 400x600 pt window (offset from the MediaBox
corner, as report printers trim a larger sheet). No text, so the page
composites whole. A second copy carries /Rotate 90. Run from the repo root:
    python3 test/fixtures/pdf/make_cropbox_scan_fixtures.py
"""
import os
import fitz

OUT = os.path.dirname(os.path.abspath(__file__))
W, H = 595, 842
CROP = fitz.Rect(60, 142, 460, 742)          # 400 x 600 pt, top-left origin


def half(shade, w_px=720, h_px=560):
    """One half of a scanned form: a grey field with a dark frame."""
    pix = fitz.Pixmap(fitz.csRGB, fitz.IRect(0, 0, w_px, h_px), False)
    pix.set_rect(pix.irect, (shade, shade, shade))
    pix.set_rect(fitz.IRect(0, 0, w_px, 6), (40, 40, 40))
    pix.set_rect(fitz.IRect(0, 0, 6, h_px), (40, 40, 40))
    return pix.tobytes("png")


def page(rotate):
    doc = fitz.open()
    p = doc.new_page(width=W, height=H)
    p.insert_image(fitz.Rect(80, 162, 440, 442), stream=half(190))
    p.insert_image(fitz.Rect(80, 442, 440, 722), stream=half(150))
    p.set_cropbox(CROP)
    if rotate:
        p.set_rotation(rotate)
    return doc


for name, rot in (("cropbox_scan", 0), ("cropbox_scan_rot90", 90)):
    doc = page(rot)
    doc.save(f"{OUT}/{name}.pdf", garbage=4, deflate=True)
    doc.close()
