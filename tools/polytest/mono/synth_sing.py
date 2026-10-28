# Synthetic singer with exact f0: melody with slides, vibrato, breaths and consonant noise; formant-filtered pulse source
import numpy as np, soundfile as sf, scipy.signal as sg, sys
sr=44100; rng=np.random.default_rng(int(sys.argv[2]) if len(sys.argv)>2 else 3)
def make(lo,hi,seconds=24):
    n=int(seconds*sr); f0=np.zeros(n); t=0
    cur=rng.integers(lo,hi)
    while t<n-sr:
        dur=int(rng.uniform(0.25,0.9)*sr); nxt=int(np.clip(cur+rng.choice([-5,-3,-2,-1,1,2,3,4,5,7,-7,12,-12],p=[.08,.1,.14,.14,.14,.14,.1,.04,.04,.03,.03,.01,.01]),lo,hi))
        m=np.full(dur,float(nxt)); sl=int(0.07*sr); m[:sl]=np.linspace(cur,nxt,sl)   # slide in
        vib=np.clip((np.arange(dur)-0.2*sr)/(0.3*sr),0,1)*0.35*np.sin(2*np.pi*5.6*np.arange(dur)/sr)  # vibrato after 200 ms, +-35 c
        f0[t:t+dur]=440*2**((m+vib-69)/12); t+=dur; cur=nxt
        if rng.random()<0.25: t+=int(rng.uniform(0.08,0.3)*sr)   # breath / gap
    return f0
f0=make(int(sys.argv[3]),int(sys.argv[4]))
ph=np.cumsum(2*np.pi*f0/sr); voiced=f0>0
src=np.zeros_like(f0)
for k in range(1,40):
    ok=(k*f0<9000)
    src+=ok*np.sin(k*ph)/k**1.2
src*=voiced
env=sg.lfilter([1],[1,-0.995],(np.abs(np.diff(np.r_[0,voiced.astype(float)]))>0)*0)+voiced  # on/off
src*=np.convolve(env,np.ones(400)/400,'same')
# vowel formants (changes every ~1 s)
out=np.zeros_like(src); seg=sr
for i in range(0,len(src),seg):
    F=[(730,1090,2440),(270,2290,3010),(530,1840,2480),(570,840,2410),(440,1020,2240)][rng.integers(5)]
    y=np.zeros(min(seg,len(src)-i))
    for fc,bw in zip(F,(90,110,170)):
        b,a=sg.iirpeak(fc,fc/bw,sr); y+=sg.lfilter(b,a,src[i:i+seg])
    out[i:i+seg]=y
noise=rng.normal(0,1,len(out))*0.02
cons=np.zeros(len(out))
for i in np.where(np.diff(voiced.astype(int))==1)[0]:
    cons[max(0,i-1500):i]+=rng.normal(0,0.15,len(cons[max(0,i-1500):i]))   # consonant before each phrase
hp=sg.butter(4,3000,'hp',fs=sr,output='sos'); out+=sg.sosfilt(hp,cons)+noise
out/=np.abs(out).max()/0.8
sf.write(sys.argv[1]+'.wav',out.astype(np.float32),sr); np.save(sys.argv[1]+'_f0.npy',f0)
print(sys.argv[1], round(voiced.mean(),2),'voiced')
