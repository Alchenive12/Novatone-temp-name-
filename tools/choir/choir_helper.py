"""NovaTone Choir helper: turns the plugin's voice requests into AI singer voices (Seed-VC), one part at a time.

Run it while you use the Choir tab (it watches Documents/NovaTone/Choir/*/requests):
    cd D:\\nt-choir\\seed-vc
    D:\\nt-choir\\venv\\Scripts\\python.exe <repo>\\tools\\choir\\choir_helper.py

For each request <part>_<voice>.req:
  1. part channel of parts.wav -> Seed-VC (f0 44 kHz model, cfg 0.7, 30 steps) with refs/<voice>.wav;
     low / mid voices use the fine-tuned accent checkpoint when there is one, high voices the base model.
  2. Voice check (CAMPPlus): the result must sound closer to its singer than to the lead singer (margin 0.02).
     Fine-tuned fails -> try the base model; both fail -> <part>_<voice>.fail.
  3. Loudness matched to the part (A-weighted RMS), resampled to the session rate, same length as parts.wav,
     written to voices/<part>_<voice>.wav (atomically; the plugin picks it up within a second).
"""
import glob, json, os, subprocess, sys, tempfile, time
import numpy as np, scipy.signal as sg, soundfile as sf, librosa

ROOT = os.path.join(os.path.expanduser('~'), 'Documents', 'NovaTone', 'Choir')
SEEDVC = os.environ.get('NT_SEEDVC', r'D:\nt-choir\seed-vc')
REFS = os.environ.get('NT_REFS', r'D:\nt-choir\refs')
FT_RUN = os.environ.get('NT_FT_RUN', os.path.join(SEEDVC, 'runs', 'hebrew_accent'))
HIGH = {'fhigh_a', 'fhigh_b', 'female_high', 'female_high2'}          # base model only (high voices)
if os.path.isdir(r'D:\nt-choir\hf'): os.environ.setdefault('HF_HOME', r'D:\nt-choir\hf')

def ft_checkpoint():
    c = sorted(glob.glob(os.path.join(FT_RUN, '*600*.pth'))) or sorted(glob.glob(os.path.join(FT_RUN, '*.pth')), key=os.path.getmtime)
    y = glob.glob(os.path.join(FT_RUN, '*.yml'))
    return (c[-1], y[0]) if c and y else (None, None)

def convert(src, ref, out_dir, ft):
    cmd = [sys.executable, os.path.join(SEEDVC, 'inference.py'), '--source', src, '--target', ref, '--output', out_dir,
           '--diffusion-steps', '30', '--length-adjust', '1.0', '--inference-cfg-rate', '0.7',
           '--f0-condition', 'True', '--auto-f0-adjust', 'False', '--semi-tone-shift', '0']
    if ft: cmd += ['--checkpoint', ft[0], '--config', ft[1]]
    subprocess.run(cmd, cwd=SEEDVC, check=True)
    return max(glob.glob(os.path.join(out_dir, '*.wav')), key=os.path.getmtime)

_campp = None
def emb(y, sr):
    global _campp
    import torch, torchaudio
    if _campp is None:
        sys.path.insert(0, SEEDVC)
        from modules.campplus.DTDNN import CAMPPlus
        from hf_utils import load_custom_model_from_hf
        ck = load_custom_model_from_hf('funasr/campplus', 'campplus_cn_common.bin', config_filename=None)
        _campp = CAMPPlus(feat_dim=80, embedding_size=192); _campp.load_state_dict(torch.load(ck, map_location='cpu')); _campp.eval()
    y = librosa.resample(y, orig_sr=sr, target_sr=16000) if sr != 16000 else y
    y = voiced(y, 16000)[:16000 * 20]
    f = torchaudio.compliance.kaldi.fbank(torch.tensor(y, dtype=torch.float32)[None], num_mel_bins=80, dither=0, sample_frequency=16000)
    with torch.no_grad(): e = _campp((f - f.mean(0, keepdim=True))[None])[0]
    return torch.nn.functional.normalize(e, dim=0).numpy()

