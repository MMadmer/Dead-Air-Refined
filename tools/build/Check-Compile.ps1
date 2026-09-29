# Compiles single translation units with the exact flags the Release build uses, into a private
# output folder: several people (or agents) can check their files at once without touching the
# shared ninja build directory, whose log and deps a second ninja process would corrupt.
#
#   tools\build\Check-Compile.ps1 -File src\Layers\xrRenderPC_R4\r4_rendertarget_phase_taa.cpp
#   tools\build\Check-Compile.ps1 -File src\Layers\xrRenderDX12\d12Context.cpp -Template src\Layers\xrRenderDX11\dx11HW.cpp
#
# A file the build does not know yet borrows the command of -Template (a file of the same target);
# -Define adds definitions, -Remove drops the ones matching a pattern (e.g. '-DUSE_DX11').
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$File,
    [string]$Template,
    [string]$Target,
    [string[]]$Define = @(),
    [string[]]$Remove = @(),
    [string[]]$Include = @(),
    [ValidateSet("Release", "Debug", "Mixed")][string]$Configuration = "Release",
    [string]$Out,
    [switch]$NoPch
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$buildDir = Join-Path $repo "build\ninja-x64"
$db = Join-Path $buildDir "compile_commands.json"
if (-not (Test-Path -LiteralPath $db)) { throw "no compile_commands.json in $buildDir - run a build first" }

# The parsed database is cached next to it per configuration: 20 MB of JSON costs seconds to parse.
$cache = Join-Path $buildDir "compile_commands.$Configuration.clixml"
if (-not (Test-Path -LiteralPath $cache) -or (Get-Item $cache).LastWriteTime -lt (Get-Item $db).LastWriteTime) {
    $entries = Get-Content -LiteralPath $db -Raw | ConvertFrom-Json |
        Where-Object { $_.command -match "\\$Configuration\\" -and $_.command -match '\s-c\s' } |
        ForEach-Object { [pscustomobject]@{ file = ($_.file -replace '/', '\').ToLowerInvariant(); command = $_.command } }
    $entries | Export-Clixml -LiteralPath $cache
} else {
    $entries = Import-Clixml -LiteralPath $cache
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$devCmd = Join-Path $vs "Common7\Tools\VsDevCmd.bat"

if (-not $Out) { $Out = Join-Path ([IO.Path]::GetTempPath()) ("xr-check-" + [guid]::NewGuid().ToString("N").Substring(0, 8)) }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

function Find-Command([string]$path) {
    $key = $path.ToLowerInvariant()
    $hits = @($entries | Where-Object { $_.file -eq $key })
    if ($Target) { $hits = @($hits | Where-Object { $_.command -match [regex]::Escape("\$Target.dir\") }) }
    return $hits | Select-Object -First 1
}

$failed = 0
foreach ($f in $File) {
    $full = if ([IO.Path]::IsPathRooted($f)) { $f } else { Join-Path $repo $f }
    $full = [IO.Path]::GetFullPath($full)
    $entry = Find-Command $full
    $source = $full
    if (-not $entry) {
        if (-not $Template) { throw "$f is not in the build and no -Template was given" }
        $tfull = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($Template)) { $Template } else { Join-Path $repo $Template }))
        $entry = Find-Command $tfull
        if (-not $entry) { throw "template $Template is not in the build either" }
    }
    $cmd = $entry.command
    # the source: the last quoted or bare path after -c
    $cmd = [regex]::Replace($cmd, '\s-c\s+("[^"]+"|\S+)\s*$', " -c `"$source`"")
    $name = [IO.Path]::GetFileNameWithoutExtension($source)
    # Only the object goes elsewhere: the precompiled header is bound to the pdb it was built
    # with, so /Fd and /GL stay as the build has them (/FS serialises the pdb writes).
    $cmd = [regex]::Replace($cmd, '/Fo\S+', "/Fo`"$Out\$name.obj`"")
    if ($NoPch) { $cmd = [regex]::Replace($cmd, '"/(Yu|Fp|FI)[^"]*"', '') }
    foreach ($r in $Remove) { $cmd = [regex]::Replace($cmd, "\s$([regex]::Escape($r))(=\S+)?(?=\s)", ' ') }
    $extra = ($Define | ForEach-Object { " -D$_" }) -join ''
    $extra += ($Include | ForEach-Object { " -I`"$_`"" }) -join ''
    $cmd = $cmd -replace '(cl\.exe)\s', "`$1 $extra "
    $bat = Join-Path $Out "$name.cmd"
    Set-Content -LiteralPath $bat -Value "@call `"$devCmd`" -arch=x64 -host_arch=x64 >nul`r`n@cd /d `"$buildDir`"`r`n$cmd" -Encoding ASCII
    $output = & cmd.exe /d /c "`"$bat`"" 2>&1
    $code = $LASTEXITCODE
    $diag = $output | Where-Object { $_ -match ': (error|fatal error|warning) [A-Z]+\d+' }
    if ($code -ne 0) {
        $failed++
        Write-Output "FAIL $f"
        ($diag | Select-Object -First 60) | ForEach-Object { Write-Output "  $_" }
        if (-not $diag) { $output | Select-Object -Last 30 | ForEach-Object { Write-Output "  $_" } }
    } else {
        $warn = @($diag | Where-Object { $_ -match 'warning' })
        Write-Output ("OK   {0}{1}" -f $f, $(if ($warn.Count) { " ($($warn.Count) warning(s))" } else { "" }))
        $warn | Select-Object -First 20 | ForEach-Object { Write-Output "  $_" }
    }
}
Remove-Item -LiteralPath $Out -Recurse -Force -ErrorAction SilentlyContinue
if ($failed) { exit 1 }
