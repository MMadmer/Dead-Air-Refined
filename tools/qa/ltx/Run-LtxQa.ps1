# What a module's config patches and items say, without the game: builds the .ltxp reader
# (src\xrCore\XMS\xms_ltxp.cpp), the inheritance rule (xms_inherit.h), the item planner
# (xms_items.cpp) and the module data reader they stand on, with their cases (ltx_test.cpp), into
# _work\ltx-qa and runs them.
param(
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot "_work\ltx-qa"
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
$include = Join-Path $repoRoot "src"
$xms = Join-Path $repoRoot "src\xrCore\XMS"
$sources = @(
    (Join-Path $PSScriptRoot "ltx_test.cpp"),
    (Join-Path $xms "xms_ltxp.cpp"),
    (Join-Path $xms "xms_items.cpp"),
    (Join-Path $xms "xms_luadata.cpp")
) | ForEach-Object { '"{0}"' -f $_ }
$testExecutable = Join-Path $outputPath "ltx_test.exe"

$compile = 'call "{0}" -arch=amd64 -host_arch=amd64 >nul && ' +
    'cl.exe /nologo /O2 /EHsc /std:c++20 /utf-8 /MD /W4 /WX /DXMS_LUADATA_STANDALONE /I"{1}" ' +
    '/Fo"{2}\\" /Fe"{3}" {4}'
$compile = $compile -f $developerShell, $include, $outputPath, $testExecutable, ($sources -join ' ')
& cmd.exe /d /s /c $compile
if ($LASTEXITCODE -ne 0) {
    throw "config patch test compilation failed with exit code $LASTEXITCODE"
}

& $testExecutable
if ($LASTEXITCODE -ne 0) {
    throw "config patch QA failed with exit code $LASTEXITCODE"
}
