"""Regenerate the chart-tick, dot-leader and fragmented-figure fixtures used by
test_pdf ([20] and [21]).

PyMuPDF with embedded (subset) Arial and AppleGothic. Run from the repo root:
    python3 test/fixtures/pdf/make_chart_toc_fixtures.py
"""
import os
import fitz

OUT = os.path.dirname(os.path.abspath(__file__))
LATIN = "/System/Library/Fonts/Supplemental/Arial.ttf"
HANGUL = "/System/Library/Fonts/Supplemental/AppleGothic.ttf"
FL = fitz.Font(fontfile=LATIN)
FH = fitz.Font(fontfile=HANGUL)
W, H = 595, 842
PROSE = ("Body text of the page runs here in ordinary sentences so that the "
         "chart stands between paragraphs and not alone on the page. ") * 2


class Page:
    def __init__(self, hangul=False):
        self.doc = fitz.open()
        self.p = self.doc.new_page(width=W, height=H)
        self.p.insert_font(fontname="FL", fontfile=LATIN)
        if hangul:
            self.p.insert_font(fontname="FH", fontfile=HANGUL)

    def text(self, x, y, t, fs=9, font="FL"):
        self.p.insert_text((x, y), t, fontsize=fs, fontname=font)

    def rtext(self, xr, y, t, fs=9, font="FL"):
        f = FH if font == "FH" else FL
        self.text(xr - f.text_length(t, fontsize=fs), y, t, fs, font)

    def para(self, x0, y0, x1, y1, t=PROSE, fs=9, font="FL"):
        self.p.insert_textbox(fitz.Rect(x0, y0, x1, y1), t, fontsize=fs, fontname=font)


def save(doc, name):
    path = f"{OUT}/{name}.pdf"
    doc.subset_fonts()
    doc.save(path, garbage=4, deflate=True)
    doc.close()


def axis_chart(pg, x0, x1, top, left, right, title):
    """A bar chart with tick labels on both value axes (left ticks right-
    aligned at x0, right ticks left-aligned at x1), one row every 24pt."""
    pg.text(x0 - 20, top - 14, title, 10)
    ys = [top + 24 * i for i in range(len(left))]
    for y, l, r in zip(ys, left, right):
        pg.rtext(x0, y + 3, l)
        pg.text(x1, y + 3, r)
        pg.p.draw_line((x0 + 4, y), (x1 - 4, y), color=(0.8, 0.8, 0.8), width=0.4)
    pg.p.draw_line((x0 + 4, ys[0]), (x0 + 4, ys[-1]), width=0.6)
    pg.p.draw_line((x1 - 4, ys[0]), (x1 - 4, ys[-1]), width=0.6)
    for i, hgt in enumerate((0.4, 0.7, 0.55, 0.9, 0.3)):
        bx = x0 + 20 + i * (x1 - x0 - 40) / 5
        pg.p.draw_rect(fitz.Rect(bx, ys[-1] - hgt * (ys[-1] - ys[0]), bx + 14, ys[-1]),
                       color=None, fill=(0.2, 0.4, 0.7))


def chart_ticks():
    """Two charts whose tick labels stand on both value axes: mirrored
    (30 | 30) and dual scales (20 | 2,500). Neither is a table. Below them a
    statement with dot leaders (a table, its group header kept on its own
    row) and a year-stub table (three columns)."""
    pg = Page()
    pg.para(60, 50, 540, 90)
    axis_chart(pg, 90, 300, 130, ["30", "20", "10", "0"], ["30", "20", "10", "0"],
               "A. Mirrored axes")
    axis_chart(pg, 90, 300, 260, ["20", "15", "10", "5"],
               ["2,500", "2,000", "1,500", "1,000"], "B. Dual axes")
    pg.para(60, 360, 540, 400)
    rows = [("Revenues", "1,250.0", "1,100.5"), ("Cost of sales", "(640.2)", "(590.1)"),
            ("Gross profit", "609.8", "510.4"), ("Operating expenses:", "", ""),
            ("Research and development", "120.0", "99.0"), ("Marketing", "80.5", "70.1"),
            ("Operating income", "409.3", "341.3")]
    y = 430
    for label, a, b in rows:
        pg.text(60, y, label)
        if a:
            end = 60 + FL.text_length(label, fontsize=9) + 3
            n = int((380 - end) / FL.text_length(".", fontsize=9))
            pg.text(end, y, "." * n)
            pg.rtext(430, y, a)
            pg.rtext(500, y, b)
        y += 14
    y += 30
    for yr, a, b in (("Year", "Berkshire", "S&P 500"), ("1965", "49.5", "10.0"),
                     ("1966", "(3.4)", "(11.7)"), ("1967", "13.3", "30.9"),
                     ("1968", "77.8", "11.0"), ("1969", "19.4", "(8.4)"),
                     ("1970", "(4.6)", "3.9")):
        pg.text(60, y, yr)
        pg.rtext(330, y, a)
        pg.rtext(450, y, b)
        y += 13
    save(pg.doc, "chart_ticks")


def leader_toc():
    """A Hangul contents list: titles whose word gaps line up across rows,
    joined to their page numbers by ellipsis leaders, some entries indented;
    and a Latin one with period leaders. Each entry is one row of two cells
    (title | page)."""
    pg = Page(hangul=True)
    pg.text(250, 60, "목 차", 14, "FH")
    entries = [(0, "Ⅳ. 실험 및 성능평가", "28"), (12, "4.4.1 입력 조건 변수의 기여도 분석", "50"),
               (12, "4.4.2 모델 구조의 기여도 분석", "55"), (12, "4.4.3 손실 함수의 기여도 분석", "59"),
               (12, "4.4.4 학습 조건 변수의 기여도 분석", "61"), (0, "Ⅴ. 결 론", "63"),
               (0, "참 고 문 헌", "65")]
    y = 100
    for ind, title, page in entries:
        x = 70 + ind
        pg.text(x, y, title, 10, "FH")
        end = x + FH.text_length(title, fontsize=10) + 4
        n = int((440 - end) / FH.text_length("…", fontsize=10))
        pg.text(end, y, "…" * n, 10, "FH")
        pg.rtext(460, y, page, 10, "FH")
        y += 18
    y += 40
    for title, page in (("Introduction", "1"), ("The year under review", "4"),
                        ("Pressure points and risks ahead", "9"), ("Policy challenges", "13"),
                        ("Conclusion", "21")):
        pg.text(70, y, title, 10)
        end = 70 + FL.text_length(title, fontsize=10) + 3
        n = int((440 - end) / FL.text_length(".", fontsize=10))
        pg.text(end, y, "." * n, 10)
        pg.rtext(460, y, page, 10)
        y += 16
    save(pg.doc, "leader_toc")


if __name__ == "__main__":
    chart_ticks()
    leader_toc()
