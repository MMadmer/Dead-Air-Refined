param(
    [Parameter(Mandatory = $true)]
    [string]$InputScoc,

    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$inputPath = (Resolve-Path $InputScoc).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot "_work\marshal-incremental-qa"
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
$luaInclude = Join-Path $repoRoot "Externals\LuaJIT\src"
$luaLibrary = Join-Path $repoRoot "build\lib\x64\Release\LuaJIT.lib"
$luaRuntime = Join-Path $repoRoot "bin\x64\Release"
$marshalSource = Join-Path $repoRoot "Externals\xrLuaFix\lua-marshal\lmarshal.c"
$captureScript = Join-Path $repoRoot "packaging\dead-air-x64\compatibility\gamedata\scripts\alife_storage_manager.script"
$testSource = Join-Path $PSScriptRoot "marshal_incremental_test.cpp"
$marshalObject = Join-Path $outputPath "lmarshal.obj"
$testExecutable = Join-Path $outputPath "marshal_incremental_test.exe"

$compileMarshal = 'call "{0}" -arch=amd64 -host_arch=amd64 >nul && ' +
    'cl.exe /nologo /c /O2 /MD /W3 /WX /wd4018 /wd4244 /wd4267 /TC /I"{1}" /Fo"{2}" "{3}"'
$compileMarshal = $compileMarshal -f $developerShell, $luaInclude, $marshalObject, $marshalSource
& cmd.exe /d /s /c $compileMarshal
if ($LASTEXITCODE -ne 0) {
    throw "lmarshal.c compilation failed with exit code $LASTEXITCODE"
}

$compileTest = 'call "{0}" -arch=amd64 -host_arch=amd64 >nul && ' +
    'cl.exe /nologo /O2 /EHsc /std:c++20 /utf-8 /MD /W4 /WX /I"{1}" ' +
    '/Fe"{2}" "{3}" "{4}" "{5}"'
$compileTest = $compileTest -f `
    $developerShell, $luaInclude, $testExecutable, $testSource, $marshalObject, $luaLibrary
& cmd.exe /d /s /c $compileTest
if ($LASTEXITCODE -ne 0) {
    throw "marshal incremental test compilation failed with exit code $LASTEXITCODE"
}

Push-Location $luaRuntime
try {
    & $testExecutable $inputPath $captureScript
    if ($LASTEXITCODE -ne 0) {
        throw "marshal incremental QA failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
