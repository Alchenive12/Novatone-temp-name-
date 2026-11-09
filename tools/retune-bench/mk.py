import numpy as np, scipy.signal as sg
sr=44100; rng=np.random.default_rng(7)
segs=[]; f0s=[]
def note(dur, base, vib=0.0):
    n=int(dur*sr); t=np.arange(n)/sr
    drift=np.cumsum(rng.normal(0,1,n)); drift=sg.sosfilt(sg.butter(2,3,fs=sr,output='sos'),drift); drift=drift/np.abs(drift).max()*0.12
    jit=sg.sosfilt(sg.butter(2,25,fs=sr,output='sos'),rng.normal(0,1,n)); jit=jit/np.abs(jit).max()*0.03
    m=base+drift+jit+vib*np.clip((t-0.25)/0.3,0,1)*np.sin(2*np.pi*5.5*t)
    f0=440*2**((m-69)/12); ph=np.cumsum(2*np.pi*f0/sr); y=np.zeros(n)
    for k in range(1,45):
        y+=(k*f0<9000)*np.sin(k*ph)/k**1.2
    env=np.minimum(1,np.minimum(t/0.03,(dur-t)/0.05)); y*=env
    out=np.zeros(n)
    for fc,bw in ((700,90),(1150,110),(2500,170)):
        b,a=sg.iirpeak(fc,fc/bw,sr); out+=sg.lfilter(b,a,y)
    return out/np.abs(out).max()*0.5, m
def breath(dur, hp=False):
    n=int(dur*sr); x=rng.normal(0,1,n)
    sos=sg.butter(4,4000,'hp',fs=sr,output='sos') if hp else sg.butter(2,[300,3000],'bp',fs=sr,output='sos')
    x=sg.sosfilt(sos,x); x*=np.hanning(n); return x/np.abs(x).max()*0.15, np.full(n,np.nan)
for part in [note(1.4,56.7), breath(0.35), note(1.4,56.7,0.3), breath(0.25,True), note(1.4,56.7)]:
    segs.append(part[0]); f0s.append(part[1])
x=np.concatenate(segs).astype(np.float32); m=np.concatenate(f0s)
x.tofile('in.f32'); np.save('in_midi.npy',m)
print(len(x)/sr)
