#!/usr/bin/env python3
"""Fixture for CID fonts read through their Adobe character collection.

cid_collection.pdf: one page, two Type0 fonts shown through Identity-H with
no /ToUnicode and no font program (as producers leave system CJK fonts):
  F1  CIDFontType0, /CIDSystemInfo Adobe-Korea1, shows CIDs 3296 1204 2479
      (Adobe-Korea1: 한 국 어)
  F2  CIDFontType2, /CIDSystemInfo Adobe-GB1, shows CIDs 4559 3795
      (Adobe-GB1: 中 文)
The CIDs are their collections' published values, so the text reads only
through the collection tables. Written by hand: no library emits a font
without a program.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def cid_font(name, subtype, ordering, supplement):
    return (f"<< /Type /Font /Subtype {subtype} /BaseFont /{name} "
            f"/CIDSystemInfo << /Registry (Adobe) /Ordering ({ordering}) /Supplement {supplement} >> "
            f"/FontDescriptor << /Type /FontDescriptor /FontName /{name} /Flags 4 "
            f"/FontBBox [0 -120 1000 880] /ItalicAngle 0 /Ascent 880 /Descent -120 "
            f"/CapHeight 700 /StemV 80 >> /DW 1000 >>")


def main():
    content = (b"BT /F1 24 Tf 72 700 Td <0CE004B409AF> Tj ET\n"
               b"BT /F2 24 Tf 72 650 Td <11CF0ED3> Tj ET\n")
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R /F2 7 0 R >> >> /Contents 4 0 R >>",
        b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream",
        b"<< /Type /Font /Subtype /Type0 /BaseFont /HYGothic-Medium /Encoding /Identity-H "
        b"/DescendantFonts [6 0 R] >>",
        cid_font("HYGothic-Medium", "/CIDFontType0", "Korea1", 2).encode(),
        b"<< /Type /Font /Subtype /Type0 /BaseFont /SimSun /Encoding /Identity-H "
        b"/DescendantFonts [8 0 R] >>",
        cid_font("SimSun", "/CIDFontType2", "GB1", 4).encode(),
    ]
    out = bytearray(b"%PDF-1.7\n")
    offsets = []
    for i, o in enumerate(objs, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for off in offsets:
        out += b"%010d 00000 n \n" % off
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    open(os.path.join(HERE, "cid_collection.pdf"), "wb").write(out)


if __name__ == "__main__":
    main()
