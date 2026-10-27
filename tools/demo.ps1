param(
    [ValidateSet('Check','StarFault','NetworkFault')][string]$Action='Check',
    [string]$MasterPort='COM6',
    [string]$StarPort='COM5',
    [switch]$Preview
)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'serial/board.psm1') -Force
$logs=Join-Path (Split-Path $PSScriptRoot -Parent) 'recording-logs'

function Command([string]$port,[string]$cmd){
    $serial=Open-BoardPort -Port $port
    try{
        $r=Invoke-BoardCommand $serial $cmd -TimeoutMs 7000
        $text=$r.Initial+$r.Output
        Write-Host ("`n{0} {1} {2}" -f (Get-Date -Format o),$port,$cmd)
        Write-Host $text
        Add-Content -LiteralPath $script:logFile -Value ((Get-Date -Format o)+" $port $cmd`n"+$text) -Encoding utf8
        if(!$r.Completed){throw "Command did not complete: $port $cmd"}
        return $text
    }finally{$serial.Dispose()}
}
function Status{
    $cloud=Command $MasterPort 'cloud stat'
    $bb=Command $MasterPort 'bbstat'
    $health=Command $MasterPort 'apphealth'
    return @{Cloud=$cloud;Blackbox=$bb;Health=$health}
}
function RequireBaseline($state){
    if($state.Cloud -notmatch '\bonline=1\b' -or $state.Cloud -notmatch 'tls=1 auth_verified=1' -or
       $state.Cloud -notmatch '\bqueued=0\b' -or $state.Blackbox -notmatch 'slave2 online=1 stale=0' -or
       $state.Blackbox -notmatch 'slave ON' -or $state.Blackbox -notmatch 'env_valid 1 \| imu_valid 1' -or
       $state.Health -notmatch 'tripped=0.*bb_ok=1'){
        throw 'Online TLS, empty queue, two healthy nodes and watchdog baseline required'
    }
}
if($Preview){
    Write-Output "Action=$Action Master=$MasterPort Star=$StarPort"
    Write-Output 'Check: cloud stat, bbstat, apphealth (read only)'
    if($Action -eq 'StarFault'){Write-Output 'Stop Star responder for 3 seconds; finally restart at address 1; read status'}
    if($Action -eq 'NetworkFault'){Write-Output 'Pause upload 30 seconds; stop/restart Star responder; display queue; wait up to 75 seconds for authenticated TLS and empty queue'}
    return
}
$null=New-Item -ItemType Directory -Path $logs -Force
$script:logFile=Join-Path $logs ((Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+$Action+'.txt')
$baseline=Status
RequireBaseline $baseline
if($Action -eq 'Check'){
    Write-Host 'PASS current baseline. For recording growth, compare a later Check with this log.'
    return
}
if($Action -eq 'NetworkFault'){
    $null=Command $MasterPort 'cloud pause 30'
    Start-Sleep -Seconds 1
}
$restoreNeeded=$false
try{
    # Set the cleanup flag before issuing a stop that could time out after applying.
    $restoreNeeded=$true
    $null=Command $StarPort 'rs485_slave stop'
    Start-Sleep -Seconds 3
    $null=Command $MasterPort 'bbstat'
}finally{
    if($restoreNeeded){$null=Command $StarPort 'rs485_slave start 1'}
}
Start-Sleep -Seconds 1
$offline=Command $MasterPort 'cloud stat'
if($Action -eq 'NetworkFault'){
    if($offline -notmatch '\bonline=0\b' -or $offline -notmatch '\bqueued=([1-9][0-9]*)\b'){
        throw 'Offline queued event was not observed; retain the log and rehearse again'
    }
    $deadline=[Diagnostics.Stopwatch]::StartNew()
    do{
        Start-Sleep -Seconds 2
        $state=Command $MasterPort 'cloud stat'
        $ready=$state -match '\bonline=1\b' -and $state -match '\bqueued=0\b' -and $state -match 'tls=1 auth_verified=1'
        if($ready){break}
        if($deadline.Elapsed.TotalSeconds -gt 75){throw 'Upload did not recover within the filming deadline'}
    }while($true)
}
$final=Status
RequireBaseline $final
Write-Host 'PASS current recovery checks. Inspect the new phone event and compare storage counters before concluding the video.'
