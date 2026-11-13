"""Regenerate the form fixtures used by test_pdf ([44], [45]).

  form_party_boxes.pdf   the head of a trade statement: two boxed tables side
      by side (supplier, buyer), a label column beside a wide value column,
      every cell border painted as a thin filled rectangle (as a browser
      prints a table). Values of different lengths leave a whitespace stretch
      inside the wide column on the short rows only; the long values run
      over it. Each value is one cell.
  form_stacked_boxes.pdf  a statement page of boxed tables of one width
      stacked 6 pt apart: a numbered section box ("II | Supplies", a
      heading), an item table whose first column (No.) is narrower than a
      rule fragment, a one-row total box (its label on two lines), a
      two-row balance table, and a
      one-cell notice box. Three tables, each on its own, the No. column
      kept; the section box and the notice are text.

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


def grid(pg, x_edges, top, rows, rh=18.0, head=0):
    """A table of boxed cells; rows of (text, colspan) cells, top rows shaded."""
    y = top
    for r, row in enumerate(rows):
        c = 0
        for text, span in row:
            x0, x1 = x_edges[c], x_edges[c + span]
            box(pg, x0, y, x1, y + rh, fill=0.93 if r < head else None)
            if text:
                pg.text(x0 + 4, y + 12.5, text, "F2" if r < head else "F1", 9)
            c += span
        y += rh
    return y


def make_stacked_boxes():
    pg = Page()
    L, R = 60.0, 540.0
    pg.text(240, 70, "STATEMENT", "F2", 16)
    y = grid(pg, [L, 90.0, R], 90, [[("II", 1), ("Supplies", 1)]])
    xs = [L, 86.0, 230.0, 330.0, 380.0, 460.0, R]
    y = grid(pg, xs, y + 14, [[("No.", 1), ("Item", 1), ("Spec", 1), ("Qty", 1), ("Price", 1), ("Amount", 1)],
                              [("1", 1), ("GPU server", 1), ("48 GB", 1), ("2", 1), ("9,800", 1), ("19,600", 1)],
                              [("2", 1), ("NVMe drive", 1), ("4 TB", 1), ("3", 1), ("520", 1), ("1,560", 1)],
                              [("3", 1), ("Rack kit", 1), ("2U", 1), ("1", 1), ("95", 1), ("95", 1)]], head=1)
    # The total box's label runs to two lines beside a one-line value.
    box(pg, L, y + 6, 180.0, y + 36, fill=0.93)
    box(pg, 180.0, y + 6, R, y + 36)
    pg.text(L + 4, y + 18, "Total", "F2", 9)
    pg.text(L + 4, y + 30, "(with tax)", "F1", 8)
    pg.text(184.0, y + 24, "Twenty-two thousand one hundred (22,100)", "F1", 9)
    y += 36
    xb = [L, 130.0, 240.0, 310.0, 420.0, 470.0, R]
    y = grid(pg, xb, y + 6, [[("Balance", 1), ("0", 1), ("Paid", 1), ("0", 1), ("Due", 1), ("21,255", 1)],
                             [("Note", 1), ("Contract C-0388", 3), ("Signed", 1), ("(seal)", 1)]])
    y = grid(pg, [L, R], y + 34, [[("Notice: issued electronically, valid without a seal.", 1)]])
    pg.save("form_stacked_boxes.pdf")


if __name__ == "__main__":
    make_party_boxes()
    make_stacked_boxes()
