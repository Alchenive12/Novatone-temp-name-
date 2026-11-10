# energy below the (shifted) fundamental = growl / "monster". usage: find_sub.py k label=path ...
import sys, numpy as np, soundfile as sf
k=int(sys.argv[1]); sr=44100; hop=512; N=4096
mi=np.load('prep/yv_midi.npy'); f0=440*2**((mi+k-69)/12)
fr=np.fft.rfftfreq(N,1/sr); w=np.hanning(N); R={}
for a in sys.argv[2:]:
    lab,p=a.split('='); y,_=sf.read(p); y=np.pad(y,(N//2,N))
    r=np.full(len(mi),np.nan)
    for i in range(len(mi)):
        if np.isnan(f0[i]): continue
        S=np.abs(np.fft.rfft(y[i*hop:i*hop+N]*w))**2
        tot=S[fr>60].sum()
        if tot<1e-6: continue
        r[i]=S[(fr>60)&(fr<0.75*f0[i])].sum()/tot
    R[lab]=r
labs=list(R); base=R[labs[0]]
for lab in labs[1:]:
    bad=np.where((R[lab]>0.15)&(R[lab]>np.nan_to_num(base)+0.10))[0]; g=[]
    for i in bad:
        if g and i-g[-1][1]<=3: g[-1][1]=i
        else: g.append([i,i])
    print(lab,'growl spots vs',labs[0],':',len(g),'total %.2fs'%(len(bad)*hop/sr),'|',' '.join('%.2f(%dms)'%(a*hop/sr,(b-a+1)*hop*1000/sr) for a,b in g[:30]))
