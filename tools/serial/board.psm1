Set-StrictMode -Version Latest
function Open-BoardPort {
    param([string]$Port='COM6',[int]$Baud=115200)
    if($Port -notmatch '^COM[1-9][0-9]*$'){throw "Invalid port: $Port"}
    $sp=[System.IO.Ports.SerialPort]::new($Port,$Baud,'None',8,'One')
    $sp.DtrEnable=$false;$sp.RtsEnable=$false;$sp.ReadTimeout=100;$sp.WriteTimeout=2000
    try{$sp.Open();return $sp}catch{$sp.Dispose();throw}
}
function Invoke-BoardCommand {
    param([System.IO.Ports.SerialPort]$Serial,[string]$Command,[int]$TimeoutMs=10000,[int]$MaxChars=8388608)
    if($Command -match '[\r\n]'){throw 'Only one shell command is accepted per call'}
    $initial=$Serial.ReadExisting() # Preserve unsolicited/panic output; never silently discard it.
    $Serial.Write($Command+"`r")
    $capture=[System.Text.StringBuilder]::new();$watch=[System.Diagnostics.Stopwatch]::StartNew()
    $lastRead=0L;$prompt=$false
    while($watch.ElapsedMilliseconds -lt $TimeoutMs){
        if($Serial.BytesToRead -gt 0){
            [void]$capture.Append($Serial.ReadExisting());$lastRead=$watch.ElapsedMilliseconds
            if($capture.Length -gt $MaxChars){throw "Output limit exceeded: $Command"}
            # Accept CR-only terminal lines and NUL padding; preserve captured output.
            if($capture.ToString() -match '(?m)(?:^|[\r\n])\x00*(aic />|msh >)[ \t]*'){$prompt=$true}
        }elseif($prompt -and $watch.ElapsedMilliseconds-$lastRead -ge 30){break}
        Start-Sleep -Milliseconds 5
    }
    [PSCustomObject]@{Command=$Command;Initial=$initial;Output=$capture.ToString();Completed=$prompt;ElapsedMs=$watch.ElapsedMilliseconds}
}
Export-ModuleMember -Function Open-BoardPort,Invoke-BoardCommand
