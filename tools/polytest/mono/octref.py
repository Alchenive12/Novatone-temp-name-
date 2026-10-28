import numpy as np, soundfile as sf, librosa, warnings, logging, os, sys
warnings.filterwarnings("ignore"); logging.disable(logging.WARNING)
sys.argv=['x']; exec(open('evalmono.py').read().split('for wav in sys.argv[1:]:')[0])
wav='/home/claude/vocaltest/part.wav'; x,sr=sf.read(wav); y=librosa.to_mono(x.T)
fr,vf,_=librosa.pyin(y,fmin=60,fmax=1200,sr=sr,frame_length=2048,hop_length=441); fr=np.where(vf,fr,0); tr=np.arange(len(fr))*441/sr
te,fe=mpm(wav); out,_,_=predict(wav,MODEL); tb,fb=bp_f0(out)
near=lambda T,F: np.array([F[min(len(F)-1,max(0,np.searchsorted(T,t)))] for t in tr])
m=near(te,fe); b=near(tb,fb)
cm=1200*np.log2(np.where((m>0)&(fr>0),m,1)/np.where(fr>0,fr,1))
flip=(fr>0)&(m>0)&(np.abs(np.abs(cm)-1200)<100)
cb=1200*np.log2(np.where((b>0)&(fr>0),b,1)/np.where(fr>0,fr,1)); cbm=1200*np.log2(np.where((b>0)&(m>0),b,1)/np.where(m>0,m,1))
n=flip.sum(); bpy=(flip&(b>0)&(np.abs(cb)<100)).sum(); bmpm=(flip&(b>0)&(np.abs(cbm)<100)).sum(); none=(flip&(b==0)).sum()
print(f"frames where MPM and pYIN are an octave apart: {n}")
print(f"  Basic Pitch sides with pYIN: {bpy}   sides with MPM: {bmpm}   silent: {none}   other: {n-bpy-bmpm-none}")
