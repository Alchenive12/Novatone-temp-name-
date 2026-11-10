import numpy as np, subprocess, sys
x=np.fromfile('in.f32',np.float32); nb=len(x)//512+9
seg=slice(int(0.3*44100),int(1.2*44100))
r0=np.sqrt(np.mean(x[seg]**2))
for exe in sys.argv[1:]:
    row=[]
    for s in (-5,-3,-1,0,1,3,5):
        np.full(nb,56.7+s,np.float32).tofile('tg.f32')
        subprocess.run([f'./{exe}','in.f32','o.f32','tg.f32','1','0.5'],check=True)
        y=np.fromfile('o.f32',np.float32); row.append(f"{s:+d}:{20*np.log10(np.sqrt(np.mean(y[seg]**2))/r0):+.2f}")
    print(exe,' '.join(row))
