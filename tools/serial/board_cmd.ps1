param([string]$Port='COM6',[string[]]$Commands=@('bbstat'),[string]$Log,[int]$TimeoutMs=10000,[switch]$ExpectDisconnect)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'board.psm1') -Force
$serial=Open-BoardPort -Port $Port
try {
    foreach($cmd in $Commands){
        $result=Invoke-BoardCommand $serial $cmd -TimeoutMs $TimeoutMs
        $output=$result.Initial+$result.Output
        Write-Output $output
        if($Log){Add-Content -LiteralPath $Log -Value $output -Encoding UTF8}
        if(!$result.Completed -and !$ExpectDisconnect){throw "No shell prompt for: $cmd"}
    }
} finally {$serial.Dispose()}
