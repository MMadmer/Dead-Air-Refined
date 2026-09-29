# Builds and runs the xrD3D12 differential tests: every scenario runs on Direct3D 11 and on the
# layer (D3D12 debug layer on), results are compared; a debug layer error fails the scenario.
# Exit code = number of failed scenarios (0 = all passed).
#
#   tools\qa\d3d12layer\Run-LayerTests.ps1                  all scenarios + the draw benchmark
#   tools\qa\d3d12layer\Run-LayerTests.ps1 -Gbv             with GPU-based validation (slow)
#   tools\qa\d3d12layer\Run-LayerTests.ps1 -Dxil            the layer's shaders built with DXC (SM 6.0)
#   tools\qa\d3d12layer\Run-LayerTests.ps1 -NoCopyQueue     creation-time uploads through the direct queue
#   tools\qa\d3d12layer\Run-LayerTests.ps1 -Filter hazard   scenarios whose name contains "hazard"
[CmdletBinding()]
param(
    [switch]$Gbv,
    # The layer device gets every scenario shader as DXIL (DXC, SM 6.0); D3D11 keeps FXC's DXBC.
    [switch]$Dxil,
    # The layer device uploads creation-time data through the direct queue (-dx12_no_copy_queue).
    [switch]$NoCopyQueue,
    [string]$Filter,
    [switch]$NoBench,
    [switch]$NoBuild,
    # the discrete GPU (hybrid laptops enumerate the integrated one first)
    [switch]$HighPerformance,
    [int]$BenchFrames = 12,
    [int]$BenchDraws = 20000
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..")).Path
# One build or run of the shared test executable at a time (layertest.lock).
. (Join-Path $repo "tools\qa\dx12\Dx12Locks.ps1")
$lock = if ($env:DX12_LAYERTEST_LOCK_HELD) { $null } else { Enter-Lock 'layertest' }
$env:DX12_LAYERTEST_LOCK_HELD = '1'
try {
if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot "Build-LayerTests.ps1")
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
$exe = Join-Path $repo "build\qa\d3d12layer\d3d12layer_tests.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "$exe is missing - build it first" }
$arguments = @()
if ($Gbv) { $arguments += "-gbv" }
if ($Dxil) { $arguments += "-dxil" }
if ($NoCopyQueue) { $arguments += "-nocopyqueue" }
if ($Filter) { $arguments += @("-filter", $Filter) }
if ($NoBench) { $arguments += "-nobench" }
if ($HighPerformance) { $arguments += "-highperf" }
$arguments += @("-benchframes", "$BenchFrames", "-benchdraws", "$BenchDraws")
& $exe @arguments
$code = $LASTEXITCODE
} finally {
    if ($lock) { Remove-Item Env:DX12_LAYERTEST_LOCK_HELD -ErrorAction SilentlyContinue }
    Exit-Lock $lock
}
exit $code
