# Proves that LuaJIT keeps working when nothing else is left below 2 GB.
#
# The engine build has no LJ_GC64, so all GC memory has to come from the low 2 GB. The test takes
# that range away the way bottom-up placed archive mappings do and runs a workload on top.
# -BaselineDll names a LuaJIT.dll without the arena (any release up to 1.4.2): it has to fail under
# the very same conditions, which is what shows that the test starves the allocator for real.
param(
    [string]$LuaJitDll = "",

    [string]$BaselineDll = "",

    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
if (-not $LuaJitDll) {
    $LuaJitDll = Join-Path $repoRoot "bin\x64\Release\LuaJIT.dll"
}
$luaJitPath = (Resolve-Path $LuaJitDll).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot "build\qa\luajit-lowmem"
}
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$installationPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installationPath) {
    throw "Visual Studio C++ tools were not found"
}

$developerShell = Join-Path $installationPath "Common7\Tools\VsDevCmd.bat"
$testSource = Join-Path $PSScriptRoot "lowmem_arena_test.c"
$testObject = Join-Path $outputPath "lowmem_arena_test.obj"
$testExecutable = Join-Path $outputPath "lowmem_arena_test.exe"

# No import library on purpose: the DLL under test is chosen at run time.
$compileTest = 'call "{0}" -arch=amd64 -host_arch=amd64 >nul && ' +
    'cl.exe /nologo /O2 /MD /W4 /WX /TC /Fo"{1}" /Fe"{2}" "{3}"'
$compileTest = $compileTest -f $developerShell, $testObject, $testExecutable, $testSource
& cmd.exe /d /s /c $compileTest
if ($LASTEXITCODE -ne 0) {
    throw "low memory test compilation failed with exit code $LASTEXITCODE"
}

& $testExecutable $luaJitPath
if ($LASTEXITCODE -ne 0) {
    throw "LuaJIT low memory QA failed with exit code $LASTEXITCODE"
}

if ($BaselineDll) {
    $baselinePath = (Resolve-Path $BaselineDll).Path
    Write-Host ""
    & $testExecutable $baselinePath expect-starved
    if ($LASTEXITCODE -ne 0) {
        throw "the baseline DLL survived the starvation: the test no longer proves anything"
    }

    # Scripts write a save with one pairs() walk and read it with another, so a LuaJIT update must
    # not move a single key. Upstream on its own walks differently from run to run.
    Write-Host ""
    $fingerprint = Join-Path $PSScriptRoot "table_order_fingerprint.lua"
    $baselineOrder = & $testExecutable $baselinePath run $fingerprint
    if ($LASTEXITCODE -ne 0) {
        throw "the fingerprint script failed on the baseline DLL"
    }
    $currentOrder = & $testExecutable $luaJitPath run $fingerprint
    if ($LASTEXITCODE -ne 0) {
        throw "the fingerprint script failed on the DLL under test"
    }
    if (-not $baselineOrder -or (Compare-Object $baselineOrder $currentOrder -CaseSensitive)) {
        Compare-Object $baselineOrder $currentOrder -CaseSensitive | Format-Table -AutoSize | Out-String | Write-Host
        throw "pairs() walks tables in another order than the baseline DLL: saves would load differently"
    }
    Write-Host ("table order: {0} fingerprint lines identical to the baseline DLL" -f @($currentOrder).Count)
}
