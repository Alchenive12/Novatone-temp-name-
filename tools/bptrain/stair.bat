@echo off
REM Staircase fine-tune (quiet notes). Reuses nt-train venv + GuitarSet; NSynth train set on D:. Restartable.
setlocal
cd /d %~dp0
if not exist logs mkdir logs
set LOG=logs\stair.log
set VPY=..\nt-train\venv\Scripts\python.exe
set NS=D:\nt-stair-data\nsynth-train
set GS=..\nt-train\data\guitarset
if not exist %NS%\examples.json (
  echo NO NSYNTH >> %LOG%
  echo 9> logs\stair.exit
  exit /b 9
)
copy /y ..\nt-train\test_piano.wav . >nul
copy /y ..\nt-train\test_guitar.wav . >nul
for %%S in (0 1) do (
  echo == stage train seed %%S >> %LOG%
  if not exist runs\s%%S\model.onnx "%VPY%" train_stair.py --stock ..\nt-train\nmp_stock.onnx --out runs\s%%S --nsynth %NS% --guitarset %GS% --start start.json --seed %%S --steps 8000 --batch 16 --lr 1e-5 --workers 6 >> %LOG% 2>&1
)
echo == stage eval >> %LOG%
"%VPY%" evaluate.py ..\nt-train\nmp_stock.onnx runs\s0\model.onnx runs\s1\model.onnx --guitarset %GS% --out stair_compare.txt >> %LOG% 2>&1
echo %ERRORLEVEL%> logs\stair.exit
echo == stage end >> %LOG%
