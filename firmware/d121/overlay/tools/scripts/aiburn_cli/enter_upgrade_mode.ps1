# enter_upgrade_mode.ps1
#
# Resolve the board's serial port AUTOMATICALLY, then put the board into upgrade
# mode from its *running* state, so that `scons --aicupg` (VSCode F8 / the
# extension's "Flash Image" button) works as a one-shot action -- and keeps
# working after the USB adapter is replugged into a different port.
#
# Why the port must be resolved here:
#   A Windows COM number is assigned per USB port, so moving the cable to another
#   USB socket changes COM6 -> COM7 etc.  Hard-coding it in burn.cfg would break
#   silently.  Instead we enumerate the serial ports, prefer real USB-serial
#   bridges (WCH / CP210x / FTDI / Prolific ...) over Bluetooth virtual ports,
#   and then verify by asking for the RT-Thread shell prompt.
#
# Why the board must be restarted at all:
#   While the board runs RT-Thread, UART0 carries an msh shell that ECHOES the
#   bytes.  upgcmd's autobaud handshake sends 'C' (0x43) repeatedly; the shell
#   echoes it back, so the host only ever sees "Got 43" and stays in state
#   "Detecting" forever, ending with "No uart device connected".  Sending
#   `aicupg gotobl` to that shell makes the board set a WRI/RTC reboot reason and
#   restart straight into BootLoader upgrade mode.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File enter_upgrade_mode.ps1 `
#              -Port auto -Baud 115200 -EnterCmd "aicupg gotobl"
#
# Parameters:
#   -Port        'auto' (default) = detect, or a fixed 'COMx' to try first
#   -EnterCmd    shell command to enter upgrade mode
#   -DetectOnly  only resolve and open the port; send nothing to the board
#
# Machine-readable output (parsed by aic_build.py):
#   UPG_PORT=<COMx>      the port that was successfully opened
#   UPG_STATE=<shell|upgrade-mode>
#
# Exit codes:
#   0 = port resolved and usable, safe to continue with the burn
#   2 = candidates exist but every one of them is busy (serial monitor holding it)
#   3 = no usable serial port found at all

param(
    [string]$Port = 'auto',
    [int]$Baud = 115200,
    [string]$EnterCmd = 'aicupg gotobl',
    [int]$SettleMs = 800,
    [switch]$DetectOnly
)
throw 'Historical experiment disabled. Use work/scripts/serial/board_cmd.ps1, work/scripts/sd/sd_check.ps1 or work/scripts/flash/d12x_flash.ps1. See work/scripts/README.md.'


# USB-serial bridge vendor IDs: WCH, Silicon Labs, FTDI, Prolific, Arduino, Espressif
$preferredVids = @('1A86', '10C4', '0403', '067B', '2341', '303A')

function Get-SerialCandidates {
    $list = @()
    foreach ($dev in (Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue)) {
        if ($dev.FriendlyName -notmatch '\((COM\d+)\)') { continue }
        $com = $Matches[1]
        $vid = ''
        if ($dev.InstanceId -match 'VID_([0-9A-Fa-f]{4})') { $vid = $Matches[1].ToUpper() }
        $isBluetooth = ($dev.InstanceId -match 'BTHENUM')
        $isPreferred = ($preferredVids -contains $vid) -and (-not $isBluetooth)
        $list += New-Object psobject -Property @{
            Com  = $com
            Vid  = $vid
            Name = $dev.FriendlyName
            Rank = $(if ($isBluetooth) { 2 } elseif ($isPreferred) { 0 } else { 1 })
        }
    }
    # stable order: preferred bridges first, then other real ports, bluetooth last
    return @($list | Sort-Object Rank, Com)
}

function Test-ShellPrompt($sp) {
    # Probe a few times: right after a burn the board is still booting, and a
    # single probe would wrongly conclude "already in upgrade mode".
    for ($try = 0; $try -lt 3; $try++) {
        $sp.DiscardInBuffer()
        $sp.Write("`r`n")
        Start-Sleep -Milliseconds 700
        if ($sp.ReadExisting() -match '/>') { return $true }
    }
    return $false
}

$candidates = Get-SerialCandidates

# An explicitly configured port is tried first, but never replaces the fallback
# list -- that is what makes replugging to another USB socket a non-issue.
$attempts = @()
if ($Port -and $Port -ne 'auto') {
    $attempts += $Port
    Write-Output "[enter-upgrade] configured port $Port will be tried first"
}
$attempts += @($candidates | ForEach-Object { $_.Com })
$attempts = @($attempts | Where-Object { $_ } | Select-Object -Unique)

if ($attempts.Count -eq 0) {
    Write-Output "[enter-upgrade] no serial port found on this machine."
    Write-Output "[enter-upgrade] check that the board is plugged in and powered."
    exit 3
}

$busy = @()
foreach ($com in $attempts) {
    $sp = $null
    try {
        $sp = New-Object System.IO.Ports.SerialPort $com, $Baud, 'None', 8, 'One'
        $sp.ReadTimeout  = 500
        $sp.WriteTimeout = 1500
        $sp.Open()
    }
    catch {
        Write-Output "[enter-upgrade] $com : cannot open ($($_.Exception.Message.Trim()))"
        $busy += $com
        if ($sp) { try { $sp.Dispose() } catch { } }
        continue
    }

    try {
        Start-Sleep -Milliseconds 300
        $isShell = Test-ShellPrompt $sp

        if ($DetectOnly) {
            Write-Output "[enter-upgrade] $com : opened (detect-only, nothing sent to the board)"
        }
        elseif (-not $isShell) {
            Write-Output "[enter-upgrade] $com : no shell prompt -- board looks already in upgrade mode"
        }
        else {
            Write-Output "[enter-upgrade] $com : RT-Thread shell found, sending: $EnterCmd"
            $sp.Write($EnterCmd + [char]13)
            Start-Sleep -Milliseconds ($SettleMs + 700)
            $resp = $sp.ReadExisting()
            if ($resp -match 'Restarting|reset|reboot') {
                Write-Output "[enter-upgrade] board is restarting into upgrade mode."
            }
            else {
                Write-Output "[enter-upgrade] command sent, board should be rebooting now."
            }
        }

        if ($isShell) { Write-Output "UPG_STATE=shell" } else { Write-Output "UPG_STATE=upgrade-mode" }
        Write-Output "UPG_PORT=$com"
        $sp.Close()
        exit 0
    }
    catch {
        Write-Output "[enter-upgrade] $com : warning: $($_.Exception.Message)"
        if ($sp) { try { $sp.Close() } catch { } }
    }
}

if ($busy.Count -gt 0) {
    Write-Output "[enter-upgrade] every candidate serial port is busy: $($busy -join ', ')"
    Write-Output "[enter-upgrade] hint: disconnect VSCode Serial Monitor / any serial assistant."
    exit 2
}
exit 3
