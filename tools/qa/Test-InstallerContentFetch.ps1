# The install-time content fetch through the real Setup wizard, against the asset mock.
#
# Why this exists: the Setup first published for 1.4.0 reported every real download as "interrupted" ten seconds
# in, because the wizard's liveness probe opened a mutex under a name the fetcher never
# created. Every earlier test drove the fetcher directly and never crossed the wizard's poll
# loop, and the one Setup run before the release fetched a synthetic set that finished inside
# the start grace. So this test throttles the mock until the fetch outlasts the grace window
# and runs Setup itself - silently, over a skeleton of an original installation - end to end.
#
# Three cases: a slow fetch survives the wizard's poll loop; a fetcher left running by an
# earlier attempt is stopped before the new one starts; the fetcher refuses to run beside
# another one. Nothing outside the work root is touched except the per-user uninstall key
# Setup writes, which is removed (and any earlier value restored) at the end.
[CmdletBinding()]
param(
    # A Setup built against the synthetic manifest. Built here, into the work root, when omitted.
    [string]$Setup,
    [string]$Fetcher = (Join-Path $PSScriptRoot "..\..\build\installer\DeadAirContent.exe"),
    [string]$SyntheticRoot = (Join-Path $env:TEMP "dar-synth-content"),
    [string]$WorkRoot = (Join-Path $env:TEMP "dar-installer-test"),
    [int]$Port = 8788,
    # Bytes per second at the mock. The synthetic set is about 21 MB; at this rate the fetch
    # takes well over the wizard's ten-second start grace, which is the whole point.
    [long]$Throttle = 512KB
)

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$mockScript = Join-Path $PSScriptRoot "Start-ContentAssetMock.ps1"
$manifest = Join-Path $SyntheticRoot "content-manifest.txt"
$assetRoot = Join-Path $SyntheticRoot "cache"
$engine = Join-Path $repositoryRoot "bin\x64\Release\xrEngine.exe"
$game = Join-Path $WorkRoot "game"
$cache = Join-Path $game ".dead-air-x64\content-cache"
$cancelFlag = Join-Path $cache "content-fetch-cancel.txt"
$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{9732DFF1-E40D-4B23-B215-6D28B1DD0DE0}_is1"
$failures = [Collections.Generic.List[string]]::new()

function Assert-That {
    param([Parameter(Mandatory)][bool]$Condition, [Parameter(Mandatory)][string]$Because)
    if ($Condition) { Write-Host "  ok   $Because" }
    else { Write-Host "  FAIL $Because" -ForegroundColor Red; $script:failures.Add($Because) }
}

function Get-PortVersion {
    $header = [IO.File]::ReadAllText((Join-Path $repositoryRoot "src\xrCore\ProductVersion.h"))
    $match = [regex]::Match($header, '(?m)^#define\s+DAR_VERSION_STRING\s+"(?<version>\d+\.\d+\.\d+)"\s*$')
    if (-not $match.Success) { throw "DAR_VERSION_STRING was not found in ProductVersion.h" }
    $match.Groups['version'].Value
}

function Get-ManifestBundles {
    $rows = [Collections.Generic.List[object]]::new()
    $inBundles = $false
    foreach ($line in [IO.File]::ReadAllLines($manifest)) {
        if ($line -eq "[bundles]") { $inBundles = $true; continue }
        if ($line -like "[[]*[]]") { $inBundles = $false; continue }
        if (-not $inBundles -or -not $line.Trim()) { continue }
        $fields = $line -split "`t"
        $rows.Add([pscustomobject]@{ Hash = $fields[0]; Size = [int64]$fields[1]; Name = $fields[2] })
    }
    $rows
}

