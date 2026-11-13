"""Regenerate nbsp_table.pdf used by test_pdf ([24] table rows spelled
with no-break spaces).

A fully ruled 4x5 table: a shaded header row in Helvetica-Bold, centred
labels in the first column, right-aligned numbers, prose above and below.
The word spaces inside cells are drawn as U+00A0 (code 160 -> /nbspace),
as PDF writers that subset a font's space glyph under its no-break name
produce them: header labels ("Area ha"), a first-column label ("North
Park") and a number with a thin thousands gap ("12 300"). Every row's text
must reach the markdown once, inside the table, and never again as prose.

The PDF is written by hand (base-14 fonts, no font files) so the fixture
is the same on every OS. Run from the repo root:
    python3 test/fixtures/pdf/make_nbsp_table_fixture.py
"""
import os

OUT = os.path.dirname(os.path.abspath(__file__))
NB = "\xa0"

HEAD = ["Park", "Area" + NB + "ha", "Inside" + NB + "temp", "Road" + NB + "temp"]
ROWS = [["Central", "42", "31.2", "33.9"],
        ["River", "28", "31.8", "34.1"],
        ["North" + NB + "Park", "12" + NB + "300", "32.4", "34.0"],
        ["Corner", "3", "33.6", "34.2"]]
COLW = [110, 80, 120, 130]
RH = 24
PW, PH = 595, 842
FS = 10.5

# Helvetica widths (1/1000 em) for the characters used in the table.
W = {" ": 278, NB: 278, ".": 278, "0": 556, "1": 556, "2": 556, "3": 556,
     "4": 556, "5": 556, "6": 556, "7": 556, "8": 556, "9": 556}
for c in "abcdeghknopqsuvxyz":
    W[c] = 556
W.update({"f": 278, "i": 222, "j": 222, "l": 222, "m": 833, "r": 333,
          "t": 278, "w": 722, "A": 667, "C": 722, "N": 722, "P": 667,
          "R": 722})


def width(s, bold=False):
    # Bold widths differ slightly; a 6% allowance keeps alignment close.
    return sum(W.get(c, 556) for c in s) * FS / 1000.0 * (1.06 if bold else 1.0)


def esc(s):
    out = ""
    for c in s:
        b = ord(c)
        if c in "()\\":
            out += "\\" + c
        elif b > 126:
            out += "\\%03o" % b
        else:
            out += c
    return out


def text(font, size, x, y, s):
    return "BT /%s %.1f Tf %.2f %.2f Td (%s) Tj ET\n" % (font, size, x, y, esc(s))


def content():
    c = ""
    c += text("F1", 14, 62, 742, "3. Results")
    c += text("F1", 11.5, 62, 716, "Larger parks were cooler than the roads around them.")
    xs = [77.5]
    for w in COLW:
        xs.append(xs[-1] + w)
    top = 632
    ys = [top - i * RH for i in range(len(ROWS) + 2)]
    c += "0.93 0.94 0.96 rg %.2f %.2f %.2f %.2f re f 0 g\n" % (
        xs[0], ys[1], xs[-1] - xs[0], RH)
    c += "0.6 w 0 G\n"
    for y in ys:
        c += "%.2f %.2f m %.2f %.2f l S\n" % (xs[0], y, xs[-1], y)
    for x in xs:
        c += "%.2f %.2f m %.2f %.2f l S\n" % (x, ys[0], x, ys[-1])
    for r, row in enumerate([HEAD] + ROWS):
        bold = r == 0
        font = "F2" if bold else "F1"
        yb = ys[r] - RH / 2 - 4
        for k, v in enumerate(row):
            w = width(v, bold)
            if r == 0 or k == 0:
                x = xs[k] + (xs[k + 1] - xs[k] - w) / 2
            else:
                x = xs[k + 1] - 12 - w
            c += text(font, FS, x, yb, v)
    c += text("F1", 11.5, 62, ys[-1] - 34, "The smallest park was only 0.6 degrees cooler.")
    return c.encode("latin-1")


def build(path):
    enc = "<< /Type /Encoding /BaseEncoding /WinAnsiEncoding /Differences [160 /nbspace] >>"
    body = content()
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
         "/Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> /Contents 4 0 R >>"
         % (PW, PH)).encode(),
        b"<< /Length %d >>\nstream\n" % len(body) + body + b"\nendstream",
        ("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding %s >>" % enc).encode(),
        ("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding %s >>" % enc).encode(),
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
    with open(path, "wb") as f:
        f.write(out)


if __name__ == "__main__":
    build(os.path.join(OUT, "nbsp_table.pdf"))
