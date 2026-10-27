param([string]$MasterPort='COM6',[string]$StarPort='COM5')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
Import-Module (Join-Path $PSScriptRoot 'serial/board.psm1') -Force
$cfg=Get-Content (Join-Path $root 'config.private.json') -Raw | ConvertFrom-Json
$session=Invoke-RestMethod -Uri 'http://127.0.0.1:8765/api/auth/local' -Method Post -ContentType 'application/json' -Body (@{code=$cfg.server.development_token}|ConvertTo-Json)
$headers=@{Authorization='Bearer '+$session.token}
$log=Join-Path $root 'hardware-test.txt'
function Cmd([string]$port,[string]$command){
 $serial=Open-BoardPort -Port $port
 try{$r=Invoke-BoardCommand $serial $command;Add-Content -LiteralPath $log -Value ((Get-Date -Format o)+"`n"+$r.Initial+$r.Output);if(!$r.Completed){throw "No prompt: $port $command"};return $r.Output}finally{$serial.Dispose()}
}
function GetApi([string]$path){Invoke-RestMethod -Uri ('http://127.0.0.1:8765'+$path) -Headers $headers -TimeoutSec 5}
$slaveStopped=$false
try {
 $ready=[Diagnostics.Stopwatch]::StartNew()
 do{$s=GetApi '/api/status';if($s.state -eq 'online' -and $s.latest.nodes[0].online -and $s.latest.nodes[1].online -and $s.diagnostic){break};Start-Sleep -Seconds 1}while($ready.ElapsedMilliseconds -lt 15000)
 if($s.state -ne 'online' -or !$s.latest.nodes[0].online -or !$s.latest.nodes[1].online){throw 'Baseline not healthy'}
 $initialStorage=$s.diagnostic.storage
 $slaveStopped=$true;[void](Cmd $StarPort 'rs485_slave stop');Start-Sleep -Seconds 4
 $s=GetApi '/api/status';if($s.state -ne 'online' -or $s.latest.nodes[0].online -or !$s.latest.nodes[1].online){throw 'Single-node failure was not isolated'}
 $items=(GetApi '/api/events?node=1&limit=10').items
 $incident=$items|Where-Object {$_.title -eq '通信中断'}|Select-Object -First 1
 if(!$incident -or !$incident.snapshot -or $null -eq $incident.snapshot.nodes[0].temperature){throw 'No pre-outage snapshot'}
 $detail=GetApi ('/api/events/'+$incident.id)
 if($detail.available_points -lt 2 -or $detail.diagnosis.confidence -ne '原因尚未确认'){throw 'Pre-outage curve or evidence boundary failed'}
 $detail|ConvertTo-Json -Depth 15|Out-File (Join-Path $root 'data/star-outage.json') -Encoding utf8
 Write-Output 'PASS: actual Star outage, Loongson unaffected, before-data retained, no fabricated cause'
 [void](Cmd $StarPort 'rs485_slave start 1');$slaveStopped=$false;Start-Sleep -Seconds 4
 if(!(GetApi '/api/status').latest.nodes[0].online){throw 'Star did not recover'}
 [void](Cmd $MasterPort 'cloud pause 25');Start-Sleep -Seconds 18
 $s=GetApi '/api/status'
 if($s.state -ne 'offline' -or !$s.collector.connected){throw 'Master uplink outage not detected independently'}
 $items=(GetApi '/api/events?node=0&limit=5').items
 $incident=$items|Where-Object {$_.title -eq '主站上传中断'}|Select-Object -First 1
 if(!$incident -or !$incident.snapshot){throw 'Master outage lost last data'}
 $s|ConvertTo-Json -Depth 15|Out-File (Join-Path $root 'data/master-outage.json') -Encoding utf8
 Write-Output 'PASS: actual uplink pause, cloud observer connected, last snapshot accessible while master offline'
 $watch=[Diagnostics.Stopwatch]::StartNew()
 do {Start-Sleep -Seconds 2;$s=GetApi '/api/status'}while($s.state -ne 'online' -and $watch.ElapsedMilliseconds -lt 45000)
 if($s.state -ne 'online' -or !$s.latest.nodes[0].online -or !$s.latest.nodes[1].online){throw 'Two-node uplink recovery failed'}
 $detail=GetApi ('/api/events/'+$incident.id)
 if(!$detail.resolved){throw 'Outage recovery was not recorded'}
 if(!($detail.diagnosis.observations -match '未观察到主站重新启动')){throw 'Same-boot recovery evidence was not recorded'}
 if($s.diagnostic.storage -ne $initialStorage -or $s.diagnostic.write_errors -ne 0){throw 'Storage changed or failed during the test'}
 [void](Cmd $MasterPort 'bbstat');[void](Cmd $MasterPort 'cloud stat');[void](Cmd $MasterPort 'apphealth')
 Write-Output "PASS: reconnect, same-boot diagnosis and persisted incident resolution; storage=$initialStorage remained active"
 Add-Content -LiteralPath $log -Value 'PASS: hardware cloud history and offline recovery checks'
} finally {
 if($slaveStopped){try{[void](Cmd $StarPort 'rs485_slave start 1')}catch{Write-Warning 'Star restore requires attention'}}
}
