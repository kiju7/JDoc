"""Regenerate the chart fixture used by test_pdf ([36] a chart's labels are
not a table).

  chart_labels.pdf  a paragraph, then two chart panels side by side: a bar
      chart (four coloured bars of different heights, tick labels up the
      left axis, years under the bars, a two-entry legend) and a line chart
      (two polylines over gridlines, tick labels, years). The tick labels
      and years align across both panels as neatly as any table's cells;
      only the drawing around them says they are a figure's. A paragraph
      follows. No label may come out as a table cell, and every label must
      still be in the text.

The PDF is written by hand (base-14 Helvetica, no font files) so the fixture
is the same on every OS. Run from the repo root:
    python3 test/fixtures/pdf/make_chart_fixtures.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_short_page_fixtures import Page, PH, width  # noqa: E402

PROSE = ["Output grew in every region last year while prices eased, as the two",
         "panels below show for four economies, with the bars giving the level",
         "and the lines the change over the year."]


def make_chart_labels():
    pg = Page()
    y = 90
    for ln in PROSE:
        pg.text(72, y, ln, size=10)
        y += 13
    top, bottom = 150, 300          # plot area, page-top coordinates
    # Panel A: bars. Axis, gridlines, four bars in two colours.
    ax0, ax1 = 110, 280
    pg.text(ax0, top - 14, "A. Output level", "F2", 9)
    for i, tick in enumerate(("0", "20", "40", "60")):
        yy = bottom - i * 50
        pg.text(ax0 - 8 - width(tick, "F1", 8), yy + 3, tick, size=8)
        pg.c += "0.3 w 0.8 G %.2f %.2f m %.2f %.2f l S\n" % (ax0, PH - yy, ax1, PH - yy)
    pg.c += "0.6 w 0 G %.2f %.2f m %.2f %.2f l S\n" % (ax0, PH - bottom, ax0, PH - top)
    heights = (70, 110, 55, 130)
    for i, h in enumerate(heights):
        x = ax0 + 15 + i * 40
        colour = "0.15 0.35 0.65 rg" if i % 2 == 0 else "0.85 0.45 0.15 rg"
        pg.c += "%s %.2f %.2f %.2f %.2f re f\n" % (colour, x, PH - bottom, 22, h)
        pg.text(x + 2, bottom + 12, str(2019 + i), size=8)
    pg.c += "0.15 0.35 0.65 rg %.2f %.2f 8 8 re f\n" % (ax0 + 10, PH - (bottom + 30))
    pg.text(ax0 + 22, bottom + 30, "Advanced", size=8)
    pg.c += "0.85 0.45 0.15 rg %.2f %.2f 8 8 re f\n" % (ax0 + 90, PH - (bottom + 30))
    pg.text(ax0 + 102, bottom + 30, "Emerging", size=8)
    # Panel B: lines. Gridlines, two polylines.
    bx0, bx1 = 330, 520
    pg.text(bx0, top - 14, "B. Change over the year", "F2", 9)
    for i, tick in enumerate(("-4", "0", "4", "8")):
        yy = bottom - i * 50
        pg.text(bx0 - 8 - width(tick, "F1", 8), yy + 3, tick, size=8)
        pg.c += "0.3 w 0.8 G %.2f %.2f m %.2f %.2f l S\n" % (bx0, PH - yy, bx1, PH - yy)
    pts_a = [(bx0 + 10, bottom - 60), (bx0 + 60, bottom - 95), (bx0 + 110, bottom - 70), (bx0 + 160, bottom - 120)]
    pts_b = [(bx0 + 10, bottom - 30), (bx0 + 60, bottom - 45), (bx0 + 110, bottom - 100), (bx0 + 160, bottom - 85)]
    for pts, col in ((pts_a, "0.15 0.35 0.65 RG"), (pts_b, "0.85 0.45 0.15 RG")):
        pg.c += "1.2 w %s %.2f %.2f m " % (col, pts[0][0], PH - pts[0][1])
        pg.c += " ".join("%.2f %.2f l" % (x, PH - yy) for x, yy in pts[1:]) + " S\n"
    for i in range(4):
        pg.text(bx0 + 2 + i * 50, bottom + 12, str(2019 + i), size=8)
    pg.c += "0 g 0 G\n"
    y = bottom + 56
    for ln in PROSE:
        pg.text(72, y, ln, size=10)
        y += 13
    pg.save("chart_labels.pdf")


if __name__ == "__main__":
    make_chart_labels()
