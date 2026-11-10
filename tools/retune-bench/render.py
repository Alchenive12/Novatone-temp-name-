# render.py exe label shifts... -> r_<label>_<k>.wav
import sys, subprocess, numpy as np, soundfile as sf
exe,lab=sys.argv[1],sys.argv[2]
for k in sys.argv[3:]:
    subprocess.run([exe,'prep/yv.f32','r.f32',f'prep/notes{k}.txt','1.0','0.5','512'],check=True,stderr=subprocess.DEVNULL)
    sf.write(f'r_{lab}_{k}.wav',np.fromfile('r.f32',np.float32),44100,subtype='FLOAT')
