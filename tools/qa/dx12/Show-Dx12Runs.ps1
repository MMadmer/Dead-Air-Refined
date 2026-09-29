# One line per stand run: FPS, busy GPU clock/power, VRAM peak, the mean of every [gpu] field, and
# the distinct [preset] lines (tools\qa\dx12\Dx12RunStats.ps1).
#
#   tools\qa\dx12\Show-Dx12Runs.ps1 core-g0-pedro-r5-a core-g0-pedro-r5-b
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$Names,
    [string]$Dir = 'D:\Games\Dead Air\_work\dx12-features-20260928\qa'
)
. (Join-Path $PSScriptRoot 'Dx12RunStats.ps1')
$all = foreach ($n in $Names | ForEach-Object { $_ -split ',' } | Where-Object { $_ }) { Get-Dx12RunStats (Join-Path $Dir $n) }
$all | ForEach-Object { Format-Dx12RunStats $_ }
$all | Where-Object preset | ForEach-Object preset | Sort-Object -Unique | ForEach-Object { "[preset] $_" }
