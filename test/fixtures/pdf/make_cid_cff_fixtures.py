"""Regenerate the CID-keyed CFF fixtures used by test_pdf ([17] glyph
selection in CID-keyed CFF fonts whose charset is not the identity).

A two-glyph CID-keyed CFF font is built with fontTools: glyph 1 is a bar on
the left half of the em, glyph 2 a bar on the right half. The ToUnicode of
every fixture maps the shown code to "A" (glyph 1), so a correct composite
is dark on the left of the em and light on the right.

- cid_cff_otf_gids.pdf: an OpenType font (FontFile3 /OpenType, with a cmap)
  whose charset swaps the CIDs (glyph 1 = CID 2, glyph 2 = CID 1), shown
  with glyph indices as CIDs (code 1), as PDF libraries that subset by
  glyph index write it. Read by the charset, code 1 is glyph 2.
- cid_cff_otf_cids.pdf: the same font shown by CID as the spec reads it
  (code 2 = glyph 1 through the charset).
- cid_cff_bare_gids.pdf: a bare CFF (FontFile3 /CIDFontType0C, no cmap)
  whose charset keeps sparse CIDs of a larger collection (glyph 1 = CID
  1001, glyph 2 = CID 1002), shown with glyph indices (code 1).
- cid_cff_bare_cids.pdf: the same font shown by CID (code 1001).

The PDF is written by hand so every object is under control. Run from the
repo root (needs fontTools):
    python3 test/fixtures/pdf/make_cid_cff_fixtures.py
"""
import io
import os
import zlib

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.cffLib import FDArrayIndex, FDSelect, FontDict

OUT = os.path.dirname(os.path.abspath(__file__))


def bar(x0, x1):
    pen = T2CharStringPen(1000, None)
    pen.moveTo((x0, 0))
    pen.lineTo((x1, 0))
    pen.lineTo((x1, 700))
    pen.lineTo((x0, 700))
    pen.closePath()
    return pen.getCharString()


def notdef():
    pen = T2CharStringPen(1000, None)
    return pen.getCharString()


def cid_font(cid_left, cid_right):
    """OpenType CFF font, CID-keyed: glyph 1 (left bar, "A") has CID
    cid_left, glyph 2 (right bar, "B") CID cid_right."""
    names = [".notdef", "cid%05d" % cid_left, "cid%05d" % cid_right]
    fb = FontBuilder(1000, isTTF=False)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({ord("A"): names[1], ord("B"): names[2]})
    fb.setupCFF("JDocBars", {"FullName": "JDocBars"},
                {names[0]: notdef(), names[1]: bar(50, 450), names[2]: bar(550, 950)},
                {})
    fb.setupHorizontalMetrics({n: (1000, 0) for n in names})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": "JDocBars", "styleName": "Regular"})
    fb.setupOS2()
    fb.setupPost()
    font = fb.font
    # Turn the name-keyed CFF into a CID-keyed one: ROS, one FDArray entry
    # holding the Private dict, every glyph in FD 0. The "cidNNNNN" glyph
    # names become the charset's CIDs.
    cff = font["CFF "].cff
    top = cff.topDictIndex[0]
    top.ROS = ("Adobe", "Identity", 0)
    top.CIDCount = max(cid_left, cid_right) + 1
    fd = FontDict()
    fd.setCFF2(False)
    fd.Private = top.Private
    fd.FontMatrix = [0.001, 0, 0, 0.001, 0, 0]
    fda = FDArrayIndex()
    fda.append(fd)
    top.FDArray = fda
    top.FDSelect = FDSelect()
    top.FDSelect.format = 3
    top.FDSelect.gidArray = [0] * len(names)
    del top.Private
    if hasattr(top, "Encoding"):
        del top.Encoding
    cs = top.CharStrings
    for name in names:
        cs[name].private = fd.Private
    buf = io.BytesIO()
    font.save(buf)
    otf = buf.getvalue()
    bare = font["CFF "].compile(font)
    return otf, bare


def pdf(name, program, subtype, code):
    """One page: body text in Helvetica, a light grey raster, and one glyph
    of the CID font drawn over it at 100 pt, its em the raster's width."""
    objs = []

    def add(body):
        objs.append(body)
        return len(objs)

    def stream(dict_items, data, compress=True):
        if compress:
            data = zlib.compress(data)
            dict_items += b" /Filter /FlateDecode"
        return b"<< " + dict_items + b" /Length %d >>\nstream\n" % len(data) + data + \
            b"\nendstream"

    catalog = add(None)
    pages = add(None)
    page = add(None)
    helv = add(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    img = add(stream(b"/Type /XObject /Subtype /Image /Width 64 /Height 64 "
                     b"/ColorSpace /DeviceGray /BitsPerComponent 8",
                     bytes([225]) * (64 * 64)))
    prog = add(stream(b"/Subtype /" + subtype, program))
    desc = add(b"<< /Type /FontDescriptor /FontName /JDocBars /Flags 4 "
               b"/FontBBox [0 -200 1000 800] /ItalicAngle 0 /Ascent 800 "
               b"/Descent -200 /CapHeight 700 /StemV 80 /FontFile3 %d 0 R >>" % prog)
    cidfont = add(b"<< /Type /Font /Subtype /CIDFontType0 /BaseFont /JDocBars "
                  b"/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) "
                  b"/Supplement 0 >> /FontDescriptor %d 0 R /DW 1000 >>" % desc)
    tounicode = add(stream(
        b"",
        b"/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        b"/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        b"/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
        b"1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n"
        b"1 beginbfchar\n<%04X> <0041>\nendbfchar\n"
        b"endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n" % code))
    type0 = add(b"<< /Type /Font /Subtype /Type0 /BaseFont /JDocBars "
                b"/Encoding /Identity-H /DescendantFonts [%d 0 R] "
                b"/ToUnicode %d 0 R >>" % (cidfont, tounicode))
    text = (b"BT /F1 11 Tf 72 760 Td (A glyph drawn over a picture, from a "
            b"CID-keyed CFF font.) Tj ET\n"
            b"q 100 0 0 100 250 400 cm /Im1 Do Q\n"
            b"BT /F2 100 Tf 250 430 Td <%04X> Tj ET\n"
            b"BT /F1 11 Tf 72 300 Td (The glyph is a bar on the left half of "
            b"its em.) Tj ET\n" % code)
    content = add(stream(b"", text))
    objs[catalog - 1] = b"<< /Type /Catalog /Pages %d 0 R >>" % pages
    objs[pages - 1] = b"<< /Type /Pages /Kids [%d 0 R] /Count 1 >>" % page
    objs[page - 1] = (b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 600 842] "
                      b"/Resources << /Font << /F1 %d 0 R /F2 %d 0 R >> "
                      b"/XObject << /Im1 %d 0 R >> >> /Contents %d 0 R >>"
                      % (pages, helv, type0, img, content))
    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, body in enumerate(objs, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for off in offsets:
        out += b"%010d 00000 n \n" % off
    out += b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
        len(objs) + 1, catalog, xref)
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(out)


swapped_otf, _ = cid_font(2, 1)
pdf("cid_cff_otf_gids.pdf", swapped_otf, b"OpenType", 1)
pdf("cid_cff_otf_cids.pdf", swapped_otf, b"OpenType", 2)
_, sparse_bare = cid_font(1001, 1002)
pdf("cid_cff_bare_gids.pdf", sparse_bare, b"CIDFontType0C", 1)
pdf("cid_cff_bare_cids.pdf", sparse_bare, b"CIDFontType0C", 1001)
