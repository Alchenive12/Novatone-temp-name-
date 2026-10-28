@echo off
REM From-scratch Basic Pitch net, width 2 then width 1 (control). NSynth train on D:, GuitarSet from nt-train. Restartable.
setlocal
cd /d %~dp0
if not exist logs mkdir logs
set LOG=logs\wide.log
set VPY=..\nt-train\venv\Scripts\python.exe
set NS=D:\nt-stair-data\nsynth-train
set GS=..\nt-train\data\guitarset
set NT_STOCK=..\nt-train\nmp_stock.onnx
copy /y ..\nt-train\test_piano.wav . >nul
copy /y ..\nt-train\test_guitar.wav . >nul
for %%W in (2 1) do (
  echo == stage train width %%W >> %LOG%
  if not exist runs\w%%W\model.head.onnx "%VPY%" train_wide.py --stock %NT_STOCK% --out runs\w%%W --nsynth %NS% --guitarset %GS% --width %%W --steps 20000 --batch 16 --lr 1e-3 --workers 6 >> %LOG% 2>&1
)
echo == stage eval >> %LOG%
"%VPY%" evaluate.py %NT_STOCK% runs\w2\model.head.onnx runs\w1\model.head.onnx --guitarset %GS% --out wide_compare.txt >> %LOG% 2>&1
echo %ERRORLEVEL%> logs\wide.exit
echo == stage end >> %LOG%
