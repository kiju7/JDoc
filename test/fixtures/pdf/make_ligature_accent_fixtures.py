"""Regenerate the ligature and accent fixture used by test_pdf ([34]
ligatures and spacing accents).

  ligature_accent.pdf  three lines in Helvetica whose /Differences map codes
      to the glyph names "fl", "fi" and "tilde", as TeX output does:
      1. "efﬂuents and ﬁelds", the ligatures one glyph each;
      2. "Muñoz", set as n with a tilde drawn back over it from the same font;
      3. "Y" with a circumflex from another font (Helvetica-Bold) drawn over
         it, as an equation sets a hat over a variable. It stays as drawn.

The PDF is written by hand (base-14 fonts, no font files) so the fixture is
the same on every OS. Run from the repo root:
    python3 test/fixtures/pdf/make_ligature_accent_fixtures.py
"""
import os

OUT = os.path.dirname(os.path.abspath(__file__))
PW, PH = 595, 842

# Codes 128.. map to glyph names through /Differences.
FL, FI, TILDE, CIRC = "\\200", "\\201", "\\202", "\\203"
# Helvetica advances (1/1000 em): n 556, tilde 333; Y 667, circumflex 333.
BACK_N = (556 + 333) / 2          # from after "n" back to the tilde's left
FWD_N = (556 - 333) / 2           # from after the tilde to after "n"
BACK_Y = (667 + 333) / 2


def content():
    c = "BT /F1 12 Tf 72 700 Td (ef%s) Tj (uents and %selds) Tj ET\n" % (FL, FI)
    c += "BT /F1 12 Tf 72 680 Td [(Mun) %.0f (%s) %.0f (oz) ] TJ ET\n" % (BACK_N, TILDE, -FWD_N)
    c += "BT /F1 12 Tf 72 660 Td (The estimate Y) Tj /F2 12 Tf [%.0f (%s)] TJ /F1 12 Tf ( is close.) Tj ET\n" % (BACK_Y, CIRC)
    return c


def save(name):
    body = content().encode("latin-1")
    enc = b"<< /Type /Encoding /BaseEncoding /WinAnsiEncoding /Differences [128 /fl /fi /tilde /circumflex] >>"
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
         "/Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> /Contents 4 0 R >>" % (PW, PH)).encode(),
        b"<< /Length %d >>\nstream\n" % len(body) + body + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding 7 0 R >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding 7 0 R >>",
        enc,
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
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(out)


if __name__ == "__main__":
    save("ligature_accent.pdf")
    print("wrote ligature_accent.pdf to", OUT)
