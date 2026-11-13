#!/usr/bin/env python3
"""Fixture for glyphs drawn again over themselves (overprint.pdf).

Line 1 fakes bold by showing "Bold Title" twice, the second time 0.3pt to
the right. Line 2 is an outlined headline: "Shadow" shown eight times, each
copy 0.15pt right and down of the last. Line 3 is plain text with doubled
letters, "Hallucination Association III 77", which must keep every letter.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ops = ["BT /F1 14 Tf 72 700 Td (Bold Title) Tj ET",
           "BT /F1 14 Tf 72.3 700 Td (Bold Title) Tj ET"]
    for k in range(8):
        ops.append("BT /F1 20 Tf %.2f %.2f Td (Shadow) Tj ET" % (72 + 0.15 * k, 650 - 0.15 * k))
    ops.append("BT /F1 10 Tf 72 600 Td (Hallucination Association III 77) Tj ET")
    content = "\n".join(ops).encode()
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for i, o in enumerate(objs, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for off in offsets:
        out += b"%010d 00000 n \n" % off
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    open(os.path.join(HERE, "overprint.pdf"), "wb").write(out)


if __name__ == "__main__":
    main()
