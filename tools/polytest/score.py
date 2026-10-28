import re
from gt import GT, MEL, SIX
NAMES={'C':0,'C#':1,'D':2,'D#':3,'E':4,'F':5,'F#':6,'G':7,'G#':8,'A':9,'A#':10,'B':11}
NT=[]
for l in open('nt_out.txt'):
    m=re.match(r'\s+([\d.]+)-([\d.]+)\s+([A-G]#?)(-?\d+)',l)
    if m: NT.append((12*(int(m.group(4))+1)+NAMES[m.group(3)], float(m.group(1))/SIX, float(m.group(2))/SIX))
def score(name, det):
    gt=[g for g in GT if g[2]>2.0]          # Melodyne's view started at 16th 1.9: skip notes that ended before
    det=[d for d in det if d[2]>2.0 and d[1]<63]   # notes starting on bar 5's downbeat belong to bar 5
    used=set(); hits=[]; octv=0
    for g in gt:
        best=None
        for i,d in enumerate(det):
            if i in used or d[0]!=g[0]: continue
            if abs(max(d[1],1.9)-max(g[1],1.9))<=1.5 and (best is None or abs(d[1]-g[1])<abs(det[best][1]-g[1])): best=i
        if best is not None: used.add(best); hits.append(g)
    missed=[g for g in gt if g not in hits]
    extra=[d for i,d in enumerate(det) if i not in used]
    for d in extra:
        if any(abs(d[0]-g[0]) in (12,24) and abs(d[1]-g[1])<=1.5 for g in missed): octv+=1
    R=len(hits)/len(gt); P=len(hits)/max(1,len(det)); F=2*P*R/max(1e-9,P+R)
    nm=lambda m:['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'][m%12]+str(m//12-1)
    print(f"{name:10s} found {len(det):2d} notes | correct {len(hits)}/{len(gt)} (recall {R:.0%}) | precision {P:.0%} | F1 {F:.2f} | octave errors {octv}")
    print("   missed:", ", ".join(f"{nm(g[0])}@{g[1]:g}" for g in missed))
    print("   extra :", ", ".join(f"{nm(d[0])}@{d[1]:.1f}" for d in extra))
    return R,P,F
score("Melodyne", MEL)
score("NovaTone", NT)
