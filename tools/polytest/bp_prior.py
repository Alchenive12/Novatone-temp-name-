# Basic Pitch posteriorgrams -> prior file for NovaTone: header "NTPRIOR frames fps" then frames x 88 note floats, frames x 88 onset floats
import sys, os, warnings, struct, numpy as np; warnings.filterwarnings("ignore")
import logging; logging.disable(logging.WARNING)
import basic_pitch
from basic_pitch.inference import predict
from basic_pitch.constants import AUDIO_SAMPLE_RATE, FFT_HOP
model=os.path.join(os.path.dirname(basic_pitch.__file__),'saved_models','icassp_2022','nmp.onnx')
out,_,_=predict(sys.argv[1], model)
note=out['note'].astype(np.float32); onset=out['onset'].astype(np.float32)
fps=AUDIO_SAMPLE_RATE/FFT_HOP
with open(sys.argv[2],'wb') as f:
    f.write(struct.pack('<8sif', b'NTPRIOR\0', note.shape[0], fps)); f.write(note.tobytes()); f.write(onset.tobytes())
print(sys.argv[2], note.shape, round(fps,2), 'max', note.max().round(2))
