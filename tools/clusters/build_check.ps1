# Builds cluster_check.exe (the engine's cluster hierarchy builder outside the engine) with the latest MSVC.
#   pwsh -File tools/clusters/build_check.ps1 [-Out <dir>]
param([string]$Out = "$PSScriptRoot\bin")
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\..\.."
$gen = "$root\src\Layers\xrRender"
New-Item -ItemType Directory -Force $Out | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$dev = Join-Path $vs "Common7\Tools\VsDevCmd.bat"
$src = @("$PSScriptRoot\cluster_check.cpp", "$gen\da_cluster_dag.cpp", "$gen\da_graph_partition.cpp", "$gen\da_mesh_simplify.cpp") | ForEach-Object { "`"$_`"" }
$cl = "cl /nologo /O2 /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I`"$gen`" /Fo`"$Out\\`" /Fe`"$Out\cluster_check.exe`" $($src -join ' ')"
& cmd.exe /d /s /c "call `"$dev`" -arch=x64 -host_arch=x64 >nul && $cl"
exit $LASTEXITCODE
