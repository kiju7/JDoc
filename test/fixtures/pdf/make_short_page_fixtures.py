"""Regenerate the short-page column fixtures used by test_pdf ([29] column
detection on pages with few lines).

  short_page_columns.pdf  a running head (journal line left, page number
      right), a centred title, then two columns side by side: a heading and
      four lines on the left (the last one short), a heading and three lines
      on the right. Ten rows in all, too few for the gutter histogram alone.
  short_page_table.pdf    a short single-column page with a borderless
      two-column table centred on the page, each cell centred in its column.
  short_page_form.pdf     a short form: labels in a narrow left column,
      long values in a wide right column.
  short_page_single.pdf   a short single-column page: heading, paragraph,
      a short list.
  short_page_numbers.pdf  a caption, column heads set sideways, and three
      rows of figures across the page, the cells as close together as a
      word gap is wide, so one of the gaps between cells falls on the page
      centre.

The table and the form put their two columns either side of the page centre
with a clear gap, as two-column text does; only the text inside the columns
(centred cells, columns of unequal width) tells them apart. Each must keep
its rows together.

The PDFs are written by hand (base-14 Helvetica, no font files) so the
fixtures are the same on every OS. Run from the repo root:
    python3 test/fixtures/pdf/make_short_page_fixtures.py
"""
import os

OUT = os.path.dirname(os.path.abspath(__file__))
PW, PH = 595, 842
ML, MR, GUT = 52, 52, 22
CW = (PW - ML - MR - GUT) / 2

# Helvetica and Helvetica-Bold advance widths (1/1000 em), printable ASCII.
_HV = [278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333,
       278, 278] + [556] * 10 + [278, 278, 584, 584, 584, 556, 1015, 667, 667,
       722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667,
       778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469,
       556, 333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222,
       833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500,
       334, 260, 334, 584]
_HB = [278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333,
       278, 278] + [556] * 10 + [333, 333, 584, 584, 584, 611, 975, 722, 722,
       722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778, 667,
       778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584,
       556, 333, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278,
       889, 611, 611, 611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500,
       389, 280, 389, 584]
WIDTHS = {"F1": _HV, "F2": _HB}

PROSE_L = ("Green space in a city is known to lower the air temperature on a "
           "summer afternoon, but how much it lowers it varies with the size "
           "of the park and the roads around it, which this study measures.")
PROSE_R = ("The readings were taken last summer in twelve city parks, twice a "
           "day, with the same calibrated thermometer at every site and the "
           "same height above the ground.")


def width(s, font, size):
    return sum(WIDTHS[font][ord(c) - 32] for c in s) * size / 1000.0


def wrap(text, w, font, size):
    lines, cur = [], ""
    for word in text.split():
        cand = (cur + " " + word).strip()
        if cur and width(cand, font, size) > w:
            lines.append(cur)
            cur = word
        else:
            cur = cand
    if cur:
        lines.append(cur)
    return lines


class Page:
    def __init__(self):
        self.c = ""

    def text(self, x, y, s, font="F1", size=11, gray=None):
        # y counts down from the top of the page, as the layout reads it.
        s = s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
        col = "%.2f g " % gray if gray is not None else ""
        self.c += "BT %s/%s %.1f Tf %.2f %.2f Td (%s) Tj ET%s\n" % (
            col, font, size, x, PH - y, s, " 0 g" if gray is not None else "")

    def centred(self, y, s, font="F1", size=11):
        self.text((PW - width(s, font, size)) / 2, y, s, font, size)

    def sideways(self, x, y, s, size=8):
        # Rotated 90 degrees counter-clockwise, reading upwards from (x, y).
        self.c += "BT /F1 %.1f Tf 0 1 -1 0 %.2f %.2f Tm (%s) Tj ET\n" % (
            size, x, PH - y, s)

    def rule(self, x0, x1, y):
        self.c += "0.5 w %.2f %.2f m %.2f %.2f l S\n" % (x0, PH - y, x1, PH - y)

    def save(self, name):
        body = self.c.encode("latin-1")
        objs = [
            b"<< /Type /Catalog /Pages 2 0 R >>",
            b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
             "/Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> /Contents 4 0 R >>"
             % (PW, PH)).encode(),
            b"<< /Length %d >>\nstream\n" % len(body) + body + b"\nendstream",
            b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
            b"/Encoding /WinAnsiEncoding >>",
            b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold "
            b"/Encoding /WinAnsiEncoding >>",
        ]
        out = bytearray(b"%PDF-1.4\n")
        offs = []
        for i, o in enumerate(objs, 1):
            offs.append(len(out))
            out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
        xref = len(out)
        out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
        for o in offs:
            out += b"%010d 00000 n \n" % o
        out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
            len(objs) + 1, xref)
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(out)


