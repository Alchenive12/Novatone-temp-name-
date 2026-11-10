# rough.py label=path ... : per-frame periodicity (autocorr clarity at the frame's own pitch), amplitude flutter and
# HF noise, each against the dry take. Lists where a render is clearly rougher than dry.
import sys, numpy as np, soundfile as sf, librosa, scipy.signal as sg
sr=44100; hop=512; N=2048
x=np.fromfile('prep/yv.f32',np.float32)
def clar(y):
    f,_,p=librosa.pyin(y.astype(float),fmin=70,fmax=1100,sr=sr,frame_length=N,hop_length=hop)
    c=np.full(len(f),np.nan); yp=np.pad(y,(N//2,N))
    for i,fi in enumerate(f):
        if np.isnan(fi): continue
        s=yp[i*hop:i*hop+N].astype(float); L=int(round(sr/fi))
        if L<8 or L>N//2: continue
        a=s[:-L]; b=s[L:]; den=np.sqrt((a*a).sum()*(b*b).sum())
        if den>1e-9: c[i]=(a*b).sum()/den
    return f,c,p
def flutter(y):   # 25-90 Hz wobble of the level envelope, relative
    e=np.sqrt(sg.lfilter(np.ones(256)/256,1,y.astype(float)**2)+1e-12)[::64]; fs=sr/64
    le=20*np.log10(e+1e-6); bp=sg.sosfiltfilt(sg.butter(2,[25,90],'bp',fs=fs,output='sos'),le)
    return np.sqrt(sg.lfilter(np.ones(40)/40,1,bp**2))[::8]   # one value per 512 samples
def hf(y):
    S=np.abs(librosa.stft(y,n_fft=N,hop_length=hop))**2; fr=np.fft.rfftfreq(N,1/sr)
    return 10*np.log10(S[fr>6000].sum(0)+1e-12)-10*np.log10(S[(fr>200)&(fr<4000)].sum(0)+1e-12)
f0,c0,p0=clar(x); fl0=flutter(x); h0=hf(x)
rms=librosa.feature.rms(y=x,frame_length=N,hop_length=hop)[0]; ok=(p0>0.8)&(20*np.log10(rms[:len(p0)]/np.abs(x).max()+1e-9)>-30)
for a in sys.argv[1:]:
    lab,p=a.split('='); y,_=sf.read(p); y=y.astype(np.float32)[:len(x)]; y=np.pad(y,(0,len(x)-len(y)))
    f,c,_=clar(y); n=min(len(c),len(c0)); m=ok[:n]
    dc=(c[:n]-c0[:n]); fl=flutter(y); k=min(len(fl),len(fl0),n); dfl=fl[:k]-fl0[:k]; h=hf(y); dh=h[:n]-h0[:n]
    bad=np.where(m&((dc<-0.08)|np.isnan(c[:n])))[0]; g=[]
    for i in bad:
        if g and i-g[-1][1]<=2: g[-1][1]=i
        else: g.append([i,i])
    g=[q for q in g if q[1]-q[0]>=1]
    print(f'{lab:8s} periodicity vs dry: mean {np.nanmean(dc[m]):+.3f}, rough frames {len(bad)} ({len(bad)*hop/sr:.2f}s) in {len(g)} spots | flutter vs dry {np.nanmean(dfl[m[:k]]):+.2f} dB (p95 {np.nanpercentile(dfl[m[:k]],95):+.2f}) | HF noise vs dry {np.nanmean(dh[m]):+.1f} dB')
    print('         spots:',' '.join('%.2f(%dms)'%(a0*hop/sr,(b0-a0+1)*hop*1000/sr) for a0,b0 in sorted(g,key=lambda q:q[0]-q[1])[:14]))
