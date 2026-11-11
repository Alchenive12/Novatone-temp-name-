# Synthetic sung vocal, female range, with breath noise in the voice (HF), voiced-consonant note ends ("z"),
# fading note tails into near-silence, and gaps. Writes sv.wav (44.1k mono).
import numpy as np, scipy.signal as sg, soundfile as sf
sr=44100; rng=np.random.default_rng(5)
def lp(x,fc): return sg.sosfiltfilt(sg.butter(2,fc,fs=sr,output='sos'),x)
phrases=[[(62.2,.45),(64.1,.5),(65.8,.6),(64.2,.45),(62.1,.8)],
         [(67.1,.5),(65.9,.4),(64.0,.7)],
         [(60.2,.4),(62.1,.4),(64.2,.45),(65.1,.9)],
         [(69.1,.6),(67.2,.5),(65.0,.5),(64.1,1.0)]]
out=[]
for pi,ph in enumerate(phrases):
    n=int(sum(d for _,d in ph)*sr); t=np.arange(n)/sr; m=np.zeros(n); b=0; bounds=[]
    for mi,d in ph: e=b+int(d*sr); m[b:e]=mi; bounds.append((b,e)); b=e
    m=lp(m,10.0); m-=1.0*np.exp(-t/0.05)
    dr=np.cumsum(rng.normal(0,1,n)); dr=lp(dr,3); m+=dr/np.abs(dr).max()*0.08
    m+=0.25*np.clip((t-t[-1]+0.7)/0.25,0,1)*np.sin(2*np.pi*5.4*t)   # vibrato on the last note
    f0=440*2**((m-69)/12); ph_=np.cumsum(f0/sr)%1.0
    glot=np.zeros(n)
    for k in range(1,60): glot+=(k*f0<11000)*np.sin(2*np.pi*k*ph_)/k**1.1
    pulse=np.exp(-((ph_-0.1)%1.0)/0.12)                              # breath noise gated by the glottal cycle
    asp=sg.sosfilt(sg.butter(2,2500,'hp',fs=sr,output='sos'),rng.normal(0,1,n))*pulse*0.06
    src=glot/np.abs(glot).max()+asp
    y=np.zeros(n)
    for fc,bw,g in ((750,250,1),(1300,300,.7),(2800,450,.5),(3600,500,.35),(7000,2500,.25)):
        bb,aa=sg.iirpeak(fc,fc/bw,sr); y+=g*sg.lfilter(bb,aa,src)
    env=np.minimum(1,t/0.03)
    # note-end behaviour per phrase: 0/2 fade to near silence, 1/3 end in a voiced "z" (pitched + strong HF noise)
    tail=int(0.25*sr)
    if pi%2==0: env[-tail:]*=np.linspace(1,0.01,tail)**2
    else:
        z=int(0.12*sr); env[-z:]*=0.6
        hiss=sg.sosfilt(sg.butter(4,[4000,9000],'bp',fs=sr,output='sos'),rng.normal(0,1,z))*np.hanning(2*z)[z:]*0.15
        y[-z:]+=hiss/0.6
        env[-int(0.02*sr):]*=np.linspace(1,0,int(0.02*sr))
    y=y*env; y=y/np.abs(y).max()*0.5; out.append(y)
    gap=int(0.35*sr); nb=sg.sosfilt(sg.butter(2,[300,4000],'bp',fs=sr,output='sos'),rng.normal(0,1,gap))*np.hanning(gap)*0.03
    out.append(nb)
sf.write('sv.wav',np.concatenate(out).astype(np.float32),sr,subtype='FLOAT')
