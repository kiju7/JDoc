"""Regenerate the table-layout fixtures used by test_pdf ([13] table layouts).

PyMuPDF with an embedded (subset) Arial; 're h' is rewritten to 're' the
way Word/Hangul producers write rectangles. Run from the repo root:
    python3 test/fixtures/pdf/make_table_fixtures.py
"""
import os
import fitz

OUT = os.path.dirname(os.path.abspath(__file__))
FONT = "/System/Library/Fonts/Supplemental/Arial.ttf"
FNT = fitz.Font(fontfile=FONT)
W, H = 595, 842
PROSE = ("Body text of the page runs here in ordinary sentences so that the "
         "table stands between paragraphs and not alone on the page.")


class Page:
    def __init__(self):
        self.doc = fitz.open()
        self.p = self.doc.new_page(width=W, height=H)
        self.p.insert_font(fontname="F0", fontfile=FONT)

    def text(self, x, y, t, fs=9):
        self.p.insert_text((x, y), t, fontsize=fs, fontname="F0")

    def para(self, x0, y0, x1, y1, t=PROSE, fs=9):
        self.p.insert_textbox(fitz.Rect(x0, y0, x1, y1), t, fontsize=fs, fontname="F0")

    def h(self, x0, x1, y, w=0.6):
        self.p.draw_line((x0, y), (x1, y), width=w)

    def v(self, x, y0, y1, w=0.6):
        self.p.draw_line((x, y0), (x, y1), width=w)

    def save(self, name):
        path = f"{OUT}/{name}.pdf"
        self.doc.subset_fonts()
        self.doc.save(path, garbage=4, deflate=True)
        self.doc.close()
        doc = fitz.open(path)
        for page in doc:
            for xref in page.get_contents():
                doc.update_stream(xref, doc.xref_stream(xref).replace(b" re\nh\n", b" re\n"))
        doc.save(path, incremental=True, encryption=0)
        doc.close()


def table(pg, x0, top, widths, rows, style="grid", rh=16, fs=9, align="left",
          spans=()):
    xs = [x0]
    for w in widths:
        xs.append(xs[-1] + w)
    ys = [top + rh * i for i in range(len(rows) + 1)]
    if style in ("grid", "hrules_v1"):
        for y in ys:
            pg.h(xs[0], xs[-1], y)
    if style == "grid":
        for i, x in enumerate(xs):
            for r in range(len(rows)):
                if r in spans and 0 < i < len(xs) - 1:
                    continue
                pg.v(x, ys[r], ys[r + 1])
    elif style == "hrules_v1":
        for r in range(len(rows)):
            if r not in spans:
                pg.v(xs[1], ys[r], ys[r + 1])
    elif style == "booktabs":
        pg.h(xs[0], xs[-1], ys[0], 0.9)
        pg.h(xs[0], xs[-1], ys[1], 0.5)
        pg.h(xs[0], xs[-1], ys[-1], 0.9)
    for r, row in enumerate(rows):
        base = ys[r] + rh - (rh - fs) / 2 - 2
        for c, val in enumerate(row):
            if not val:
                continue
            if r in spans:
                pg.text(xs[0] + 4, base, val, fs)
                break
            w = FNT.text_length(val, fontsize=fs)
            if align == "right" and c > 0:
                x = xs[c + 1] - 4 - w
            else:
                x = xs[c] + 4
            pg.text(x, base, val, fs)
    return ys[-1]


def side_by_side():
    # Label column ruled off, full-width section rows, no outer v-rules; the
    # table in the other page column has rules at nearly the same heights.
    pg = Page()
    pg.para(50, 60, 290, 140); pg.para(305, 60, 545, 140)
    rows = [["Section one header"], ["Tokens", "'alpha', 'beta', 'gamma'"],
            ["Labels", "('O', 'B', 'O')"], ["Section two header"],
            ["Tokens", "['a', 'b', 'c']"], ["Labels", "('O', 'O', 'B')"]]
    table(pg, 55, 180, [55, 180], rows, "hrules_v1", spans=(0, 3))
    right = [["Set", "Train", "Test"], ["Human", "4,000", "500"],
             ["GPT", "3,000", "500"], ["Mixed", "300", "500"]]
    table(pg, 310, 180 + 16 * 3 - 16 * 4 + 1.2, [80, 60, 60], right, "grid")
    pg.para(50, 300, 290, 420); pg.para(305, 300, 545, 420)
    pg.save("tables_side_by_side")


