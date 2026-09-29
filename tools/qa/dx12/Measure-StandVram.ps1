# Samples the dedicated and shared GPU memory of the rig's xrEngine.exe every 2 s until it exits (or
# -MaxSeconds pass) and writes the peaks, the last value and the samples. Started by Invoke-Dx12Perf.
param(
    [Parameter(Mandatory)][string]$OutFile,
    [int]$MaxSeconds = 600,
    [string]$Rig = 'D:\Games\Dead Air\_qa\lightdiag'
)
$deadline = (Get-Date).AddSeconds($MaxSeconds)
$proc = $null
while (-not $proc -and (Get-Date) -lt $deadline) {
    $proc = Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Rig\*" } | Select-Object -First 1
    if (-not $proc) { Start-Sleep -Seconds 2 }
}
if (-not $proc) { 'no stand engine' | Set-Content -LiteralPath $OutFile; return }
$enginePid = $proc.Id
$peakDed = 0; $peakShared = 0; $lastDed = 0; $lastShared = 0
$samples = [Collections.Generic.List[string]]::new()
while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
    try {
        $ded = (Get-Counter "\GPU Process Memory(pid_$($enginePid)_*)\Dedicated Usage" -ErrorAction Stop).CounterSamples | Measure-Object CookedValue -Sum
        $sh = (Get-Counter "\GPU Process Memory(pid_$($enginePid)_*)\Shared Usage" -ErrorAction Stop).CounterSamples | Measure-Object CookedValue -Sum
        $lastDed = [math]::Round($ded.Sum / 1MB); $lastShared = [math]::Round($sh.Sum / 1MB)
        $peakDed = [math]::Max($peakDed, $lastDed); $peakShared = [math]::Max($peakShared, $lastShared)
        $samples.Add("$([math]::Round(((Get-Date) - $proc.StartTime).TotalSeconds)) s: dedicated $lastDed MB, shared $lastShared MB")
    } catch { }
    Start-Sleep -Seconds 2
}
@("peak dedicated $peakDed MB, peak shared $peakShared MB, last dedicated $lastDed MB, last shared $lastShared MB") + $samples |
    Set-Content -LiteralPath $OutFile
