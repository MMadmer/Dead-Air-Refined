# Install-time content flow, against a real binary and a real server.
#
# Everything here runs the shipped DeadAirUpdater.exe in its content modes against a scratch
# installation and the release mock (tools\qa\release-mock, Go), because the interesting failures
# are not in any one function - they are in what a resumed part file, a dropped connection or a
# server that ignores Range leaves behind for the next stage to find.
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
$failures = [Collections.Generic.List[string]]::new()

# The mock is built from source next to the work root, never kept as a binary in the repository.
$toolRoot = "$WorkRoot-tools"
New-Item -ItemType Directory -Force -Path $toolRoot | Out-Null
$mockExe = Join-Path $toolRoot "release-mock.exe"
Push-Location (Join-Path $PSScriptRoot "release-mock")
try {
    & go build -o $mockExe .
    if ($LASTEXITCODE -ne 0) { throw "the release mock did not build" }
}
finally { Pop-Location }

function New-Installation {
    Remove-Item -LiteralPath $WorkRoot -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path (Join-Path $WorkRoot "database"),
        (Join-Path $WorkRoot ".dead-air-x64") | Out-Null
    Copy-Item -LiteralPath $Manifest -Destination (Join-Path $WorkRoot ".dead-air-x64\content-manifest.txt") -Force
}

function Start-Mock {
    param([hashtable]$Options = @{}, [string]$LogPath, [string]$Content = $AssetRoot)
    if (-not $LogPath) { $LogPath = Join-Path $toolRoot "mock.jsonl" }
    $arguments = @("-listen", "127.0.0.1:$Port", "-content", "`"$Content`"", "-log", "`"$LogPath`"", "-timeout", "10m")
    if ($Options.NoRange) { $arguments += "-norange" }
    if ($Options.DropAfter) { $arguments += @("-drop-after", $Options.DropAfter) }
    if ($Options.Throttle) { $arguments += @("-throttle", $Options.Throttle) }
    if ($Options.ExpireAfter) { $arguments += @("-expire-after", $Options.ExpireAfter) }
    $process = Start-Process -FilePath $mockExe -ArgumentList $arguments -PassThru -WindowStyle Hidden

    # Poll the port rather than sleeping: a fixed wait produces failures that look like
    # downloader bugs and are not.
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        if ($process.HasExited) { break }
        try {
            $probe = [Net.Sockets.TcpClient]::new()
            $probe.Connect("127.0.0.1", $Port)
            $probe.Close()
            return $process
        }
        catch { Start-Sleep -Milliseconds 50 }
    }
    throw "the release mock did not start listening on port $Port"
}

function Stop-Mock {
    param($Process)
    if ($Process) { $Process | Stop-Process -Force -ErrorAction SilentlyContinue; $Process.WaitForExit(5000) | Out-Null }
}

function Invoke-Updater {
    param([string[]]$Arguments)
    # The updater is a /SUBSYSTEM:WINDOWS binary, so a plain call does not wait for it.
    # Start-Process joins the list with bare spaces: a path with a space must arrive quoted.
    $quoted = $Arguments | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }
    $process = Start-Process -FilePath $Updater -ArgumentList $quoted -PassThru -NoNewWindow
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

