# Builds shrub_preview.exe (the engine's shrub generator outside the engine) with the latest MSVC.
#   pwsh -File tools/shrubs/preview/build_preview.ps1 [-Out <dir>]
param([string]$Out = "$PSScriptRoot\bin")
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\..\..\.."
$gen = "$root\src\Layers\xrRender"
New-Item -ItemType Directory -Force $Out | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$dev = Join-Path $vs "Common7\Tools\VsDevCmd.bat"
$cl = "cl /nologo /O2 /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I`"$gen`" /Fo`"$Out\\`" /Fe`"$Out\shrub_preview.exe`" `"$PSScriptRoot\shrub_preview.cpp`" `"$gen\da_shrub_gen.cpp`""
& cmd.exe /d /s /c "call `"$dev`" -arch=x64 -host_arch=x64 >nul && $cl"
exit $LASTEXITCODE
