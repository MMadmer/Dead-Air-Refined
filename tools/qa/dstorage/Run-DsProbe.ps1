# Offline DirectStorage probe (docs/dead-air/dx12-features/dstorage.md 2.4, 9): builds dsprobe.exe with
# cl.exe (no ninja lock needed; when missing or older than its sources), puts the vendored DirectStorage
# runtime next to it and loads the .dds entries of stand archives into D3D12 textures along each
# variant, reading a sample back (verify). Pass: every run exits 0 (no request, codec or verify failure).
#
#   tools\qa\dstorage\Run-DsProbe.ps1                                   # raw HD bundle + stock LZO archive
#   tools\qa\dstorage\Run-DsProbe.ps1 -Archive textures.xdb2 -Variants cpu,ds-placed -Verify 64
#   tools\qa\dstorage\Run-DsProbe.ps1 -Cold                              # unbuffered copies (not in the file cache)
#   tools\qa\dstorage\Run-DsProbe.ps1 -PrefixCheck                       # rtc_decompress_prefix (bin's xrCore.dll,
#                                                                        # ordinal 1519) over every LZO .dds of every
#                                                                        # stand archive; holds ninja.lock, no stand.lock
#
# Variants: cpu (today's engine path), ds (custom codec for every entry), ds-direct (raw entries read
# straight into the textures + tail), ds-placed (ds-direct into placed heaps), dsbuf (ds-placed through
# the OS file cache), dsmem (IO ceiling into memory), gdeflate (re-packed in memory, GPU decompression),
# geom-cpu / geom-ds (level.geom vertex and index buffers: the engine's path / DirectStorage, -Entry <level>).
# Output: build\qa\dstorage\<Name>.txt (all probe lines) and the summary on the console.
[CmdletBinding()]
param(
    [string]$Stand = 'D:\Games\Dead Air\_qa\lightdiag',
    # Archive file names in <Stand>\database (wildcards allowed) or full paths.
    [string[]]$Archive = @('xtra_dead_air_x64_content_textures_02_*.xdb0', 'textures.xdb2'),
    [string[]]$Variants = @('cpu', 'ds', 'ds-direct', 'ds-placed'),
    [int]$Verify = 24,
    [int]$Threads = 4,
    [int]$LimitMb = 700,
    # Copy each archive with robocopy /J (unbuffered) into -ColdDir first; every variant then reads a
    # fresh copy, so no run is served from the file cache. Copies are deleted afterwards.
    [switch]$Cold,
    [string]$ColdDir = 'D:\Games\Dead Air\_work\dx12-features-20260928\ds\cold',
    # Checks xrCore's rtc_decompress_prefix (the export by ordinal of this xrCore.dll) on every LZO entry.
    [switch]$PrefixCheck,
    [string]$XrCore = '',
    [switch]$DebugLayer,
    # geom-cpu / geom-ds: the level.geom entries whose name contains this (e.g. l01_escape).
    [string]$Entry = '',
    # Locks held while probing (Dx12Locks.ps1): the GPU and disk are shared with stand runs, and timed
    # batches (S8) also keep compiles off the CPU. stand | all (ninja + stand) | none.
    [ValidateSet('stand', 'all', 'none')][string]$Lock = 'stand',
    [string]$Name = 'dsprobe',
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
$build = Join-Path $repo 'build\qa\dstorage'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$dsDir = Join-Path $repo 'Externals\DirectStorage'

# dsprobe.exe: cl.exe inside the Visual Studio x64 developer environment.
$source = Join-Path $PSScriptRoot 'dsprobe.cpp'
$inl = Join-Path $repo 'src\xrCore\Compression\lzo1x_decompress_safe_fast.inl'
$exe = Join-Path $build 'dsprobe.exe'
$newest = @($source, $inl | ForEach-Object { (Get-Item -LiteralPath $_).LastWriteTime } | Sort-Object)[-1]
if ($Rebuild -or -not (Test-Path -LiteralPath $exe) -or (Get-Item -LiteralPath $exe).LastWriteTime -lt $newest) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'A Visual Studio installation with the x64 C++ toolchain is required.' }
    $includes = @((Join-Path $dsDir 'include'), (Join-Path $repo 'sdk\include'), (Join-Path $repo 'src\xrCore\Compression')) |
        ForEach-Object { "/I `"$_`"" }
    $script = Join-Path $build 'build_dsprobe.cmd'
    Set-Content -LiteralPath $script -Encoding ASCII -Value @(
        '@set VSLANG=1033',
        "@call `"$(Join-Path $vs 'Common7\Tools\VsDevCmd.bat')`" -arch=x64 -host_arch=x64 >nul 2>nul",
        "@cd /d `"$build`"",
        "cl.exe /nologo /std:c++20 /EHsc /O2 /MD /W4 $($includes -join ' ') /Fe`"$exe`" `"$source`" /link /INCREMENTAL:NO /LIBPATH:`"$(Join-Path $dsDir 'lib\x64')`""
    )
    $log = & cmd.exe /d /c "`"$script`"" 2>&1
    if ($LASTEXITCODE -ne 0) {
        $log | Select-Object -Last 30 | ForEach-Object { Write-Output $_ }
        throw "dsprobe build failed ($LASTEXITCODE)"
    }
    $log | Where-Object { $_ -match ': (warning|error) ' } | ForEach-Object { Write-Output $_ }
    Write-Output "built $exe"
}
# dstorage.dll loads dstoragecore.dll from its own folder.
foreach ($dll in 'dstorage.dll', 'dstoragecore.dll') {
    $from = Join-Path $dsDir "bin\x64\$dll"
    $to = Join-Path $build $dll
    if (-not (Test-Path -LiteralPath $to) -or (Get-FileHash -LiteralPath $to).Hash -ne (Get-FileHash -LiteralPath $from).Hash) {
        Copy-Item -LiteralPath $from -Destination $to -Force
    }
}

