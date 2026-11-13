#!/usr/bin/env python3
"""Generate the word-spacing fixtures (stdlib only, Courier, no space glyphs
between separated runs: every gap is geometry, the way most producers set
cells, footers and page numbers).

  rotated_text.pdf  page 1: a 4x6 table turned 90 degrees (each cell its own
                    text object), page 2: the same table at 180, page 3: at
                    270, page 4: upright body text with a sideways (90)
                    caption whose words are separated by TJ offsets only,
                    the last one set with an "ff" ligature glyph.
  far_gap.pdf       a running footer and its page number on one baseline,
                    a running head with its page number at the far left,
                    a section heading, words that must stay whole
                    (Tc-spread, kerned tight, one glyph per Td), a written
                    space kerned nearly shut (still a space) and one kerned
                    out entirely (not a space).

Run from the repo root: python3 test/fixtures/pdf/gen_text_spacing.py
"""
import math, os

HERE = os.path.dirname(os.path.abspath(__file__))


def pdf(pages, path):
    """pages: list of (width, height, content-bytes)."""
    objs = []
    # '~' maps to the two letters "ff" (a ligature glyph); every other code
    # maps to itself.
    cmap = (b"/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
            b"/CMapName /Lig def 1 begincodespacerange <00> <FF> "
            b"endcodespacerange\n1 beginbfrange <20> <7D> <0020> endbfrange\n"
            b"1 beginbfchar <7E> <00660066> endbfchar\n"
            b"endcmap CMapName currentdict /CMap defineresource pop end end")
    objs.append(b"<< /Length %d >>\nstream\n" % len(cmap) + cmap +
                b"\nendstream")
    font = len(objs) + 1
    objs.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Courier "
                b"/FirstChar 32 /LastChar 126 /Widths [" +
                b" ".join([b"600"] * 95) + b"] /ToUnicode 1 0 R >>")
    kids = []
    page_ids = []
    for w, h, content in pages:
        cid = len(objs) + 1
        objs.append(b"<< /Length %d >>\nstream\n" % len(content) + content +
                    b"\nendstream")
        pid = len(objs) + 1
        objs.append(None)  # page, filled once the pages id is known
        page_ids.append((pid, cid, w, h))
    pages_id = len(objs) + 1
    for pid, cid, w, h in page_ids:
        objs[pid - 1] = (b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %d %d] "
                         b"/Resources << /Font << /F1 %d 0 R >> >> "
                         b"/Contents %d 0 R >>" % (pages_id, w, h, font, cid))
        kids.append(b"%d 0 R" % pid)
    objs.append(b"<< /Type /Pages /Kids [" + b" ".join(kids) +
                b"] /Count %d >>" % len(kids))
    cat = len(objs) + 1
    objs.append(b"<< /Type /Catalog /Pages %d 0 R >>" % pages_id)

    out = bytearray(b"%PDF-1.4\n")
    offs = []
    for i, o in enumerate(objs):
        offs.append(len(out))
        out += b"%d 0 obj\n" % (i + 1) + o + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for o in offs:
        out += b"%010d 00000 n \n" % o
    out += (b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n"
            % (len(objs) + 1, cat, xref))
    with open(path, "wb") as f:
        f.write(out)


def esc(s):
    return s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def text_at(s, x, y, deg=0, size=10):
    """One text object: string s with its origin at (x, y), baseline at deg."""
    c = round(math.cos(math.radians(deg)), 6)
    n = round(math.sin(math.radians(deg)), 6)
    return ("BT /F1 %g Tf %g %g %g %g %g %g Tm (%s) Tj ET\n" %
            (size, c, n, -n, c, x, y, esc(s))).encode()


TABLE = [["Item", "Q1", "Q2", "Q3"]] + \
    [["Row%02d" % i, str(10 + i), str(20 + i), str(30 + i)] for i in range(1, 6)]


def rotated_table(deg):
    """The table laid out in its own frame (columns 130 pt apart, wider than
    8 em of gap after a cell, rows 20 pt apart), then turned by deg about the
    page centre."""
    cx, cy = 306, 396
    out = b""
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    for r, row in enumerate(TABLE):
        for k, cell in enumerate(row):
            fx, fy = -200 + 130 * k, 50 - 20 * r   # frame coordinates
            px, py = cx + fx * c - fy * s, cy + fx * s + fy * c
            out += text_at(cell, px, py, deg)
    return out


def tj_words(words, x, y, deg=0, size=10):
    """Words in one TJ, separated by a -600 offset (one Courier space) and no
    space glyph."""
    c = round(math.cos(math.radians(deg)), 6)
    n = round(math.sin(math.radians(deg)), 6)
    arr = " -600 ".join("(%s)" % esc(w) for w in words)
    return ("BT /F1 %g Tf %g %g %g %g %g %g Tm [%s] TJ ET\n" %
            (size, c, n, -n, c, x, y, arr)).encode()


def rotated_pdf():
    mixed = text_at("Upright body text stays on the page grid.", 72, 700)
    mixed += tj_words(["Sideways", "caption", "with", "five", "words", "o~set"],
                      560, 200, 90)
    pdf([(612, 792, rotated_table(90)),
         (612, 792, rotated_table(180)),
         (612, 792, rotated_table(270)),
         (612, 792, mixed)], os.path.join(HERE, "rotated_text.pdf"))


def far_gap_pdf():
    body = b""
    for i, y in enumerate(range(660, 560, -14)):
        body += text_at("Body text line %d of the introduction section." % (i + 1),
                        72, y)
    c = body
    # Tc-spread: letters 8 pt further apart than their advance.
    c += b"BT /F1 10 Tf 8 Tc 72 520 Td (SPREAD) Tj 0 Tc ET\n"
    # Kerned tight: a positive TJ offset pulls each letter in.
    c += b"BT /F1 10 Tf 72 500 Td [(K) 80 (e) 80 (r) 80 (n) 80 (e) 80 (d)] TJ ET\n"
    # One glyph per Td, at the natural advance.
    c += b"BT /F1 10 Tf 72 480 Td (G) Tj 6 0 Td (l) Tj 6 0 Td (y) Tj " \
         b"6 0 Td (p) Tj 6 0 Td (h) Tj 6 0 Td (s) Tj ET\n"
    # A written space set in a 6-pt font 5 pt up, the next word kerned back
    # so 1.2 pt (0.12 em) of gap is left: off the baseline, so line grouping
    # loses it, and narrower than a word space, so geometry alone glues.
    c += (b"BT /F1 10 Tf 72 460 Td (Maria) Tj /F1 6 Tf 5 Ts ( ) Tj "
          b"0 Ts [400] TJ /F1 10 Tf (Hazel) Tj ET\n")
    # A break space kerned out entirely (zero gap): no word space.
    c += b"BT /F1 10 Tf 72 440 Td [(www.) ( ) 600 (example.org)] TJ ET\n"
    # Running footer and its page number on one baseline.
    c += text_at("Annual Economic Report 2024", 72, 50) + text_at("99", 520, 50)
    # Page 2: a running head with its page number at the far left (an even
    # page), then a numbered section heading.
    c2 = text_at("4", 72, 740) + text_at("Annual Economic Report 2024", 380, 740)
    c2 += text_at("1 Introduction", 72, 690, size=14) + body
    pdf([(612, 792, c), (612, 792, c2)], os.path.join(HERE, "far_gap.pdf"))


if __name__ == "__main__":
    rotated_pdf()
    far_gap_pdf()
