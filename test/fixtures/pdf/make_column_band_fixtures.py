"""Regenerate the column band fixtures used by test_pdf ([31] two columns in
part of a page).

  column_band_sidebar.pdf  a narrow side column (keywords, correspondence)
      beside a wide abstract, over a two-column body whose gutter is at the
      page centre: the side column's gutter is elsewhere.
  column_band_terms.pdf    a borderless table of short terms beside long
      definitions, wrapped over two lines each. Its rows must stay whole.

The PDFs are written by hand (base-14 Helvetica, no font files) so the
fixtures are the same on every OS. Run from the repo root:
    python3 test/fixtures/pdf/make_column_band_fixtures.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_short_page_fixtures import PW, Page, wrap  # noqa: E402

ML, MR = 52, 52
TW = PW - ML - MR

ABSTRACT = ("Parks lower the air temperature of the streets around them, but "
            "by how much depends on their size, their trees, and the wind. "
            "We measured air temperature in twelve parks of one city twice a "
            "day for a year and compared it with readings taken at the same "
            "hours in the streets nearby. Large parks with tall trees cooled "
            "the most, and their effect reached several blocks.")
SIDE = ["Keywords", "urban heat island; city parks", "", "Correspondence",
        "Department of Geography, City", "University, 12 Riverside Road,",
        "Riverside 12345, Country.", "E-mail: parks.study@example.org"]
BODY_L = ("Cities are warmer than the country around them because pavement "
          "and roofs store the heat of the day and release it at night. "
          "Parks interrupt this, and planners want to know how large a park "
          "must be before the streets around it feel the difference. ") * 3
BODY_R = ("Earlier studies measured single parks over a few weeks. We kept "
          "the same instruments in twelve parks for a full year so that "
          "seasons, winds and park sizes could be compared on one footing. ") * 3
TERMS = [("Albedo", "The share of sunlight a surface reflects instead of "
          "absorbing, from zero for black to one for white."),
         ("Canopy", "The layer of leaves and branches that shades the ground "
          "below a group of trees in full leaf."),
         ("Heat island", "A city area that stays warmer than its surroundings, "
          "most noticeably during the night."),
         ("Sky view", "The fraction of the sky visible from a point in the "
          "street, which sets how fast it cools after dark.")]


def two_columns(pg, y, left, right, x_l, w_l, x_r, w_r, size=10, lead=13):
    ll, rl = wrap(left, w_l, "F1", size), wrap(right, w_r, "F1", size)
    for i in range(max(len(ll), len(rl))):
        if i < len(ll):
            pg.text(x_l, y + i * lead, ll[i], size=size)
        if i < len(rl):
            pg.text(x_r, y + i * lead, rl[i], size=size)
    return y + max(len(ll), len(rl)) * lead


def make_sidebar():
    pg = Page()
    pg.text(ML, 60, "Journal of Urban Climate  Vol. 8  No. 2  2026", size=8)
    pg.centred(96, "Parks and Street Temperature", "F2", 15)
    side_w, gut = 150, 22
    ax = ML + side_w + gut
    aw = PW - MR - ax
    pg.text(ax, 130, "Abstract", "F2", 10)
    al = wrap(ABSTRACT + " " + ABSTRACT, aw, "F1", 9)
    y = 146
    for i in range(max(len(SIDE), len(al))):
        if i < len(SIDE) and SIDE[i]:
            bold = SIDE[i] in ("Keywords", "Correspondence")
            pg.text(ML, y, SIDE[i], "F2" if bold else "F1", 8)
        if i < len(al):
            pg.text(ax, y, al[i], size=9)
        y += 12
    y += 18
    cw, cg = (TW - 22) / 2, 22
    pg.text(ML, y, "1. Introduction", "F2", 11)
    two_columns(pg, y + 18, BODY_L, BODY_R, ML, cw, ML + cw + cg, cw)
    pg.save("column_band_sidebar.pdf")


def make_terms():
    pg = Page()
    pg.text(ML, 70, "Glossary", "F2", 13)
    tx, dx = ML, ML + 100
    dw = PW - MR - dx
    y = 100
    for term, text in TERMS:
        pg.text(tx, y, term, "F2", 10)
        for line in wrap(text, dw, "F1", 10):
            pg.text(dx, y, line, size=10)
            y += 13
        y += 6
    pg.save("column_band_terms.pdf")


if __name__ == "__main__":
    make_sidebar()
    make_terms()
    print("wrote column_band_{sidebar,terms}.pdf to", os.path.dirname(os.path.abspath(__file__)))