# pwsh -File passes "a,b" as one string.
$Archive = @($Archive | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Variants = @($Variants | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$appdata = Join-Path $Stand 'appdata'
# -PrefixCheck without -Archive: every archive of the stand that has a vfs-index cache (matched by size).
if ($PrefixCheck -and -not $PSBoundParameters.ContainsKey('Archive')) {
    $sizes = @{}
    foreach ($c in Get-ChildItem -LiteralPath $appdata -Filter 'vfs-index-*.cache' -File) {
        $head = [byte[]]::new(16)
        $fs = [IO.File]::OpenRead($c.FullName)
        try { $null = $fs.Read($head, 0, 16) } finally { $fs.Dispose() }
        $sizes[[BitConverter]::ToUInt64($head, 8)] = $true
    }
    $Archive = @(Get-ChildItem -LiteralPath (Join-Path $Stand 'database') -File | Where-Object { $sizes.ContainsKey([uint64]$_.Length) } |
        Sort-Object Name | ForEach-Object FullName)
}
$paths = foreach ($a in $Archive) {
    if (Test-Path -LiteralPath $a) { (Resolve-Path -LiteralPath $a).Path; continue }
    $found = @(Get-ChildItem -Path (Join-Path $Stand 'database') -Filter $a -File)
    if (-not $found) { throw "archive not found: $a" }
    $found.FullName
}

$variantArgs = @{
    'cpu'       = @('cpu')
    'ds'        = @('ds')
    'ds-direct' = @('ds', '--direct')
    'ds-placed' = @('ds', '--direct', '--placed')
    'dsbuf'     = @('dsbuf', '--direct', '--placed')
    'dsmem'     = @('dsmem')
    'gdeflate'  = @('gdeflate')
    'geom-cpu'  = @('geom')
    'geom-ds'   = @('geom', '--ds')
}
foreach ($v in $Variants) { if (-not $variantArgs.ContainsKey($v)) { throw "unknown variant $v" } }

$text = Join-Path $build "$Name.txt"
Set-Content -LiteralPath $text -Value "dsprobe $(Get-Date -Format s)" -Encoding UTF8
$failed = 0
$summary = New-Object System.Collections.Generic.List[string]

# Runs the probe once: every line into the log, the interesting ones on the console; returns the exit code.
function Invoke-Probe([string[]]$probeArgs) {
    $lines = @(& $exe @probeArgs 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    Add-Content -LiteralPath $text -Value (@("> dsprobe $($probeArgs -join ' ')") + $lines + "exit $code")
    $lines | Where-Object { $_ -match '^(cpu|ds|dsbuf|dsmem|gdefl|prefix|gcpu|gds|geom)|verify|failures|MISMATCH|FAILED|direct:|codec cpu|short at|BROKEN|export' } |
        ForEach-Object { Write-Host "  $_" }
    return $code
}

. (Join-Path $repo 'tools\qa\dx12\Dx12Locks.ps1')
if (-not $env:DX12_LANE) { $env:DX12_LANE = 'ds' }
$held = New-Object System.Collections.Generic.List[string]
try {
    # The prefix check needs no GPU; it loads bin's xrCore.dll, which no build may relink meanwhile.
    if ($Lock -eq 'all' -or ($PrefixCheck -and $Lock -ne 'none')) { $held.Add((Enter-Lock 'ninja')) }
    if ($Lock -ne 'none' -and -not $PrefixCheck) { $held.Add((Enter-Lock 'stand')) }
    if ($PrefixCheck) {
        if (-not $XrCore) { $XrCore = Join-Path $repo 'bin\x64\Release\xrCore.dll' }
        foreach ($p in $paths) {
            Write-Host "prefix $([IO.Path]::GetFileName($p))"
            $code = Invoke-Probe @($p, $appdata, 'prefix', '--xrcore', $XrCore)
            if ($code -ne 0) { $failed++ }
            $line = (Select-String -LiteralPath $text -Pattern '^prefix: \d+ lzo dds.*' | Select-Object -Last 1).Line
            $summary.Add(("prefix     {0,-60} exit {1}  {2}" -f [IO.Path]::GetFileName($p), $code, ($line -replace '^prefix: ', '')))
        }
    }
    else {
        foreach ($p in $paths) {
            foreach ($v in $Variants) {
                $target = $p
                if ($Cold) {
                    New-Item -ItemType Directory -Force -Path $ColdDir | Out-Null
                    $target = Join-Path $ColdDir ([IO.Path]::GetFileName($p))
                    Remove-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue
                    & robocopy.exe (Split-Path $p) $ColdDir ([IO.Path]::GetFileName($p)) /J /NJH /NJS /NFL /NDL /NP | Out-Null
                    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE) for $p" }
                }
                Write-Host ("{0} {1}{2}" -f $v, [IO.Path]::GetFileName($p), $(if ($Cold) { ' (cold copy)' } else { '' }))
                $probeArgs = @($target, $appdata) + $variantArgs[$v] + @('--threads', $Threads, '--limit', $LimitMb, '--verify', $Verify)
                if ($DebugLayer) { $probeArgs += '--debug' }
                if ($Entry) { $probeArgs += @('--entry', $Entry) }
                $code = Invoke-Probe $probeArgs
                if ($code -ne 0) { $failed++ }
                $wall = (Select-String -LiteralPath $text -Pattern 'wall\s+([\d.]+) ms' | Select-Object -Last 1).Matches.Groups[1].Value
                $summary.Add(("{0,-10} {1,-60} wall {2,9} ms  exit {3}{4}" -f $v, [IO.Path]::GetFileName($p), $wall, $code, $(if ($Cold) { '  cold' } else { '' })))
                if ($Cold) { Remove-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue }
            }
        }
    }
}
finally {
    # Stand before ninja: the reverse of the order they were taken.
    for ($i = $held.Count - 1; $i -ge 0; --$i) { Exit-Lock $held[$i] }
}

Write-Output ''
$summary | ForEach-Object { Write-Output $_ }
Write-Output "log: $text"
if ($failed) {
    Write-Output "FAIL ($failed run(s))"
    exit 1
}
Write-Output 'PASS'
exit 0
