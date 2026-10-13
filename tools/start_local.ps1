$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$python=Join-Path $root '.venv\Scripts\python.exe'
if(!(Test-Path -LiteralPath $python)){throw 'Run python -m venv .venv and install requirements first'}
$data=Join-Path $root 'data';New-Item -ItemType Directory -Force -Path $data | Out-Null
try { $health=Invoke-RestMethod -Uri 'http://127.0.0.1:8765/health' -TimeoutSec 2; if($health.service -eq 'd121-monitor'){Write-Output 'D121 local service is already running.';return} } catch {}
$process=Start-Process -FilePath $python -ArgumentList @('-m','uvicorn','backend.api:app','--host','127.0.0.1','--port','8765','--no-access-log') -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $data 'service.stdout.log') -RedirectStandardError (Join-Path $data 'service.stderr.log')
$ready=$false
for($i=0;$i -lt 30;$i++){
 if($process.HasExited){throw 'Local service exited. Check data/service.stderr.log.'}
 try{$result=Invoke-RestMethod -Uri 'http://127.0.0.1:8765/health' -TimeoutSec 1; if($result.service -eq 'd121-monitor'){$ready=$true;break}}catch{}
 Start-Sleep -Milliseconds 300
}
if(!$ready){throw 'Local service did not become ready'}
[IO.File]::WriteAllText((Join-Path $data 'service.pid'),[string]$process.Id)
Write-Output "D121 local service ready: http://127.0.0.1:8765 (PID $($process.Id))"
