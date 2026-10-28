# For every MIDI note: did the tool find that pitch class near its start, and in which octave?
import re, sys
sys.path.insert(0,'.')
from gt import GT, MEL, SIX
N={'C':0,'C#':1,'D':2,'D#':3,'E':4,'F':5,'F#':6,'G':7,'G#':8,'A':9,'A#':10,'B':11}
NT=[]
for l in open('nt_out.txt'):
    m=re.match(r'\s+([\d.]+)-([\d.]+)\s+([A-G]#?)(-?\d+)',l)
    if m: NT.append((12*(int(m.group(4))+1)+N[m.group(3)],float(m.group(1))/SIX,float(m.group(2))/SIX))
gt=[g for g in GT if g[2]>2.0]
for name,det in (("Melodyne",MEL),("NovaTone",NT)):
    det=[d for d in det if d[2]>2.0 and d[1]<63]
    right=wrong=none=0
    for p,a,b in gt:
        c=[d for d in det if d[0]%12==p%12 and abs(max(d[1],1.9)-max(a,1.9))<=1.5]
        if not c: none+=1
        elif any(d[0]==p for d in c): right+=1
        else: wrong+=1
    found=right+wrong
    print(f"{name:9s} pitch class found {found}/{len(gt)} | right octave {right}/{found} ({right/max(1,found):.0%}) | wrong octave {wrong}")
