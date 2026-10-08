"""Regenerate the fragment-separation fixtures used by test_pdf ([43]):
pictures that sit edge to edge or share a layout rule stay separate images,
while pieces of one raster still composite into one.

Base-14 Helvetica text, rasters drawn with PyMuPDF. Run from the repo root:
    python3 test/fixtures/pdf/make_fragment_separation_fixtures.py
Fixtures:
  abut_two_charts      two charts stored as two images, edge to edge, each
                       scaled on its own (pixel densities differ)  -> 2 images
  abut_split_raster    one raster cut down the middle into two halves at the
                       same density                                -> 1 image
  strip_row_rule       three charts, each three raster bands, a rule drawn
                       under the whole row                         -> 3 images
  strip_arrow_diagram  two strip pictures joined by a connector line that
                       only reaches the second one's edge          -> 1 image
  strip_shared_notes   two strip charts with one note line running from the
                       left chart into the right one               -> 2 images
"""
import os
import fitz

OUT = os.path.dirname(os.path.abspath(__file__))
W, H = 595, 842
PROSE = ("Body text of the report runs here in ordinary sentences so that the "
         "page carries real text around its figures. ")


def chart_pixmap(w_px, h_px, seed):
    """A bar chart raster: frame and six bars."""
    pix = fitz.Pixmap(fitz.csRGB, fitz.IRect(0, 0, w_px, h_px), False)
    pix.set_rect(pix.irect, (255, 255, 255))
    for r in (fitz.IRect(0, 0, w_px, 3), fitz.IRect(0, h_px - 3, w_px, h_px),
              fitz.IRect(0, 0, 3, h_px), fitz.IRect(w_px - 3, 0, w_px, h_px)):
        pix.set_rect(r, (60, 60, 60))
    for k in range(6):
        bh = int(h_px * (0.2 + 0.12 * ((k * 7 + seed) % 6)))
        x = int(w_px * (0.1 + 0.14 * k))
        pix.set_rect(fitz.IRect(x, h_px - 10 - bh, x + int(w_px * 0.08), h_px - 10),
                     (40 + 30 * seed, 90, 160))
    return pix


def crop_png(pix, x0, y0, x1, y1):
    """PNG of the pixel rectangle [x0,x1) x [y0,y1) of an RGB pixmap."""
    stride, data = pix.width * 3, pix.samples
    rows = b"".join(data[r * stride + x0 * 3: r * stride + x1 * 3] for r in range(y0, y1))
    return fitz.Pixmap(fitz.csRGB, x1 - x0, y1 - y0, rows, False).tobytes("png")


def chart(w_px, h_px, seed, band=None):
    """The chart as PNG; `band` = (i, n) gives its i-th of n horizontal bands."""
    pix = chart_pixmap(w_px, h_px, seed)
    if band is None:
        return pix.tobytes("png")
    i, n = band
    return crop_png(pix, 0, h_px * i // n, w_px, h_px * (i + 1) // n)


def text_page():
    doc = fitz.open()
    p = doc.new_page(width=W, height=H)
    y = 72
    for _ in range(6):
        p.insert_text((60, y), PROSE[:95], fontsize=10, fontname="helv")
        y += 14
    return doc, p


def body_after(p, y0):
    y = y0
    for _ in range(6):
        p.insert_text((60, y), PROSE[:95], fontsize=10, fontname="helv")
        y += 14


def strips(p, rect, seed, n=3, w_px=400, h_px=300):
    step = rect.height / n
    for i in range(n):
        p.insert_image(fitz.Rect(rect.x0, rect.y0 + i * step, rect.x1, rect.y0 + (i + 1) * step),
                       stream=chart(w_px, h_px, seed, (i, n)), keep_proportion=False)


def abut_two_charts():
    doc, p = text_page()
    p.insert_image(fitz.Rect(80, 200, 297, 360), stream=chart(600, 440, 1), keep_proportion=False)
    p.insert_image(fitz.Rect(297, 200, 514, 360), stream=chart(640, 500, 2), keep_proportion=False)
    body_after(p, 400)
    return doc


def abut_split_raster():
    doc, p = text_page()
    pix = chart_pixmap(1200, 440, 1)
    p.insert_image(fitz.Rect(80, 200, 297, 360), stream=crop_png(pix, 0, 0, 600, 440), keep_proportion=False)
    p.insert_image(fitz.Rect(297, 200, 514, 360), stream=crop_png(pix, 600, 0, 1200, 440), keep_proportion=False)
    body_after(p, 400)
    return doc


def strip_row_rule():
    doc, p = text_page()
    for k, x in enumerate((70, 230, 390)):
        strips(p, fitz.Rect(x, 200, x + 135, 320), k)
    p.draw_line((60, 320), (535, 320), color=(0, 0, 0), width=0.4)
    body_after(p, 360)
    return doc


def strip_arrow_diagram():
    doc, p = text_page()
    strips(p, fitz.Rect(90, 200, 250, 320), 0)
    strips(p, fitz.Rect(330, 200, 490, 320), 1)
    p.draw_line((250, 260), (330, 260), color=(0, 0, 0), width=0.6)
    body_after(p, 360)
    return doc


def strip_shared_notes():
    """Two card-sized strip charts side by side whose notes, set on one
    baseline in their feet, read as one text line running from the left card
    into the right one (line building joins notes a few points apart). The
    line is set here as one run, narrower than 0.4 of the page as card notes
    are, with more of it over the left card than the right."""
    doc, p = text_page()
    strips(p, fitz.Rect(100, 200, 250, 340), 0)
    strips(p, fitz.Rect(262, 200, 412, 340), 1)
    note = "Rates rose and stocks moved sideways as homes rose"
    while fitz.get_text_length(note, "helv", 8) > 225:
        note = note.rsplit(" ", 1)[0]
    p.insert_text((106, 330), note, fontsize=8, fontname="helv")
    body_after(p, 380)
    return doc

for name, fn in (("abut_two_charts", abut_two_charts), ("abut_split_raster", abut_split_raster),
                 ("strip_row_rule", strip_row_rule), ("strip_arrow_diagram", strip_arrow_diagram),
                 ("strip_shared_notes", strip_shared_notes)):
    doc = fn()
    doc.save(f"{OUT}/{name}.pdf", garbage=4, deflate=True)
    doc.close()
