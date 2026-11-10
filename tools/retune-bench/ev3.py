# ev3.py scenario(A|B) exe lag...   prints how well (out pitch - in pitch) matches each note's shift
import numpy as np, librosa, subprocess, sys
sr=44100; hop=256
x=np.fromfile('in.f32',np.float32); nb=np.loadtxt('notes_base.txt')
sc=sys.argv[1]; rows=[]
for i,(s,e,d) in enumerate(nb):
    ed = 1 if sc=='A' else int(i in (2,5)); tg = round(d)+(1 if sc=='A' else 2)
    rows.append((s,e,d,tg,ed))
np.savetxt('notes.txt',np.array(rows),fmt='%d %d %.4f %.4f %d')
def f0(y):
    f,_,_=librosa.pyin(y.astype(float),fmin=120,fmax=700,sr=sr,frame_length=2048,hop_length=hop); return librosa.hz_to_midi(f)
fi=f0(x); tt=np.arange(len(fi))*hop
for exe in sys.argv[2].split(','):
  for lag in sys.argv[3:]:
    subprocess.run([f'./{exe}','in.f32',f'o_{exe}_{sc}.f32','notes.txt','1','0.5',lag],check=True,stderr=subprocess.DEVNULL)
    y=np.fromfile(f'o_{exe}_{sc}.f32',np.float32); fo=f0(y)[:len(fi)]; dsh=fo-fi
    core=[];edge=[];lvl=[]
    for (s,e,d,tg,ed) in rows:
        if not ed: continue
        k=(tt>=s)&(tt<e)&~np.isnan(dsh); err=(dsh[k]-(tg-d))*100; t=tt[k]
        c=(t>s+0.07*sr)&(t<e-0.07*sr); core+=list(err[c]); edge+=list(err[~c])
        a,b=int(s+0.1*sr),int(e-0.1*sr); lvl.append(20*np.log10(np.sqrt(np.mean(y[a:b]**2)/np.mean(x[a:b]**2))))
    core=np.array(core);edge=np.array(edge)
    jump=np.nanmax(np.abs(np.diff(fo-fi)))*100
    print(f"{sc} {exe} lag{lag}: core rms {np.sqrt(np.mean(core**2)):.1f}c bias {core.mean():+.1f}c | edges(70ms) rms {np.sqrt(np.mean(edge**2)):.1f}c max {np.abs(edge).max():.0f}c | biggest hop-to-hop shift jump {jump:.0f}c | level {np.mean(lvl):+.2f} dB")
    np.save(f'd_{exe}_{sc}_{lag}.npy',dsh)
