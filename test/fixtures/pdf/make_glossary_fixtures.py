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
  leader_words.pdf  three asset classes joined by dot leaders to the lives
      they are depreciated over, in words ("5 to 40 years"), one value
      wrapping onto a second line, inside a page of prose. No rule, no
      figure column: the leaders pair the rows, and the rows must keep
      their alignment in a layout block.
  hanging_terms.pdf  a table of six products beside the terms of their
      revenue recognition, a bold header over both columns and no rule: a
      narrow column of terms (two wrapping onto a second line) beside a
      wide one of descriptions running from three to nine lines. The
      description lines beside no term are the cells' wrapped lines, and
      the band must hold them all; the shape (a narrow label column, a
      bold header) says table where the cells' lengths say prose.
  tables_apart.pdf  two tables of different make parted by one line of
      prose running from the left edge to the right: labels beside values
      set at the far right, then four columns of figures under a header.
      The line of prose ends the first band; the rows after it do not keep
      its column gap.
  group_labels.pdf  a two-column configuration table whose bold group
      labels span most of its width between the rows they head. A group
      label is no line of prose between two tables: the rows below it keep
      the column gap of the rows above, so the band holds together.
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
from make_short_page_fixtures import Page, width, wrap, PW  # noqa: E402

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


LIVES = [("Buildings and related improvements", "5 to 40 years"),
         ("Leasehold improvements", "Lesser of remaining term of the lease or"),
         ("Machinery and equipment", "1 to 15 years")]
BODY = ["Net property, plant, and equipment is recorded at cost less accumulated depreciation.",
        "Maintenance and repair expenditures are charged to expense when incurred. Depreciation",
        "is calculated using the straight-line method over the estimated useful lives as follows:"]


def make_leader_words():
    pg = Page()
    y = 100
    for ln in BODY:
        pg.text(72, y, ln, size=10)
        y += 13
    y += 8
    for a, b in LIVES:
        right = 330
        dots = ""
        while width(a + " " + dots + " .", "F1", 10) < right - 110 - 1:
            dots += " ."
        pg.text(110, y, a + " " + dots.strip(), size=10)
        pg.text(right + 18, y, b, size=10)
        y += 13
        if b.endswith("or"):
            pg.text(right + 18 + width("Lesser of ", "F1", 10), y, "economic useful life", size=10)
            y += 13
    y += 8
    for ln in BODY:
        pg.text(72, y, ln, size=10)
        y += 13
    pg.save("leader_words.pdf")


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


TERMS = [("Products and services", "Nature, timing of satisfaction of performance obligations, and significant payment terms"),
         ("Instruments", "For instruments that include installation, and if the installation meets the criteria to be "
                         "considered a separate performance obligation, product revenue is generally recognized upon "
                         "delivery or when title has transferred to the customer, which is generally the point in time "
                         "where control of the products has been transferred to customers, and installation revenue is "
                         "recognized when the installation is complete. Certain of the products require specialized "
                         "installation and configuration at the customer's site. Revenue for these products is deferred "
                         "until installation is complete and customer acceptance has been received. Payment terms and "
                         "conditions vary, although terms generally include a requirement of payment within 30 to 60 days."),
         ("Consumables and reagents", "Revenue from the sale of consumables and reagents is recognized upon delivery or when "
                                      "title has transferred to the customer, which is generally the point in time where "
                                      "control of the products has been transferred to customers. Payment terms and "
                                      "conditions vary, although terms generally include a requirement of payment within 30 days."),
         ("Software licenses and subscriptions", "Customers may purchase perpetual or term licenses, or subscribe to licenses, "
                                                 "which provide customers with the same functionality and differ mainly in the "
                                                 "duration over which the customer benefits from the software."),
         ("Cloud services", "Cloud services, which allow customers to use hosted software over the contract period without "
                            "taking possession of the software, are provided on either a subscription or consumption basis. "
                            "Revenue related to cloud services provided on a subscription basis is recognized ratably over "
                            "the contract period."),
         ("Extended warranty", "Revenue for extended warranties is recognized on a straight-line basis over the extended "
                               "warranty period in service revenue. The customary warranty period is one year and the "
                               "extended warranty covers periods beyond year one."),
         ("Laboratory services and training", "Service offerings include service contracts, field service, including related "
                                              "time and materials, and training. Revenue for the service contracts is "
                                              "recognized over the contract period or at a point in time when the service "
                                              "is billable based on time and materials.")]


