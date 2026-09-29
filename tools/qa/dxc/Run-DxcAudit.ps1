# The DXC audit with the pipeline probe (docs/dead-air/dx12-features/dxc.md 6.1, test 2): compiles
# every R5 shader permutation with DXC (tools\qa\Check-DxcShaders.py --out), then creates every
# pipeline on the real device with the layer's root signature layouts (pso_probe.exe, built here with
# cl.exe when missing or older than its source). Pass: the audit finds no DXC-only failure and the
# probe refuses only pipelines built behind a legacy switch (the gated old cascades).
#
#   tools\qa\dxc\Run-DxcAudit.ps1                     # SM 6.6, the preset permutations
#   tools\qa\dxc\Run-DxcAudit.ps1 -Sm 6_0 -ConsoleExtras
#   tools\qa\dxc\Run-DxcAudit.ps1 -Loose <snapshot>\gamedata\shaders\r3 -AuditArgs '--preprocess;dxc'
#
# Output (audit text, DXIL blobs, pipelines.txt, probe text) in -Out, by default
# build\qa\dxc\audit-<sm>[-extras]. Exit code 0 pass, 1 fail.
[CmdletBinding()]
param(
    [string]$Sm = '6_6',
    [switch]$ConsoleExtras,
    [string]$Out = '',
    # Another loose shader overlay (e.g. the Gate-0 snapshot's gamedata\shaders\r3).
    [string]$Loose = '',
    # Further Check-DxcShaders.py arguments (--unpack, --dxc-dir, --preprocess ...); 'a;b' separates
    # several when the script is started with pwsh -File.
    [string[]]$AuditArgs = @(),
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
$build = Join-Path $repo 'build\qa\dxc'
if (-not $Out) { $Out = Join-Path $build ("audit-$Sm" + $(if ($ConsoleExtras) { '-extras' } else { '' })) }
New-Item -ItemType Directory -Force -Path $build, $Out | Out-Null

# pso_probe.exe: cl.exe inside the Visual Studio x64 developer environment (no ninja lock needed).
$source = Join-Path $PSScriptRoot 'pso_probe.cpp'
$exe = Join-Path $build 'pso_probe.exe'
if ($Rebuild -or -not (Test-Path -LiteralPath $exe) -or (Get-Item -LiteralPath $exe).LastWriteTime -lt (Get-Item -LiteralPath $source).LastWriteTime) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'A Visual Studio installation with the x64 C++ toolchain is required.' }
    $script = Join-Path $build 'build_pso_probe.cmd'
    Set-Content -LiteralPath $script -Encoding ASCII -Value @(
        '@set VSLANG=1033',
        "@call `"$(Join-Path $vs 'Common7\Tools\VsDevCmd.bat')`" -arch=x64 -host_arch=x64 >nul 2>nul",
        "@cd /d `"$build`"",
        "cl.exe /nologo /std:c++20 /EHsc /O2 /MD /W4 /Fe`"$exe`" `"$source`" /link /INCREMENTAL:NO"
    )
    $log = & cmd.exe /d /c "`"$script`"" 2>&1
    if ($LASTEXITCODE -ne 0) {
        $log | Select-Object -Last 30 | ForEach-Object { Write-Output $_ }
        throw "pso_probe build failed ($LASTEXITCODE)"
    }
    $log | Where-Object { $_ -match ': (warning|error) ' } | ForEach-Object { Write-Output $_ }
    Write-Output "built $exe"
}

$python = (Get-Command python -ErrorAction Stop).Source
$audit = @((Join-Path $repo 'tools\qa\Check-DxcShaders.py'), '--sm', $Sm, '--out', $Out) +
    @($AuditArgs | ForEach-Object { $_ -split ';' } | Where-Object { $_ })
if ($Loose) { $audit += @('--loose', $Loose) }
if ($ConsoleExtras) { $audit += '--console-extras' }
$auditText = Join-Path $Out 'audit.txt'
& $python @audit 2>&1 | Tee-Object -FilePath $auditText | Out-Host
$auditCode = $LASTEXITCODE

$probeText = Join-Path $Out 'pso_probe.txt'
& $exe $Out 2>&1 | Tee-Object -FilePath $probeText | Select-Object -Last 3 | Out-Host
$probeCode = $LASTEXITCODE

# The refusals the probe printed, grouped by pipeline.
$refused = Select-String -LiteralPath $probeText -Pattern '^REFUSED( \(gated\))? 0x[0-9a-f]+ (.*?) \[' |
    ForEach-Object { [pscustomobject]@{ Gated = [bool]$_.Matches[0].Groups[1].Success; Title = $_.Matches[0].Groups[2].Value } }
$ungated = @($refused | Where-Object { -not $_.Gated } | Group-Object Title)
$gated = @($refused | Where-Object Gated | Group-Object Title)
Write-Output ''
Write-Output ("audit (SM {0}{1}): exit {2}; probe: {3} refused outside the gated set, {4} gated pipeline(s) refused" -f
    $Sm.Replace('_', '.'), $(if ($ConsoleExtras) { ', console extras' } else { '' }), $auditCode, $ungated.Count, $gated.Count)
$gated | ForEach-Object { Write-Output ("  gated   {0} ({1} permutation(s))" -f $_.Name, $_.Count) }
$ungated | ForEach-Object { Write-Output ("  REFUSED {0} ({1} permutation(s))" -f $_.Name, $_.Count) }
Write-Output "output: $Out"
if ($auditCode -ne 0 -or $probeCode -ne 0) {
    Write-Output 'FAIL'
    exit 1
}
Write-Output 'PASS'
exit 0
