# Reads a stand run folder (Invoke-Dx12Stand / Invoke-Dx12Perf): FPS, GPU clock/power/P-state over the
# FPS window (the last 12 in-game nvidia-smi samples), VRAM peak, the mean of every [gpu] field over
# the run's log lines (the first one skipped: it carries the load) and the last [preset] line.
# Dot-sourced by Invoke-Dx12Perf.ps1 and Show-Dx12Runs.ps1.

# Numbers print with a decimal point whatever the machine's locale is.
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture

function Get-Dx12RunStats([string]$dir) {
    $name = Split-Path $dir -Leaf
    $stats = [ordered]@{ name = $name; fps = $null; clock = $null; power = $null; pstate = ''; busy = 0; vram = $null
        fields = [ordered]@{}; gpuLines = 0; preset = ''; fatal = $false; ok = $false; smi = $false }
    $log = Join-Path $dir 'log.txt'
    if (-not (Test-Path -LiteralPath $log)) { return [pscustomobject]$stats }

    $fps = Select-String -LiteralPath $log -Pattern '\[qa\] FPS ([0-9.]+)' | Select-Object -Last 1
    if ($fps) { $stats.fps = [double]$fps.Matches[0].Groups[1].Value }
    $stats.fatal = [bool](Select-String -LiteralPath $log -Pattern 'FATAL|device removed' -Quiet)

    # [gpu] frame=.. scene=.. ms (ao=.. ...): every key=number pair, averaged.
    $lines = @(Select-String -LiteralPath $log -Pattern '\[gpu\] (.*)' | Select-Object -Skip 1)
    $stats.gpuLines = $lines.Count
    $sum = [ordered]@{}
    foreach ($l in $lines) {
        foreach ($m in [regex]::Matches($l.Matches[0].Groups[1].Value, '(\w+)=(-?[0-9.]+)')) {
            $k = $m.Groups[1].Value
            $sum[$k] = $(if ($sum.Contains($k)) { $sum[$k] } else { 0.0 }) + [double]$m.Groups[2].Value
        }
    }
    foreach ($k in $sum.Keys) { $stats.fields[$k] = $sum[$k] / [math]::Max(1, $lines.Count) }

    $preset = Select-String -LiteralPath $log -Pattern '\[preset\] (.*)' | Select-Object -Last 1
    if ($preset) { $stats.preset = $preset.Matches[0].Groups[1].Value.Trim() }

    # smi.csv: timestamp, pstate, clocks.gr, clocks.mem, power.draw, utilization.gpu, temperature, memory.used
    $smi = Join-Path $dir 'smi.csv'
    if (Test-Path -LiteralPath $smi) {
        $stats.smi = $true
        $rows = Get-Content -LiteralPath $smi | ForEach-Object {
            $p = $_ -split ',\s*'
            if ($p.Count -ge 6 -and $p[2] -match '^[0-9]') {
                [pscustomobject]@{ pstate = $p[1]; gr = [int]($p[2] -replace ' MHz'); pw = [double]($p[4] -replace ' W')
                    util = [int]($p[5] -replace ' %') }
            }
        }
        # The clock of the FPS window: the last 12 in-game samples at any utilisation (a variant with less
        # GPU work idles more, and a busy-only filter would count against it); busy samples as information.
        $window = @($rows | Where-Object { $_.util -ge 30 } | Select-Object -Last 12)
        if ($window) {
            $stats.busy = @($rows | Where-Object { $_.util -ge 85 }).Count
            $stats.clock = ($window | Measure-Object gr -Average).Average
            $stats.power = ($window | Measure-Object pw -Average).Average
            $stats.pstate = ($window | Group-Object pstate | Sort-Object Count -Descending | Select-Object -First 1).Name
        }
    }
    $vram = Join-Path $dir 'vram.txt'
    if ((Test-Path -LiteralPath $vram) -and ((Get-Content -LiteralPath $vram -TotalCount 1) -match 'peak dedicated (\d+) MB')) {
        $stats.vram = [int]$Matches[1]
    }
    # threads.txt (Measure-StandThreads): CPU ms of the main thread and of the process over the window
    # before the exit, per frame at the run's FPS.
    $stats.main = $null; $stats.cpu = $null
    $threads = Join-Path $dir 'threads.txt'
    if ($stats.fps -and (Test-Path -LiteralPath $threads) -and
        ((Get-Content -LiteralPath $threads -TotalCount 1) -replace ',', '.' -match 'window ([0-9.]+) s main ([0-9.]+) ms .* process ([0-9.]+) ms')) {
        $frames = $stats.fps * [double]$Matches[1]
        if ($frames -gt 0) {
            $stats.main = [double]$Matches[2] / $frames
            $stats.cpu = [double]$Matches[3] / $frames
        }
    }
    # Level load (texture_load runs): the load phases through the one that ends after "Final texture
    # upload", the early batch wall, the final upload call, the geometry files, sync loads in play.
    $stats.load = $null
    $text = Get-Content -LiteralPath $log
    $final = ($text | Select-String -Pattern '^\* Final texture upload' | Select-Object -First 1)
    if ($final) {
        $load = [ordered]@{ phases = 0.0; batch = $null; call = $null; geom = 0.0; geomPath = ''; p95 = ''; failed = 0 }
        $seenFinal = $false
        for ($i = 0; $i -lt $text.Count; $i++) {
            $l = $text[$i]
            if ($l -match '^\* phase time: (\d+) ms') {
                $load.phases += [double]$Matches[1]
                if ($seenFinal) { break }
            }
            elseif ($l -match '^\* Final texture upload') { $seenFinal = $true }
        }
        $m = $text | Select-String -Pattern '\[texload\] batch early: .* wall (\d+) ms' | Select-Object -First 1
        if ($m) { $load.batch = [double]$m.Matches[0].Groups[1].Value }
        $m = $text | Select-String -Pattern '\[texload\] final upload call (\d+) ms' | Select-Object -First 1
        if ($m) { $load.call = [double]$m.Matches[0].Groups[1].Value }
        foreach ($g in $text | Select-String -Pattern '\[texload\] geometry level\.geomx?: .*?(\d+) ms \((\w+)\)' | Select-Object -First 2) {
            $load.geom += [double]$g.Matches[0].Groups[1].Value
            $load.geomPath = $g.Matches[0].Groups[2].Value
        }
        # The last "in game so far" line: p95 per path.
        $m = $text | Select-String -Pattern '\[texload\] in game so far: (.*)' | Select-Object -Last 1
        if ($m) {
            $load.p95 = (([regex]::Matches($m.Matches[0].Groups[1].Value, '(cpu|ds raw|ds lzo) \d+: [0-9.]+ ms, max [0-9.]+, p95 ([0-9.]+)') |
                ForEach-Object { '{0} {1}' -f ($_.Groups[1].Value -replace ' ', '_'), $_.Groups[2].Value }) -join ' ')
            if ($m.Matches[0].Groups[1].Value -match 'failed (\d+)') { $load.failed = [int]$Matches[1] }
        }
        $stats.load = [pscustomobject]$load
    }
    $stats.ok = $null -ne $stats.fps -and -not $stats.fatal
    [pscustomobject]$stats
}

