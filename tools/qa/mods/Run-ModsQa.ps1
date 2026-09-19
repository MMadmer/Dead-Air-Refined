<#
.SYNOPSIS
    Acceptance test of the XMS module update flow inside the real game (docs/dead-air/MOD_UPDATES.md).

.DESCRIPTION
    Builds an isolated QA root next to the installed game (<GameRoot>\_qa\mods) and runs the freshly
    built engine there on a hidden desktop, so the player's root, gamedata, database, appdata, modules
    and saves are never touched. The fixture - a set of installed modules and the release assets
    published for them - is generated into the QA root on every run, and a loopback mock
    (Start-ContentAssetMock.ps1) stands in for github.com: throttled and dropping every connection
    mid-body, so a large file only arrives by resuming. The fixture writes descriptors, file indexes
    and packages on its own, from the contract text - an independent second reading of it next to
    XFined Editor's packager.

    The engine is driven through its console only (xms_mods, xms_update, xms_update_status appended
    to the QA user.ltx). Nothing is clicked: every verdict comes from the engine log, the mock's byte
    count and what is on disk afterwards. Window captures are saved next to the results as evidence.

    Phases:
      Update   check every module, take every available release, stage it; a release that differs
               in a few files downloads those files only and reuses a renamed one
      Apply    the next start swaps the staged modules in, after waiting for the process a relaunch
               names in DAR_RELAUNCH_WAIT_PID
      Locked   a module folder held open by another process keeps its update staged, and takes it on
               the start after the handle is gone
      Recover  a swap interrupted between its two renames is completed; an update staged for a
               module the player has since deleted is discarded

.EXAMPLE
    pwsh -NoProfile -File tools\qa\mods\Run-ModsQa.ps1 -ResultLabel run1
#>
[CmdletBinding()]
param(
    [string]$GameRoot = 'D:\Games\Dead Air',

    [string]$BuildRoot,

    [ValidatePattern('^[A-Za-z0-9_.-]+$')]
    [string]$ResultLabel = (Get-Date -Format 'yyyyMMdd-HHmmss'),

    [int]$Port = 8793,

    [int]$TimeoutSeconds = 240,

    # 1280x720 exercises ui_mods_16.xml, 1024x768 ui_mods.xml.
    [string]$VidMode = '1280x720',

    [switch]$KeepRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------- layout
$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $BuildRoot) { $BuildRoot = Join-Path $repositoryRoot 'bin\x64\Release' }
$GameRoot = (Resolve-Path -LiteralPath $GameRoot).Path

$qaRoot = Join-Path $GameRoot '_qa\mods'
$qaAppData = Join-Path $qaRoot 'appdata'
$qaGameData = Join-Path $qaRoot 'gamedata'
$qaModules = Join-Path $qaRoot 'modules'
$qaStaged = Join-Path $qaModules '.staged'
$qaAssets = Join-Path $qaRoot 'fixture-assets'
$qaEngine = Join-Path $qaRoot 'xrEngine.exe'
$qaLog = Join-Path $qaAppData 'logs\openxray_admin.log'

$compatGameData = Join-Path $repositoryRoot 'packaging\dead-air-x64\compatibility\gamedata'
$manifestPath = Join-Path $repositoryRoot 'packaging\dead-air-x64\installer\runtime-files.txt'
$helper = Join-Path $repositoryRoot 'tools\qa\Start-DetachedHiddenDesktopProcess.ps1'
$capture = Join-Path $repositoryRoot 'tools\qa\Capture-HiddenDesktopWindow.ps1'
$mock = Join-Path $repositoryRoot 'tools\qa\Start-ContentAssetMock.ps1'

$resultRoot = Join-Path $PSScriptRoot "results\$ResultLabel"
$layersKey = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers'
$utf8 = [Text.UTF8Encoding]::new($false)

foreach ($required in @($manifestPath, $helper, $capture, $mock, $compatGameData)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing mods QA dependency: $required" }
}
New-Item -ItemType Directory -Path $resultRoot -Force | Out-Null

$script:checks = [Collections.Generic.List[object]]::new()
function Assert-That {
    param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][bool]$Condition, [string]$Detail = '')
    $script:checks.Add([pscustomobject]@{ Name = $Name; Pass = $Condition; Detail = $Detail })
    Write-Host ("   {0} {1}{2}" -f $(if ($Condition) { 'PASS' } else { 'FAIL' }), $Name, $(if ($Detail) { " - $Detail" } else { '' }))
}

function Copy-TreeContents {
    param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination)
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $output = & robocopy $Source $Destination /S /NFL /NDL /NJH /NJS /NP /R:2 /W:1
    if ($LASTEXITCODE -ge 8) { throw "robocopy '$Source' -> '$Destination' failed ($LASTEXITCODE): $output" }
    $global:LASTEXITCODE = 0
}

function Assert-NoTargetEngine {
    $running = @(Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $qaEngine })
    if ($running.Count) { throw "The QA engine is already running (pid $($running.Id -join ', ')); stop it first." }
}

function Get-Sha256 {
    # an empty file is a legitimate member of a release, and an empty array a legitimate argument
    param([Parameter(Mandatory)][AllowEmptyCollection()][byte[]]$Bytes)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hasher.ComputeHash($Bytes)).Replace('-', '').ToLowerInvariant() }
    finally { $hasher.Dispose() }
}