# What the mock actually sent for one asset, over every request.
function Get-Served {
    param([string]$LogPath, [string]$Name)
    $total = 0L
    foreach ($line in Get-Content -LiteralPath $LogPath -ErrorAction SilentlyContinue) {
        $entry = $line | ConvertFrom-Json
        if ($entry.path.EndsWith("/$Name") -and $entry.status -in 200, 206) { $total += [long]$entry.bytes }
    }
    return $total
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

$env:DAR_QA_CONTENT_BASE = "http://127.0.0.1:$Port"
# The manifest's bundles, not the folder's: a bundle cache also keeps earlier revisions.
$declared = Get-Content -LiteralPath $Manifest | ForEach-Object { ($_ -split "`t")[2] } | Where-Object { $_ -like "*.xdb0" }
$expected = $declared | ForEach-Object { Get-Item -LiteralPath (Join-Path $AssetRoot $_) }

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
$mockLog = Join-Path $toolRoot "drop-mock.jsonl"
$mock = Start-Mock @{ DropAfter = [long]($largest.Length / 4) } $mockLog
try {
    $code = Invoke-Updater @("--content-fetch", "--game-dir", $WorkRoot)
    Assert-Case "a repeatedly dropped transfer still completes" ($code -eq 0) "exit $code, $(Get-Result)"
}
finally { Stop-Mock $mock }

# The proof that it resumed rather than restarted: the server saw Range requests from a
# non-zero offset. Without that the case would pass on a downloader that simply retried from
# the beginning every time.
$resumed = @(Get-Content -LiteralPath $mockLog -ErrorAction SilentlyContinue |
    Where-Object { ($_ | ConvertFrom-Json).range -match '^bytes=[1-9]' })
Assert-Case "it resumed instead of restarting" ($resumed.Count -gt 0)

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

# ---- rebuilds from chunk maps -------------------------------------------------------------------
# A second revision of the largest bundle with a small edit in the middle, its chunk map, and an
# installation sitting on the first revision. The client has to choose the rebuild - find the
# revision it holds, fetch the map, ask only for the changed runs - from every place a previous
# revision can be, and fall back to the whole bundle whenever the rebuild cannot work.
$chunkTool = Join-Path $PSScriptRoot "..\..\bin\x64\Release\DarDelta.exe"
if (-not (Test-Path -LiteralPath $chunkTool -PathType Leaf)) {
    # A case that cannot run is not a case that passed. This suite is a release gate, and a run
    # that quietly drops the rebuild coverage and still prints "all cases passed" is worse than
    # one that fails: it is a green light for something nobody looked at.
    $failures.Add("the rebuild cases did not run: DarDelta.exe is not built (build it with tools\build\build_x64.ps1)")
    Write-Host "  FAIL the rebuild cases did not run: DarDelta.exe is not built" -ForegroundColor Red
}
else {
    # Beside the work root, not inside it: New-Installation wipes the work root, and the fixture
    # has to survive being installed onto.
    $v2Root = "$WorkRoot-v2"
    if (Test-Path -LiteralPath $v2Root) { Remove-Item -LiteralPath $v2Root -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $v2Root | Out-Null
    $expected | Copy-Item -Destination $v2Root -Force

    $victim = Get-ChildItem $v2Root -Filter "xtra_dead_air_x64_content_*.xdb0" | Sort-Object Length -Descending | Select-Object -First 1
    $slot = [regex]::Match($victim.Name, '^xtra_dead_air_x64_content_(.+)_[0-9a-f]{16}\.xdb0$').Groups[1].Value
    $oldHash = (Get-FileHash -Algorithm SHA256 $victim.FullName).Hash.ToLower()

    # Two small edits far apart, which is what a rebuild exists for - and two runs to fetch, so the
    # storage URL of the first is reused for the second.
    $bytes = [IO.File]::ReadAllBytes($victim.FullName)
    foreach ($at in [int]($bytes.Length / 4), [int]($bytes.Length * 3 / 4)) {
        for ($i = 0; $i -lt 4096; $i++) { $bytes[$at + $i] = [byte]((($i * 29) + 7) % 256) }
    }
    $newHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLower()
    $newName = "xtra_dead_air_x64_content_${slot}_" + $newHash.Substring(0, 16) + ".xdb0"
    [IO.File]::WriteAllBytes((Join-Path $v2Root $newName), $bytes)
    Remove-Item -LiteralPath $victim.FullName -Force
    $newSize = $bytes.Length
    & $chunkTool --chunk-map (Join-Path $v2Root $newName) (Join-Path $v2Root "$newName.chunks") | Out-Null
    Assert-Case "the chunk map is cut" ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $v2Root "$newName.chunks")))

    # The v2 manifest: the same bundles with one replaced.
    $header = [Collections.Generic.List[string]]::new()
    $rows = [Collections.Generic.List[string]]::new()
    $inBundles = $false
    foreach ($line in [IO.File]::ReadAllLines($Manifest)) {
        if ($line -eq "[bundles]") { $inBundles = $true; continue }
        if ($line -eq "[deltas]") { $inBundles = $false; continue }
        if (-not $inBundles) {
            if ($line -and -not $line.StartsWith("content-id=")) { $header.Add($line) }
            continue
        }
        if (-not $line.Trim()) { continue }
        $f = $line -split "`t"
        if ($f[2] -eq $victim.Name) { $rows.Add($newHash + "`t" + $newSize + "`t" + $newName + "`t" + $f[3]) }
        else { $rows.Add($line) }
    }
    $ordered = [Linq.Enumerable]::ToArray([Linq.Enumerable]::OrderBy([string[]]$rows,
        [Func[string, string]] { param($r) ($r -split "`t")[2] }, [StringComparer]::Ordinal))
    $idInput = [Text.StringBuilder]::new()
    foreach ($row in $ordered) { $f = $row -split "`t"; [void]$idInput.Append($f[2]).Append("`n").Append($f[0]).Append("`n") }
    $contentId = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData(
        [Text.Encoding]::UTF8.GetBytes($idInput.ToString()))).ToLower()
    $out = [Collections.Generic.List[string]]::new()
    $out.Add($header[0]); $out.Add($header[1]); $out.Add("content-id=$contentId"); $out.Add($header[2])
    $out.Add("[bundles]")
    foreach ($row in $ordered) { $out.Add($row) }
    $out.Add("[deltas]")
    $manifestV2 = Join-Path $v2Root "content-manifest-v2.txt"
    [IO.File]::WriteAllText($manifestV2, [string]::Join("`n", $out) + "`n", [Text.UTF8Encoding]::new($false))

    # Installs v1 from the original assets, then hands the installation the v2 manifest.
    function Install-V1 {
        New-Installation
        $mock = Start-Mock
        try {
            Invoke-Updater @("--content-fetch", "--game-dir", $WorkRoot) | Out-Null
            Invoke-Updater @("--content-commit", "--game-dir", $WorkRoot) | Out-Null
        }
        finally { Stop-Mock $mock }
        Copy-Item -LiteralPath $manifestV2 -Destination (Join-Path $WorkRoot ".dead-air-x64\content-manifest.txt") -Force
    }

    function Invoke-V2Fetch {
        param([string]$Label, [hashtable]$Options = @{})
        $log = Join-Path $toolRoot "$Label.jsonl"
        $mock = Start-Mock $Options $log $v2Root
        try { $code = Invoke-Updater @("--content-fetch", "--game-dir", $WorkRoot) }
        finally { Stop-Mock $mock }
        return [pscustomobject]@{ Code = $code; Served = (Get-Served $log $newName); Map = (Get-Served $log "$newName.chunks"); Log = $log }
    }

    Write-Host "a changed bundle is rebuilt from the revision in database\, not downloaded whole"
    Install-V1
    $run = Invoke-V2Fetch "rebuild"
    Assert-Case "the rebuild fetch succeeds" ($run.Code -eq 0) "exit $($run.Code), $(Get-Result)"
    Assert-Case "it read the chunk map" ($run.Map -gt 0)
    Assert-Case "it fetched a small part of the bundle" ($run.Served -gt 0 -and $run.Served -lt $newSize / 4) "$($run.Served) of $newSize bytes"
    $code = Invoke-Updater @("--content-commit", "--game-dir", $WorkRoot)
    Assert-Case "the rebuilt bundle commits" ($code -eq 0) "exit $code, $(Get-Result)"
    Assert-Case "the rebuilt bundle is installed" (Test-Path (Join-Path $WorkRoot "database\$newName"))
    Assert-Case "the old revision is retired into the cache" ((-not (Test-Path (Join-Path $WorkRoot "database\$($victim.Name)"))) -and
        (Test-Path (Join-Path $WorkRoot ".dead-air-x64\content-cache\$oldHash")))
    Write-Host "    fetched $($run.Served) of $newSize bytes"

    Write-Host "a revision an older client retired into the cache is rebuilt from"
    Install-V1
    # What a 1.4-1.5 commit leaves after an update whose content did not arrive: the old revision
    # renamed into the cache under its hash, its name only in content-state.txt.
    Move-Item -LiteralPath (Join-Path $WorkRoot "database\$($victim.Name)") `
        -Destination (Join-Path $WorkRoot ".dead-air-x64\content-cache\$oldHash") -Force
    $run = Invoke-V2Fetch "retired"
    Assert-Case "the fetch succeeds" ($run.Code -eq 0) "exit $($run.Code), $(Get-Result)"
    Assert-Case "it rebuilt from the cache" ($run.Served -gt 0 -and $run.Served -lt $newSize / 4) "$($run.Served) of $newSize bytes"
    $code = Invoke-Updater @("--content-commit", "--game-dir", $WorkRoot)
    Assert-Case "and commits" ($code -eq 0) "exit $code, $(Get-Result)"

    Write-Host "a rebuild walks through dropped connections"
    Install-V1
    $run = Invoke-V2Fetch "rebuild-drop" @{ DropAfter = 20000 }
    Assert-Case "the dropped rebuild completes" ($run.Code -eq 0) "exit $($run.Code), $(Get-Result)"
    Assert-Case "and still fetched a small part" ($run.Served -lt $newSize / 4) "$($run.Served) of $newSize bytes"

    Write-Host "a storage URL that expires sends the next request back through the redirect"
    Install-V1
    $run = Invoke-V2Fetch "rebuild-expire" @{ ExpireAfter = 1 }
    $refused = @(Get-Content -LiteralPath $run.Log | Where-Object { ($_ | ConvertFrom-Json).status -eq 403 })
    Assert-Case "the fetch succeeds" ($run.Code -eq 0) "exit $($run.Code), $(Get-Result)"
    Assert-Case "an expired URL was refused and the rebuild went on" ($refused.Count -gt 0 -and $run.Served -lt $newSize / 4) "$($refused.Count) refusal(s), $($run.Served) bytes"

    Write-Host "a rebuild against a server that ignores Range falls back to the whole bundle"
    Install-V1
    $run = Invoke-V2Fetch "rebuild-norange" @{ NoRange = $true }
    Assert-Case "the fetch still succeeds" ($run.Code -eq 0) "exit $($run.Code), $(Get-Result)"
    $code = Invoke-Updater @("--content-commit", "--game-dir", $WorkRoot)
    Assert-Case "and commits" ($code -eq 0) "exit $code, $(Get-Result)"

    Write-Host "a bundle without a chunk map is downloaded whole"
    Install-V1
    Move-Item -LiteralPath (Join-Path $v2Root "$newName.chunks") -Destination "$v2Root.chunks-aside" -Force
    try { $run = Invoke-V2Fetch "nomap" }
    finally { Move-Item -LiteralPath "$v2Root.chunks-aside" -Destination (Join-Path $v2Root "$newName.chunks") -Force }
    Assert-Case "the fetch succeeds" ($run.Code -eq 0) "exit $($run.Code), $(Get-Result)"
    Assert-Case "it fetched the whole bundle" ($run.Served -ge $newSize) "$($run.Served) of $newSize bytes"
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
