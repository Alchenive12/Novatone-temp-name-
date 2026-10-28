# NovaTone training job: babysit only (co-worker method)

ROLE: start ONE job, watch it, write ONE report. Do NOT edit any script. Folder: `C:\Users\didi\Desktop\nt-train` (call it NT).

HARD RULES
- Write only in NT\gemini\ (and the files the job itself creates). Never commit, push or edit scripts.
- If a step fails: stop, save the command, exit code and the last 60 lines of NT\logs\job.log into NT\gemini\report.md, write "failed: <step> <one sentence>" to NT\gemini\status.txt.
- Google AI Pro plan only: no API keys, no paid services.
- Do not click antivirus / security popups.
- GPU 90 C or more on any reading: cancel at once
  (PowerShell: Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'train.py' } | Invoke-CimMethod -MethodName Terminate),
  write CANCELLED-HOT in NT\gemini\timeline.md and status.txt.

STEP 1  Find a Python that has PyTorch with CUDA. Try, in order: `python`, `py -3`, every python.exe under C:\Users\didi\Miniconda3 and
        C:\Users\didi\Minicoda4 and C:\Users\didi\.conda\envs and C:\Users\didi\anaconda3 (envs\*\python.exe), and any venv used for
        The Drum Master training (D:\dm-data). Test each with:
        <python> -c "import torch;print(torch.__version__, torch.cuda.is_available())"
        Use the first one that prints True. Write its full path to NT\gemini\python.txt. If none prints True, use one that has torch at all
        and note "CPU only" in timeline.md (the job still works, slower). If no Python has torch: write failed and stop.
STEP 2  In a terminal: cd NT ; set PY=<that python.exe> ; start the job in the background: cmd /c job.bat
        (PowerShell: $env:PY='<path>'; Start-Process cmd -ArgumentList '/c job.bat' -WorkingDirectory NT -WindowStyle Minimized)
        Confirm NT\logs\job.log grows within 2 minutes.
STEP 3  Every 10 min append ONE line to NT\gemini\timeline.md: local time, job.log size, last "== stage" line, last "== step" line,
        logs\job.exit present?, GPU temp + utilisation (nvidia-smi --query-gpu=temperature.gpu,utilization.gpu --format=csv,noheader).
        Write "HOT" if above 85 C twice in a row.
STEP 4  When NT\logs\job.exit exists: write NT\gemini\report.md = status, start/end time, duration of each "== stage", max GPU temp,
        the python + torch line from job.log, the full text of NT\run_compare.txt, and the last 30 lines of job.log.
        Then write NT\gemini\status.txt: "done: <one sentence>" or "failed: <step> <one sentence>".