# ---------------------------------------------------------------------------- QA root
function New-QaRoot {
    Write-Host "== building the QA root: $qaRoot"
    if (Test-Path -LiteralPath $qaRoot) {
        # The database entry is a junction; drop the link before the tree so the real content stays.
        $databaseLink = Join-Path $qaRoot 'database'
        if (Test-Path -LiteralPath $databaseLink) { [IO.Directory]::Delete($databaseLink, $false) }
        Remove-Item -LiteralPath $qaRoot -Recurse -Force
    }
    foreach ($directory in @($qaRoot, $qaAppData, (Join-Path $qaAppData 'logs'), (Join-Path $qaAppData 'savedgames'))) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }

    $manifest = @(Get-Content -LiteralPath $manifestPath | Where-Object { $_.Trim() -ne '' })
    foreach ($file in $manifest) {
        $built = Join-Path $BuildRoot $file
        $installed = Join-Path $GameRoot $file
        $source = if (Test-Path -LiteralPath $built) { $built } elseif (Test-Path -LiteralPath $installed) { $installed }
            else { throw "Neither the build nor the installed game has the runtime file: $file" }
        Copy-Item -LiteralPath $source -Destination (Join-Path $qaRoot $file) -Force
    }
    Get-ChildItem -LiteralPath $GameRoot -File -Filter '*.dll' | Where-Object { $manifest -notcontains $_.Name } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $qaRoot $_.Name) -Force }

    New-Item -ItemType Junction -Path (Join-Path $qaRoot 'database') -Target (Join-Path $GameRoot 'database') | Out-Null
    Copy-Item -LiteralPath (Join-Path $GameRoot 'fsgame.ltx') -Destination (Join-Path $qaRoot 'fsgame.ltx') -Force

    # COPIED, never linked: the content service writes its state and latch files there.
    $meta = Join-Path $qaRoot '.dead-air-x64'
    New-Item -ItemType Directory -Path $meta -Force | Out-Null
    foreach ($file in @('content-manifest.txt', 'content-state.txt')) {
        $source = Join-Path $GameRoot ".dead-air-x64\$file"
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $meta -Force }
    }
    foreach ($cache in @('shaders_cache', 'shaders_cache_oxr', 'render-hardware.cache')) {
        $source = Join-Path $GameRoot "appdata\$cache"
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $qaAppData -Recurse -Force }
    }
    Copy-TreeContents -Source (Join-Path $GameRoot 'gamedata') -Destination $qaGameData
    Copy-TreeContents -Source $compatGameData -Destination $qaGameData
}

function Set-QaUserLtx {
    param([string[]]$Commands = @())
    $userLtxSource = Join-Path $GameRoot 'appdata\user.ltx'
    $userLtx = if (Test-Path -LiteralPath $userLtxSource) { @(Get-Content -LiteralPath $userLtxSource) } else { @() }
    $overrides = [ordered]@{
        'renderer' = 'renderer_r4'; 'vid_mode' = $VidMode; 'vid_window_mode' = 'st_opt_windowed'
        'rs_fullscreen' = 'off'; 'rs_v_sync' = 'off'; 'rs_fps_limit' = '60'; 'g_pause_in_background' = '0'
        'snd_volume_music' = '0'; 'snd_volume_eff' = '0'
    }
    $userLtx = @($userLtx | Where-Object {
            $line = $_
            -not (@($overrides.Keys | Where-Object { $line -match "^\s*$_\s" }).Count)
        })
    foreach ($key in $overrides.Keys) { $userLtx += "$key $($overrides[$key])" }
    Set-Content -LiteralPath (Join-Path $qaAppData 'user.ltx') -Value ($userLtx + $Commands) -Encoding Default
}

# ---------------------------------------------------------------------------- fixture
function New-ManifestText {
    param([string]$Id, [string]$Name, [string]$Version, [string]$Author = '', [string]$Description = '',
        [string]$Website = '', [string]$Github = '')
    $lines = @('[module]', "id          = $Id", "name        = $Name", "version     = $Version", 'api         = 1')
    if ($Author) { $lines += "author      = $Author" }
    if ($Description) {
        $escaped = $Description.Replace('\', '\\').Replace('"', '\"').Replace("`n", '\n')
        $lines += "description = `"$escaped`""
    }
    if ($Website) { $lines += "website     = $Website" }
    $lines += 'target      = default'
    if ($Github) { $lines += @('', '[update]', "github      = $Github") }
    return (($lines -join "`r`n") + "`r`n")
}

function Install-Module {
    param([string]$Id, [string]$ManifestText, [hashtable]$Files = @{})
    $root = Join-Path $qaModules $Id
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'mod.ltx'), $ManifestText, $utf8)
    foreach ($relative in $Files.Keys) {
        $path = Join-Path $root $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllBytes($path, [byte[]]$Files[$relative])
    }
}

