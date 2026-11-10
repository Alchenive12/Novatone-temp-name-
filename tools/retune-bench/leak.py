# leak.py k label=path ... : frames INSIDE roll notes where the dry take has a pitch but the render is not shifted
# (out-in nearer 0 than the note's shift) = bits of the note passing dry -> pitch blip of the whole move.
import sys, numpy as np, soundfile as sf, librosa
k=sys.argv[1]; sr=44100; hop=256
x=np.fromfile('prep/yv.f32',np.float32); nt=np.loadtxt(f'prep/notes{k}.txt')
z=np.load('mi256b.npz'); mi,pv=z['mi'],z['pv']
rms=20*np.log10(librosa.feature.rms(y=x,frame_length=1024,hop_length=hop)[0][:len(mi)]/np.abs(x).max()+1e-9)
for a in sys.argv[2:]:
    lab,p=a.split('='); y,_=sf.read(p)
    f,_,_=librosa.pyin(y.astype(float),fmin=70,fmax=1100,sr=sr,frame_length=2048,hop_length=hop); mo=librosa.hz_to_midi(f)
    n=min(len(mo),len(mi)); d=mo[:n]-mi[:n]; leak=[];tot=0;where=[]
    for s,e,c,tg,_ in nt:
        a0,b0=int(s//hop),int(e//hop)
        if b0-a0<30: continue
        md=np.nanmedian(d[a0+10:b0-10])
        if np.isnan(md) or abs(md)<1.5: continue
        seg=d[a0:b0]; v=~np.isnan(seg)&(rms[a0:b0]>-45); tot+=v.sum()
        lk=v&(np.abs(seg)<0.35*abs(md)); leak.append(lk.sum())
        if lk.sum()>=3: where.append((a0+np.where(lk)[0][0],lk.sum(),'start' if np.where(lk)[0].mean()<(b0-a0)/2 else 'end'))
    print(f'{lab}: unshifted frames inside moved notes {sum(leak)} of {tot} ({sum(leak)*hop/sr:.2f}s, {100*sum(leak)/max(1,tot):.1f}%) in {len(where)} notes |',' '.join('%.2f(%dms,%s)'%(w*hop/sr,c*hop*1000/sr,pos) for w,c,pos in where[:25]))
