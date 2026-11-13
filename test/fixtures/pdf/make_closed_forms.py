#!/usr/bin/env python3
"""Handwritten PDF regressions; no benchmark data or external dependencies."""
from pathlib import Path

def pdf(name, commands):
    stream = '\n'.join(commands).encode('ascii')
    objects = [b'<< /Type /Catalog /Pages 2 0 R >>',
               b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
               b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
               b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
               b'<< /Length '+str(len(stream)).encode()+b' >>\nstream\n'+stream+b'\nendstream']
    data = bytearray(b'%PDF-1.4\n'); offsets = [0]
    for i, obj in enumerate(objects, 1):
        offsets.append(len(data)); data.extend(f'{i} 0 obj\n'.encode()+obj+b'\nendobj\n')
    start = len(data); data.extend(b'xref\n0 6\n0000000000 65535 f \n')
    for offset in offsets[1:]: data.extend(f'{offset:010d} 00000 n \n'.encode())
    data.extend(f'trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n{start}\n%%EOF\n'.encode())
    Path(__file__).with_name(name+'.pdf').write_bytes(data)

def text(x,y,s): return f'BT /F1 10 Tf {x} {y} Td ({s}) Tj ET'
def line(x0,y0,x1,y1): return f'{x0} {y0} m {x1} {y1} l S'

for name, cols, rows in [('closed_blank_form',3,6),('closed_single_column',1,8)]:
    commands=['0 G 0.5 w']
    for r in range(rows+1): commands.append(line(50,700-r*30,500,700-r*30))
    for c in range(cols+1): commands.append(line(50+c*450/cols,700-rows*30,50+c*450/cols,700))
    for r in range(rows): commands.append(text(60,680-r*30,'Heading' if r==0 else f'Item {r}'))
    if cols>1:
        for c in range(1,cols): commands.append(text(60+c*450/cols,680,'Record'))
    pdf(name,commands)
    if cols == 1:
        fragmented = list(commands)
        for r in range(rows):
            for x in [50,500]: fragmented.append(line(x,700-(r+1)*30+6,x,700-r*30-6))
        pdf('closed_single_fragments',fragmented)

# An outer frame with row rules still contains several text-aligned columns.
commands=['0 G 0.5 w']
for r in range(6): commands.append(line(50,700-r*30,500,700-r*30))
for x in [50,500]: commands.append(line(x,550,x,700))
for r in range(5):
    for c,s in enumerate(['Country','Domestic','Imported','Unknown','Total'] if r==0 else ['Region','12','34','54','100']):
        commands.append(text(60+c*90,680-r*30,s))
pdf('closed_text_columns',commands)

# Two fully boxed rows: several list lines belong to each data cell.
commands=['0 G 0.5 w']
for y in [450,650,680]: commands.append(line(50,y,500,y))
for x in [50,275,500]: commands.append(line(x,450,x,680))
commands += [text(60,660,'Materials'),text(285,660,'Equipment')]
for r in range(8):
    commands += [text(60,630-r*20,f'Material item {r}'),text(285,630-r*20,f'Equipment item {r}')]
pdf('closed_two_row_lists',commands)

# Negative controls: a single callout cell and a framed paragraph.
for name,content in [('closed_callout',['Notice']),('closed_paragraph',['First prose line','Second prose line','Third prose line'])]:
    commands=['0 G 0.5 w',line(50,600,500,600),line(50,700,500,700),line(50,600,50,700),line(500,600,500,700)]
    commands += [text(60,680-r*15,s) for r,s in enumerate(content)]
    pdf(name,commands)
