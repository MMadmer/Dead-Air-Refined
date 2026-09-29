# A/B performance batch on the stand (PLAN.md 0.5). Holds ninja.lock for the whole batch (no compile
# shares the CPU, bin cannot change mid-batch), stages the binaries and the overlay once, runs the
# variants interleaved a,b,a,b,... (or a,b,c,a,b,c,...) through Invoke-Dx12Stand with nvidia-smi, VRAM
# and thread-CPU sampling, and prints the table. The first round is the warm-up; a pair counts only
# when the busy GPU clocks of its two runs agree within 3 %.
#
#   tools\qa\dx12\Invoke-Dx12Perf.ps1 -Name vrs-v7-perf-pedro -Save qa_pedro `
#       -A off -APre 'r__vrs 0' -B on -BPre 'r__vrs 1'
#   tools\qa\dx12\Invoke-Dx12Perf.ps1 -Name vrs-v7-perf-pedro -AnalyzeOnly
#
# More than two variants (from PowerShell): -Variants takes hashtables with key, args, pre, globals and,
# per variant, bin / overlay / modules (a snapshot such as the Gate-0 baseline). Every variant is
# compared with the first; -Compare 'on:vrs2' adds further pairs. One shared baseline instead of a
# batch per question keeps the run count down (PLAN.md 0.1a):
#
#   Invoke-Dx12Perf.ps1 -Name vrs-final-pedro -Variants @(
#       @{ key = 'off'; pre = 'r__vrs 0' }, @{ key = 'on'; pre = 'r__vrs 1' },
#       @{ key = 'vrs2'; pre = 'r__vrs 2' }) -Compare 'on:vrs2'
#
# Protocol: 2560x1440, _preset Extreme, TAA, no v-sync, no upscaler or frame generation, r__gpu_log 300,
# qa_fps_ms = 12000; a variant's own pre-commands and globals come after these. Only relative A/B
# inside one batch is valid: the stand's presents are capped and absolute numbers are not the user's.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Name,
    [string]$Save = 'qa_pedro',
    [string]$A = 'a',
    [string]$AArgs = '-r5 -always_active',
    [string[]]$APre = @(),
    [string]$AGlobals = '',
    [string]$B = 'b',
    [string]$BArgs = '-r5 -always_active',
    [string[]]$BPre = @(),
    [string]$BGlobals = '',
    # Replaces A/B when given: @{ key; args; pre; globals; bin; overlay; modules }.
    [object[]]$Variants = @(),
    # Pairs besides "each variant against the first": 'x:y' compares y with x.
    [string[]]$Compare = @(),
    # Rounds including the warm-up round; at most 8 runs per two-variant batch.
    [ValidateRange(2, 6)][int]$Pairs = 3,
    [string]$Bin = '',
    [string]$Overlay = '',
    [string]$Resolution = '2560x1440',
    [string]$Scenario = 'render_check',
    [string]$Weather = 'af3_bright_clear_4',
    [int]$FpsMs = 12000,
    [int]$Seconds = 150,
    [string[]]$ExtraModules = @(),
    # The caller holds ninja.lock already (a whole perf phase under one hold).
    [switch]$NinjaHeld,
    [switch]$AnalyzeOnly,
    [string]$Out = 'D:\Games\Dead Air\_work\dx12-features-20260928\qa'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Dx12Locks.ps1')
# 'a;b' is two commands: pwsh -File from another shell hands a string[] over as one string.
function Split-Commands($list) { @($list | ForEach-Object { "$_" -split ';' } | ForEach-Object Trim | Where-Object { $_ }) }
. (Join-Path $PSScriptRoot 'Dx12RunStats.ps1')
. (Join-Path $PSScriptRoot 'Measure-ForeignGpu.ps1')
# The user may work on the machine meanwhile (an editor, a game): a run starts only once no other
# program loads the discrete GPU, and a pair whose runs saw such load does not count.
$ForeignLimit = 5.0
function Wait-GpuQuiet([string]$run) {
    $deadline = (Get-Date).AddMinutes(20)
    $quiet = 0
    while ($quiet -lt 3 -and (Get-Date) -lt $deadline) {
        $load = Get-ForeignGpuLoad
        if ($load -lt $ForeignLimit) { $quiet++ } else { $quiet = 0; Write-Host "$run waits: another program loads the GPU ($load %)"; Start-Sleep -Seconds 20 }
    }
}
$repo = $script:Dx12Repo
if (-not $Variants.Count) {
    $Variants = @(
        @{ key = $A; args = $AArgs; pre = $APre; globals = $AGlobals },
        @{ key = $B; args = $BArgs; pre = $BPre; globals = $BGlobals })
}
$variantList = @(foreach ($v in $Variants) {
    [pscustomobject]@{
        key = [string]$v.key
        args = if ($v.args) { [string]$v.args } else { '-r5 -always_active' }
        pre = Split-Commands $v.pre
        globals = if ($v.globals) { [string]$v.globals } else { '' }
        bin = if ($v.bin) { [string]$v.bin } elseif ($Bin) { $Bin } else { Join-Path $repo 'bin\x64\Release' }
        overlay = if ($v.overlay) { [string]$v.overlay } elseif ($Overlay) { $Overlay } else { Join-Path $repo 'packaging\dead-air-x64\compatibility\gamedata' }
        modules = @($ExtraModules) + @($v.modules | Where-Object { $_ })
    }
})
if (($variantList.key | Sort-Object -Unique).Count -ne $variantList.Count) { throw 'the variants need different names' }
$first = $variantList[0].key
$pairsToCompare = @($variantList | Select-Object -Skip 1 | ForEach-Object { , @($first, $_.key) }) +
    @($Compare | ForEach-Object { , @($_ -split ':') })
