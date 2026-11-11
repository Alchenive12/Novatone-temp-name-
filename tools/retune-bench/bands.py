# bands.py label=path ... : output/dry power ratio per band on clearly voiced frames (dry pYIN p>0.8)
import sys, numpy as np, soundfile as sf, librosa
sr=44100; N=2048; hop=512
x=np.fromfile('prep/yv.f32',np.float32)
_,_,p=librosa.pyin(x.astype(float),fmin=70,fmax=1100,sr=sr,frame_length=N,hop_length=hop)
X=np.abs(librosa.stft(x,n_fft=N,hop_length=hop))**2; fr=np.fft.rfftfreq(N,1/sr); n=min(X.shape[1],len(p)); ok=p[:n]>0.8
edges=[200,1000,2000,3000,4000,5000,6000,8000,11000,16000]
print('band kHz   '+' '.join('%5.1f-%-4.1f'%(a/1000,b/1000) for a,b in zip(edges,edges[1:])))
for a in sys.argv[1:]:
    lab,pth=a.split('='); y,_=sf.read(pth); y=y.astype(np.float32)[:len(x)]
    Y=np.abs(librosa.stft(y,n_fft=N,hop_length=hop))**2
    r=[10*np.log10(Y[(fr>=lo)&(fr<hi)][:,:n][:,ok].sum()/X[(fr>=lo)&(fr<hi)][:,:n][:,ok].sum()) for lo,hi in zip(edges,edges[1:])]
    print('%-10s '%lab+' '.join('%+9.1f'%v for v in r))
