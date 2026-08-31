# Install-time content flow, against a real binary and a real server.
#
# Everything here runs the shipped DeadAirUpdater.exe in its content modes against a scratch
# installation and the asset mock, because the interesting failures are not in any one function
# - they are in what a resumed part file, a dropped connection or a server that ignores Range
# leaves behind for the next stage to find.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Updater,
    # Directory of bundle files to serve, and the manifest that declares them.
    [Parameter(Mandatory)][string]$AssetRoot,
    [Parameter(Mandatory)][string]$Manifest,
    [string]$WorkRoot,
    [int]$Port = 8788
)

$ErrorActionPreference = "Stop"

if (-not $WorkRoot) {
    $WorkRoot = Join-Path ([IO.Path]::GetTempPath()) "dar-content-flow"
}
$mockScript = Join-Path $PSScriptRoot "Start-ContentAssetMock.ps1"
$failures = [Collections.Generic.List[string]]::new()

function New-Installation {
    Remove-Item -LiteralPath $WorkRoot -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path (Join-Path $WorkRoot "database"),
        (Join-Path $WorkRoot ".dead-air-x64") | Out-Null
    Copy-Item -LiteralPath $Manifest -Destination (Join-Path $WorkRoot ".dead-air-x64\content-manifest.txt") -Force
}

