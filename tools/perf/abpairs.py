"""Pair the windows of an in-run A/B (an EXP_ABxxx build that flips a switch at every report).

usage: python tools/perf/abpairs.py <log> <TAG> <text of the "new" state> [min vertices]

<log> is a run_dol.ps1 log (a bare name is looked for in %TEMP%\tkdr-perf), TAG the A/B line the build prints
(ABBONE, ABMAT, ABLIT, ABSND, ABSIG ...: "TAG the window above had|took <state>" closes the window above it)
and the third argument a piece of the state text that identifies the "new" mode ("in the fast", "fast", ...).
Windows of at least <min vertices> (default 12000) are listed; adjacent windows of opposite modes and of
sizes within 10% are paired, and the mean and sd of (new - old) for ge, other, frame and the draw-cache misses
a frame are printed.  A mean within about two standard errors (sd / sqrt(pairs)) is not a result.
"""
import re,os,sys,statistics as st
fn=sys.argv[1]; tag=sys.argv[2]; key=sys.argv[3]
minv=int(sys.argv[4]) if len(sys.argv)>4 else 12000
if not os.path.isabs(fn) and not os.path.exists(fn): fn=os.path.join(os.environ.get('TEMP','.'),'tkdr-perf',fn)
rows=[];cur={}
for l in open(fn,errors='replace'):
    m=re.search(r'SCENE (\d+) verts, (\d+) draws .*hits (\d+) misses (\d+)',l)
    if m: cur.update(verts=int(m.group(1)),hits=int(m.group(3)),miss=int(m.group(4)))
    m=re.search(r'DRAWCATS .*cache-hit ([\d.]+) \((\d+)\) cache-miss ([\d.]+) \((\d+)\)',l)
    if m: cur.update(hit_ms=float(m.group(1)),hit_n=int(m.group(2)),miss_ms=float(m.group(3)),miss_n=int(m.group(4)))
    m=re.search(r'DEMOFPS fe=(\d+) guest=(\d+) .*guest/s=([\d.]+).*ge=([\d.]+)ms.*other=([\d.]+)ms',l)
    if m: cur.update(fe=int(m.group(1)),gps=float(m.group(3)),ge=float(m.group(4)),other=float(m.group(5)))
    m=re.search(tag+r' the window above (?:had|took) (.*)$',l)
    if m:
        cur['mode']= 1 if key in m.group(1) else 0
        rows.append(dict(cur)); cur={}
rows=[r for r in rows if r.get('verts',0)>=minv and 'miss_n' in r]
print(len(rows),'windows of enough vertices')
for r in rows: print('  fe=%5d new=%d verts=%6d ge=%5.2f other=%5.2f frame=%5.2f hits/f=%4d (%.2f ms) miss/f=%4d (%.2f ms) missing=%.1f%%'%(r['fe'],r['mode'],r['verts'],r['ge'],r['other'],r['ge']+r['other'],r['hit_n'],r['hit_ms'],r['miss_n'],r['miss_ms'],100*r['miss']/max(1,r['miss']+r['hits'])))
# pair adjacent windows with opposite modes and similar size
pairs=[]
for a,b in zip(rows,rows[1:]):
    if a['mode']!=b['mode'] and abs(a['verts']-b['verts'])<=0.1*max(a['verts'],b['verts']):
        new,old=(a,b) if a['mode'] else (b,a)
        pairs.append((new['ge']-old['ge'],new['other']-old['other'],(new['ge']+new['other'])-(old['ge']+old['other']),new['miss_n']-old['miss_n']))
print(len(pairs),'pairs (new - old):')
if pairs:
    for i,n in enumerate(('ge','other','frame','misses/frame')):
        v=[p[i] for p in pairs]
        print('  %-13s mean %+.2f  sd %.2f'%(n,st.mean(v),st.pstdev(v)))
