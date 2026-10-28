import numpy as np, soundfile as sf, onnxruntime as ort, os, time, librosa, warnings; warnings.filterwarnings("ignore")
import basic_pitch
from basic_pitch.constants import AUDIO_SAMPLE_RATE as SR, AUDIO_N_SAMPLES as N, FFT_HOP
sess=ort.InferenceSession(os.path.join(os.path.dirname(basic_pitch.__file__),'saved_models','icassp_2022','nmp.onnx'),providers=['CPUExecutionProvider'])
inp=sess.get_inputs()[0].name; outs=[o.name for o in sess.get_outputs()]
x,sr=sf.read('female.wav'); f0=np.load('female_f0.npy'); y=librosa.resample(x,orig_sr=sr,target_sr=SR)
def f0_at(window, frame_from_end):
    o=dict(zip(outs,sess.run(None,{inp:window[None,:,None].astype(np.float32)})))
    c=[v for k,v in o.items() if v.shape[-1]==264][0][0]; n=[v for k,v in o.items() if v.shape[-1]==88]
    i=c.shape[0]-1-frame_from_end; k=int(c[i].argmax()); return 440*2**((21+(k-1)/3-69)/12), c.shape[0]
rng=np.random.default_rng(0); ts=rng.uniform(3,22,80)
for L in (0.0,0.05,0.1,0.25,0.5):
    ok=0; tot=0; dt=0
    for t in ts:
        ref=f0[int(t*sr)]
        if ref<=0: continue
        end=int((t+L)*SR); w=y[max(0,end-N):end]
        if len(w)<N: w=np.r_[np.zeros(N-len(w)),w]
        t0=time.time(); est,nf=f0_at(w, int(round(L*SR/FFT_HOP))); dt+=time.time()-t0
        tot+=1; ok+= abs(1200*np.log2(est/ref))<50
    print(f"look-ahead {int(L*1000):3d} ms: within 50c {ok/tot:6.1%}   ({dt/tot*1000:.0f} ms CPU per call, 2 s window)")
