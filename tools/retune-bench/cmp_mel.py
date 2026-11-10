"""Melodyne vs NovaTone on the same take. usage: python3 -I cmp_mel.py <prepdir> label=path ...
Each render is aligned to the dry take, then judged per sung note against ITS OWN shift (median of out-in),
so different note choices (Melodyne snaps to the key) don't count against anyone."""
import sys, numpy as np, soundfile as sf, librosa, scipy.signal as sg
d=sys.argv[1]; sr=44100; hop=512
x=np.fromfile(f'{d}/yv.f32',np.float32); mi=np.load(f'{d}/yv_midi.npy'); nt=np.loadtxt(f'{d}/notes0.txt')
def env(y): return np.sqrt(sg.lfilter(np.ones(441)/441,1,y.astype(float)**2))[::441]
def mel(y):
    M=librosa.feature.melspectrogram(y=y.astype(np.float32),sr=sr,n_fft=2048,hop_length=hop,n_mels=24,fmax=8000); return 10*np.log10(M+1e-9)
E0=mel(x); loud=E0.max(0)>E0.max()-40; ex=env(x)
for a in sys.argv[2:]:
    lab,p=a.split('='); y,r=sf.read(p); assert r==sr; y=(y.mean(1) if y.ndim>1 else y).astype(np.float32)
    ey=env(y); n=min(len(ex),len(ey)); c=sg.correlate(ey[:n],ex[:n],'full'); lag=(np.argmax(c)-(n-1))*441
    # refine to the sample on the waveform envelope around the coarse lag is not needed for 11.6 ms frames
    y=y[lag:] if lag>=0 else np.concatenate([np.zeros(-lag,np.float32),y]); y=np.pad(y,(0,max(0,len(x)-len(y))))[:len(x)]
    f,_,_=librosa.pyin(y.astype(float),fmin=70,fmax=1100,sr=sr,frame_length=2048,hop_length=hop)
    mo=librosa.hz_to_midi(f); m=min(len(mo),len(mi)); dsh=mo[:m]-mi[:m]
    core=[];edge=[];lv=[];off=[];oct=0;sh=[]
    for s,e,cen,_,_ in nt:
        a0,b0=int(s//hop),int(e//hop); seg=dsh[a0:b0]
        if np.sum(~np.isnan(seg))<6: continue
        md=np.nanmedian(seg[3:-3]) if b0-a0>10 else np.nanmedian(seg); sh.append(md)
        err=(seg-md)*100; oct+=int(np.sum(err<-900)); err=np.where(np.abs(err)>600,np.nan,err)
        core+=list(err[6:-6]); edge+=list(err[:6])+list(err[-6:])
        land=cen+md; off.append(abs(land-round(land))*100)
        if e-s>8000: lv.append(10*np.log10(np.mean(y[int(s)+2000:int(e)-2000]**2)/np.mean(x[int(s)+2000:int(e)-2000]**2)))
    E=mel(y); k=min(E.shape[1],E0.shape[1]); dist=np.sqrt(np.mean((E[:,:k]-E0[:,:k])**2,0))[loud[:k]]
    rr=lambda v: np.sqrt(np.nanmean(np.array(v)**2))
    print(f'{lab:14s} lag {lag/sr:+.2f}s | median move {np.median(sh):+.2f} st | sung shape bent: inside notes {rr(core):3.0f} c, edges {rr(edge):3.0f} c | '
          f'notes land {np.mean(off):2.0f} c off | level {np.mean(lv):+.2f} dB | timbre change {dist.mean():.1f} dB (worst 1%: {np.percentile(dist,99):.0f}) | octave-drop frames {oct}')
