"""Regenerate the form fixtures used by test_pdf ([44] ...).

  form_party_boxes.pdf   the head of a trade statement: two boxed tables side
      by side (supplier, buyer), a label column beside a wide value column,
      every cell border painted as a thin filled rectangle (as a browser
      prints a table). Values of different lengths leave a whitespace stretch
      inside the wide column on the short rows only; the long values run
      over it. Each value is one cell.

The PDFs are written by hand (base-14 Helvetica, no font files) so the
fixtures are the same on every OS. Run from the repo root:
    python3 test/fixtures/pdf/make_forms_kr_fixtures.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_short_page_fixtures import Page, width, PH  # noqa: E402

BW = 0.75  # border width


def box(pg, x0, y0, x1, y1, fill=None):
    """One cell: optional shading, then its four borders as filled bars."""
    if fill is not None:
        pg.c += "%.3f g %.2f %.2f %.2f %.2f re f 0 g\n" % (fill, x0, PH - y1, x1 - x0, y1 - y0)
    for (a, b, c, d) in ((x0, y0, x1, y0 + BW), (x0, y1 - BW, x1, y1),
                         (x0, y0, x0 + BW, y1), (x1 - BW, y0, x1, y1)):
        pg.c += "0.2 g %.2f %.2f %.2f %.2f re f 0 g\n" % (a, PH - d, c - a, d - b)


SUPPLIER = [("Reg. No.", "314-86-00000"), ("Name", "Hangyeol TechSolutions Co."),
            ("CEO", "Moon"), ("Address", "000 Daehak-ro, Yuseong-gu, Daejeon"),
            ("Phone", "042-000-7788")]
BUYER = [("Reg. No.", "305-82-00000"), ("Name", "Hanbit University Foundation"),
         ("Contact", "Intelligent Systems Lab"), ("Address", "000 Hanbit-ro, Yuseong-gu"),
         ("Manager", "Prof. Yoon")]


def make_party_boxes():
    pg = Page()
    pg.text(250, 80, "TRADE STATEMENT", "F2", 16)
    top, rh = 124.5, 20.0
    for (x0, xm, x1, title, rows) in ((51.0, 113.6, 291.8, "Supplier", SUPPLIER),
                                      (303.0, 366.0, 544.5, "Buyer", BUYER)):
        box(pg, x0, top, x1, top + rh, fill=0.93)
        pg.text((x0 + x1 - width(title, "F2", 9)) / 2, top + 13.5, title, "F2", 9)
        for i, (k, v) in enumerate(rows):
            y0 = top + rh * (i + 1)
            box(pg, x0, y0, xm, y0 + rh, fill=0.93)
            box(pg, xm, y0, x1, y0 + rh)
            pg.text((x0 + xm - width(k, "F2", 9)) / 2, y0 + 13.5, k, "F2", 9)
            pg.text(xm + 5, y0 + 13.5, v, "F1", 9)
    pg.save("form_party_boxes.pdf")


if __name__ == "__main__":
    make_party_boxes()
