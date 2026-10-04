"""Regenerate shaded_band_table.pdf used by test_pdf ([20] banded tables
whose first or last row is unshaded).

Rule-less tables whose rows are separated by background shading only.

Table 1 ("Park size") has a darker header band and zebra body rows: body
row 2 is shaded, rows 1 and 3 are white. Its fills are written as
`x y w h re h f`, the redundant closepath MuPDF emits; the duplicate h must
not turn the fill into a non-rect path whose edges become fake rules.

Table 2 ("Hour") leaves the header white and shades body rows 1 and 3, so
the white header (above the first stripe) and white row 4 (below the last
stripe) both sit outside the painted run.

Table 3 ("Samples") tints only its two header rows; the body below runs at
a tighter row pitch. The body is not a continuation of the bands: its first
row must stay with the rest of the body, not join the header's table.

Every row of tables 1 and 2 must reach the markdown inside its table, never as
prose; the caption above and the paragraph below each table must stay out.
With table detection off, the right-aligned numbers of a row must keep their
word spaces ("Small 3 2.8 0.5"), however wide the gap between them.

The PDF is written by hand (base-14 Helvetica, no font files) so the
fixture is the same on every OS. Run from the repo root:
    python3 test/fixtures/pdf/make_shaded_band_fixture.py
"""
import os

OUT = os.path.dirname(os.path.abspath(__file__))
PW, PH = 595, 842
FS = 10.5
RH = 24
COLW = [120, 90, 110, 110]
X0 = (PW - sum(COLW)) / 2

T1_HEAD = ["Size", "Parks", "Mean area ha", "Cooling"]
T1_ROWS = [["Large", "4", "38.5", "2.4"], ["Medium", "5", "12.0", "1.3"],
           ["Small", "3", "2.8", "0.5"]]
T2_HEAD = ["Hour", "Inside", "Road", "Gap"]
T2_ROWS = [["10 am", "28.4", "29.6", "1.2"], ["2 pm", "31.9", "34.1", "2.2"],
           ["6 pm", "29.7", "31.0", "1.3"], ["10 pm", "25.2", "25.9", "0.7"]]
T3_HEAD = [["", "Sample", "2007", "2006"], ["Country", "size", "n", "n"]]
T3_ROWS = [["Austria", "25g", "109", "93"], ["Germany", "25g", "123", "290"],
           ["Spain", "25g", "36", "40"], ["Total", "", "268", "423"]]

# Helvetica widths (1/1000 em) for the characters used.
W = {" ": 278, ".": 278}
for c in "0123456789abcdeghknopqsuvxyz":
    W[c] = 556
W.update({"f": 278, "i": 222, "j": 222, "l": 222, "m": 833, "r": 333,
          "t": 278, "w": 722, "C": 722, "G": 778, "H": 722, "I": 278,
          "L": 556, "M": 833, "P": 667, "R": 722, "S": 667, "T": 611})


def width(s, size=FS, bold=False):
    return sum(W.get(c, 556) for c in s) * size / 1000.0 * (1.06 if bold else 1.0)


def text(font, size, x, y, s):
    s = s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
    return "BT /%s %.1f Tf %.2f %.2f Td (%s) Tj ET\n" % (font, size, x, y, s)


def table(top, head, rows, shaded, closepath, caption, body_rh=RH):
    """head: one header row or a list of header rows (each RH tall); body
    rows are body_rh tall. shaded maps a row index (headers first) to RGB."""
    heads = head if isinstance(head[0], list) else [head]
    c = text("F2", FS, (PW - width(caption, bold=True)) / 2, top + 8, caption)
    xs = [X0]
    for w in COLW:
        xs.append(xs[-1] + w)
    ys = [top]
    for r in range(len(heads) + len(rows)):
        ys.append(ys[-1] - (RH if r < len(heads) else body_rh))
    for r, gray in shaded.items():
        c += "q %.2f %.2f %.2f rg %.2f %.2f %.2f %.2f re%s f Q\n" % (
            gray[0], gray[1], gray[2], xs[0], ys[r + 1], xs[-1] - xs[0],
            ys[r] - ys[r + 1], " h" if closepath else "")
    for r, row in enumerate(heads + rows):
        bold = r < len(heads)
        font = "F2" if bold else "F1"
        yb = (ys[r] + ys[r + 1]) / 2 - 4
        for k, v in enumerate(row):
            if not v:
                continue
            if k == 0:
                x = xs[0] + 12
            else:
                x = xs[k + 1] - 14 - width(v, bold=bold)
            c += text(font, FS, x, yb, v)
    return c, ys[-1]


def content():
    c = text("F2", 14, 62, 760, "5. Park size and time of day")
    t, bottom = table(712, T1_HEAD, T1_ROWS,
                      {0: (0.80, 0.86, 0.95), 2: (0.92, 0.95, 0.99)}, True,
                      "Table 1. Cooling by park size")
    c += t
    c += text("F1", 11.5, 62, bottom - 26,
              "Larger parks were cooler than the roads around them, by up to 2.4 degrees.")
    t, bottom = table(bottom - 90, T2_HEAD, T2_ROWS,
                      {1: (0.85, 0.85, 0.85), 3: (0.85, 0.85, 0.85)}, False,
                      "Table 2. Mean temperature by hour")
    c += t
    c += text("F1", 11.5, 62, bottom - 26,
              "The gap between the park and the road was widest at 2 pm in the afternoon.")
    t, bottom = table(bottom - 90, T3_HEAD, T3_ROWS,
                      {0: (0.80, 0.86, 0.95), 1: (0.80, 0.86, 0.95)}, False,
                      "Table 3. Samples by year", body_rh=14)
    c += t
    return c.encode("latin-1")


def build(path):
    body = content()
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
         "/Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> /Contents 4 0 R >>"
         % (PW, PH)).encode(),
        b"<< /Length %d >>\nstream\n" % len(body) + body + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>",
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
    build(os.path.join(OUT, "shaded_band_table.pdf"))