def stacked_beside_frame():
    # Three grids stacked in the left column, a caption above each; a framed
    # figure in the right column spans all three.
    pg = Page()
    y = 80
    for t, letters in enumerate(("ABCD", "EFGH", "JKLM")):
        pg.text(55, y + 9, f"Table {t + 4}. Mapping pairs in type {3 * (t + 1)}", 8)
        y += 14
        rows = [["sample", "result", "sample", "result"]] + [list(letters)] * 3
        y = table(pg, 55, y, [55] * 4, rows, "grid", rh=14) + 14
    pg.p.draw_rect(fitz.Rect(310, 100, 540, 330), width=0.6)
    pg.text(310, 345, "Fig. 8. Results of the survey.", 8)
    pg.para(50, y + 20, 290, y + 120)
    pg.save("tables_stacked_frame")


def small_boxes():
    # Two closed two-row grids 34pt apart (a key/value box and a result
    # box), then a box with an empty cell in each row (no table claim).
    pg = Page()
    pg.para(50, 60, 545, 120)
    table(pg, 60, 170, [120, 140], [["Platform", "Linux"], ["Framework", "PyTorch 2.1"]], rh=18)
    table(pg, 60, 240, [120, 70, 70], [["Model", "Acc", "F1"], ["Ours", "91.2", "88.7"]], rh=18)
    table(pg, 60, 310, [120, 140], [["Note", ""], ["", "see text"]], rh=18)
    pg.para(50, 400, 545, 480)
    pg.save("tables_small_boxes")


def wide_label_column():
    # A grid whose label column is wider than the value columns, under a
    # running-head rule 30pt above it.
    pg = Page()
    pg.h(50, 545, 50, 0.4)
    pg.text(50, 45, "Journal of Synthetic Tables", 8)
    rows = [["Item", "2017", "2018", "2019", "2020"], ["Alpha", "1.5", "2.5", "3.5", "4.5"],
            ["Beta", "5.5", "6.5", "7.5", "8.5"], ["Gamma", "9.5", "10.5", "11.5", "12.5"]]
    table(pg, 50, 80, [100, 65, 65, 65, 65], rows, "grid")
    pg.para(50, 170, 545, 260)
    pg.save("tables_wide_label")


def caption_between():
    # Two borderless tables, a table caption between them.
    pg = Page()
    pg.para(50, 60, 545, 120)
    a = [["Item", "2017", "2018", "2019"], ["Alpha", "26.8", "89.3", "94.8"],
         ["Beta", "14.5", "21.4", "30.5"], ["Gamma", "58.3", "33.2", "26.8"]]
    b = table(pg, 60, 150, [90, 70, 70, 70], a, "none")
    pg.text(60, b + 16, "Table 2. A second table placed right below the first", 9)
    c = [["Code", "Min", "Max", "Avg"], ["A1", "3", "9", "6"], ["B2", "1", "4", "2"],
         ["C3", "7", "8", "7"]]
    b = table(pg, 60, b + 24, [90, 70, 70, 70], c, "none")
    pg.para(50, b + 24, 545, b + 100)
    pg.save("tables_caption_between")


def sparse_columns():
    # Booktabs table, values right-aligned, many empty cells: the last
    # column holds values in four of ten rows, an inner column in three.
    pg = Page()
    pg.para(50, 60, 545, 120)
    cols = ["Region", "2019", "2020", "2021", "2022", "2023", "2024"]
    data = [
        ["Seoul", "", "75.7", "", "44.8", "78.2", ""],
        ["Busan", "", "43.1", "0.7", "71.6", "", "89.3"],
        ["Incheon", "", "", "93.0", "21.8", "3.4", ""],
        ["Daegu", "49.3", "", "", "", "29.0", ""],
        ["Daejeon", "55.3", "18.8", "85.2", "", "71.6", "92.7"],
        ["Gwangju", "82.3", "", "87.4", "50.3", "3.9", ""],
        ["Ulsan", "41.3", "", "69.7", "37.4", "50.6", "51.8"],
        ["Sejong", "48.7", "", "", "97.3", "39.3", ""],
        ["Suwon", "97.2", "53.7", "23.4", "94.3", "45.7", ""],
        ["Jeonju", "94.8", "", "81.3", "73.4", "51.6", "42.5"],
    ]
    b = table(pg, 72, 150, [78] + [60] * 6, [cols] + data, "booktabs", rh=18, align="right")
    pg.para(50, b + 20, 545, b + 90)
    pg.save("tables_sparse_columns")


if __name__ == "__main__":
    side_by_side(); stacked_beside_frame(); small_boxes()
    wide_label_column(); caption_between(); sparse_columns()
