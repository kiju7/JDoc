"""Regenerate the overlay-text fixtures used by test_pdf ([15] text over a raster).

PyMuPDF with an embedded (subset) Arial; every raster is one JPEG placement.
Run from the repo root:
    python3 test/fixtures/pdf/make_overlay_fixtures.py
"""
import os
import fitz

OUT = os.path.dirname(os.path.abspath(__file__))
FONT = "/System/Library/Fonts/Supplemental/Arial.ttf"
W, H = 595, 842
PROSE = ("Body text of the page runs here in ordinary sentences so that the "
         "picture stands between paragraphs and not alone on the page. ")


def jpeg(w, h, rgb):
    """A flat-colour JPEG with a darker band, so it is visibly a picture."""
    pix = fitz.Pixmap(fitz.csRGB, fitz.IRect(0, 0, w, h), False)
    pix.set_rect(pix.irect, rgb)
    pix.set_rect(fitz.IRect(0, h * 2 // 3, w, h), tuple(c // 2 for c in rgb))
    return pix.tobytes("jpeg")


class Page:
    def __init__(self):
        self.doc = fitz.open()
        self.p = self.doc.new_page(width=W, height=H)
        self.p.insert_font(fontname="F0", fontfile=FONT)

    def text(self, x, y, t, fs=9, color=(0, 0, 0), mode=0):
        self.p.insert_text((x, y), t, fontsize=fs, fontname="F0", color=color,
                           render_mode=mode)

    def para(self, x0, y0, x1, y1, t=PROSE * 3, fs=9, mode=0):
        self.p.insert_textbox(fitz.Rect(x0, y0, x1, y1), t, fontsize=fs,
                              fontname="F0", render_mode=mode)

    def image(self, x0, y0, x1, y1, rgb=(200, 215, 235), px=(600, 360)):
        self.p.insert_image(fitz.Rect(x0, y0, x1, y1),
                            stream=jpeg(px[0], px[1], rgb))

    def save(self, name):
        path = f"{OUT}/{name}.pdf"
        self.doc.subset_fonts()
        self.doc.save(path, garbage=4, deflate=True)
        self.doc.close()


# A chart saved as one raster, its title, values and axis labels typed over
# it (one label white on the dark band, one straddling the top edge), with
# body text above and a caption and body text below.
pg = Page()
pg.para(60, 60, 535, 160)
pg.image(120, 200, 475, 413)
pg.text(140, 205, "Quarterly revenue", fs=14)
pg.text(140, 260, "+12%", fs=24)
pg.text(140, 300, "Profit +8%", fs=12)
pg.text(140, 400, "Q1   Q2   Q3   Q4", fs=10, color=(1, 1, 1))
pg.text(120, 430, "Figure 1. Revenue by quarter.", fs=9)
pg.para(60, 450, 535, 560)
pg.save("overlay_chart")

# The same picture with nothing over it: the original JPEG passes through.
pg = Page()
pg.para(60, 60, 535, 160)
pg.image(120, 200, 475, 413)
pg.text(120, 430, "Figure 1. Revenue by quarter.", fs=9)
pg.para(60, 450, 535, 560)
pg.save("overlay_none")

# A page-sized background behind ordinary body text.
pg = Page()
pg.image(0, 0, W, H, rgb=(245, 240, 225), px=(595, 842))
pg.text(60, 70, "Annual letter", fs=18)
pg.para(60, 100, 535, 400, t=PROSE * 12)
pg.para(60, 420, 535, 700, t=PROSE * 12)
pg.save("overlay_page_background")

# A mid-page picture with body text running across it (a watermark-like
# backdrop under the paragraphs).
pg = Page()
pg.image(200, 150, 400, 350, rgb=(235, 235, 235), px=(400, 400))
pg.para(60, 100, 535, 420, t=PROSE * 14)
pg.save("overlay_backdrop")

# A scanned figure with an invisible OCR layer (Tr 3) over it.
pg = Page()
pg.para(60, 60, 535, 160)
pg.image(120, 200, 475, 413)
pg.text(140, 260, "Invisible ocr words", fs=12, mode=3)
pg.text(140, 300, "More invisible text", fs=12, mode=3)
pg.para(60, 450, 535, 560)
pg.save("overlay_ocr_figure")

# Labels drawn first and then covered by the picture: hidden, not overlaid.
pg = Page()
pg.para(60, 60, 535, 160)
pg.text(140, 260, "Hidden label", fs=12)
pg.text(140, 300, "Another hidden label", fs=12)
pg.image(120, 200, 475, 413)
pg.para(60, 450, 535, 560)
pg.save("overlay_hidden_text")
