@echo off
REM NovaTone pitch-model fine-tune (co-worker method). Restartable: finished stages are skipped.
REM Needs PY = a python.exe that has torch with CUDA (Gemini finds it, see the prompt).
setlocal
cd /d %~dp0
if not exist logs mkdir logs
if not exist data mkdir data
set LOG=logs\job.log
if "%PY%"=="" set PY=python
echo == stage setup %DATE% %TIME% >> %LOG%
if not exist venv\Scripts\python.exe "%PY%" -m venv --system-site-packages venv >> %LOG% 2>&1
set VPY=venv\Scripts\python.exe
"%VPY%" -m pip install -q onnx onnxruntime soundfile scipy mir_eval pretty_midi librosa resampy >> %LOG% 2>&1
"%VPY%" -m pip install -q --no-deps basic-pitch >> %LOG% 2>&1
"%VPY%" -c "import torch;print('torch',torch.__version__,'cuda',torch.cuda.is_available())" >> %LOG% 2>&1
echo == stage data %TIME% >> %LOG%
if not exist data\guitarset mkdir data\guitarset
if not exist data\annotation.zip curl.exe -L --retry 3 -o data\annotation.zip "https://zenodo.org/records/3371780/files/annotation.zip?download=1" >> %LOG% 2>&1
if not exist data\audio_mono-mic.zip curl.exe -L --retry 3 -o data\audio_mono-mic.zip "https://zenodo.org/records/3371780/files/audio_mono-mic.zip?download=1" >> %LOG% 2>&1
if not exist data\guitarset\unzipped.txt (
  tar -xf data\annotation.zip -C data\guitarset >> %LOG% 2>&1
  tar -xf data\audio_mono-mic.zip -C data\guitarset >> %LOG% 2>&1
  echo ok> data\guitarset\unzipped.txt
)
for %%S in (0 1 2) do (
  echo == stage train seed %%S %TIME% >> %LOG%
  if not exist runs\s%%S\model.onnx "%VPY%" train.py --stock nmp_stock.onnx --out runs\s%%S --guitarset data\guitarset --seed %%S --steps 3000 --batch 16 --lr 1e-5 --gs_frac 0.7 --workers 6 --save_every 1000 >> %LOG% 2>&1
)
echo == stage eval %TIME% >> %LOG%
"%VPY%" evaluate.py nmp_stock.onnx runs\s0\model.onnx runs\s1\model.onnx runs\s2\model.onnx --guitarset data\guitarset --out run_compare.txt >> %LOG% 2>&1
echo %ERRORLEVEL%> logs\job.exit
echo == stage end %TIME% >> %LOG%
