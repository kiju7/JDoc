# usage: python3 -I analyze.py <samples_root> <run_dir>   -> <run_dir>/analysis.jsonl
import sys, os, json, re, zipfile, io, collections, unicodedata
root, rd = sys.argv[1], sys.argv[2]
cache = os.path.join(os.path.dirname(rd), 'truthcache'); os.makedirs(cache, exist_ok=True)
import fitz
from PIL import Image
Image.MAX_IMAGE_PIXELS = None
def norm(s):
    s = re.sub(r'!\[[^\]]*\]\([^)]*\)', ' ', s)
    s = re.sub(r'\]\([^)]*\)', ' ', s)
    return ''.join(c for c in unicodedata.normalize('NFC', s) if c.isalnum())
def bigrams(s):
    return collections.Counter(s[i:i+2] for i in range(len(s)-1))
def truth_text(rel):
    parts = rel.split('/')
    if parts[0] not in ('corpus','manualtest') or len(parts) < 3: return None
    stem = os.path.splitext(parts[-1])[0]
    tp = os.path.join(root, parts[0], '_truth', *parts[1:-1], stem + '.pdf')
    if not os.path.exists(tp) or rel.lower().endswith('.pdf') and parts[1]=='pdf' and False: return None
    if not os.path.exists(tp): return None
    cp = os.path.join(cache, re.sub(r'[/ ]', '_', rel) + '.txt')
    if os.path.exists(cp): return open(cp, encoding='utf-8').read()
    try:
        doc = fitz.open(tp); t = '\n'.join(p.get_text() for p in doc)
    except Exception as e: t = ''
    open(cp, 'w', encoding='utf-8').write(t); return t
def rp(out, ref):
    a, b = bigrams(norm(out)), bigrams(norm(ref))
    inter = sum((a & b).values())
    return (inter / max(1, sum(b.values())), inter / max(1, sum(a.values())), sum(b.values()))
IMG_EXT = ('.png','.jpg','.jpeg','.gif','.bmp','.tif','.tiff','.webp')
def container_imgs(src):
    try: z = zipfile.ZipFile(src)
    except Exception: return None
    n = 0
    for i in z.infolist():
        nm = i.filename.lower()
        if not nm.endswith(IMG_EXT): continue
        if 'thumbnail' in nm or 'preview' in nm: continue
        try:
            with z.open(i) as f: im = Image.open(io.BytesIO(f.read())); w, h = im.size
            if w >= 50 and h >= 50: n += 1
        except Exception: n += 1
    return n
def garble(s):
    return dict(fffd=s.count('�'),
                pua=sum(1 for c in s if 0xE000 <= ord(c) <= 0xF8FF),
                ctrl=sum(1 for c in s if ord(c) < 32 and c not in '\n\t\r' ),
                latin=len(re.findall(r'[À-ÿ]{3,}', s)),
                xml=len(re.findall(r'<\?xml|</?[a-z]+:[A-Za-z]+[ >/]', s)))
def table_issues(md):
    bad = 0; tables = 0; rows = []
    def flush():
        nonlocal bad, tables
        if len(rows) >= 2:
            tables += 1
            cnt = [len(re.findall(r'(?<!\\)\|', r)) for r in rows]
            if len(set(cnt)) > 1: bad += 1
        rows.clear()
    for line in md.splitlines():
        if line.startswith('|'): rows.append(line)
        else: flush()
    flush(); return tables, bad
out = open(os.path.join(rd, 'analysis.jsonl'), 'w')
for line in open(os.path.join(rd, 'results.jsonl')):
    r = json.loads(line); d = os.path.join(rd, r['key'])
    md = open(os.path.join(d,'out.md'), encoding='utf-8', errors='replace').read() if r['md_bytes'] >= 0 else ''
    tx = open(os.path.join(d,'out.txt'), encoding='utf-8', errors='replace').read() if r['txt_bytes'] >= 0 else ''
    a = dict(rel=r['rel'], key=r['key'])
    a['g_md'] = garble(md); a['g_txt'] = garble(tx)
    a['tables'], a['bad_tables'] = table_issues(md)
    nm, nt = norm(md), norm(tx); a['nchar_md'], a['nchar_txt'] = len(nm), len(nt)
    if nm or nt:
        rec, pre, _ = rp(tx, md); a['txt_vs_md'] = [round(rec,3), round(pre,3)]
    t = truth_text(r['rel'])
    if t is not None:
        rec, pre, n = rp(md, t); a['truth_md'] = [round(rec,3), round(pre,3), n]
        rec, pre, n = rp(tx, t); a['truth_txt'] = [round(rec,3), round(pre,3), n]
    ext = os.path.splitext(r['rel'])[1].lower()
    if ext in ('.docx','.xlsx','.pptx','.hwpx','.odt','.ods','.odp'):
        a['cont_imgs'] = container_imgs(os.path.join(root, r['rel']))
    bad = []
    im = os.path.join(d,'img')
    if os.path.isdir(im):
        for f in os.listdir(im):
            p = os.path.join(im, f)
            if f.lower().endswith(IMG_EXT):
                try:
                    with Image.open(p) as x: x.verify()
                except Exception as e: bad.append(f)
    a['bad_imgs'] = bad
    out.write(json.dumps(a, ensure_ascii=False) + '\n')
