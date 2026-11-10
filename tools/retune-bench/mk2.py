# Legato sung phrase with scoops, slides between notes, vibrato and a breath. Writes in.f32, in_midi.npy, notes.txt
import numpy as np, scipy.signal as sg
sr=44100; rng=np.random.default_rng(11)
phr=[[(57.15,.5),(59.2,.45),(60.85,.6),(59.1,.4),(57.2,.7)],[(62.2,.5),(60.9,.6)]]
segs=[];mids=[];notes=[];pos=0
def lp(x,fc): return sg.sosfiltfilt(sg.butter(2,fc,fs=sr,output='sos'),x)
for pi,ph in enumerate(phr):
    n=int(sum(d for _,d in ph)*sr); t=np.arange(n)/sr
    m=np.zeros(n); b=0; bounds=[]
    for mi,d in ph:
        e=b+int(d*sr); m[b:e]=mi; bounds.append((b,e,mi)); b=e
    m=lp(m,9.0)                                   # ~70 ms slides between notes
    m-= (1.2 if pi==0 else 1.6)*np.exp(-t/0.045)  # scoop into the phrase
    d=np.cumsum(rng.normal(0,1,n)); d=lp(d,3); m+=d/np.abs(d).max()*0.10
    j=lp(rng.normal(0,1,n),25); m+=j/np.abs(j).max()*0.03
    b0,e0,_=bounds[2 if pi==0 else 1]; tv=np.arange(e0-b0)/sr
    m[b0:e0]+=0.3*np.clip((tv-0.2)/0.2,0,1)*np.sin(2*np.pi*5.5*tv)
    f0=440*2**((m-69)/12); phs=np.cumsum(2*np.pi*f0/sr); y=np.zeros(n)
    for k in range(1,45): y+=(k*f0<9000)*np.sin(k*phs)/k**1.2
    y*=np.minimum(1,np.minimum(t/0.03,(t[-1]-t)/0.05)); out=np.zeros(n)
    for fc,bw in ((700,300),(1150,350),(2500,500)):
        bb,aa=sg.iirpeak(fc,fc/bw,sr); out+=sg.lfilter(bb,aa,y)
    segs.append(out/np.abs(out).max()*0.5); mids.append(m)
    for (b,e,mi) in bounds:
        core=m[b+int(.08*sr):e-int(.06*sr)]
        notes.append((pos+b,pos+e,float(np.median(core))))
    pos+=n
    if pi==0:
        nb=int(0.3*sr); x=sg.sosfilt(sg.butter(2,[300,3000],'bp',fs=sr,output='sos'),rng.normal(0,1,nb))*np.hanning(nb)
        segs.append(x/np.abs(x).max()*0.12); mids.append(np.full(nb,np.nan)); pos+=nb
np.concatenate(segs).astype(np.float32).tofile('in.f32'); np.save('in_midi.npy',np.concatenate(mids))
np.savetxt('notes_base.txt',np.array(notes),fmt='%d %d %.4f')