# A skeleton of what the installer accepts as an original installation: the three files its
# directory check looks for, with a real executable so GetBinaryType answers.
function New-Skeleton {
    if (Test-Path -LiteralPath $game) { Remove-Item -LiteralPath $game -Recurse -Force }
    New-Item -ItemType Directory -Force -Path (Join-Path $game "database") | Out-Null
    Copy-Item -LiteralPath $engine -Destination (Join-Path $game "xrEngine.exe")
    Set-Content -LiteralPath (Join-Path $game "fsgame.ltx") -Value '$game_data$ = false| true| $fs_root$| gamedata\'
    [IO.File]::WriteAllBytes((Join-Path $game "database\configs.xdb0"), (New-Object byte[] 4096))
}

function Start-Mock {
    param([string]$LogPath)
    # Quote every path: Start-Process does not quote array elements, and the tree normally sits
    # under a directory with a space in its name.
    $arguments = @("-NoProfile", "-File", "`"$mockScript`"", "-AssetRoot", "`"$assetRoot`"",
        "-Port", $Port, "-TimeoutSeconds", 900, "-Throttle", $Throttle)
    $process = Start-Process -FilePath "pwsh" -ArgumentList $arguments -PassThru -NoNewWindow `
        -RedirectStandardOutput $LogPath -RedirectStandardError "$LogPath.err"
    for ($attempt = 0; $attempt -lt 150; $attempt++) {
        if ($process.HasExited) { break }
        try {
            $probe = [Net.Sockets.TcpClient]::new()
            $probe.Connect("127.0.0.1", $Port)
            $probe.Close()
            return $process
        }
        catch { Start-Sleep -Milliseconds 100 }
    }
    throw "the asset mock did not come up on port $Port"
}

# Runs Setup silently over the skeleton and returns exit code, log text and the fetch phase
# duration, measured between the wizard extracting the fetcher and starting the installation.
function Invoke-Setup {
    param([Parameter(Mandatory)][string]$LogPath)
    Remove-Item -LiteralPath $LogPath -Force -ErrorAction SilentlyContinue
    $arguments = @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER", "/NOICONS",
        "/MERGETASKS=!desktopicon", "`"/DIR=$game`"", "`"/LOG=$LogPath`"")
    $process = Start-Process -FilePath $Setup -ArgumentList $arguments -PassThru -WindowStyle Hidden
    if (-not $process.WaitForExit(600000)) { $process | Stop-Process -Force; throw "Setup did not finish in ten minutes" }
    $text = if (Test-Path -LiteralPath $LogPath) { [IO.File]::ReadAllText($LogPath) } else { "" }
    $stamp = '(?m)^(?<time>\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\s+'
    $started = [regex]::Match($text, $stamp + 'Extracting temporary file: .*DeadAirContent\.exe')
    $finished = [regex]::Match($text, $stamp + 'Starting the installation process\.')
    $seconds = -1.0
    if ($started.Success -and $finished.Success) {
        $format = "yyyy-MM-dd HH:mm:ss.fff"
        $culture = [Globalization.CultureInfo]::InvariantCulture
        $seconds = ([datetime]::ParseExact($finished.Groups['time'].Value, $format, $culture) -
            [datetime]::ParseExact($started.Groups['time'].Value, $format, $culture)).TotalSeconds
    }
    [pscustomobject]@{ ExitCode = $process.ExitCode; Log = $text; FetchSeconds = $seconds }
}

function Read-FetchResult {
    $path = Join-Path $cache "content-fetch-result.txt"
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $values = @{}
    foreach ($line in [IO.File]::ReadAllLines($path)) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) { $values[$line.Substring(0, $separator)] = $line.Substring($separator + 1) }
    }
    [pscustomobject]$values
}