function Get-ZipLayout {
    # Where the data of every entry sits, read back from the finished archive: central directory
    # for the method and the packed size, local header for the two lengths that precede the data.
    param([Parameter(Mandatory)][string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    $end = -1
    for ($i = $bytes.Length - 22; $i -ge 0; --$i) {
        if ([BitConverter]::ToUInt32($bytes, $i) -eq 0x06054b50) { $end = $i; break }
    }
    if ($end -lt 0) { throw "no end of central directory in $Path" }
    $count = [BitConverter]::ToUInt16($bytes, $end + 10)
    $cursor = [int][BitConverter]::ToUInt32($bytes, $end + 16)
    $layout = @{}
    for ($n = 0; $n -lt $count; ++$n) {
        $nameLength = [BitConverter]::ToUInt16($bytes, $cursor + 28)
        $local = [int][BitConverter]::ToUInt32($bytes, $cursor + 42)
        $name = [Text.Encoding]::UTF8.GetString($bytes, $cursor + 46, $nameLength)
        $layout[$name] = [pscustomobject]@{
            Method = [BitConverter]::ToUInt16($bytes, $cursor + 10)
            Packed = [BitConverter]::ToUInt32($bytes, $cursor + 20)
            Offset = $local + 30 + [BitConverter]::ToUInt16($bytes, $local + 26) + [BitConverter]::ToUInt16($bytes, $local + 28)
        }
        $cursor += 46 + $nameLength + [BitConverter]::ToUInt16($bytes, $cursor + 30) + [BitConverter]::ToUInt16($bytes, $cursor + 32)
    }
    return $layout
}

function Publish-Release {
    # Descriptor, file index and $Parts package(s). -HostilePath renames one file in the index to a
    # path the contract forbids; -Tamper flips a byte of stored data after the index was made.
    param([string]$Id, [string]$Version, [string]$ManifestText, [hashtable]$Files = @{}, [string]$RequiresGame = '',
        [int]$Parts = 1, [string]$HostilePath, [switch]$Tamper)
    $entries = @{ 'mod.ltx' = $utf8.GetBytes($ManifestText) }
    foreach ($relative in $Files.Keys) { $entries[$relative] = [byte[]]$Files[$relative] }
    $names = @($entries.Keys | Sort-Object)

    $packages = @()
    $rows = @()
    for ($part = 0; $part -lt $Parts; ++$part) {
        $asset = if ($Parts -eq 1) { "$Id-$Version.zip" } else { "$Id-$Version.part$($part + 1).zip" }
        $path = Join-Path $qaAssets $asset
        $stream = [IO.File]::Create($path)
        $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
        $held = @()
        for ($index = $part; $index -lt $names.Count; $index += $Parts) {
            $entry = $zip.CreateEntry("modules/$Id/$($names[$index])", [IO.Compression.CompressionLevel]::Optimal)
            $target = $entry.Open()
            $bytes = [byte[]]$entries[$names[$index]]
            $target.Write($bytes, 0, $bytes.Length)
            $target.Dispose()
            $held += $names[$index]
        }
        $zip.Dispose()
        $stream.Dispose()

        $layout = Get-ZipLayout -Path $path
        foreach ($name in $held) {
            $place = $layout["modules/$Id/$name"]
            $bytes = [byte[]]$entries[$name]
            $listed = if ($HostilePath -and $name -ne 'mod.ltx') { $HostilePath; $HostilePath = $null } else { $name }
            $rows += "file $(Get-Sha256 $bytes) $($bytes.Length) $($part + 1) $($place.Offset) $($place.Packed) $($place.Method) $listed"
        }
        if ($Tamper -and $part -eq 0) {
            $place = $layout["modules/$Id/mod.ltx"]
            $archive = [IO.File]::Open($path, 'Open', 'ReadWrite')
            $archive.Position = $place.Offset + 1
            $byte = $archive.ReadByte()
            $archive.Position = $place.Offset + 1
            $archive.WriteByte($byte -bxor 0xFF)
            $archive.Dispose()
        }
        $packages += [pscustomobject]@{ Name = $asset; Line = "$asset = $((Get-Item -LiteralPath $path).Length), $((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant())" }
    }

    $indexName = "$Id-$Version.files"
    $indexText = (@('xms-files 1') + @($packages | ForEach-Object { "pack $($_.Name)" }) + $rows) -join "`n"
    $indexBytes = $utf8.GetBytes($indexText + "`n")
    [IO.File]::WriteAllBytes((Join-Path $qaAssets $indexName), $indexBytes)

    $total = 0L
    foreach ($bytes in $entries.Values) { $total += ([byte[]]$bytes).Length }
    $lines = @('; XMS module release descriptor. QA fixture.', '[release]', 'schema        = 1',
        "id            = $Id", "version       = $Version")
    if ($RequiresGame) { $lines += "requires_game = $RequiresGame" }
    $lines += @("files         = $($entries.Count)", "unpacked      = $total",
        "index         = $indexName, $($indexBytes.Length), $(Get-Sha256 $indexBytes)", '', '[packages]')
    $lines += @($packages | ForEach-Object { $_.Line })
    [IO.File]::WriteAllText((Join-Path $qaAssets "$Id.update.ltx"), (($lines -join "`r`n") + "`r`n"), $utf8)
}

function New-Payload {
    param([int]$Bytes, [int]$Seed)
    $buffer = [byte[]]::new($Bytes)
    [Random]::new($Seed).NextBytes($buffer)
    return , $buffer
}

function New-Fixture {
    Write-Host '== generating the fixture'
    foreach ($directory in @($qaModules, $qaAssets)) {
        if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    $text = [Text.Encoding]::ASCII

    # Cyrillic metadata on purpose: the manifest is UTF-8 and the UI is Windows-1251.
    $storyName = 'История Агропрома'
    $storyDescription = "Сюжетная линия на Агропроме; новые квесты.`nФраза в `"кавычках`" и слэш \ тоже доходят до окна."
    $storyArgs = @{ Id = 'qa.story'; Name = $storyName; Author = 'Мадмер'; Description = $storyDescription
        Website = 'https://ap-pro.ru/forums/topic/15259-konkurs-kvestov-2026/'; Github = 'QaOwner/qa-story' }
    Install-Module -Id 'qa.story' -ManifestText (New-ManifestText @storyArgs -Version '1.0.0') -Files @{
        'gamedata\configs\qa_story_marker.ltx' = $text.GetBytes("[qa_story]`r`nversion = 1.0.0`r`n")
        'gamedata\configs\removed_in_next.ltx' = $text.GetBytes("[gone]`r`n")
    }
    Publish-Release -Id 'qa.story' -Version '1.1.0' -ManifestText (New-ManifestText @storyArgs -Version '1.1.0') -Files @{
        'gamedata/configs/qa_story_marker.ltx' = $text.GetBytes("[qa_story]`r`nversion = 1.1.0`r`n")
        'gamedata/configs/empty.ltx'           = [byte[]]::new(0)
        'gamedata/textures/qa/payload.bin'     = (New-Payload -Bytes (3 * 1024 * 1024) -Seed 1)
    }

    # The point of the whole design: a big module that changed in a few files. big.bin stays,
    # old_name.bin comes back as new_name.bin, removed.ltx goes, changed.ltx and added.ltx arrive.
    $script:deltaBig = New-Payload -Bytes (4 * 1024 * 1024) -Seed 10
    $script:deltaMoved = New-Payload -Bytes (300 * 1024) -Seed 11
    $deltaArgs = @{ Id = 'qa.delta'; Name = 'Delta Update'; Github = 'QaOwner/qa-delta' }
    Install-Module -Id 'qa.delta' -ManifestText (New-ManifestText @deltaArgs -Version '1.0.0') -Files @{
        'gamedata\big.bin'      = $script:deltaBig
        'gamedata\old_name.bin' = $script:deltaMoved
        'gamedata\changed.ltx'  = $text.GetBytes("[delta]`r`nrevision = 1`r`n")
        'gamedata\removed.ltx'  = $text.GetBytes("[gone]`r`n")
    }
    # three versions later on purpose: the gap is not supposed to matter
    Publish-Release -Id 'qa.delta' -Version '1.3.0' -ManifestText (New-ManifestText @deltaArgs -Version '1.3.0') -Files @{
        'gamedata/big.bin'            = $script:deltaBig
        'gamedata/moved/new_name.bin' = $script:deltaMoved
        'gamedata/changed.ltx'        = $text.GetBytes("[delta]`r`nrevision = 4`r`n")
        'gamedata/added.ltx'          = $text.GetBytes("[added]`r`n")
    }

    $current = New-ManifestText -Id 'qa.current' -Name 'Current Module' -Version '2.0' -Author 'Somebody' `
        -Website 'https://www.moddb.com/mods/dead-air' -Github 'QaOwner/qa-current'
    Install-Module -Id 'qa.current' -ManifestText $current
    Publish-Release -Id 'qa.current' -Version '2.0.0' -ManifestText $current

    Install-Module -Id 'qa.blocked' -ManifestText (New-ManifestText -Id 'qa.blocked' -Name 'Needs Newer Game' `
            -Version '1.0.0' -Github 'QaOwner/qa-blocked')
    Publish-Release -Id 'qa.blocked' -Version '3.0.0' -RequiresGame '99.0.0' `
        -ManifestText (New-ManifestText -Id 'qa.blocked' -Name 'Needs Newer Game' -Version '3.0.0')

    Install-Module -Id 'qa.nosource' -ManifestText (New-ManifestText -Id 'qa.nosource' -Name 'No Update Source' `
            -Version '0.9' -Website 'https://moddb.com/mods/example')

    # a website outside the allow-list and a malformed repository: both must be dropped
    Install-Module -Id 'qa.badsite' -ManifestText (New-ManifestText -Id 'qa.badsite' -Name 'Bad Links' -Version '1.0.0' `
            -Website 'https://ap-pro.ru@evil.example/steal' -Github 'not a repo')

    Install-Module -Id 'qa.norelease' -ManifestText (New-ManifestText -Id 'qa.norelease' -Name 'No Release Yet' `
            -Version '1.0.0' -Github 'QaOwner/qa-norelease')

    # The author took 1.0.0 down and published 1.0.0 again: the same manifest, one file fixed, one
    # added. No version says so - only the content does.
    $script:reissueKeep = New-Payload -Bytes (1024 * 1024) -Seed 20
    $reissue = New-ManifestText -Id 'qa.reissue' -Name 'Published Again' -Version '1.0.0' -Github 'QaOwner/qa-reissue'
    Install-Module -Id 'qa.reissue' -ManifestText $reissue -Files @{
        'gamedata\keep.bin'  = $script:reissueKeep
        'gamedata\fixed.ltx' = $text.GetBytes("[reissue]`r`nrevision = 1`r`n")
    }
    Publish-Release -Id 'qa.reissue' -Version '1.0.0' -ManifestText $reissue -Files @{
        'gamedata/keep.bin'  = $script:reissueKeep
        'gamedata/fixed.ltx' = $text.GetBytes("[reissue]`r`nrevision = 2`r`n")
        'gamedata/extra.ltx' = $text.GetBytes("[extra]`r`n")
    }

    $secondArgs = @{ Id = 'qa.second'; Name = 'Second Update'; Github = 'QaOwner/qa-second' }
    Install-Module -Id 'qa.second' -ManifestText (New-ManifestText @secondArgs -Version '1.2')
    Publish-Release -Id 'qa.second' -Version '1.3' -Parts 2 -ManifestText (New-ManifestText @secondArgs -Version '1.3') -Files @{
        'gamedata/a.bin' = (New-Payload -Bytes 200000 -Seed 2)
        'gamedata/b.bin' = (New-Payload -Bytes 200000 -Seed 3)
        'gamedata/c.bin' = (New-Payload -Bytes 200000 -Seed 4)
    }

    # an index that names a path outside the module: must fail and leave the module alone
    Install-Module -Id 'qa.broken' -ManifestText (New-ManifestText -Id 'qa.broken' -Name 'Broken Release' `
            -Version '1.0.0' -Github 'QaOwner/qa-broken')
    Publish-Release -Id 'qa.broken' -Version '1.0.1' -HostilePath '../../escape.txt' -ManifestText (New-ManifestText `
            -Id 'qa.broken' -Name 'Broken Release' -Version '1.0.1') -Files @{ 'gamedata/x.ltx' = $text.GetBytes('never written') }

    # a package whose bytes are not the ones its index vouches for
    Install-Module -Id 'qa.tampered' -ManifestText (New-ManifestText -Id 'qa.tampered' -Name 'Tampered Package' `
            -Version '1.0.0' -Github 'QaOwner/qa-tampered')
    Publish-Release -Id 'qa.tampered' -Version '1.0.1' -Tamper -ManifestText (New-ManifestText `
            -Id 'qa.tampered' -Name 'Tampered Package' -Version '1.0.1')
}

# ---------------------------------------------------------------------------- engine
function Start-Mock {
    $arguments = '-NoProfile -File "{0}" -AssetRoot "{1}" -Port {2} -TimeoutSeconds 3600 -Throttle 1500000 -DropAfter 1500000' -f
        $mock, $qaAssets, $Port
    $script:mockLog = Join-Path $resultRoot 'mock.log'
    return Start-Process pwsh -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $script:mockLog
}

function Invoke-Engine {
    # Boots the QA engine, waits for $WaitFor in the log (every pattern), captures, stops the engine.
    param([Parameter(Mandatory)][string]$Phase, [string[]]$Commands = @(), [string[]]$WaitFor = @(),
        [int]$RelaunchParentSeconds = 0, [int]$SettleSeconds = 3)

    Assert-NoTargetEngine
    Set-QaUserLtx -Commands $Commands
    if (Test-Path -LiteralPath $qaLog) { Remove-Item -LiteralPath $qaLog -Force }

    $env:DAR_QA_MOD_UPDATE_BASE = "http://127.0.0.1:$Port"
    # the game's own update check goes to the same mock, which answers 404: no dialog in the way
    $env:DAR_QA_UPDATE_API = "http://127.0.0.1:$Port/no-game-releases"
    $env:DAR_RELAUNCH_WAIT_PID = $null
    if ($RelaunchParentSeconds -gt 0) {
        $parent = Start-Process pwsh -ArgumentList "-NoProfile -Command Start-Sleep $RelaunchParentSeconds" `
            -WindowStyle Hidden -PassThru
        $env:DAR_RELAUNCH_WAIT_PID = "$($parent.Id)"
    }

    New-Item -Path $layersKey -Force | Out-Null
    New-ItemProperty -Path $layersKey -Name $qaEngine -Value '~ HIGHDPIAWARE' -PropertyType String -Force | Out-Null

    $desktop = "DeadAirMods$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
    $processIdFile = Join-Path $resultRoot "$Phase-engine.pid"
    $launcherIdFile = Join-Path $resultRoot "$Phase-launcher.pid"
    Remove-Item -LiteralPath $processIdFile, $launcherIdFile -Force -ErrorAction SilentlyContinue
    $arguments = '-i -fsltx fsgame.ltx -always_active -silent_error_mode -force_flushlog -r4 -nointro -qa_update'
    Write-Host "== [$Phase] xrEngine.exe $arguments"

    $engine = $null
    try {
        & $helper -FilePath $qaEngine -Arguments $arguments -ProcessIdFile $processIdFile `
            -LauncherIdFile $launcherIdFile -DesktopName $desktop -TimeoutSeconds ($TimeoutSeconds + 60) | Out-Null
        $deadline = [DateTime]::UtcNow.AddSeconds(60)
        while (-not (Test-Path -LiteralPath $processIdFile -PathType Leaf)) {
            if ([DateTime]::UtcNow -ge $deadline) { throw 'The hidden launcher did not publish the engine PID.' }
            Start-Sleep -Milliseconds 100
        }
        $engine = Get-Process -Id ([int](Get-Content -LiteralPath $processIdFile -Raw)) -ErrorAction Stop

        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        $pending = @($WaitFor)
        while ($pending.Count -and [DateTime]::UtcNow -lt $deadline -and -not $engine.HasExited) {
            Start-Sleep -Milliseconds 500
            if (-not (Test-Path -LiteralPath $qaLog)) { continue }
            $pending = @($pending | Where-Object {
                    -not (Select-String -LiteralPath $qaLog -Pattern $_ -Quiet -ErrorAction SilentlyContinue) })
        }
        Assert-That "$Phase reached its log state" (-not $pending.Count) $(if ($pending.Count) { "missing: $($pending -join ' | ')" })
        Start-Sleep -Seconds $SettleSeconds
        try { & $capture -DesktopName $desktop -ProcessId $engine.Id -OutputPath (Join-Path $resultRoot "$Phase.png") | Out-Null }
        catch { Write-Warning "capture failed: $_" }
    }
    finally {
        if ($engine -and -not $engine.HasExited) {
            Stop-Process -Id $engine.Id -Force
            # the next phase renames and copies over files this process has mapped
            $engine.WaitForExit(20000) | Out-Null
        }
        if (Test-Path -LiteralPath $launcherIdFile) {
            $launcher = Get-Process -Id ([int](Get-Content -LiteralPath $launcherIdFile -Raw)) -ErrorAction SilentlyContinue
            if ($launcher) { Stop-Process -Id $launcher.Id -Force -ErrorAction SilentlyContinue }
        }
        if (Test-Path -LiteralPath $qaLog) { Copy-Item -LiteralPath $qaLog -Destination (Join-Path $resultRoot "$Phase.log") -Force }
    }
    $log = if (Test-Path -LiteralPath $qaLog) { Get-Content -LiteralPath $qaLog -Raw } else { '' }
    $fatal = $log -match 'FATAL ERROR|stack trace:|Assertion failed'
    Assert-That "$Phase finished without an engine fault" (-not $fatal)
    return $log
}

function Get-ModuleVersion {
    param([string]$Id)
    $manifest = Join-Path $qaModules "$Id\mod.ltx"
    if (-not (Test-Path -LiteralPath $manifest)) { return $null }
    $line = Select-String -LiteralPath $manifest -Pattern '^\s*version\s*=\s*(\S+)' | Select-Object -First 1
    return $(if ($line) { $line.Matches[0].Groups[1].Value } else { $null })
}

function Get-MockBytes {
    # bytes the mock actually sent for one asset, over every request
    param([string]$Asset)
    $sent = 0L
    foreach ($line in Select-String -LiteralPath $script:mockLog -Pattern ("<- {0} sent=(\d+)" -f [regex]::Escape($Asset))) {
        $sent += [long]$line.Matches[0].Groups[1].Value
    }
    return $sent
}

function Test-FileIs {
    param([string]$Path, [byte[]]$Bytes)
    return (Test-Path -LiteralPath $Path) -and ((Get-Sha256 ([IO.File]::ReadAllBytes($Path))) -eq (Get-Sha256 $Bytes))
}

# ---------------------------------------------------------------------------- run
Assert-NoTargetEngine
New-QaRoot
New-Fixture
$mockProcess = Start-Mock
try {
    # ---- Update
    $log = Invoke-Engine -Phase 'update' -Commands @('xms_mods', 'xms_update all') -WaitFor @(
        '\[mods\] qa\.story: 1\.1\.0 staged', '\[mods\] qa\.second: 1\.3 staged', '\[mods\] qa\.delta: 1\.3\.0 staged',
        '\[mods\] qa\.reissue: 1\.0\.0 staged',
        '\[mods\] qa\.broken: update failed', '\[mods\] qa\.tampered: update failed')
    Assert-That 'website outside the allow-list is refused' ($log -match 'module \[qa\.badsite\] website ignored')
    Assert-That 'malformed repository is refused' ($log -match 'module \[qa\.badsite\] update source ignored')
    Assert-That 'current module reports nothing newer' ($log -match '\[mods\] qa\.current: installed 2\.0, released 2\.0\.0')
    Assert-That 'missing release is a 404, not an error' ($log -match '\[mods\] qa\.norelease: .* publishes no release')
    Assert-That 'release for a newer game is not taken' ($log -notmatch '\[mods\] qa\.blocked: .* staged')
    Assert-That 'index path outside the module is refused' ($log -match 'qa\.broken: update failed - file index holds a malformed or unsafe record')
    # a flipped byte either breaks the deflate stream or survives it and fails the hash
    Assert-That 'package bytes that differ from the index are refused' ($log -match
        'qa\.tampered: update failed - a file (does not match the index|of the package is damaged)')
    Assert-That 'hostile entry was written nowhere' (-not @(Get-ChildItem -LiteralPath $qaRoot -Recurse -Filter 'escape.txt' -File).Count)
    Assert-That 'failed updates leave no staging behind' (-not (Test-Path -LiteralPath (Join-Path $qaStaged 'qa.broken')) -and
        -not (Test-Path -LiteralPath (Join-Path $qaStaged 'qa.tampered')))
    Assert-That 'staged update is armed' (Test-Path -LiteralPath (Join-Path $qaStaged 'qa.story\ready.ltx'))
    Assert-That 'installed module is untouched until the next start' ((Get-ModuleVersion 'qa.story') -eq '1.0.0')
    Assert-That 'empty file is staged' ((Test-Path -LiteralPath (Join-Path $qaStaged 'qa.story\payload\gamedata\configs\empty.ltx')) -and
        (Get-Item -LiteralPath (Join-Path $qaStaged 'qa.story\payload\gamedata\configs\empty.ltx')).Length -eq 0)
    $storyRequests = @(Select-String -LiteralPath $script:mockLog -Pattern '-> qa\.story-1\.1\.0\.zip range=').Count
    Assert-That 'dropped download resumed at the file in progress' ($storyRequests -ge 3) "$storyRequests request(s)"

    # the delta: 4.3 MB release, a few hundred bytes of it new
    $deltaPackage = (Get-Item -LiteralPath (Join-Path $qaAssets 'qa.delta-1.3.0.zip')).Length
    $deltaSent = Get-MockBytes 'qa.delta-1.3.0.zip'
    Assert-That 'delta downloads what changed, not the package' ($deltaSent -gt 0 -and $deltaSent * 8 -lt $deltaPackage) "$deltaSent of $deltaPackage byte(s)"
    Assert-That 'delta counts reused files' ($log -match '\[mods\] qa\.delta: 2 of 5 file\(s\) already here')
    Assert-That 'check quotes the delta, not the package' ($log -match '\[mods\] qa\.delta: about (\d+) of (\d+) byte' -and
        [long]$Matches[1] * 4 -lt [long]$Matches[2]) "$($Matches[1]) of $($Matches[2])"

    # the same version published again: an update by content, not by number
    Assert-That 'a version published again is offered as an update' ($log -match
        '\[mods\] qa\.reissue: 1\.0\.0 was published again with other content - about (\d+) of (\d+) byte') "$($Matches[1]) of $($Matches[2])"
    $reissuePackage = (Get-Item -LiteralPath (Join-Path $qaAssets 'qa.reissue-1.0.0.zip')).Length
    $reissueSent = Get-MockBytes 'qa.reissue-1.0.0.zip'
    Assert-That 'and downloads the fixed files alone' ($reissueSent -gt 0 -and $reissueSent * 8 -lt $reissuePackage) "$reissueSent of $reissuePackage byte(s)"
    Assert-That 'the same version with the same content is not an update' ($log -match
        '\[mods\] qa\.current: the installed files are release 2\.0\.0' -and $log -notmatch '\[mods\] qa\.current: .* staged')

    # ---- Apply
    $log = Invoke-Engine -Phase 'apply' -RelaunchParentSeconds 12 -Commands @('xms_update_status') -WaitFor @(
        '\[mods\] qa\.story: installed 1\.1\.0, released 1\.1\.0')
    Assert-That 'story module was swapped in' ($log -match 'module \[qa\.story\] updated to 1\.1\.0' -and (Get-ModuleVersion 'qa.story') -eq '1.1.0')
    Assert-That 'two-package module was swapped in' ((Get-ModuleVersion 'qa.second') -eq '1.3' -and
        (Test-Path -LiteralPath (Join-Path $qaModules 'qa.second\gamedata\c.bin')))
    Assert-That 'file dropped by the new version is gone' (-not (Test-Path -LiteralPath (Join-Path $qaModules 'qa.story\gamedata\configs\removed_in_next.ltx')))
    $delta = Join-Path $qaModules 'qa.delta\gamedata'
    Assert-That 'delta module is the released one, file for file' ((Get-ModuleVersion 'qa.delta') -eq '1.3.0' -and
        (Test-FileIs (Join-Path $delta 'big.bin') $script:deltaBig) -and
        (Test-FileIs (Join-Path $delta 'moved\new_name.bin') $script:deltaMoved) -and
        (Test-FileIs (Join-Path $delta 'changed.ltx') ([Text.Encoding]::ASCII.GetBytes("[delta]`r`nrevision = 4`r`n"))) -and
        (Test-Path -LiteralPath (Join-Path $delta 'added.ltx')) -and
        -not (Test-Path -LiteralPath (Join-Path $delta 'old_name.bin')) -and
        -not (Test-Path -LiteralPath (Join-Path $delta 'removed.ltx')) -and
        @(Get-ChildItem -LiteralPath (Join-Path $qaModules 'qa.delta') -Recurse -File).Count -eq 5)
    $reissued = Join-Path $qaModules 'qa.reissue\gamedata'
    Assert-That 'the version published again was swapped in' ((Get-ModuleVersion 'qa.reissue') -eq '1.0.0' -and
        (Test-FileIs (Join-Path $reissued 'fixed.ltx') ([Text.Encoding]::ASCII.GetBytes("[reissue]`r`nrevision = 2`r`n"))) -and
        (Test-FileIs (Join-Path $reissued 'keep.bin') $script:reissueKeep) -and
        (Test-Path -LiteralPath (Join-Path $reissued 'extra.ltx')))
    Assert-That 'and is not offered a second time' ($log -match '\[mods\] qa\.reissue: the installed files are release 1\.0\.0' -and
        $log -notmatch 'qa\.reissue: 1\.0\.0 was published again')
    Assert-That 'staging folder is gone' (-not (Test-Path -LiteralPath $qaStaged))
    $stage = [regex]::Match($log, 'Startup checkpoint:\s*([\d.]+) ms stage,[^\r\n]*Filesystem initialized')
    $waited = if ($stage.Success) { [double]$stage.Groups[1].Value } else { 0 }
    Assert-That 'start waited for the relaunching process' ($waited -gt 5000) ("filesystem stage {0:N0} ms" -f $waited)

    # ---- Locked: another process holds a file of the module open
    Remove-Item -LiteralPath (Join-Path $qaModules 'qa.story') -Recurse -Force
    $storyArgs = @{ Id = 'qa.story'; Name = 'Story'; Github = 'QaOwner/qa-story' }
    Install-Module -Id 'qa.story' -ManifestText (New-ManifestText @storyArgs -Version '1.0.0') -Files @{
        'gamedata\held.bin' = (New-Payload -Bytes 1024 -Seed 5) }
    $log = Invoke-Engine -Phase 'locked-stage' -Commands @('xms_update qa.story') -WaitFor @('\[mods\] qa\.story: 1\.1\.0 staged')
    $held = [IO.File]::Open((Join-Path $qaModules 'qa.story\gamedata\held.bin'), 'Open', 'Read', 'Read')
    try {
        $log = Invoke-Engine -Phase 'locked-refused' -WaitFor @('module \[qa\.story\] is in use')
        Assert-That 'locked module keeps its installed version' ((Get-ModuleVersion 'qa.story') -eq '1.0.0')
        Assert-That 'locked module keeps its update staged' (Test-Path -LiteralPath (Join-Path $qaStaged 'qa.story\ready.ltx'))
    }
    finally { $held.Dispose() }
    $log = Invoke-Engine -Phase 'locked-released' -WaitFor @('module \[qa\.story\] updated to 1\.1\.0')
    Assert-That 'update lands once the handle is gone' ((Get-ModuleVersion 'qa.story') -eq '1.1.0')

    # The comparison reads the whole module, so its verdict is kept. Over every start so far the
    # index of the unchanged release was fetched once, and the reissue's twice: when it was offered,
    # and after the swap, to see that the swap took.
    $state = Join-Path $qaModules '.update_state'
    Assert-That 'the verdict of a comparison is kept' ((Test-Path -LiteralPath $state) -and
        (Select-String -LiteralPath $state -Pattern "^qa\.current`t" -Quiet))
    $currentIndexFetches = @(Select-String -LiteralPath $script:mockLog -Pattern '<- qa\.current-2\.0\.0\.files sent=').Count
    $reissueIndexFetches = @(Select-String -LiteralPath $script:mockLog -Pattern '<- qa\.reissue-1\.0\.0\.files sent=').Count
    Assert-That 'and spares the next starts the comparison' ($currentIndexFetches -eq 1 -and $reissueIndexFetches -eq 2) "current $currentIndexFetches, reissue $reissueIndexFetches"

    # ---- Recover: a swap interrupted between its two renames, and an update for a deleted module
    $interrupted = Join-Path $qaStaged 'qa.current'
    New-Item -ItemType Directory -Path (Join-Path $interrupted 'payload') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $interrupted 'payload\mod.ltx'),
        (New-ManifestText -Id 'qa.current' -Name 'Current Module' -Version '2.5'), $utf8)
    Move-Item -LiteralPath (Join-Path $qaModules 'qa.current') -Destination (Join-Path $interrupted 'previous')
    [IO.File]::WriteAllText((Join-Path $interrupted 'ready.ltx'),
        "[staged]`r`nid = qa.current`r`nversion = 2.5`r`nroot = modules`r`nfolder = qa.current`r`n", $utf8)

    $orphan = Join-Path $qaStaged 'qa.nosource'
    New-Item -ItemType Directory -Path (Join-Path $orphan 'payload') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $orphan 'payload\mod.ltx'),
        (New-ManifestText -Id 'qa.nosource' -Name 'No Update Source' -Version '1.0'), $utf8)
    [IO.File]::WriteAllText((Join-Path $orphan 'ready.ltx'),
        "[staged]`r`nid = qa.nosource`r`nversion = 1.0`r`nroot = modules`r`nfolder = qa.nosource`r`n", $utf8)
    Remove-Item -LiteralPath (Join-Path $qaModules 'qa.nosource') -Recurse -Force

    $log = Invoke-Engine -Phase 'recover' -WaitFor @('module \[qa\.current\] updated to 2\.5')
    Assert-That 'interrupted swap is completed' ((Get-ModuleVersion 'qa.current') -eq '2.5')
    Assert-That 'update for a deleted module is discarded' ($log -match 'module \[qa\.nosource\] is no longer installed' -and
        -not (Test-Path -LiteralPath (Join-Path $qaModules 'qa.nosource')))
    Assert-That 'staging folder is gone after recovery' (-not (Test-Path -LiteralPath $qaStaged))
}
finally {
    if ($mockProcess -and -not $mockProcess.HasExited) { Stop-Process -Id $mockProcess.Id -Force -ErrorAction SilentlyContinue }
    Remove-ItemProperty -Path $layersKey -Name $qaEngine -ErrorAction SilentlyContinue
    $env:DAR_QA_MOD_UPDATE_BASE = $null
    $env:DAR_QA_UPDATE_API = $null
    $env:DAR_RELAUNCH_WAIT_PID = $null
}

$failed = @($script:checks | Where-Object { -not $_.Pass })
$script:checks | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $resultRoot 'checks.json')
Write-Host ''
Write-Host ("MODSQA: DONE {0}/{1}" -f ($script:checks.Count - $failed.Count), $script:checks.Count)

if (-not $KeepRoot) {
    $databaseLink = Join-Path $qaRoot 'database'
    if (Test-Path -LiteralPath $databaseLink) { [IO.Directory]::Delete($databaseLink, $false) }
    Remove-Item -LiteralPath $qaRoot -Recurse -Force
}
if ($failed.Count) { exit 1 }
