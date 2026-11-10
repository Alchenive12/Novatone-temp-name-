# edge2.py k label=path ... : error vs each note's own median shift, by frame from note start/end,
# only on frames where the dry take is clearly pitched (pYIN voiced prob > 0.8, level > -30 dB re peak)
import sys, numpy as np, soundfile as sf, librosa
k=sys.argv[1]; sr=44100; hop=256
x=np.fromfile('prep/yv.f32',np.float32); nt=np.loadtxt(f'prep/notes{k}.txt')
def f0(y):
    f,_,p=librosa.pyin(y.astype(float),fmin=70,fmax=1100,sr=sr,frame_length=2048,hop_length=hop); return librosa.hz_to_midi(f),p
try: z=np.load('mi256b.npz'); mi,pv=z['mi'],z['pv']
except Exception: mi,pv=f0(x); np.savez('mi256b.npz',mi=mi,pv=pv)
rms=librosa.feature.rms(y=x,frame_length=1024,hop_length=hop)[0][:len(mi)]; ok=(pv>0.8)&(20*np.log10(rms/np.abs(x).max()+1e-9)>-30)
for a in sys.argv[2:]:
    lab,p=a.split('='); y,_=sf.read(p); mo,_=f0(y); n=min(len(mo),len(mi)); d=np.where(ok[:n],mo[:n]-mi[:n],np.nan)
    S={'gap-start':[], 'touch-start':[], 'gap-end':[], 'touch-end':[]}; allc=[];alle=[]
    for i,(s,e,c,tg,_) in enumerate(nt):
        a0,b0=int(s//hop),int(e//hop)
        if b0-a0<30 or np.sum(~np.isnan(d[a0+10:b0-10]))<5: continue
        md=np.nanmedian(d[a0+10:b0-10]); err=(d[a0:b0]-md)*100; err=np.where(np.abs(err)>700,np.nan,err)
        ts= i>0 and nt[i-1][1]==s; te= i+1<len(nt) and nt[i+1][0]==e
        S['touch-start' if ts else 'gap-start'].append(err[:12]); S['touch-end' if te else 'gap-end'].append(err[-12:]); allc+=list(err[12:-12]); alle+=list(err[:12])+list(err[-12:])
    r=lambda v: np.sqrt(np.nanmean(np.array(v)**2))
    print(f'{lab}: inside {r(allc):.0f} c, edges(70ms) {r(alle):.0f} c')
    for kk,v in S.items():
        v=np.array(v); print('   %-11s n=%3d rms by frame:'%(kk,len(v)),' '.join('%3.0f'%q for q in np.sqrt(np.nanmean(v**2,0))),'| mean:',' '.join('%4.0f'%q for q in np.nanmean(v,0)))
