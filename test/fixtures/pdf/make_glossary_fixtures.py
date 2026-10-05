"""Regenerate the fixtures used by test_pdf ([35] a two-column table of
words is not two columns of text).

  column_glossary.pdf  a page holding a heading, one line of prose and a
      glossary of twenty-four rows: a short term at the left, its definition
      beside it, two definitions wrapping onto a second line, one term long
      enough to reach the definitions. The gap between the terms
      and the definitions runs the whole page down, so the page's gutter
      histogram dips there as it would between two text columns, and the
      long term keeps the rows from reading as one table. The terms never
      reach the right edge of their column, which is what tells the glossary
      from two columns of prose: each row must keep its term beside its
      definition.

  glossary_long.pdf  symbols beside definitions that run from a few words
      to most of a line, every definition on one line and no rule anywhere:
      a notation table. The definitions average more than thirty characters,
      which once made the candidate prose; their ragged right edge is what
      tells the table from a column of text.
  ruled_rows.pdf  a table of two columns of equal width, both holding
      sentences, three rows to a cell, a rule across the table under every
      cell row and above the first: a dialogue beside its replies. Nothing
      but the rules tells it from two columns of prose, and the rules must:
      each row keeps its two cells together.

The PDFs are written by hand (base-14 Helvetica, no font files) so the fixture
is the same on every OS. Run from the repo root:
    python3 test/fixtures/pdf/make_glossary_fixtures.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_short_page_fixtures import Page, width, wrap  # noqa: E402

ROWS = [("ASC", "Accounting Standards Codification"),
        ("ANS", "Alaskan North Slope crude oil, an oil index benchmark price"),
        ("ASU", "Accounting Standards Update"),
        ("ASR", "Accelerated share repurchase"),
        ("ATB", "Articulated tug barges"),
        ("barrel", "One stock tank barrel, or 42 United States gallons liquid volume, "
                   "used in reference to crude oil or other liquid hydrocarbons."),
        ("bcf/d", "One billion cubic feet per day"),
        ("CARB", "California Air Resources Board"),
        ("CARBOB", "California Reformulated Gasoline Blendstock for Oxygenate Blending"),
        ("CBOB", "Conventional Blending for Oxygenate Blending"),
        ("DEI", "Designated Environmental Incidents"),
        ("EBITDA (a non-GAAP financial measure)", "Earnings Before Interest, Tax, Depreciation and Amortization"),
        ("EPA", "United States Environmental Protection Agency"),
        ("FASB", "Financial Accounting Standards Board"),
        ("GAAP", "Accounting principles generally accepted in the United States"),
        ("IDR", "Incentive Distribution Right"),
        ("LCM", "Lower of cost or market"),
        ("LIFO", "Last in, first out"),
        ("mbpd", "Thousand barrels per day"),
        ("Mcf", "One thousand cubic feet of natural gas"),
        ("NGL", "Natural gas liquids, such as ethane, propane, butanes and natural "
                "gasoline"),
        ("NYMEX", "New York Mercantile Exchange"),
        ("OPEC", "Organization of Petroleum Exporting Countries"),
        ("ppb", "Parts per billion"),
        ("WTI", "West Texas Intermediate crude oil, an oil index benchmark price")]


def make_glossary():
    pg = Page()
    pg.text(72, 90, "Glossary of terms", "F2", 12)
    pg.text(72, 112, "Throughout this report the following terms and abbreviations are used:", size=10)
    x_term, x_def, right = 90, 265, 560
    y, lead = 140, 13
    for term, definition in ROWS:
        lines = wrap(definition, right - x_def, "F1", 10)
        pg.text(x_term, y, term, size=10)
        for ln in lines:
            pg.text(x_def, y, ln, size=10)
            y += lead
    pg.save("column_glossary.pdf")


TURNS = [("Input: hear it ?", "Choice: first reply"),
         ("First: i am sorry i did not hear you .", "Second: it is a little early in the morning ."),
         ("Fifth: that is the only one who could ever be .", "Seventh: what is the meaning of this ?"),
         ("Input: it feels like i have been asleep for weeks .", "Choice: first reply"),
         ("First: i am sorry i cannot help you .", "Second: it has been so long ."),
         ("Fifth: and then i felt sorry about it .", "Seventh: i am sorry i woke you up ."),
         ("Input: we lived in apartments inside red brick .", "Choice: second reply"),
         ("First: oh, really ?", "Second: we got a lot of stuff in the trunk ."),
         ("Fifth: we got to get back to the hotel .", "Seventh: i lived in a hotel ."),
         ("Input: you know you ought to find yourself a job .", "Choice: first reply"),
         ("First: i am not going to do that .", "Second: you know i am a real looker ."),
         ("Fifth: i am gonna make you some of your own .", "Seventh: you are in a big house !"),
         ("Input: the man who does that is not afraid to die .", "Choice: second reply"),
         ("First: i am not afraid of him .", "Second: but he is not afraid of the truth ."),
         ("Fifth: the man is a man of faith .", "Seventh: it is my duty to protect the father .")]


NOTATION = [("Symbol", "Description"),
            ("A", "sender agent"),
            ("R", "receiver agent"),
            ("S", "set of all possible messages used for communication by both agents"),
            ("O", "set of mammal classes"),
            ("Q", "set of mammal images available to the sender"),
            ("W", "set of mammal descriptions available to the receiver"),
            ("g", "ground-truth map between the images and the descriptions"),
            ("m", "binary message sent by the sender"),
            ("n", "binary message sent by the receiver"),
            ("T", "maximal number of time steps in a conversation"),
            ("t", "time step in conversation between sender and receiver"),
            ("h", "hidden state vector of the sender"),
            ("k", "hidden state of the receiver at time step t"),
            ("B", "baseline feedforward network of the sender"),
            ("L", "per-instance reinforcement learning loss"),
            ("H", "entropy regularization coefficient for the binary message distributions")]


def make_glossary_long():
    pg = Page()
    pg.text(72, 90, "Table 1: notation used throughout the paper.", size=10)
    y = 116
    for i, (a, b) in enumerate(NOTATION):
        font = "F2" if i == 0 else "F1"
        pg.text(72, y, a, font, 10)
        pg.text(140, y, b, font, 10)
        y += 13
    pg.save("glossary_long.pdf")


def make_ruled_rows():
    pg = Page()
    x0, xm, x1 = 60, 300, 540
    y, lead = 120, 12
    pg.rule(x0, x1, y - 10)
    for i, (a, b) in enumerate(TURNS):
        pg.text(x0 + 4, y, a, size=9)
        pg.text(xm + 4, y, b, size=9)
        y += lead
        if i % 3 == 2:
            pg.rule(x0, x1, y - 9)
            y += 2
    pg.save("ruled_rows.pdf")


if __name__ == "__main__":
    make_glossary()
    make_glossary_long()
    make_ruled_rows()
