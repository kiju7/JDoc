#!/usr/bin/env python3
"""Fixture for LZWDecode content streams (lzw_content.pdf).

One page whose content stream is LZW-compressed with EarlyChange 1 (the
default) by the encoder below, written as PDF 32000-1 7.4.4 describes: codes
widen from 9 to 10, 11 and 12 bits one entry early. The text is long enough
to pass every width change (about 2,600 codes); the last line is "END OF LZW
TEXT". MuPDF reads all 220 lines.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def lzw_encode(data, early=1):
    out, acc, nacc = bytearray(), 0, 0
    bits = 9

    def emit(code):
        nonlocal acc, nacc
        acc = (acc << bits) | code
        nacc += bits
        while nacc >= 8:
            nacc -= 8
            out.append((acc >> nacc) & 0xFF)

    table = {bytes([i]): i for i in range(256)}
    nxt = 258
    emit(256)
    w = b""
    for c in data:
        wc = w + bytes([c])
        if wc in table:
            w = wc
            continue
        emit(table[w])
        table[wc] = nxt
        nxt += 1
        # The decoder adds each entry one code later than the encoder.
        if nxt - 1 + early >= (1 << bits) and bits < 12:
            bits += 1
        if nxt - 1 >= 4096 - early:
            emit(256)
            table = {bytes([i]): i for i in range(256)}
            nxt, bits = 258, 9
        w = bytes([c])
    if w:
        emit(table[w])
    emit(257)
    if nacc:
        out.append((acc << (8 - nacc)) & 0xFF)
    return bytes(out)


def main():
    lines = [f"Line {i:03d} of a long LZW compressed page, words {i * 7} and {i * 13}." for i in range(220)]
    lines.append("END OF LZW TEXT")
    ops = ["BT /F1 3 Tf 3.4 TL 40 770 Td"]
    for t in lines:
        ops.append("(%s) Tj T*" % t)
    ops.append("ET")
    content = lzw_encode("\r".join(ops).encode())
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        b"<< /Length %d /Filter /LZWDecode >>\nstream\n" % len(content) + content + b"\nendstream",
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
    open(os.path.join(HERE, "lzw_content.pdf"), "wb").write(out)


if __name__ == "__main__":
    main()