$base = @("vid_mode $Resolution (60Hz)", '_preset Extreme', 'r__aa st_aa_taa', 'rs_v_sync off', 'r__fsr st_fsr_off',
    'r__dlss st_dlss_off', 'r__framegen st_fg_off', 'r__gpu_log 300')
$runName = { param($v, $p) "$Name-$v-$p" }

if (-not $AnalyzeOnly) {
    $stage = Join-Path $env:TEMP ('dx12perf-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $ninja = if ($NinjaHeld) { $null } else { Enter-Lock 'ninja' }
    try {
        # Binaries and overlays frozen once for the batch; variants that share a source share a copy.
        $binCopies = @{}; $overlayCopies = @{}
        foreach ($v in $variantList) {
            $binKey = "$($v.bin)|$($v.modules -join ',')"
            if (-not $binCopies.ContainsKey($binKey)) {
                $binDir = Join-Path $stage "bin$($binCopies.Count)"
                New-Item -ItemType Directory -Force -Path $binDir | Out-Null
                foreach ($m in @('xrRender_R4', 'xrRender_R5', 'xrCore') + $v.modules) {
                    foreach ($ext in '.dll', '.pdb') {
                        $f = Join-Path $v.bin "$m$ext"
                        if (Test-Path -LiteralPath $f) { Copy-Item -LiteralPath $f -Destination $binDir -Force }
                    }
                }
                foreach ($r in 'dxcompiler.dll', 'dxil.dll', 'dstorage.dll', 'dstoragecore.dll') {
                    $f = Join-Path $v.bin $r
                    if (Test-Path -LiteralPath $f) { Copy-Item -LiteralPath $f -Destination $binDir -Force }
                }
                $binCopies[$binKey] = $binDir
            }
            $v | Add-Member -NotePropertyName binDir -NotePropertyValue $binCopies[$binKey] -Force
            if (-not $overlayCopies.ContainsKey($v.overlay)) {
                $overlayDir = Join-Path $stage "gamedata$($overlayCopies.Count)"
                robocopy $v.overlay $overlayDir /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
                $overlayCopies[$v.overlay] = $overlayDir
            }
            $v | Add-Member -NotePropertyName overlayDir -NotePropertyValue $overlayCopies[$v.overlay] -Force
        }

        for ($p = 0; $p -lt $Pairs; $p++) {
            foreach ($v in $variantList) {
                $run = & $runName $v.key $p
                $dir = Join-Path $Out $run
                New-Item -ItemType Directory -Force -Path $Out | Out-Null
                $smiFile = Join-Path $Out "$run-smi.csv"
                $vramFile = Join-Path $Out "$run-vram.txt"
                $threadFile = Join-Path $Out "$run-threads.txt"
                Wait-GpuQuiet $run
                $foreignFile = Join-Path $Out "$run-foreign.txt"
                $foreign = Start-Process -FilePath pwsh -NoNewWindow -PassThru -ArgumentList '-NoProfile', '-File',
                    "`"$(Join-Path $PSScriptRoot 'Measure-ForeignGpu.ps1')`"", '-OutFile', "`"$foreignFile`"", '-MaxSeconds', ($Seconds + 200)
                $smi = Start-Process -FilePath nvidia-smi -NoNewWindow -PassThru -RedirectStandardOutput $smiFile -ArgumentList `
                    '--query-gpu=timestamp,pstate,clocks.gr,clocks.mem,power.draw,utilization.gpu,temperature.gpu,memory.used',
                    '--format=csv,noheader', '-l', '1'
                $vram = Start-Process -FilePath pwsh -NoNewWindow -PassThru -ArgumentList '-NoProfile', '-File',
                    "`"$(Join-Path $PSScriptRoot 'Measure-StandVram.ps1')`"", '-OutFile', "`"$vramFile`"", '-MaxSeconds', ($Seconds + 200)
                $cpu = Start-Process -FilePath pwsh -NoNewWindow -PassThru -ArgumentList '-NoProfile', '-File',
                    "`"$(Join-Path $PSScriptRoot 'Measure-StandThreads.ps1')`"", '-OutFile', "`"$threadFile`"", '-MaxSeconds', ($Seconds + 200)
                try {
                    & (Join-Path $PSScriptRoot 'Invoke-Dx12Stand.ps1') -Name $run -Scenario $Scenario -Save $Save -Weather $Weather `
                        -Seconds $Seconds -ExtraArgs $v.args -PreCommands ($base + $v.pre) `
                        -Globals ("qa_fps_ms = $FpsMs`n" + $v.globals) -Bin $v.binDir -Overlay $v.overlayDir -NinjaHeld `
                        -ExtraModules $v.modules -Out $Out |
                        ForEach-Object { Write-Host $_ }
                } finally {
                    Stop-Process -Id $smi.Id -Force -ErrorAction SilentlyContinue
                    foreach ($sampler in $vram, $foreign, $cpu) {
                        $sampler.WaitForExit(30000) | Out-Null
                        if (-not $sampler.HasExited) { Stop-Process -Id $sampler.Id -Force -ErrorAction SilentlyContinue }
                    }
                    Start-Sleep -Milliseconds 500
                    if (Test-Path -LiteralPath $dir) {
                        foreach ($f in @(@($smiFile, 'smi.csv'), @($vramFile, 'vram.txt'), @($foreignFile, 'foreign.txt'), @($threadFile, 'threads.txt'))) {
                            if (Test-Path -LiteralPath $f[0]) { Move-Item -LiteralPath $f[0] -Destination (Join-Path $dir $f[1]) -Force }
                        }
                    }
                }
            }
        }
    } finally {
        Exit-Lock $ninja
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ---- analysis ----
$report = [Collections.Generic.List[string]]::new()
$report.Add("batch $Name  save $Save  scenario $Scenario")
foreach ($v in $variantList) {
    $src = if ($v.bin -ne (Join-Path $repo 'bin\x64\Release')) { "; bin $($v.bin)" } else { '' }
    $report.Add("  $($v.key): $($v.args); $($v.pre -join ' | ')$src$(if ($v.globals) { "; $($v.globals -replace "`n", ' ; ')" })")
}
$stats = @{}
for ($p = 0; $p -lt $Pairs; $p++) {
    foreach ($v in $variantList) {
        $run = & $runName $v.key $p
        $s = Get-Dx12RunStats (Join-Path $Out $run)
        $stats["$($v.key)|$p"] = $s
        $report.Add((Format-Dx12RunStats $s) + $(if ($p -eq 0) { '  (warm-up)' } else { '' }))
    }
}
$presets = @($stats.Values | Where-Object preset | ForEach-Object preset | Sort-Object -Unique)
foreach ($pr in $presets) { $report.Add("[preset] $pr") }

function Get-ForeignMean([string]$dir) {
    $f = Join-Path $dir 'foreign.txt'
    if ((Test-Path -LiteralPath $f) -and ((Get-Content -LiteralPath $f -TotalCount 1) -match 'mean ([0-9.]+)')) { [double]$Matches[1] } else { 0.0 }
}
function Get-Mean($values) { $x = @($values | Where-Object { $null -ne $_ }); if ($x.Count) { ($x | Measure-Object -Average).Average } else { $null } }

# Per variant, every measured round whatever the clocks did (information for the pairs below).
foreach ($v in $variantList) {
    $runs = @(for ($p = 1; $p -lt $Pairs; $p++) { $stats["$($v.key)|$p"] }) | Where-Object ok
    if (-not $runs) { $report.Add("$($v.key): no measured run"); continue }
    $frame = Get-Mean ($runs | ForEach-Object { $_.fields['frame'] })
    $norm = Get-Mean ($runs | Where-Object { $_.clock -and $_.fields.Contains('frame') } | ForEach-Object { $_.fields['frame'] * $_.clock / 1300 })
    $report.Add(('{0}: mean of {1} run(s): FPS {2:n1}, frame {3:n3} ms, at 1300 MHz {4}, clock {5:f0} MHz, cpu main {6}, process {7} ms/frame, VRAM peak {8} MB' -f
        $v.key, $runs.Count, (Get-Mean $runs.fps), $frame, $(if ($null -ne $norm) { '{0:n3} ms' -f $norm } else { '-' }),
        (Get-Mean $runs.clock), $(if ($null -ne ($m = Get-Mean $runs.main)) { '{0:n2}' -f $m } else { '-' }),
        $(if ($null -ne ($c = Get-Mean $runs.cpu)) { '{0:n2}' -f $c } else { '-' }), (($runs.vram | Measure-Object -Maximum).Maximum)))
}

# Pairs of rounds 1..n: valid when both runs have FPS, busy clocks within 3 % and no other program on the GPU.
foreach ($cmp in $pairsToCompare) {
    $ka, $kb = $cmp
    $valid = @()
    for ($p = 1; $p -lt $Pairs; $p++) {
        $sa = $stats["$ka|$p"]; $sb = $stats["$kb|$p"]
        $why = if (-not ($sa -and $sb)) { 'no such runs' }
            elseif (-not ($sa.ok -and $sb.ok)) { 'a run failed' }
            elseif (-not ($sa.clock -and $sb.clock)) { 'no busy clock samples' }
            elseif ([math]::Abs($sa.clock - $sb.clock) / [math]::Max($sa.clock, $sb.clock) -gt 0.03) {
                'clocks {0:f0} vs {1:f0} MHz' -f $sa.clock, $sb.clock }
            elseif (($fa = Get-ForeignMean (Join-Path $Out (& $runName $ka $p))) -gt $ForeignLimit -or
                ($fb = Get-ForeignMean (Join-Path $Out (& $runName $kb $p))) -gt $ForeignLimit) {
                'another program loaded the GPU ({0:f1} / {1:f1} %)' -f $fa, $fb }
            else { '' }
        $report.Add("$ka vs $kb pair $p " + $(if ($why) { "invalid ($why)" } else { 'valid' }))
        if (-not $why) { $valid += , @($sa, $sb) }
    }
    if ($valid.Count) {
        $dFps = ($valid | ForEach-Object { ($_[1].fps - $_[0].fps) / $_[0].fps * 100 } | Measure-Object -Average).Average
        $aFps = ($valid | ForEach-Object { $_[0].fps } | Measure-Object -Average).Average
        $bFps = ($valid | ForEach-Object { $_[1].fps } | Measure-Object -Average).Average
        $report.Add(('{0} FPS {1:n1} -> {2} FPS {3:n1}: {4:+0.00;-0.00} % over {5} valid pair(s)' -f $ka, $aFps, $kb, $bFps, $dFps, $valid.Count))
        $keys = @($valid[0][0].fields.Keys) + @($valid[0][1].fields.Keys) | Select-Object -Unique
        $cols = foreach ($k in $keys) {
            $d = @($valid | Where-Object { $_[0].fields.Contains($k) -and $_[1].fields.Contains($k) } | ForEach-Object { $_[1].fields[$k] - $_[0].fields[$k] })
            $av = @($valid | Where-Object { $_[0].fields.Contains($k) } | ForEach-Object { $_[0].fields[$k] })
            if ($d.Count) { '{0} {1:n3}{2:+0.000;-0.000}' -f $k, ($av | Measure-Object -Average).Average, ($d | Measure-Object -Average).Average }
            else { "$k (only one side)" }
        }
        $report.Add("[gpu] $ka mean and $kb delta (ms): " + ($cols -join ', '))
        $dm = @($valid | Where-Object { $_[0].main -and $_[1].main } | ForEach-Object { $_[1].main - $_[0].main })
        if ($dm.Count) {
            $report.Add(('[cpu] {0} main {1:n2} ms/frame, {2} delta {3:+0.00;-0.00} ms ({4:+0.0;-0.0} %)' -f $ka,
                (Get-Mean ($valid | ForEach-Object { $_[0].main })), $kb, ($dm | Measure-Object -Average).Average,
                (($valid | Where-Object { $_[0].main -and $_[1].main } | ForEach-Object { ($_[1].main - $_[0].main) / $_[0].main * 100 }) | Measure-Object -Average).Average))
        }
    } else {
        $report.Add("$ka vs ${kb}: no valid pair - no A/B verdict from this batch (per-variant means above are information)")
    }
}
$report | ForEach-Object { Write-Output $_ }
$report | Set-Content -LiteralPath (Join-Path $Out "$Name.summary.txt") -Encoding UTF8
