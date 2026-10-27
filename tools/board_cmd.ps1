param([string]$Port='COM6',[string[]]$Commands=@('bbstat'),[string]$Log,[int]$TimeoutMs=10000,[switch]$ExpectDisconnect)
$ErrorActionPreference='Stop'
& (Join-Path $PSScriptRoot 'serial/board_cmd.ps1') @PSBoundParameters
