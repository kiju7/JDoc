"""Regenerate the fragment-region fixtures used by test_pdf ([16] figure
strips under multi-column body text).

PyMuPDF with an embedded (subset) Arial. The figure is stored as raster
strips (one placement per pixel row band), the way print drivers and word
processors emit a flowchart, and spans both text columns. Run from the repo
root:
    python3 test/fixtures/pdf/make_fragment_region_fixtures.py
"""
import os
import fitz

OUT = os.path.dirname(os.path.abspath(__file__))
FONT = "/System/Library/Fonts/Supplemental/Arial.ttf"
W, H = 595, 842
PROSE = ("Body text of the page runs here in ordinary sentences, set in two "
         "columns so that every line is narrower than half the page. ")

# Figure: x 80..515, y 330..450, as 60 strips of 2 pt (4 px rows each).
FX0, FY0, FX1, FY1 = 80, 330, 515, 450
STRIPS = 60


def strip(i, w_px=870, h_px=4):
    """One band of a flowchart-like raster: grey boxes on white, a dark
    frame line now and then, so neighbouring strips differ."""
    pix = fitz.Pixmap(fitz.csRGB, fitz.IRect(0, 0, w_px, h_px), False)
    pix.set_rect(pix.irect, (255, 255, 255))
    if 8 <= i < 52:
        for bx in (40, 300, 560):
            shade = 200 + (i % 5) * 5
            pix.set_rect(fitz.IRect(bx, 0, bx + 220, h_px), (shade, shade, shade))
    if i in (8, 51):
        pix.set_rect(fitz.IRect(40, 0, 780, 1), (60, 60, 60))
    return pix.tobytes("png")


def wrap(text, width, fs=9):
    """Greedy line breaks at the column measure, as a word processor sets
    a ragged-right paragraph."""
    font = fitz.Font(fontfile=FONT)
    lines, cur = [], ""
    for w in text.split():
        cand = (cur + " " + w).strip()
        if cur and font.text_length(cand, fontsize=fs) > width:
            lines.append(cur)
            cur = w
        else:
            cur = cand
    if cur:
        lines.append(cur)
    return lines


def column(p, x0, y_last, lines, pitch=13.0):
    """Set lines upward so the last one's baseline sits at y_last."""
    y = y_last - pitch * (len(lines) - 1)
    for ln in lines:
        p.insert_text((x0, y), ln, fontsize=9, fontname="F0")
        y += pitch


def page(last_line_short):
    doc = fitz.open()
    p = doc.new_page(width=W, height=H)
    p.insert_font(fontname="F0", fontfile=FONT)
    # Two columns of body text whose last baselines sit 8 pt above the
    # figure: closer than one and a half line heights, as in a tight layout.
    left = wrap(PROSE * 4, 230)
    if last_line_short:
        # A paragraph ending in one short word right above the figure.
        left = wrap(PROSE * 4, 230) + ["end."]
    right = wrap(PROSE * 4, 230)
    column(p, 60, FY0 - 8, left)
    column(p, 305, FY0 - 8, right)
    step = (FY1 - FY0) / STRIPS
    for i in range(STRIPS):
        y = FY0 + i * step
        p.insert_image(fitz.Rect(FX0, y, FX1, y + step), stream=strip(i))
    p.insert_text((FX0, FY1 + 18), "Figure 2. Overview of the evaluation flow.",
                  fontsize=9, fontname="F0")
    column(p, 60, 760, wrap(PROSE * 5, 230))
    column(p, 305, 760, wrap(PROSE * 5, 230))
    return doc


for name, short in (("strip_figure_columns", False),
                    ("strip_figure_columns_short_tail", True)):
    doc = page(short)
    doc.subset_fonts()
    doc.save(f"{OUT}/{name}.pdf", garbage=4, deflate=True)
    doc.close()
