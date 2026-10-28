import sys
VERBOSE=len(sys.argv)>1
import re
SIX=60/140/4
# Melodyne blobs read from its editor (midi, start in 16ths)
MEL=[(76,27.9),(76,59.9),(71,27.9),(71,59.9)]+[(69,s) for s in (3.8,11.8,19.6,35.8,39.9,43.9,51.8)]\
   +[(66,s) for s in (19.6,27.6,51.8,59.6)]+[(64,s) for s in (3.5,11.5,36.0,43.7)]\
   +[(61,s) for s in (3.5,11.5,19.6,27.7,35.7,43.6,51.5,59.4)]+[(54,s) for s in (0.0,9.6,31.7,41.8)]\
   +[(50,s) for s in (15.5,25.7,47.5,57.5)]+[(49,28.7)]
N={'C':0,'C#':1,'D':2,'D#':3,'E':4,'F':5,'F#':6,'G':7,'G#':8,'A':9,'A#':10,'B':11}
NT=[]
for l in open('nt.txt'):
    m=re.match(r'\s+([\d.]+)-([\d.]+)\s+([A-G]#?)(-?\d+).*level ([\d.]+)',l)
    if m: NT.append((12*(int(m.group(4))+1)+N[m.group(3)],float(m.group(1))/SIX,float(m.group(2))/SIX,float(m.group(5))))
nm=lambda m:['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'][m%12]+str(m//12-1)
used=set(); both=[]
for p,s in MEL:
    c=[i for i,d in enumerate(NT) if i not in used and d[0]==p and abs(d[1]-s)<=1.5]
    if c: used.add(c[0]); both.append((p,s))
    else:
        # inside a longer NovaTone note of the same pitch = re-strike merged away
        cov=[d for d in NT if d[0]==p and d[1]<s<d[2]]
        if VERBOSE: print(f"  Melodyne only: {nm(p)} @{s:4.1f}" + ("   (NovaTone holds it as one longer note: re-strike merged)" if cov else ("   (NovaTone has it an octave off)" if any(abs(d[0]-p)==12 and abs(d[1]-s)<=1.5 for d in NT) else "")))
extra=[d for i,d in enumerate(NT) if i not in used]
weak=[d for d in extra if d[3]<0.01]
print(f"Melodyne {len(MEL)} notes, NovaTone {len(NT)} notes, agreed {len(both)}  ({len(both)/len(MEL):.0%} of Melodyne's)")
print(f"NovaTone-only {len(extra)}: weak (<0.01) {len(weak)}, strong {len(extra)-len(weak)}")
for d in extra:
    if VERBOSE and d[3]>=0.01: print(f"  NovaTone only (strong): {nm(d[0])} @{d[1]:4.1f} len {d[2]-d[1]:4.1f} lvl {d[3]:.3f}")