def running_head(p):
    p.text(ML, 40, "Journal of Urban Climate  Vol. 8  No. 2  2026", size=9, gray=0.45)
    p.text(PW - MR - width("45", "F1", 9), 40, "45", size=9, gray=0.45)
    p.rule(ML, PW - MR, 48)


def columns_page():
    p = Page()
    running_head(p)
    p.centred(96, "Green Space and Summer Heat", "F2", 18)
    y0, lh = 140, 16
    xr = ML + CW + GUT
    p.text(ML, y0, "1. Introduction", "F2", 12)
    p.text(xr, y0, "2. Measurement", "F2", 12)
    left = wrap(PROSE_L, CW, "F1", 11)[:3] + ["around it."]
    right = wrap(PROSE_R, CW * 0.85, "F1", 11)[:3]
    for col, (x, rows) in enumerate(((ML, left), (xr, right))):
        for i, t in enumerate(rows):
            assert width(t, "F1", 11) <= CW, t
            p.text(x, y0 + 24 + i * lh, t)
    p.save("short_page_columns.pdf")
    return left, right


TABLE = [("Park", "Mean temperature"),
         ("Central", "31.2 degrees"),
         ("Riverside Garden", "33.6"),
         ("North Hill", "32.4 in the shade"),
         ("Corner lot", "34.0")]


def table_page():
    p = Page()
    running_head(p)
    p.centred(96, "Summer Temperatures by Park", "F2", 18)
    p.text(ML, 130, "Table 1 lists the mean afternoon reading in each park.")
    c1, c2 = PW / 2 - 120, PW / 2 + 120  # column centres, gap at the page centre
    for r, (a, b) in enumerate(TABLE):
        font = "F2" if r == 0 else "F1"
        y = 160 + r * 18
        p.text(c1 - width(a, font, 11) / 2, y, a, font)
        p.text(c2 - width(b, font, 11) / 2, y, b, font)
    p.save("short_page_table.pdf")


FORM = [("Applicant", "Kim Minsu, Department of Computer Engineering"),
        ("Date of submission", "5 October 2026, by the online system"),
        ("Title of the study", "Urban green space and summer air temperature"),
        ("Supervisor", "Professor Lee Jiwon, Graduate School of Software")]


def form_page():
    p = Page()
    running_head(p)
    p.centred(96, "Application Form", "F2", 18)
    for r, (a, b) in enumerate(FORM):
        y = 140 + r * 20
        p.text(ML, y, a, "F2")
        p.text(ML + 175, y, b)
    p.save("short_page_form.pdf")


def single_page():
    p = Page()
    running_head(p)
    p.text(ML, 96, "3. Results", "F2", 14)
    para = wrap(PROSE_L + " " + PROSE_R, PW - ML - MR, "F1", 11)
    for i, t in enumerate(para):
        p.text(ML, 124 + i * 16, t)
    y = 124 + len(para) * 16 + 14
    for i, t in enumerate(["- Larger parks were cooler.",
                           "- Roads warmed the park edges.",
                           "- Shade mattered more than grass."]):
        p.text(ML + 12, y + i * 16, t)
    p.save("short_page_single.pdf")


def numbers_page():
    p = Page()
    running_head(p)
    p.text(ML, 96, "Table 9: Accuracy across tasks.", size=10)
    heads = ["Caltech101", "CIFAR-100", "DTD", "Flowers102", "Pets", "Sun397",
             "SVHN", "Camelyon", "EuroSAT", "Resisc45", "Retinopathy",
             "Clevr-Count", "Clevr-Dist", "DMLab", "Mean"]
    rows = [("Model-H (large)", [95.3, 85.5, 75.2, 99.7, 97.2, 65.0, 88.9, 83.3,
                                 96.7, 91.4, 76.6, 91.7, 63.8, 53.1, 79.4]),
            ("Model-L (large)", [95.4, 81.9, 74.3, 99.7, 96.7, 63.5, 87.4, 83.6,
                                 96.5, 89.7, 77.1, 86.4, 63.1, 49.7, 74.5]),
            ("Model-L (small)", [90.8, 84.1, 74.1, 99.3, 92.7, 61.0, 80.9, 82.5,
                                 95.6, 85.2, 75.3, 70.3, 56.1, 41.9, 74.7])]
    x = ML + 62
    for h, v in zip(heads, rows[0][1]):
        p.sideways(x + 9, 170, h)
        x += width("%.1f" % v, "F1", 8) + 9.2
    for r, (label, vals) in enumerate(rows):
        y = 184 + r * 12
        p.text(ML, y, label, size=8)
        x = ML + 62
        for v in vals:
            p.text(x, y, "%.1f" % v, size=8)
            x += width("%.1f" % v, "F1", 8) + 9.2
    p.save("short_page_numbers.pdf")


if __name__ == "__main__":
    print(columns_page())
    table_page()
    form_page()
    single_page()
    numbers_page()