function Assert-Installed {
    param([Parameter(Mandatory)][string]$Case, [Parameter(Mandatory)]$Run)
    Assert-That ($Run.ExitCode -eq 0) "$Case : Setup exited 0 (got $($Run.ExitCode))"
    Assert-That ($Run.Log -match 'Installation process succeeded') "$Case : the log reports success"
    Assert-That ($Run.Log -notmatch 'Загрузка контента|Не удалось загрузить контент') "$Case : no content failure in the log"
    $result = Read-FetchResult
    Assert-That ($null -ne $result -and $result.exit -eq '0') "$Case : the fetch result says exit 0"
    foreach ($bundle in Get-ManifestBundles) {
        $installed = Join-Path $game "database\$($bundle.Name)"
        $ok = (Test-Path -LiteralPath $installed) -and ((Get-Item -LiteralPath $installed).Length -eq $bundle.Size)
        Assert-That $ok "$Case : $($bundle.Name) is in database\ at its manifest size"
    }
    Assert-That (-not (Test-Path -LiteralPath (Join-Path $game ".dead-air-x64\content-incomplete.txt"))) "$Case : no incomplete latch"
    Assert-That (-not (Get-Process DeadAirContent -ErrorAction SilentlyContinue)) "$Case : no fetcher left running"
}

