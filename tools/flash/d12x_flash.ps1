param(
    [Parameter(Mandatory=$true)][string]$Image,
    [string]$Port='COM6',
    [ValidateRange(115200,3000000)][int]$Baud=921600,
    [Parameter(Mandatory=$true)][string]$Tool,
    [ValidateSet('image.target.os','image.target.rodata','image.target.spl')]
    [string[]]$Components=@('image.target.os','image.target.rodata'),
    [string]$LogDirectory=(Join-Path $PSScriptRoot '..\..\output\flash'),
    [switch]$NoGotobl,
    [switch]$CheckOnly
)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot '..\serial\board.psm1') -Force
if($Port -match '^[1-9][0-9]*$'){$Port='COM'+$Port}
if($Port -notmatch '^COM[1-9][0-9]*$'){throw 'Port must be COM followed by its number'}
$Image=(Resolve-Path -LiteralPath $Image).Path
$Tool=(Resolve-Path -LiteralPath $Tool).Path
New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null
$LogFile=Join-Path $LogDirectory ('d12x_'+(Get-Date -Format 'yyyyMMdd_HHmmss')+'.log')
function Run-Upgrade([string[]]$Arguments,[int]$TimeoutMs=180000) {
    # Quote Windows argv, including paths with spaces and trailing backslashes.
    $quoted=foreach($arg in $Arguments){'"'+([regex]::Replace([regex]::Replace($arg,'(\\*)"','$1$1\"'),'(\\+)$','$1$1'))+'"'}
    $start=[System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$Tool;$start.Arguments=$quoted -join ' '
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $process=[System.Diagnostics.Process]::new();$process.StartInfo=$start
    try {
        if(!$process.Start()){throw 'Could not launch upgrade tool'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit($TimeoutMs)){$process.Kill();throw 'Upgrade tool timed out'}
        $output=$stdout.Result+$stderr.Result
        Add-Content -LiteralPath $LogFile -Value (($Arguments -join ' ')+"`n"+$output) -Encoding UTF8
        [PSCustomObject]@{Code=$process.ExitCode;Output=$output}
    } finally {$process.Dispose()}
}
$info=Run-Upgrade @('-i',$Image)
if($info.Code -ne 0){throw 'Image inspection failed'}
foreach($component in $Components){
    if($info.Output -notmatch [regex]::Escape($component)){throw "Missing component: $component"}
}
$targets=@([regex]::Matches($info.Output,'image\.target\.[a-zA-Z0-9_]+') | ForEach-Object {$_.Value} | Sort-Object -Unique)
if(@(Compare-Object $targets ($Components | Sort-Object -Unique)).Count){throw 'Use an image packaged with exactly the selected target partitions; this tool cannot filter partitions at burn time'}
Get-FileHash -LiteralPath $Image | Format-List | Out-File -LiteralPath ($LogFile+'.sha256')
if($CheckOnly){Write-Output "Validated image and selected components: $($Components -join ', ')
Log: $LogFile";return}
if(!$NoGotobl){
    $serial=Open-BoardPort -Port $Port
    try {
        $state=Invoke-BoardCommand $serial 'bbstat'
        Add-Content -LiteralPath $LogFile -Value ($state.Initial+$state.Output) -Encoding UTF8
        if(!$state.Completed -or $state.Output -notmatch 'running [01] \| backend'){throw 'Port did not identify the D12x blackbox application'}
        $stop=Invoke-BoardCommand $serial 'bb stop'
        $state=Invoke-BoardCommand $serial 'bbstat'
        Add-Content -LiteralPath $LogFile -Value ($stop.Initial+$stop.Output+$state.Output) -Encoding UTF8
        if(!$stop.Completed -or !$state.Completed -or $state.Output -notmatch 'running 0 \|'){throw 'Capture did not stop; files may still be open'}
        $serial.Write("aicupg gotobl`r")
        Start-Sleep -Milliseconds 1500
    } finally {$serial.Dispose()}
    Start-Sleep -Seconds 3
}
$probe=Run-Upgrade @('-u',$Port,'-b',"$Baud",'readl','0x19010218')
if($probe.Code -ne 0 -or $probe.Output -notmatch '19010218:\s+([0-9a-fA-F]{8})'){throw 'Upgrade agent did not respond'}
$mark=[Convert]::ToInt32($Matches[1].Substring(6,2),16)
if($mark -ne 4){throw "Unexpected chip marker $mark; expected D121BBV (4)"}
$burn=Run-Upgrade @('-u',$Port,'-b',"$Baud",'-p','image',$Image) 600000
Write-Output $burn.Output
if($burn.Code -ne 0 -or $burn.Output -notmatch 'Burn.*successfully'){throw "Burn failed; log: $LogFile"}
Start-Sleep -Seconds 5
$serial=Open-BoardPort -Port $Port
try {
    $state=Invoke-BoardCommand $serial 'bbstat' -TimeoutMs 15000
    Add-Content -LiteralPath $LogFile -Value ($state.Initial+$state.Output) -Encoding UTF8
    if(!$state.Completed -or $state.Output -notmatch 'running [01] \| backend'){throw 'Image written but application startup was not confirmed'}
    Write-Output $state.Output
} finally {$serial.Dispose()}
Write-Output "Burn and application startup verified. Log: $LogFile"
