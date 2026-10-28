import re, matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from gt import GT, MEL, SIX
exec(open('score.py').read().split('def score')[0])
fig,axs=plt.subplots(1,2,figsize=(15,7),sharey=True,facecolor='#16181d')
for ax,(name,det,col) in zip(axs,[("Melodyne 5 (FL Keys audio)",MEL,'#ff7a45'),("NovaTone poly, round 2 (same FL Keys audio)",NT,'#36d1b5')]):
    ax.set_facecolor('#1e2128')
    for m,a,b in GT: ax.add_patch(plt.Rectangle((a,m-0.45),b-a,0.9,fill=False,ec='#e8e8e8',lw=1.2))
    for m,a,b in det:
        if b>2 and a<64: ax.add_patch(plt.Rectangle((a,m-0.3),min(b,64)-a,0.6,color=col,alpha=0.85))
    ax.set_xlim(0,64); ax.set_ylim(30,101); ax.set_title(name,color='w',fontsize=13)
    for x in range(0,65,16): ax.axvline(x,color='#555',lw=0.8)
    ax.set_xticks([0,16,32,48,64]); ax.set_xticklabels(['bar 1','bar 2','bar 3','bar 4',''],color='#bbb')
    nm=lambda m:['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'][m%12]+str(m//12-1)
    ax.set_yticks(range(36,101,6)); ax.set_yticklabels([nm(m) for m in range(36,101,6)],color='#bbb')
    for s in ax.spines.values(): s.set_color('#444')
fig.text(0.5,0.02,"White outlines = the real MIDI notes (Cymatics WRLD MIDI 2, bars 1-4).  Filled bars = what each tool detected.",ha='center',color='#ccc',fontsize=11)
plt.tight_layout(rect=(0,0.04,1,1)); plt.savefig('melodyne_vs_novatone.png',dpi=110,facecolor='#16181d')