function Format-Dx12RunStats($s) {
    $clk = if ($s.clock) { '{0,5:f0} MHz {1,3:f0} W {2}' -f $s.clock, $s.power, $s.pstate } else { '(no busy GPU samples)' }
    $fields = ($s.fields.Keys | ForEach-Object { '{0}={1:n3}' -f $_, $s.fields[$_] }) -join ' '
    $flag = if (-not $s.ok) { ' !! no FPS or FATAL' } elseif ($s.smi -and $s.busy -lt 3) { ' !! GPU hardly busy (display asleep?)' } else { '' }
    $cpu = if ($s.main) { ' | cpu main {0:n2} process {1:n2} ms/frame' -f $s.main, $s.cpu } else { '' }
    $load = if ($s.load) {
        ' | load {0:f0} ms, batch {1}, call {2}, geom {3:f0} ms ({4}), play p95 {5}, failed {6}' -f $s.load.phases, $s.load.batch,
            $s.load.call, $s.load.geom, $s.load.geomPath, $s.load.p95, $s.load.failed
    } else { '' }
    '{0,-34} FPS {1,6} | {2,-24} | VRAM {3,5} | {4}{5}{6}{7}' -f $s.name, $(if ($null -ne $s.fps) { '{0:n1}' -f $s.fps } else { '-' }),
        $clk, $(if ($s.vram) { $s.vram } else { '-' }), $fields, $cpu, $load, $flag
}
