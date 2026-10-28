import sys, os, subprocess, warnings, time, numpy as np, soundfile as sf
warnings.filterwarnings("ignore"); import logging; logging.disable(logging.WARNING)
import basic_pitch, librosa
from basic_pitch.inference import predict
from basic_pitch.constants import AUDIO_SAMPLE_RATE, FFT_HOP
MODEL=os.path.join(os.path.dirname(basic_pitch.__file__),'saved_models','icassp_2022','nmp.onnx')
BENCH='/home/claude/build-tools/NovaTonePolyBench_artefacts/Release/NovaTonePolyBench'
def mpm(wav):
    o=subprocess.run([BENCH,'mono',wav],capture_output=True,text=True).stdout.split('\n')
    a=np.array([[float(x) for x in l.split()] for l in o if l.strip()]); return a[:,0],a[:,1]
def bp_f0(out):
    c=out['contour']; n=out['note']; T=np.arange(c.shape[0])*FFT_HOP/AUDIO_SAMPLE_RATE
    hz=np.zeros(len(T))
    for i in range(len(T)):
        if n[i].max()<0.3: continue
        k=int(c[i].argmax()); d=0
        if 0<k<c.shape[1]-1:
            y0,y1,y2=c[i,k-1],c[i,k],c[i,k+1]; den=y0-2*y1+y2; d=0.5*(y0-y2)/den if den<0 else 0
        hz[i]=440*2**((21+(k-1+d)/3-69)/12)   # bin 1 = A0 (constants.py)
    return T,hz
def score(tr,fr,te,fe,name):
    est=np.interp(tr,te,fe,left=0,right=0); near=np.array([fe[min(len(fe)-1,max(0,np.searchsorted(te,t)))] for t in tr])
    v=fr>0; ev=near>0
    both=v&ev; c=1200*np.log2(near[both]/fr[both])
    rpa=(np.abs(c)<50).sum()/v.sum(); octv=(np.abs(np.abs(c)-1200)<100).sum()/v.sum()
    print(f"  {name:11s} pitch within 50c {rpa:6.1%} | median error {np.median(np.abs(c)):5.1f} c | octave errors {octv:5.1%} | voiced frames found {both.sum()/v.sum():6.1%} | false voiced {(ev&~v).sum()/max(1,(~v).sum()):5.1%}")
for wav in sys.argv[1:]:
    print("==",wav)
    x,sr=sf.read(wav)
    if wav.endswith(('female.wav','male.wav')):
        f=np.load(wav[:-4]+'_f0.npy'); tr=np.arange(0,len(f)-1,441)/sr; fr=f[(tr*sr).astype(int)]
    else:
        y=librosa.to_mono(x.T) if x.ndim>1 else x
        fr,vf,_=librosa.pyin(y,fmin=60,fmax=1200,sr=sr,frame_length=2048,hop_length=441); fr=np.where(vf,fr,0); tr=np.arange(len(fr))*441/sr
        print("  (reference = pYIN)")
    te,fe=mpm(wav); score(tr,fr,te,fe,"NovaTone MPM")
    t0=time.time(); out,_,_=predict(wav,MODEL); dt=time.time()-t0
    tb,fb=bp_f0(out); score(tr,fr,tb,fb,"BasicPitch")
    print(f"  BasicPitch speed: {len(x)/sr/dt:.1f}x real time (offline, 1 CPU thread pool)")
