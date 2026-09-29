# GPU load that other programs put on the stand's GPU: what the user runs meanwhile (an editor viewport,
# a game, a video) adds to a perf run. Samples the 3D and compute engine utilisation of every process on
# the discrete adapter (the one with dedicated memory in use; the integrated GPU the desktop and Electron
# apps draw on is left out), minus the stand's engine and the desktop compositor.
#
#   Measure-ForeignGpu.ps1 -OutFile run\foreign.txt -MaxSeconds 350      samples until the stand engine exits
#   . Measure-ForeignGpu.ps1; Get-ForeignGpuLoad                         one reading, in percent
[CmdletBinding()]
param([string]$OutFile = '', [int]$MaxSeconds = 400)

$script:ForeignLuid = $null

function Get-DiscreteLuid {
    if ($script:ForeignLuid) { return $script:ForeignLuid }
    $mem = (Get-Counter '\GPU Adapter Memory(*)\Dedicated Usage' -ErrorAction SilentlyContinue).CounterSamples |
        Sort-Object CookedValue -Descending | Select-Object -First 1
    if ($mem -and $mem.InstanceName -match 'luid_(0x[0-9a-fA-F]+_0x[0-9a-fA-F]+)') { $script:ForeignLuid = $Matches[1] }
    $script:ForeignLuid
}

function Get-ForeignGpuLoad {
    $luid = Get-DiscreteLuid
    $stand = @(Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*\_qa\*' } | ForEach-Object Id)
    $dwm = @(Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object Id)
    $sum = 0.0
    foreach ($type in '3D', 'Compute') {
        $path = if ($luid) { "\GPU Engine(*luid_${luid}*engtype_$type)\Utilization Percentage" } else { "\GPU Engine(*engtype_$type)\Utilization Percentage" }
        foreach ($s in (Get-Counter $path -ErrorAction SilentlyContinue).CounterSamples) {
            if ($s.InstanceName -notmatch '^pid_(\d+)_') { continue }
            $procId = [int]$Matches[1]
            if ($stand -contains $procId -or $dwm -contains $procId) { continue }
            $sum += $s.CookedValue
        }
    }
    [math]::Round($sum, 1)
}

if ($OutFile) {
    $deadline = (Get-Date).AddSeconds($MaxSeconds)
    $values = [Collections.Generic.List[double]]::new()
    $seen = $false
    while ((Get-Date) -lt $deadline) {
        $engine = Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*\_qa\*' }
        if ($engine) { $seen = $true } elseif ($seen) { break }
        $values.Add((Get-ForeignGpuLoad))
        Start-Sleep -Seconds 1
    }
    $mean = if ($values.Count) { ($values | Measure-Object -Average).Average } else { 0 }
    $max = if ($values.Count) { ($values | Measure-Object -Maximum).Maximum } else { 0 }
    @(('foreign gpu mean {0:f1} % max {1:f1} % over {2} samples' -f $mean, $max, $values.Count)) + ($values | ForEach-Object { '{0:f1}' -f $_ }) |
        Set-Content -LiteralPath $OutFile -Encoding UTF8
}
