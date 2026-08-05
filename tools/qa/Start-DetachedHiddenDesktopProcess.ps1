param(
    [Parameter(Mandatory)]
    [string]$FilePath,

    [string]$Arguments = '',

    [Parameter(Mandatory)]
    [string]$ProcessIdFile,

    [Parameter(Mandatory)]
    [string]$LauncherIdFile,

    [string]$DesktopName = 'DeadAirQA',

    [ValidateRange(1, 3600)]
    [int]$TimeoutSeconds = 180,

    [string]$ExitCodeFile
)

$ErrorActionPreference = 'Stop'
$launcherScript = Join-Path $PSScriptRoot 'Start-HiddenDesktopProcess.ps1'
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = (Get-Command pwsh.exe -ErrorAction Stop).Source
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true

$launcherArguments = @(
    '-NoProfile',
    '-ExecutionPolicy',
    'Bypass',
    '-File',
    $launcherScript,
    '-FilePath',
    $FilePath,
    '-Arguments',
    $Arguments,
    '-ProcessIdFile',
    $ProcessIdFile,
    '-DesktopName',
    $DesktopName,
    '-TimeoutSeconds',
    $TimeoutSeconds
)
if ($ExitCodeFile) {
    $launcherArguments += '-ExitCodeFile', $ExitCodeFile
}

foreach ($argument in $launcherArguments) {
    [void]$startInfo.ArgumentList.Add($argument)
}

$launcher = [Diagnostics.Process]::Start($startInfo)
Set-Content -LiteralPath $LauncherIdFile -Value $launcher.Id -Encoding ascii
$launcher.Id
