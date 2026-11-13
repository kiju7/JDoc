"""Regenerate the two-column fixtures used by test_pdf ([32] ruled tables of
two columns, [33] layout blocks of two columns).

  grid_two_col_terms.pdf   a page holding nothing but a ruled table of two
      columns and twelve rows (enough for the page's gutter histogram to
      read its two columns as two text columns), terms beside their descriptions, every cell centred: a frame,
      a rule under the header and a rule between the columns drawn one row
      at a time, as LaTeX draws them. Every row
      ends a word in the left cell and starts one in the right, as any table
      of words does.
  layout_two_col_counts.pdf  a borderless table of two right-aligned columns,
      codes beside their counts, under a paragraph. No rule, no third
      column: no table detector takes it, and its rows must keep their
      alignment in a layout block.
  layout_leader_years.pdf  jurisdictions beside year ranges, joined by dot
      leaders that run right up to the value (no gap between the cells).
  layout_contents.pdf      a contents list: titles beside rising page
      numbers. A list, never a layout block.
  grid_split_prose.pdf     a paragraph set across a frame that a vertical
      rule cuts in two: the words on either side of the rule sit a word
      space apart, so the text runs on across it. It is not a table.

The PDFs are written by hand (base-14 Helvetica, no font files) so the
fixtures are the same on every OS. Run from the repo root:
    python3 test/fixtures/pdf/make_two_col_grid_fixtures.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_short_page_fixtures import Page, width  # noqa: E402

ROWS = [("Drug", "Effects"), ("heroin", "anxiety, euphoria"),
        ("cocaine", "euphoria, anxiety, comedown, paranoia"),
        ("ketamine", "euphoria, visuals, hallucinations, nausea"),
        ("methadone", "anxiety, euphoria"), ("codeine", "euphoria, anxiety, nausea"),
        ("morphine", "euphoria, anxiety, analgesic, nausea"),
        ("amphetamine", "euphoria, anxiety, comedown, visuals"),
        ("oxycodone", "euphoria, anxiety"), ("caffeine", "alertness, anxiety"),
        ("nicotine", "alertness, calm"), ("dopamine", "euphoria, anxiety, comedown")]
PROSE = ["Parks lower the air temperature of the streets around them",
         "but by how much depends on their size their trees and the wind",
         "as twelve parks of one city measured twice a day for a year show",
         "with readings taken at the same hours in the streets nearby"]


def make_terms():
    pg = Page()
    x0, xm, x1 = 140, 230, 470
    top, lead = 320, 14
    bottom = top + lead * len(ROWS) + 4
    for y in (top, top + lead + 2, bottom):
        pg.rule(x0, x1, y)
    # Vertical rules drawn one row at a time, as LaTeX tables are.
    edges = [top, top + lead + 2] + [top + lead * (i + 1) + 2 for i in range(1, len(ROWS) - 1)] + [bottom]
    for ya, yb in zip(edges, edges[1:]):
        for x in (x0, xm, x1):
            pg.c += "0.5 w %.2f %.2f m %.2f %.2f l S\n" % (x, 842 - ya, x, 842 - yb)
    for i, (a, b) in enumerate(ROWS):
        y = top + lead * (i + 1) - 2
        font = "F2" if i == 0 else "F1"
        pg.text((x0 + xm - width(a, font, 9)) / 2, y, a, font, 9)
        pg.text((xm + x1 - width(b, font, 9)) / 2, y, b, font, 9)
    pg.save("grid_two_col_terms.pdf")


def make_split_prose():
    pg = Page()
    x0, x1, top, lead = 100, 500, 300, 14
    bottom = top + lead * len(PROSE) + 6
    for y in (top, bottom):
        pg.rule(x0, x1, y)
    # The rule falls inside every line, between two words.
    cut = 300
    for x in (x0, cut, x1):
        pg.c += "0.5 w %.2f %.2f m %.2f %.2f l S\n" % (x, 842 - top, x, 842 - bottom)
    space = width(" ", "F1", 10)
    for i, line in enumerate(PROSE):
        words = line.split()
        y = top + lead * (i + 1)
        left, k = "", 0
        while k < len(words) and x0 + 6 + width((left + " " + words[k]).strip(), "F1", 10) < cut - 1:
            left = (left + " " + words[k]).strip()
            k += 1
        right = " ".join(words[k:])
        lx = cut - space / 2 - width(left, "F1", 10)
        pg.text(lx, y, left, size=10)
        pg.text(cut + space / 2, y, right, size=10)
    pg.save("grid_split_prose.pdf")


COUNTS = [("Transducer", "Frequency"), ("0000", "1"), ("000100", "8"),
          ("0001010110", "1"), ("00010110", "4"), ("0001011100", "1"),
          ("000110", "8"), ("00011100", "4"), ("01000110", "480")]
CONTENTS = [("Introduction", "1"), ("Related work", "4"), ("Measurement sites", "9"),
            ("Results", "15"), ("Discussion", "22"), ("Conclusion", "27")]


def make_counts():
    pg = Page()
    pg.text(72, 100, "The transducers below were observed over the whole corpus.", size=10)
    for i, (a, b) in enumerate(COUNTS):
        y = 130 + 13 * i
        font = "F2" if i == 0 else "F1"
        pg.text(260 - width(a, font, 10), y, a, font, 10)
        pg.text(340 - width(b, font, 10), y, b, font, 10)
    pg.save("layout_two_col_counts.pdf")


YEARS = [("Jurisdiction", "Years"), ("United States", "2006 to 2009"),
         ("United Kingdom", "2008 to 2009"), ("Canada", "2005 to 2009"),
         ("Korea", "2004 to 2009"), ("Australia", "2005 to 2009")]


def make_leader_years():
    pg = Page()
    pg.text(72, 100, "Tax years open to examination in major jurisdictions:", size=10)
    for i, (a, b) in enumerate(YEARS):
        y = 130 + 13 * i
        if i == 0:
            pg.text(72, y, a, "F2", 10)
            pg.text(500 - width(b, "F2", 10), y, b, "F2", 10)
            continue
        right = 500 - width(b, "F1", 10)
        dots = ""
        while width(a + " " + dots + " .", "F1", 10) < right - 72 - 1:
            dots += " ."
        pg.text(72, y, a + " " + dots.strip(), size=10)
        pg.text(right, y, b, size=10)
    pg.save("layout_leader_years.pdf")


def make_contents():
    pg = Page()
    pg.text(72, 100, "Contents", "F2", 12)
    for i, (a, b) in enumerate(CONTENTS):
        y = 130 + 16 * i
        pg.text(90, y, a, size=10)
        pg.text(500 - width(b, "F1", 10), y, b, size=10)
    pg.save("layout_contents.pdf")


if __name__ == "__main__":
    make_counts()
    make_leader_years()
    make_contents()
    make_terms()
    make_split_prose()
    print("wrote grid_two_col_terms.pdf, grid_split_prose.pdf, layout_*.pdf to",
          os.path.dirname(os.path.abspath(__file__)))