# Starts the fetcher the way the wizard does, so it holds the liveness mutex and is mid-download.
function Start-StaleFetcher {
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    Copy-Item -LiteralPath $manifest -Destination (Join-Path $cache "pending-manifest.txt") -Force
    Remove-Item -LiteralPath $cancelFlag -Force -ErrorAction SilentlyContinue
    $process = Start-Process -FilePath $Fetcher -PassThru -WindowStyle Hidden -ArgumentList @(
        "--content-fetch", "--game-dir", "`"$game`"", "--cancel-flag", "`"$cancelFlag`"")
    Start-Sleep -Seconds 4
    if ($process.HasExited) { throw "the stale fetcher exited early with $($process.ExitCode)" }
    $process
}

# --- arrange ---------------------------------------------------------------------------------

if (-not (Test-Path -LiteralPath $manifest)) {
    Write-Host "== building the synthetic content set"
    & (Join-Path $PSScriptRoot "New-SyntheticContent.ps1") -WorkRoot $SyntheticRoot | Out-Host
}
if (-not (Test-Path -LiteralPath $engine)) { throw "Build the x64 Release runtime first: $engine" }

New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null
$version = Get-PortVersion
if (-not $Setup) {
    Write-Host "== building a QA Setup $version against the synthetic manifest"
    $artifacts = Join-Path $WorkRoot "artifacts"
    & (Join-Path $repositoryRoot "tools\package\build_dead_air_x64_installer.ps1") -PortVersion $version `
        -ContentManifest $manifest -SkipArchive -ArtifactDirectory $artifacts | Out-Host
    $Setup = Join-Path $artifacts "Dead-Air-Refined-$version-installer-files\Dead-Air-Refined-$version-Setup.exe"
}
if (-not (Test-Path -LiteralPath $Setup)) { throw "Setup was not found: $Setup" }
$Fetcher = (Resolve-Path $Fetcher).Path

# Setup refuses while the game holds its mutex, and every fetcher on this machine shares one
# liveness mutex - so a game or a fetch that is already running would turn every case into a
# false failure, or worse, get cancelled by the wizard under test. Refuse rather than kill.
$running = @(Get-Process xrEngine, DeadAirContent, DeadAirUpdater -ErrorAction SilentlyContinue)
if ($running) { throw "already running: $(($running | ForEach-Object ProcessName) -join ', ') - wait for it to finish" }

$keyBefore = Get-ItemProperty -Path $uninstallKey -ErrorAction SilentlyContinue
$stale = $null
$first = $null
$env:DAR_QA_CONTENT_BASE = "http://127.0.0.1:$Port"
$mock = Start-Mock -LogPath (Join-Path $WorkRoot "mock.log")

try {
    # --- case 1: a fetch longer than the start grace is not "interrupted" ------------------------
    Write-Host "== case 1: slow fetch through the wizard"
    New-Skeleton
    $run = Invoke-Setup -LogPath (Join-Path $WorkRoot "setup-slow.log")
    Assert-That ($run.FetchSeconds -ge 15) ("slow : the fetch phase outlasted the start grace ({0:N1}s)" -f $run.FetchSeconds)
    Assert-Installed -Case "slow" -Run $run

    # --- case 2: a fetcher left by an earlier attempt is stopped, then the install proceeds ------
    Write-Host "== case 2: stale fetcher from an earlier attempt"
    New-Skeleton
    $stale = Start-StaleFetcher
    $run = Invoke-Setup -LogPath (Join-Path $WorkRoot "setup-stale.log")
    Assert-That ($run.Log -match 'A content fetcher from an earlier attempt is still running') "stale : the wizard noticed the earlier fetcher"
    Assert-That ($run.Log -match 'The earlier content fetcher has stopped\.') "stale : the wizard waited for it to stop"
    Assert-That ($stale.WaitForExit(30000) -and $stale.ExitCode -eq 26) "stale : the earlier fetcher exited on the cancel flag (code $($stale.ExitCode))"
    Assert-Installed -Case "stale" -Run $run

    # --- case 3: the fetcher declines to run beside another one ------------------------------
    Write-Host "== case 3: a second fetcher declines"
    New-Skeleton
    $first = Start-StaleFetcher
    $second = Start-Process -FilePath $Fetcher -PassThru -Wait -WindowStyle Hidden -ArgumentList @(
        "--content-fetch", "--game-dir", "`"$game`"")
    $result = Read-FetchResult
    Assert-That ($second.ExitCode -eq 26) "double : the second fetcher exited 26 (got $($second.ExitCode))"
    Assert-That ($null -ne $result -and $result.message -match 'another content download is still running') "double : the result names the running download"
    Assert-That (-not $first.HasExited) "double : the first fetcher kept going"
    Set-Content -LiteralPath $cancelFlag -Value "cancel"
    Assert-That ($first.WaitForExit(30000)) "double : the first fetcher stopped on its cancel flag"
}
finally {
    # Only what this run started: a fetcher that is still up here is ours, and a cancel flag is
    # the polite way to stop it.
    if (Test-Path -LiteralPath $cache) { Set-Content -LiteralPath $cancelFlag -Value "cancel" -ErrorAction SilentlyContinue }
    # Not $fetcher: PowerShell names are case-blind, so that IS the [string]$Fetcher parameter,
    # which turns each process into its type name - the teardown then threw here and never got
    # to the mock or to the uninstall entry below.
    foreach ($leftover in @($stale, $first)) {
        if ($leftover -and -not $leftover.HasExited -and -not $leftover.WaitForExit(15000)) { $leftover | Stop-Process -Force }
    }
    if ($mock -and -not $mock.HasExited) { $mock | Stop-Process -Force }
    Remove-Item Env:\DAR_QA_CONTENT_BASE -ErrorAction SilentlyContinue

    # Setup registered the skeleton as the installation. Take that back, and put back whatever
    # the key said before - a real per-user installation must not lose its uninstall entry.
    $keyAfter = Get-ItemProperty -Path $uninstallKey -ErrorAction SilentlyContinue
    if ($keyAfter -and $keyAfter.InstallLocation -and
        ($keyAfter.InstallLocation.TrimEnd('\') -ieq $game.TrimEnd('\'))) {
        Remove-Item -Path $uninstallKey -Recurse -Force
        if ($keyBefore) {
            New-Item -Path $uninstallKey -Force | Out-Null
            foreach ($property in $keyBefore.PSObject.Properties) {
                if ($property.Name -like 'PS*') { continue }
                Set-ItemProperty -Path $uninstallKey -Name $property.Name -Value $property.Value
            }
        }
    }
}

Write-Host ""
if ($failures.Count -eq 0) {
    Write-Host "all installer content fetch cases passed" -ForegroundColor Green
    Remove-Item -LiteralPath $game -Recurse -Force -ErrorAction SilentlyContinue
}
else {
    Write-Host "$($failures.Count) failure(s); the work root is kept for inspection: $WorkRoot" -ForegroundColor Red
    exit 1
}
