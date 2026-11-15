# The module data reader without the game: builds src\xrCore\XMS\xms_luadata.cpp with its cases
# (luadata_test.cpp) into _work\luadata-qa and runs them.
param(
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot "_work\luadata-qa"
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
$readerSource = Join-Path $repoRoot "src\xrCore\XMS\xms_luadata.cpp"
$testSource = Join-Path $PSScriptRoot "luadata_test.cpp"
$testExecutable = Join-Path $outputPath "luadata_test.exe"

$compile = 'call "{0}" -arch=amd64 -host_arch=amd64 >nul && ' +
    'cl.exe /nologo /O2 /EHsc /std:c++20 /utf-8 /MD /W4 /WX /DXMS_LUADATA_STANDALONE /I"{1}" ' +
    '/Fo"{2}\\" /Fe"{3}" "{4}" "{5}"'
$compile = $compile -f $developerShell, $include, $outputPath, $testExecutable, $testSource, $readerSource
& cmd.exe /d /s /c $compile
if ($LASTEXITCODE -ne 0) {
    throw "module data reader test compilation failed with exit code $LASTEXITCODE"
}

& $testExecutable
if ($LASTEXITCODE -ne 0) {
    throw "module data reader QA failed with exit code $LASTEXITCODE"
}
