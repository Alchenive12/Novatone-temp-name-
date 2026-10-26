import sys, os, warnings; warnings.filterwarnings("ignore")
import logging; logging.disable(logging.WARNING)
import basic_pitch
from basic_pitch.inference import predict
model=os.path.join(os.path.dirname(basic_pitch.__file__),'saved_models','icassp_2022','nmp.onnx')
wav,out=sys.argv[1],sys.argv[2]
_,_,ev=predict(wav, model)
NAMES=['C','C#','D','D#','E','F','F#','G','G#','A','A#','B']
with open(out,'w') as f:
    f.write(f"analysed basic-pitch, {len(ev)} notes\n")
    for s,e,p,a,_ in sorted(ev,key=lambda x:(x[0],x[2])):
        f.write(f"  {s:6.2f}-{e:<6.2f} {NAMES[p%12]}{p//12-1}  (0 c off)  level {a:.4f}\n")
print(out, len(ev))