def make_hanging_terms():
    pg = Page()
    pg.centred(80, "NOTES TO CONSOLIDATED FINANCIAL STATEMENTS (Continued)", "F2", 11)
    x_term, x_def, right = 72, 190, 540
    y, lead = 110, 12
    for i, (term, definition) in enumerate(TERMS):
        font = "F2" if i == 0 else "F1"
        size = 8 if i == 0 else 9
        tl = wrap(term, x_def - x_term - 12, font, size)
        dl = wrap(definition, right - x_def, font, size)
        for k in range(max(len(tl), len(dl))):
            if k < len(tl):
                pg.text(x_term, y, tl[k], font, size)
            if k < len(dl):
                pg.text(x_def, y, dl[k], font, size)
            y += lead
        y += 6
    pg.save("hanging_terms.pdf")


ASSUMPTIONS = [("Expected dividend yield", "$-"), ("Risk-free interest rate", "4.51%-4.99%"),
               ("Expected life of options (years)", "6.8"), ("Assumed volatility", "31.8%-35.7%"),
               ("Weighted average fair value", "$26.15")]
OPTIONS = [("Balance at December 31, 2006", "4,872", "$30.98", "2,697", "$23.80"),
           ("Granted", "433", "63.33", "", ""), ("Exercised", "(618)", "29.94", "", ""),
           ("Forfeited", "(81)", "40.92", "", ""),
           ("Balance at December 31, 2007", "4,606", "$33.98", "3,327", "$28.19"),
           ("Granted", "-", "-", "", ""), ("Exercised", "(833)", "60.13", "", ""),
           ("Forfeited", "(159)", "52.75", "", ""),
           ("Balance at December 31, 2008", "3,614", "$32.90", "3,245", "$30.39")]


def make_tables_apart():
    pg = Page()
    pg.centred(70, "NOTES TO CONSOLIDATED FINANCIAL STATEMENTS", "F2", 10)
    pg.text(72, 100, "No stock option awards were granted during the years ended December 31, 2009 and 2008.", size=10)
    pg.text(72, 113, "The following table indicates the assumptions used in estimating fair value:", size=10)
    y = 140
    pg.text(500 - width("2007", "F2", 9), y, "2007", "F2", 9)
    y += 13
    for a, b in ASSUMPTIONS:
        pg.text(90, y, a, size=9)
        pg.text(500 - width(b, "F1", 9), y, b, size=9)
        y += 13
    y += 10
    pg.text(72, y, "The following table summarizes stock option activity under the equity compensation plans:", size=10)
    y += 24
    cols = [372, 418, 466, 512]
    for x, h in zip(cols, ("Options", "Price", "Options", "Price")):
        pg.text(x - width(h, "F2", 8), y, h, "F2", 8)
    y += 13
    for row in OPTIONS:
        pg.text(72 if row[0].startswith("Balance") else 90, y, row[0], "F2" if row[0].startswith("Balance") else "F1", 9)
        for x, v in zip(cols, row[1:]):
            if v:
                pg.text(x - width(v, "F1", 9), y, v, size=9)
        y += 13
    pg.save("tables_apart.pdf")


CONFIG = [("Configuration", None),
          ("No. of Cameras in the ACS", "2"), ("No. of Joints in the ACS", "1"),
          ("Random transformations per test (n)", "30"), ("Number of tests", "100"),
          ("Transformations with fixed joint pose:", None),
          ("Rotations of cameras (RA, RB)", "2 x 30 x 100"), ("Translations of cameras (TA, TB)", "2 x 30 x 100"),
          ("General transformations:", None),
          ("Rotations of cameras (RA, RB)", "2 x 30 x 100"), ("Translations of cameras (TA, TB)", "2 x 30 x 100"),
          ("Zero mean Gaussian noise:", None),
          ("Rotation noise (degrees)", "0 to 2.4"), ("Translation noise (meters)", "0 to 0.1")]


def make_group_labels():
    pg = Page()
    pg.centred(80, "Table 2: Simulation setup.", "F1", 10)
    x_l, x_r = 180, 400
    y = 110
    for a, b in CONFIG:
        if b is None:
            if a == "Configuration":
                pg.text((PW - width(a, "F2", 10)) / 2, y, a, "F2", 10)
            else:
                pg.text(x_l, y, a, "F2", 10)
        else:
            pg.text(x_l, y, a, size=10)
            pg.text(x_r, y, b, size=10)
        y += 13
    pg.save("group_labels.pdf")


if __name__ == "__main__":
    make_glossary()
    make_glossary_long()
    make_leader_words()
    make_ruled_rows()
    make_hanging_terms()
    make_tables_apart()
    make_group_labels()
