import sys
src=open(__file__.replace('sweep.py','evaluate.py')).read().split('\nap = ')[0]; exec(src)
from basic_pitch.note_creation import model_output_to_notes as mo
for path in sys.argv[1:]:
    m=Model(path)
    P=m.track(load22('test_piano.wav')); G=m.track(load22('test_guitar.wav'))
    for ot in (0.5,):
        for ft in (0.15,0.2,0.25):
            def nf(out,ot=ot,ft=ft):
                _,ev=mo(out,onset_thresh=ot,frame_thresh=ft,min_note_len=11,infer_onsets=True,melodia_trick=True,midi_tempo=120)
                return [(int(p),s,e) for s,e,p,a,b in ev]
            globals()['notes_from']=nf
            class C: track=lambda self,a,_P=P,_G=G: _P if len(a)>len(load22('test_guitar.wav')) else _G
            print(path.split('/')[-1],ot,ft,'piano',tuple(round(x,3) for x in piano(C())),'guitar',guitar(C()),flush=True)
