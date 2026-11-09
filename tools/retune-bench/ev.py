import numpy as np, librosa, sys
sr=44100; L=2019
x=np.fromfile('in.f32',np.float32); m=np.load('in_midi.npy')
hop=256
def f0(y):
    f,v,_=librosa.pyin(y.astype(float),fmin=100,fmax=400,sr=sr,frame_length=2048,hop_length=hop)
    return librosa.hz_to_midi(f)
fi=f0(x)
for t in sys.argv[1:]:
    y=np.fromfile(f'out_{t}.f32',np.float32)[L:]; y=np.r_[y,np.zeros(len(x)-len(y))]
    fo=f0(y); n=min(len(fi),len(fo))
    idx=np.arange(n)*hop; mm=m[np.minimum(idx,len(m)-1)]
    core=~np.isnan(mm)
    # trim note edges
    edge=np.convolve(np.isnan(mm).astype(float),np.ones(25),'same')>0
    core&=~edge
    ok=core&~np.isnan(fi[:n])&~np.isnan(fo[:n])
    dev_in=fi[:n][ok]-np.nanmedian(fi[:n][ok]); dev_out=fo[:n][ok]-np.nanmedian(fo[:n][ok])
    centre=np.nanmedian(fo[:n][ok])
    keep=np.std(dev_out)/np.std(dev_in); corr=np.corrcoef(dev_in,dev_out)[0,1]
    resid=np.sqrt(np.mean((dev_out-dev_in)**2))*100
    # breath regions: error vs dry
    nanmask=np.isnan(m); seg=nanmask.copy()
    err=10*np.log10(np.sum((y[seg]-x[seg])**2)/np.sum(x[seg]**2))
    print(f"{t}: centre {centre:.2f} (target 58), expression kept {keep*100:.0f}%, corr {corr:.2f}, resid {resid:.1f}c, breath diff vs dry {err:.1f} dB")
