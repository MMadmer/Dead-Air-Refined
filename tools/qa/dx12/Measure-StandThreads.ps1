# Samples the CPU time of the rig's xrEngine.exe once a second until it exits (or -MaxSeconds pass):
# the whole process and every thread. The engine's main thread (game update + render submission, the
# "render thread" of the DirectX 12 feature docs) is the thread that started first. Writes the last
# -WindowSeconds before the exit - the scenario's FPS window ends a fraction of a second before the
# probe stops the engine - as one summary line, then the samples. Started by Invoke-Dx12Perf; the
# per-frame numbers come from Dx12RunStats (window CPU / (FPS x window)).
param(
    [Parameter(Mandatory)][string]$OutFile,
    [int]$MaxSeconds = 600,
    [int]$WindowSeconds = 11,
    [string]$Rig = 'D:\Games\Dead Air\_qa\lightdiag'
)
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$deadline = (Get-Date).AddSeconds($MaxSeconds)
$proc = $null
while (-not $proc -and (Get-Date) -lt $deadline) {
    $proc = Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Rig\*" } | Select-Object -First 1
    if (-not $proc) { Start-Sleep -Milliseconds 500 }
}
if (-not $proc) { 'no stand engine' | Set-Content -LiteralPath $OutFile; return }

# One sample: wall time, process CPU ms, main thread id and CPU ms, the busiest thread's id and CPU ms.
$samples = [Collections.Generic.List[object]]::new()
$mainId = $null
while ((Get-Date) -lt $deadline) {
    try {
        $proc.Refresh()
        if ($proc.HasExited) { break }
        $threads = @($proc.Threads | ForEach-Object {
            try { [pscustomobject]@{ id = $_.Id; start = $_.StartTime; cpu = $_.TotalProcessorTime.TotalMilliseconds } } catch { }
        })
        if (-not $mainId -and $threads) { $mainId = ($threads | Sort-Object start | Select-Object -First 1).id }
        $cpu = @{}
        foreach ($t in $threads) { $cpu[$t.id] = $t.cpu }
        $samples.Add([pscustomobject]@{ t = Get-Date; proc = $proc.TotalProcessorTime.TotalMilliseconds; threads = $cpu })
    } catch { break }
    Start-Sleep -Seconds 1
}
if ($samples.Count -lt 3) { 'too few samples' | Set-Content -LiteralPath $OutFile; return }

$last = $samples[$samples.Count - 1]
$first = $samples[[math]::Max(0, $samples.Count - 1 - $WindowSeconds)]
$span = ($last.t - $first.t).TotalSeconds
$deltas = foreach ($id in $last.threads.Keys) {
    if ($first.threads.ContainsKey($id)) { [pscustomobject]@{ id = $id; ms = $last.threads[$id] - $first.threads[$id] } }
}
$busiest = $deltas | Sort-Object ms -Descending | Select-Object -First 1
$main = $deltas | Where-Object id -eq $mainId | Select-Object -First 1
$line = 'window {0:f2} s main {1:f0} ms (thread {2}) busiest {3:f0} ms (thread {4}) process {5:f0} ms threads {6}' -f $span,
    $(if ($main) { $main.ms } else { 0 }), $mainId, $busiest.ms, $busiest.id, ($last.proc - $first.proc), $last.threads.Count
$body = foreach ($s in $samples) {
    '{0:HH:mm:ss.fff} process {1:f0} main {2:f0}' -f $s.t, $s.proc, $(if ($s.threads.ContainsKey($mainId)) { $s.threads[$mainId] } else { 0 })
}
@($line) + $body | Set-Content -LiteralPath $OutFile