function Start-Mock {
    param([hashtable]$Options = @{}, [string]$LogPath)
    if (-not $LogPath) { $LogPath = Join-Path $WorkRoot "mock.log" }
    $errorPath = "$LogPath.err"

    # Quote every path: Start-Process does not quote array elements, and both the script and the
    # asset root normally sit under "Dead Air".
    $arguments = @("-NoProfile", "-File", "`"$mockScript`"", "-AssetRoot", "`"$AssetRoot`"",
        "-Port", $Port, "-TimeoutSeconds", 300)
    foreach ($key in $Options.Keys) {
        $arguments += "-$key"
        if ($Options[$key] -isnot [switch] -and $Options[$key] -ne $true) { $arguments += $Options[$key] }
    }
    $process = Start-Process -FilePath "pwsh" -ArgumentList $arguments -PassThru -NoNewWindow `
        -RedirectStandardOutput $LogPath -RedirectStandardError $errorPath

    # Poll the port rather than sleeping. pwsh takes an unpredictable second or two to start, and
    # a fixed wait produces failures that look like downloader bugs and are not.
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
    Get-Content -LiteralPath $LogPath -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  mock: $_" }
    Get-Content -LiteralPath $errorPath -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  mock: $_" }
    throw "the asset mock did not start listening on port $Port"
}

function Stop-Mock {
    param($Process)
    if ($Process) { $Process | Stop-Process -Force -ErrorAction SilentlyContinue }
    # The port has to be free before the next case starts, or its poll succeeds against a
    # socket that is already closing.
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        try {
            $probe = [Net.Sockets.TcpClient]::new()
            $probe.Connect("127.0.0.1", $Port)
            $probe.Close()
            Start-Sleep -Milliseconds 100
        }
        catch { return }
    }
}

function Invoke-Updater {
    param([string[]]$Arguments)
    # The updater is a /SUBSYSTEM:WINDOWS binary, so a plain call does not wait for it.
    $process = Start-Process -FilePath $Updater -ArgumentList $Arguments -PassThru -NoNewWindow
    if (-not $process.WaitForExit(600000)) {
        $process.Kill()
        throw "the updater did not finish"
    }
    return $process.ExitCode
}

function Get-Result {
    $path = Join-Path $WorkRoot ".dead-air-x64\content-cache\content-fetch-result.txt"
    if (-not (Test-Path -LiteralPath $path)) { return "" }
    return (Get-Content -LiteralPath $path -Raw)
}

function Assert-Case {
    param([string]$Name, [bool]$Condition, [string]$Detail = "")
    if ($Condition) {
        Write-Host "  ok   $Name"
    }
    else {
        Write-Host "  FAIL $Name $Detail" -ForegroundColor Red
        $script:failures.Add($Name)
    }
}

# A mock left behind by an interrupted run holds the port and every case then fails as though
# the downloader were at fault.
Get-Process pwsh -ErrorAction SilentlyContinue | ForEach-Object {
    $commandLine = (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.Id)" -ErrorAction SilentlyContinue).CommandLine
    if ($commandLine -like "*Start-ContentAssetMock*") { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
}

$env:DAR_QA_CONTENT_BASE = "http://127.0.0.1:$Port"
$expected = Get-ChildItem -LiteralPath $AssetRoot -Filter "xtra_dead_air_x64_content_*"

Write-Host "clean fetch and commit"
New-Installation
$mock = Start-Mock
try {
    $code = Invoke-Updater @("--content-fetch", "--game-dir", $WorkRoot)
    Assert-Case "fetch succeeds" ($code -eq 0) "exit $code, $(Get-Result)"
    $cached = Get-ChildItem (Join-Path $WorkRoot ".dead-air-x64\content-cache") -File |
        Where-Object { $_.Name -match '^[0-9a-f]{64}$' }
    Assert-Case "every bundle is in the cache" ($cached.Count -eq $expected.Count) "$($cached.Count) of $($expected.Count)"

    $code = Invoke-Updater @("--content-commit", "--game-dir", $WorkRoot)
    Assert-Case "commit succeeds" ($code -eq 0) "exit $code, $(Get-Result)"
    $installed = Get-ChildItem (Join-Path $WorkRoot "database") -Filter "xtra_dead_air_x64_content_*"
    Assert-Case "every bundle is installed" ($installed.Count -eq $expected.Count) "$($installed.Count) of $($expected.Count)"
    Assert-Case "the latch is gone" (-not (Test-Path (Join-Path $WorkRoot ".dead-air-x64\content-incomplete.txt")))

    $code = Invoke-Updater @("--content-plan", "--game-dir", $WorkRoot)
    Assert-Case "a complete installation plans no work" ((Get-Result) -match "missing=0") (Get-Result)
}
finally { Stop-Mock $mock }

Write-Host "a server that keeps dropping is walked through by resume"
New-Installation
$largest = $expected | Sort-Object Length -Descending | Select-Object -First 1
$mockLog = Join-Path $WorkRoot "drop-mock.log"
$mock = Start-Mock @{ DropAfter = [long]($largest.Length / 4) } $mockLog
try {
    $code = Invoke-Updater @("--content-fetch", "--game-dir", $WorkRoot)
    Assert-Case "a repeatedly dropped transfer still completes" ($code -eq 0) "exit $code, $(Get-Result)"
}
finally { Stop-Mock $mock }

# The proof that it resumed rather than restarted: the server saw Range requests from a
# non-zero offset. Without that the case would pass on a downloader that simply retried from
# the beginning every time.
$resumed = (Select-String -LiteralPath $mockLog -Pattern 'range=[1-9]' -ErrorAction SilentlyContinue)
Assert-Case "it resumed instead of restarting" ($resumed -and $resumed.Count -gt 0)

$mock = Start-Mock
try {
    $code = Invoke-Updater @("--content-commit", "--game-dir", $WorkRoot)
    Assert-Case "the resumed content commits" ($code -eq 0) "exit $code, $(Get-Result)"
}
finally { Stop-Mock $mock }

Write-Host "a server that ignores Range still works"
New-Installation
$mock = Start-Mock @{ DropAfter = [long]($largest.Length / 3) }
try { Invoke-Updater @("--content-fetch", "--game-dir", $WorkRoot) | Out-Null }
finally { Stop-Mock $mock }
$mock = Start-Mock @{ NoRange = $true }
try {
    $code = Invoke-Updater @("--content-fetch", "--game-dir", $WorkRoot)
    Assert-Case "a 200 answer to a Range request restarts cleanly" ($code -eq 0) "exit $code, $(Get-Result)"
}
finally { Stop-Mock $mock }

Write-Host "a corrupt cache entry is not committed"
New-Installation
$mock = Start-Mock
try {
    Invoke-Updater @("--content-fetch", "--game-dir", $WorkRoot) | Out-Null
    $victim = Get-ChildItem (Join-Path $WorkRoot ".dead-air-x64\content-cache") -File |
        Where-Object { $_.Name -match '^[0-9a-f]{64}$' } | Select-Object -First 1
    $bytes = [IO.File]::ReadAllBytes($victim.FullName)
    $bytes[[int]($bytes.Length / 2)] = $bytes[[int]($bytes.Length / 2)] -bxor 0xFF
    [IO.File]::WriteAllBytes($victim.FullName, $bytes)

    $code = Invoke-Updater @("--content-commit", "--game-dir", $WorkRoot)
    Assert-Case "the commit refuses a tampered cache file" ($code -ne 0) "exit $code, $(Get-Result)"
}
finally { Stop-Mock $mock }

Write-Host "a changed bundle arrives as a delta, not as a whole bundle"
# Two revisions of one bundle, a published delta between them, and an installation sitting on
# the older one. The point of the case is not that the bytes come out right - the applier is
# tested directly elsewhere - it is that the client CHOOSES the delta: the resolver has to find
# a base among the files it is about to call obsolete, and the downloader has to fetch and apply
# the chain instead of the bundle.
$deltaTool = Join-Path $PSScriptRoot "..\..\bin\x64\Release\DarDelta.exe"
if (-not (Test-Path -LiteralPath $deltaTool -PathType Leaf)) {
    # A case that cannot run is not a case that passed. This suite is a release gate, and a run
    # that quietly drops the only delta coverage and still prints "all cases passed" is worse
    # than one that fails: it is a green light for something nobody looked at.
    $failures.Add("the delta case did not run: DarDelta.exe is not built (build it with tools\build\build_x64.ps1)")
    Write-Host "  FAIL the delta case did not run: DarDelta.exe is not built" -ForegroundColor Red
}
else {
    # Beside the work root, not inside it: New-Installation wipes the work root, and the fixture
    # has to survive being installed onto.
    $deltaRoot = "$WorkRoot-delta"
    if (Test-Path -LiteralPath $deltaRoot) { Remove-Item -LiteralPath $deltaRoot -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $deltaRoot | Out-Null
    Copy-Item (Join-Path $AssetRoot "*") $deltaRoot -Force

    # Pick the biggest bundle, since a delta is only taken when it is decisively smaller.
    $victim = Get-ChildItem $deltaRoot -Filter "xtra_dead_air_x64_content_*" |
        Sort-Object Length -Descending | Select-Object -First 1
    $slot = [regex]::Match($victim.Name, '^xtra_dead_air_x64_content_(.+)_[0-9a-f]{16}\.xdb0$').Groups[1].Value

    # A small edit, which is what a delta exists for.
    $bytes = [IO.File]::ReadAllBytes($victim.FullName)
    $middle = [int]($bytes.Length / 2)
    for ($i = 0; $i -lt 4096; $i++) { $bytes[$middle + $i] = [byte]((($i * 29) + 7) % 256) }
    $newBytes = $bytes
    $newHash = [BitConverter]::ToString(
        [Security.Cryptography.SHA256]::HashData($newBytes)).Replace("-", "").ToLower()
    $newName = "xtra_dead_air_x64_content_${slot}_" + $newHash.Substring(0, 16) + ".xdb0"
    [IO.File]::WriteAllBytes((Join-Path $deltaRoot $newName), $newBytes)

    $oldHash = (Get-FileHash -Algorithm SHA256 $victim.FullName).Hash.ToLower()
    $patchName = "content_${slot}_" + $oldHash.Substring(0, 16) + "_to_" + $newHash.Substring(0, 16) + ".darpatch"
    & $deltaTool $victim.FullName (Join-Path $deltaRoot $newName) (Join-Path $deltaRoot $patchName) | Out-Null
    $patch = Get-Item (Join-Path $deltaRoot $patchName)

    # The v2 manifest: the same bundles with one replaced, plus the edge between the two.
    $lines = [Collections.Generic.List[string]]::new()
    $bundleRows = [Collections.Generic.List[string]]::new()
    $inBundles = $false
    foreach ($line in [IO.File]::ReadAllLines($Manifest)) {
        if ($line -eq "[bundles]") { $inBundles = $true; continue }
        if ($line -eq "[deltas]") { $inBundles = $false; continue }
        if (-not $inBundles) {
            if ($line.StartsWith("content-id=")) { continue }
            $lines.Add($line)
            continue
        }
        if (-not $line.Trim()) { continue }
        $f = $line -split "`t"
        if ($f[2] -eq $victim.Name) {
            $bundleRows.Add($newHash + "`t" + $newBytes.Length + "`t" + $newName + "`t" + $f[3])
        }
        else { $bundleRows.Add($line) }
    }

    $ordered = $bundleRows | Sort-Object { ($_ -split "`t")[2] }
    $idInput = [Text.StringBuilder]::new()
    foreach ($row in $ordered) {
        $f = $row -split "`t"
        [void]$idInput.Append($f[2]).Append("`n").Append($f[0]).Append("`n")
    }
    $contentId = [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData(
        [Text.Encoding]::UTF8.GetBytes($idInput.ToString()))).Replace("-", "").ToLower()

    $out = [Collections.Generic.List[string]]::new()
    $out.Add($lines[0])
    $out.Add($lines[1])
    $out.Add("content-id=$contentId")
    $out.Add($lines[2])
    $out.Add("[bundles]")
    foreach ($row in $ordered) { $out.Add($row) }
    $out.Add("[deltas]")
    $out.Add((Get-FileHash -Algorithm SHA256 $patch.FullName).Hash.ToLower() + "`t" + $patch.Length +
        "`t" + $patchName + "`t" + "content-1.4.0" + "`t" + $victim.Name + "`t" + $oldHash +
        "`t" + $victim.Length + "`t" + $newHash)
    $manifestV2 = Join-Path $deltaRoot "content-manifest-v2.txt"
    [IO.File]::WriteAllText($manifestV2, [string]::Join("`n", $out) + "`n", [Text.UTF8Encoding]::new($false))

    # Install v1, then hand the installation the v2 manifest.
    New-Installation
    $mock = Start-Mock
    try {
        Invoke-Updater @("--content-fetch", "--game-dir", $WorkRoot) | Out-Null
        Invoke-Updater @("--content-commit", "--game-dir", $WorkRoot) | Out-Null
    }
    finally { Stop-Mock $mock }

    Copy-Item -LiteralPath $manifestV2 -Destination (Join-Path $WorkRoot ".dead-air-x64\content-manifest.txt") -Force
    # The mock has to serve the v2 assets, so point it at the directory that holds them.
    $deltaLog = Join-Path $deltaRoot "delta-mock.log"
    $savedRoot = $AssetRoot
    $AssetRoot = $deltaRoot
    $mock = Start-Mock @{} $deltaLog
    try {
        $code = Invoke-Updater @("--content-fetch", "--game-dir", $WorkRoot)
        Assert-Case "the upgrade fetch succeeds" ($code -eq 0) "exit $code, $(Get-Result)"

        $served = Get-Content -LiteralPath $deltaLog -ErrorAction SilentlyContinue
        $tookDelta = $served | Where-Object { $_ -like "*$patchName*" }
        $tookBundle = $served | Where-Object { $_ -like "*$newName*" }
        Assert-Case "it fetched the delta" ([bool]$tookDelta)
        Assert-Case "it did not fetch the whole bundle" (-not $tookBundle)

        $code = Invoke-Updater @("--content-commit", "--game-dir", $WorkRoot)
        Assert-Case "the rebuilt bundle commits" ($code -eq 0) "exit $code, $(Get-Result)"
        Assert-Case "the rebuilt bundle is installed" `
            (Test-Path (Join-Path $WorkRoot "database\$newName"))
    }
    finally {
        Stop-Mock $mock
        $AssetRoot = $savedRoot
    }
}

Write-Host ""
if ($failures.Count -eq 0) {
    Write-Host "all content flow cases passed" -ForegroundColor Green
}
else {
    Write-Host "FAILURES:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
