#!/usr/bin/env python3
"""Write test/fixtures/pdf/base14_nowidths.pdf.

Every font on the page is a standard 14 face (or a common alias of one) with
no /Widths and no font program, as PDF 32000-1 9.6.2.2 allows. Each word is
placed on its own at the position its real (AFM) advance widths give, with
no space glyphs in between -- the way many producers lay out justified or
tabular text. A reader without the built-in metrics sizes every glyph with a
fallback width, its word boxes overrun the next word and the gaps between
words disappear.

Usage: gen_base14_fixture.py OUT.pdf  (stdlib only, no dependencies)
"""
import sys

# Advance widths (1/1000 em) of the glyphs used below, from Adobe's Core14
# AFM files (Helvetica, Helvetica-Bold, Times-BoldItalic, Times-Roman).
HELV = {' ': 278, 'i': 222, 'l': 222, 'c': 500, 't': 278, 'e': 556, 's': 500,
        'f': 278, 'w': 722, 'B': 667, ',': 278, '?': 556, 'I': 278,
        'T': 611, 'o': 556, 'a': 556, '1': 556, '2': 556, '3': 556, '4': 556,
        '.': 278}
HELV_BOLD = {'M': 833, 'i': 278, 'n': 611, 'm': 889, 'a': 556, 'l': 278,
             'w': 778, 'g': 611, 'y': 556, 'f': 333, 's': 556}
TIMES_BI = {'f': 333, 'i': 278, 'l': 278, 't': 278, 'I': 389}
TIMES = {'C': 667, 'a': 444, 'f': 333, '\x8e': 444, 'r': 333, 'u': 500,
         'm': 778, 'n': 500, 'e': 444, 'i': 278, 'l': 278, 'o': 500, 's': 389,
         'd': 500, 'c': 444}
SYMBOL = {'a': 631, 'b': 549, 'g': 411}
ZAPF = {'4': 846}

FONTS = [
    # resource, font dictionary body
    ("F1", "/Type /Font /Subtype /Type1 /BaseFont /Helvetica"),
    ("F2", "/Type /Font /Subtype /TrueType /BaseFont /Arial,Bold /Encoding /WinAnsiEncoding"),
    ("F3", "/Type /Font /Subtype /Type1 /BaseFont /TimesNewRomanPS-BoldItalicMT "
           "/Encoding /WinAnsiEncoding"),
    ("F4", "/Type /Font /Subtype /Type1 /BaseFont /ABCDEF+Times-Roman "
           "/Encoding << /BaseEncoding /MacRomanEncoding >>"),
    ("F5", "/Type /Font /Subtype /Type1 /BaseFont /Symbol"),
    ("F6", "/Type /Font /Subtype /Type1 /BaseFont /ZapfDingbats"),
]


def words_line(font, size, widths, x, y, words, space):
    """Place each word at its true position, no space glyphs between."""
    ops = []
    for w in words:
        esc = w.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
        esc = "".join(c if 32 <= ord(c) < 127 else "\\%03o" % ord(c) for c in esc)
        ops.append(f"BT /{font} {size} Tf {x:.3f} {y} Td ({esc}) Tj ET")
        x += sum(widths[c] for c in w) * size / 1000.0 + space * size / 1000.0
    return ops


def main(out):
    ops = []
    ops += words_line("F1", 12, HELV, 72, 720,
                      ["illicit", "little", "lilies", "fill", "it,", "will", "Bill?"], HELV[' '])
    ops += words_line("F2", 12, HELV_BOLD, 72, 690,
                      ["Minimal", "wiggly", "lily", "fills"], 278)
    ops += words_line("F3", 12, TIMES_BI, 72, 660, ["fill", "it", "till", "I", "lift"], 250)
    ops += words_line("F4", 12, TIMES, 72, 630, ["Caf\x8e", "menu", "is", "closed"], 250)
    ops += words_line("F5", 12, SYMBOL, 72, 600, ["a", "b", "g"], 250)
    ops += words_line("F6", 12, ZAPF, 72, 570, ["4"], 278)
    ops += words_line("F1", 12, HELV, 72, 540, ["Total", "1234.12"], HELV[' '])
    content = "\n".join(ops).encode("latin-1")

    objs = []
    objs.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    objs.append(b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    font_refs = " ".join(f"/{name} {5 + i} 0 R" for i, (name, _) in enumerate(FONTS))
    objs.append(("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 f"/Resources << /Font << {font_refs} >> >> /Contents 4 0 R >>").encode())
    objs.append(b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream")
    for _, body in FONTS:
        objs.append(f"<< {body} >>".encode())

    pdf = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, body in enumerate(objs):
        offsets.append(len(pdf))
        pdf += b"%d 0 obj\n" % (i + 1) + body + b"\nendobj\n"
    xref = len(pdf)
    pdf += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for off in offsets:
        pdf += b"%010d 00000 n \n" % off
    pdf += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    with open(out, "wb") as f:
        f.write(pdf)


if __name__ == "__main__":
    main(sys.argv[1])
