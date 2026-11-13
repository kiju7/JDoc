# Whole-corpus regression sweep over the shared samples (TEST_SAMPLE/docu-parser:
# corpus/, manualtest/, autotest/documents/). Each document is converted twice,
# markdown with --images and --format text, 120 s limit, into <out_dir>/<key>/.
#
#   python3 -I run.py     <jdoc> "$DOCU_SAMPLES" <out_dir> [jobs]   -> results.jsonl
#   python3 -I analyze.py "$DOCU_SAMPLES" <out_dir>                 -> analysis.jsonl
#   python3 -I compare.py <base_out_dir> <new_out_dir>              (changed outputs)
#
# analyze.py scores each output against the _truth/ PDFs (character bigram
# recall/precision via PyMuPDF), counts U+FFFD/PUA/control characters and
# broken markdown tables, and checks every extracted image opens (Pillow).
#
# usage: python3 -I run.py <jdoc> <samples_root> <out_dir> [jobs]
import sys, os, subprocess, json, time, hashlib, re
from concurrent.futures import ThreadPoolExecutor
jdoc, root, out = sys.argv[1], sys.argv[2], sys.argv[3]
jobs = int(sys.argv[4]) if len(sys.argv) > 4 else 8
SKIP_EXT = {'.json','.yml','.yaml','.ds_store','.gitkeep','.log','.md'}
SKIP_DIR = {'_truth','expected','_재료','.textcache'}
files = []
for sub in ('corpus','manualtest','autotest'):
    for dp, dns, fns in os.walk(os.path.join(root, sub)):
        dns[:] = [d for d in dns if d not in SKIP_DIR and not d.startswith('_truth')]
        for f in fns:
            ext = os.path.splitext(f)[1].lower()
            if ext in SKIP_EXT or f.startswith('.') or f == 'README.md': continue
            files.append(os.path.relpath(os.path.join(dp, f), root))
files.sort()
os.makedirs(out, exist_ok=True)
def key(rel): return hashlib.md5(rel.encode()).hexdigest()[:12]
def run1(args, timeout=120):
    t = time.time()
    try:
        p = subprocess.run(['/usr/bin/time','-l'] + args, capture_output=True, timeout=timeout)
        rc = p.returncode; err = p.stderr.decode('utf-8','replace')
    except subprocess.TimeoutExpired:
        return dict(rc='timeout', sec=timeout, rss=0, err='')
    m = re.search(r'(\d+)\s+maximum resident set size', err)
    rss = int(m.group(1)) if m else 0
    err = '\n'.join(l for l in err.splitlines() if not re.match(r'^\s*\d+\s+[a-z]', l) and 'real' not in l)[-2000:]
    return dict(rc=rc, sec=round(time.time()-t,3), rss=rss, err=err)
def work(rel):
    k = key(rel); d = os.path.join(out, k); os.makedirs(d, exist_ok=True)
    src = os.path.join(root, rel)
    r = dict(rel=rel, key=k, size=os.path.getsize(src))
    md = os.path.join(d, 'out.md'); tx = os.path.join(d, 'out.txt'); im = os.path.join(d, 'img')
    r['md'] = run1([jdoc, src, md, '--images', im])
    r['txt'] = run1([jdoc, src, tx, '--format', 'text'])
    r['md_bytes'] = os.path.getsize(md) if os.path.exists(md) else -1
    r['txt_bytes'] = os.path.getsize(tx) if os.path.exists(tx) else -1
    imgs = [os.path.relpath(os.path.join(dp, f), im) for dp, _, fs in os.walk(im) for f in fs] if os.path.isdir(im) else []
    r['imgs'] = len(imgs); r['img_zero'] = sum(1 for f in imgs if os.path.getsize(os.path.join(im,f)) == 0)
    return r
done = 0
with open(os.path.join(out, 'results.jsonl'), 'w') as fo, ThreadPoolExecutor(jobs) as ex:
    for r in ex.map(work, files):
        fo.write(json.dumps(r, ensure_ascii=False) + '\n'); fo.flush()
        done += 1
        if done % 200 == 0: print(done, '/', len(files), flush=True)
print('total', len(files))
