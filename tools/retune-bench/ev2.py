import numpy as np, librosa, sys
sr=44100; L=2019; hop=256
x=np.fromfile('in.f32',np.float32); m=np.load('in_midi.npy')
def f0(y):
    f,v,_=librosa.pyin(y.astype(float),fmin=100,fmax=400,sr=sr,frame_length=2048,hop_length=hop); return librosa.hz_to_midi(f)
fi=f0(x)
bounds=[(0,1.4),(1.75,3.15),(3.4,4.8)]
for t in sys.argv[1:]:
    y=np.fromfile(f'out_{t}.f32',np.float32)[L:]; y=np.r_[y,np.zeros(len(x)-len(y))]
    fo=f0(y); tt=np.arange(len(fo))*hop/sr; row=[]
    for a,b in bounds:
        k=(tt>a+0.15)&(tt<b-0.1)&~np.isnan(fo)&~np.isnan(fi[:len(fo)])
        di=fi[:len(fo)][k]-fi[:len(fo)][k].mean(); do=fo[k]-fo[k].mean()
        row.append(f"c{fo[k].mean():.2f} keep{np.std(do)/np.std(di)*100:.0f}% r{np.corrcoef(di,do)[0,1]:.2f}")
    print(t,' | '.join(row))