def voiced(y, sr):   # drop silence so the check hears singing, not room noise
    iv = librosa.effects.split(y, top_db=35)
    return np.concatenate([y[a:b] for a, b in iv]) if len(iv) else y

def a_rms(x, sr):
    z, p, k = [0, 0, 0, 0], [-2*np.pi*20.6]*2 + [-2*np.pi*107.7, -2*np.pi*737.9] + [-2*np.pi*12194]*2, (2*np.pi*12194)**2 * 10**(2/20)
    b, a = sg.bilinear(*sg.zpk2tf(z, p, k), sr)
    return np.sqrt(np.mean(sg.lfilter(b, a, x) ** 2)) + 1e-9

def handle(job):
    d = os.path.dirname(os.path.dirname(job)); name = os.path.basename(job)[:-4]
    work = job[:-4] + '.working'; os.replace(job, work)
    os.makedirs(os.path.join(d, 'voices'), exist_ok=True)
    try:
        part, voice = open(work).read().split()[:2]; part = int(part)
        sess = json.load(open(os.path.join(d, 'session.json'))); sr = int(sess['sampleRate'])
        parts, psr = sf.read(os.path.join(d, 'parts.wav'), dtype='float32', always_2d=True)
        src_y, lead = parts[:, part], parts[:, 0]
        ref = os.path.join(REFS, voice + '.wav')
        if not os.path.exists(ref): raise RuntimeError('no reference for ' + voice)
        tmp = tempfile.mkdtemp(prefix='ntchoir_'); src = os.path.join(tmp, f'part{part}.wav'); sf.write(src, src_y, psr)
        e_ref, e_lead = emb(*librosa.load(ref, sr=16000, mono=True)), emb(lead, psr)
        tries = ([ft_checkpoint()] if voice not in HIGH and ft_checkpoint()[0] else []) + [None]
        result, log = None, []
        for ft in tries:
            out = convert(src, ref, os.path.join(tmp, 'ft' if ft else 'base'), ft)
            y, ysr = sf.read(out, dtype='float32'); y = y.mean(1) if y.ndim > 1 else y
            e = emb(y, ysr); own, to_lead = float(e_ref @ e), float(e_lead @ e)
            log.append(f"{'ft' if ft else 'base'}: own singer {own:.3f}, lead {to_lead:.3f}")
            if own > to_lead + 0.02: result = (y, ysr); break
        if result is None:
            open(os.path.join(d, 'voices', name + '.fail'), 'w').write('\n'.join(log) + '\n'); print('FAIL', name, log, flush=True); return
        y, ysr = result
        y = librosa.resample(y, orig_sr=ysr, target_sr=sr) if ysr != sr else y
        y = np.pad(y, (0, max(0, len(src_y) - len(y))))[:len(src_y)]
        y *= a_rms(src_y, sr) / a_rms(y, sr)
        dst = os.path.join(d, 'voices', name + '.wav'); part_tmp = dst + '.part'
        sf.write(part_tmp, y.astype(np.float32), sr, format='WAV', subtype='PCM_24'); os.replace(part_tmp, dst)
        print('ok', name, log[-1], flush=True)
    except Exception as ex:
        open(os.path.join(d, 'voices', name + '.fail'), 'w').write(f'error: {ex}\n'); print('ERROR', name, ex, flush=True)
    finally:
        if os.path.exists(work): os.remove(work)

if __name__ == '__main__':
    print('NovaTone Choir helper watching', ROOT, '| fine-tuned:', ft_checkpoint()[0] or 'none (base model only)', flush=True)
    while True:
        jobs = sorted(glob.glob(os.path.join(ROOT, '*', 'requests', '*.req')), key=os.path.getmtime)
        if jobs: handle(jobs[0])
        else: time.sleep(0.5)
