# Builds the xrD3D12 differential tests with cl.exe: the test sources and every
# src\Layers\xrRenderDX12\*.cpp compile straight into one executable, outside the ninja build
# (safe to run while the engine builds). Output: build\qa\d3d12layer\d3d12layer_tests.exe.
#
#   tools\qa\d3d12layer\Build-LayerTests.ps1 [-NoOptimization]
[CmdletBinding()]
param(
    # /Od instead of /O2, for stepping through the layer in a debugger.
    [switch]$NoOptimization
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..")).Path
# Several agents share build\qa\d3d12layer: one build or run at a time (layertest.lock of the DX12
# lock directory; Run-LayerTests.ps1 holds it across build and run).
. (Join-Path $repo "tools\qa\dx12\Dx12Locks.ps1")
$lock = if ($env:DX12_LAYERTEST_LOCK_HELD) { $null } else { Enter-Lock 'layertest' }
try {
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere.exe not found: Visual Studio is required" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "A Visual Studio installation with the x64 C++ toolchain is required." }
$devCmd = Join-Path $vs "Common7\Tools\VsDevCmd.bat"

$layerDir = Join-Path $repo "src\Layers\xrRenderDX12"
$out = Join-Path $repo "build\qa\d3d12layer"
$obj = Join-Path $out "obj"
New-Item -ItemType Directory -Force -Path $out, $obj | Out-Null

$sources = @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter *.cpp) + @(Get-ChildItem -LiteralPath $layerDir -Filter *.cpp)
$sourceList = ($sources | ForEach-Object { "`"$($_.FullName)`"" }) -join " "
$optimization = if ($NoOptimization) { "/Od" } else { "/O2" }
$exe = Join-Path $out "d3d12layer_tests.exe"
$compile = "cl.exe /nologo /std:c++20 /EHsc $optimization /MD /W3 /Zi /MP /FS /DNOMINMAX /DWIN32_LEAN_AND_MEAN " +
    "/I`"$layerDir`" /Fo`"$obj\\`" /Fd`"$out\d3d12layer_tests.pdb`" /Fe`"$exe`" $sourceList " +
    "/link /DEBUG /INCREMENTAL:NO d3d11.lib dxgi.lib d3dcompiler.lib dxguid.lib user32.lib ole32.lib"
$script = Join-Path $out "build.cmd"
Set-Content -LiteralPath $script -Encoding ASCII -Value @(
    "@call `"$devCmd`" -arch=x64 -host_arch=x64 >nul",
    "@cd /d `"$out`"",
    $compile
)
$output = & cmd.exe /d /c "`"$script`"" 2>&1
$code = $LASTEXITCODE
$diagnostics = @($output | Where-Object { $_ -match ': (error|fatal error|warning) [A-Z]+\d+' -or $_ -match 'LNK\d+' })
$diagnostics | ForEach-Object { Write-Output $_ }
if ($code -ne 0) {
    if (-not $diagnostics) { $output | Select-Object -Last 40 | ForEach-Object { Write-Output $_ } }
    Write-Output "BUILD FAILED ($code)"
    exit $code
}
# The layer loads DXC from the executable's folder (d12_dxc.cpp); the harness's -dxil mode needs it.
foreach ($dll in 'dxcompiler.dll', 'dxil.dll') {
    $source = Join-Path $repo (Join-Path "Externals/dxc/bin/x64" $dll)
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $out -Force }
}
Write-Output ("built {0}{1}" -f $exe, $(if ($diagnostics.Count) { " ($($diagnostics.Count) diagnostics)" } else { "" }))
} finally { Exit-Lock $lock }
