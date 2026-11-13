# usage: python3 -I compare.py <base_dir> <new_dir>
import sys,json,os,filecmp
b,n=sys.argv[1],sys.argv[2]
B={json.loads(l)['rel']:json.loads(l) for l in open(b+'/results.jsonl')}
N={json.loads(l)['rel']:json.loads(l) for l in open(n+'/results.jsonl')}
ch=[]
for rel,r in B.items():
    q=N.get(rel)
    if not q: ch.append(('MISSING',rel)); continue
    diffs=[]
    for f in ('out.md','out.txt'):
        pb,pn=os.path.join(b,r['key'],f),os.path.join(n,q['key'],f)
        if os.path.exists(pb)!=os.path.exists(pn) or (os.path.exists(pb) and not filecmp.cmp(pb,pn,shallow=False)): diffs.append(f)
    if r['imgs']!=q['imgs']: diffs.append(f"imgs {r['imgs']}->{q['imgs']}")
    if r['md']['rc']!=q['md']['rc']: diffs.append(f"rc {r['md']['rc']}->{q['md']['rc']}")
    if diffs: ch.append((rel, r['md_bytes'], q['md_bytes'], diffs))
for c in ch: print(*c)
print('changed', len(ch), 'of', len(B))
