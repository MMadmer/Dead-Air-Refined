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

    -Scenario Catalog runs the mod catalog instead (docs/dead-air/MOD_CATALOG.md 15). catalog_qa.py
    makes the fixture - a catalog signed with a fresh QA maintainer key, releases signed with an
    author key, a stranger's key, a program in a release, VirusTotal answers - and serves it with a
    review inbox. The engine trusts that key only with -qa_catalog and the fixture's environment.
      Ordinary  no catalog module yet: the probe shows every escape a script has is real; the
                installations the catalog's rules refuse are refused, a withdrawn module's too,
                the good ones are staged; a release in two packages is judged by its flagged one;
                VirusTotal is asked at most four times a minute
      Protected the installation lands, protection comes on; the probe hits every wall, and a
                script cannot pass a console command off as the player's
      Ui        the Mods window as the mouse drives it; Check turns a split release to "block"
      Rollback  an older, a forged and a self-contradicting index are refused, the accepted one kept
      Review    a review arrives signed, paid for and intact; a short one is refused
      Withdraw  the authors withdraw two installed modules: the one keeps working and takes no
                update, the one also revoked is switched off; the window says which is which
      Remove    a removal is applied by the next start; protection stays on

    -Scenario Adopt holds modules copied in by hand against the catalog (MOD_CATALOG.md 14.1):
    catalog_qa.py adopt makes copies of releases, edited ones and another author's module under a
    listed id; one start takes the exact copies for browser installations and nothing else.

.EXAMPLE
    pwsh -NoProfile -File tools\qa\mods\Run-ModsQa.ps1 -ResultLabel run1

.EXAMPLE
    pwsh -NoProfile -File tools\qa\mods\Run-ModsQa.ps1 -Scenario Catalog -ResultLabel catalog1
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

    [ValidateSet('Updates', 'Catalog', 'Adopt')]
    [string]$Scenario = 'Updates',

    # -Scenario Adopt: tags\<tag>\ with the assets of a real published module's releases
    [string]$RealRelease,

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
$qaLog = Join-Path $qaAppData 'logs\grizzly_admin.log'

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
    foreach ($cache in @('shaders_cache', 'shaders_cache_grizzly', 'shaders_cache_oxr', 'render-hardware.cache')) {
        $source = Join-Path $GameRoot "appdata\$cache"
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $qaAppData -Recurse -Force }
    }
    Copy-TreeContents -Source (Join-Path $GameRoot 'gamedata') -Destination $qaGameData
    Copy-TreeContents -Source $compatGameData -Destination $qaGameData
}

function Set-QaUserLtx {
    param([string[]]$Commands = @())
    # the engine reads user.ltx in the UI code page: a review typed there is Windows-1251
    $cp1251 = [Text.Encoding]::GetEncoding(1251)
    $userLtxSource = Join-Path $GameRoot 'appdata\user.ltx'
    $userLtx = if (Test-Path -LiteralPath $userLtxSource) { @([IO.File]::ReadAllLines($userLtxSource, $cp1251)) } else { @() }
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
    [IO.File]::WriteAllLines((Join-Path $qaAppData 'user.ltx'), [string[]]($userLtx + $Commands), $cp1251)
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
    # -Catalog trusts the catalog fixture's key and mirrors (catalog_qa.py env.json).
    param([Parameter(Mandatory)][string]$Phase, [string[]]$Commands = @(), [string[]]$WaitFor = @(),
        [int]$RelaunchParentSeconds = 0, [int]$SettleSeconds = 3, [switch]$Catalog)

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
    foreach ($name in @('DAR_QA_CATALOG_KEY', 'DAR_QA_CATALOG_BASE', 'DAR_QA_VT_KEY', 'DAR_QA_VT_BASE')) {
        if ($Catalog) { Set-Item -Path "env:$name" -Value $script:catalogEnv.$name }
        else { Remove-Item -Path "env:$name" -ErrorAction SilentlyContinue }
    }

    New-Item -Path $layersKey -Force | Out-Null
    New-ItemProperty -Path $layersKey -Name $qaEngine -Value '~ HIGHDPIAWARE' -PropertyType String -Force | Out-Null

    $desktop = "DeadAirMods$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
    $processIdFile = Join-Path $resultRoot "$Phase-engine.pid"
    $launcherIdFile = Join-Path $resultRoot "$Phase-launcher.pid"
    Remove-Item -LiteralPath $processIdFile, $launcherIdFile -Force -ErrorAction SilentlyContinue
    $arguments = '-i -fsltx fsgame.ltx -always_active -silent_error_mode -force_flushlog -r4 -nointro -qa_update'
    if ($Catalog) { $arguments += ' -qa_catalog' }
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

function Clear-QaEnvironment {
    Remove-ItemProperty -Path $layersKey -Name $qaEngine -ErrorAction SilentlyContinue
    foreach ($name in @('DAR_QA_MOD_UPDATE_BASE', 'DAR_QA_UPDATE_API', 'DAR_RELAUNCH_WAIT_PID', 'DAR_QA_CATALOG_KEY',
            'DAR_QA_CATALOG_BASE', 'DAR_QA_VT_KEY', 'DAR_QA_VT_BASE')) {
        Remove-Item -Path "env:$name" -ErrorAction SilentlyContinue
    }
}

function Complete-QaRun {
    $failed = @($script:checks | Where-Object { -not $_.Pass })
    $script:checks | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $resultRoot 'checks.json')
    Write-Host ''
    Write-Host ("MODSQA: DONE {0}/{1}" -f ($script:checks.Count - $failed.Count), $script:checks.Count)

    if (-not $KeepRoot) {
        $databaseLink = Join-Path $qaRoot 'database'
        if (Test-Path -LiteralPath $databaseLink) { [IO.Directory]::Delete($databaseLink, $false) }
        Remove-Item -LiteralPath $qaRoot -Recurse -Force
    }
    exit $(if ($failed.Count) { 1 } else { 0 })
}

# ---------------------------------------------------------------------------- the mod catalog
function Read-PhaseLog {
    # The window's text reaches the log in its own code page, Windows-1251 - not what Invoke-Engine
    # decodes the log as.
    param([Parameter(Mandatory)][string]$Phase)
    return [IO.File]::ReadAllText((Join-Path $resultRoot "$Phase.log"), [Text.Encoding]::GetEncoding(1251))
}

function Test-ModsText {
    # Whether $Value is the Mods window's string $Id in one of the languages the game ships, its %s
    # filled with $Arguments in turn. Read from the tables the QA root was given.
    param([AllowNull()][AllowEmptyString()][string]$Value, [Parameter(Mandatory)][string]$Id, [string[]]$Arguments = @())
    $cp1251 = [Text.Encoding]::GetEncoding(1251)
    foreach ($language in @('rus', 'eng')) {
        $table = [xml][IO.File]::ReadAllText((Join-Path $compatGameData "configs\text\$language\dead_air_x64_mods.xml"), $cp1251)
        $node = @($table.string_table.string | Where-Object { $_.id -eq $Id }) | Select-Object -First 1
        if (-not $node) { continue }
        $expected = [string]$node.text
        foreach ($argument in $Arguments) { $expected = ([regex]'%s').Replace($expected, $argument.Replace('$', '$$'), 1) }
        if ($Value -ceq $expected) { return $true }
    }
    return $false
}

function Get-ProbeResults {
    param([string]$Log)
    $results = @{}
    foreach ($match in [regex]::Matches($Log, 'PROTECTQA: (\w+) (yes|no)')) { $results[$match.Groups[1].Value] = $match.Groups[2].Value }
    return $results
}

function Invoke-CatalogScenario {
    # MOD_CATALOG.md 15: a signed catalog on a loopback mirror, installations the catalog's rules
    # refuse or take, protected mode with the probe hitting every wall it has, a rollback and a
    # forged index, a review that has to arrive signed and paid for, removal.
    Assert-NoTargetEngine
    New-QaRoot
    if (Test-Path -LiteralPath $qaModules) { Remove-Item -LiteralPath $qaModules -Recurse -Force }
    New-Item -ItemType Directory -Path $qaModules -Force | Out-Null
    $catalogAssets = Join-Path $qaRoot 'catalog-assets'
    $catalogTool = Join-Path $PSScriptRoot 'catalog_qa.py'
    Write-Host '== generating the catalog fixture'
    & python $catalogTool fixture --qa-root $qaRoot --assets $catalogAssets --port $Port
    if ($LASTEXITCODE) { throw 'the catalog fixture could not be made' }
    $script:catalogEnv = Get-Content -LiteralPath (Join-Path $catalogAssets 'env.json') -Raw | ConvertFrom-Json
    $script:mockLog = Join-Path $resultRoot 'mock.log'
    $mockProcess = Start-Process python -ArgumentList @("`"$catalogTool`"", 'serve', '--assets', "`"$catalogAssets`"", '--port', $Port,
        '--log', "`"$($script:mockLog)`"") -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 2
    $fsgameHash = (Get-FileHash -LiteralPath (Join-Path $qaRoot 'fsgame.ltx') -Algorithm SHA256).Hash
    $state = Join-Path $qaModules '.catalog\state.ltx'

    # what each probe line stands for: a way out of the game a script has in an ordinary game
    $escapes = @('ffi_available', 'os_execute_available', 'io_popen_available', 'loadlib_available', 'string_dump_available',
        'marshal_stores_functions', 'write_dll_in_game_root', 'write_script', 'write_fsgame', 'write_outside_game',
        'fs_w_open_dll', 'ini_save_as_bat', 'console_cfg_save_path', 'lfs_link_available', 'debug_getupvalue_available',
        'alternate_stream_write')

    try {
        # ---- an ordinary game: no catalog module yet, every capability real; the installations
        # the last VirusTotal lookup is the fifth request: it has to wait for the first to be a minute old
        $after = @('xms_catalog', 'xms_catalog thumbs', 'xms_catalog reviews qa.cat.good', 'xms_catalog vt qa.cat.good',
            'xms_catalog vt qa.cat.vtlive', 'xms_catalog vt qa.cat.vtparts', 'xms_catalog reviews qa.cat.good', 'xms_catalog',
            'run_string qa_protect_probe.run()',
            'xms_install qa.cat.good', 'xms_install qa.cat.dllname', 'xms_install qa.cat.pehidden', 'xms_install qa.cat.unsigned',
            'xms_install qa.cat.wrongkey', 'xms_install qa.cat.revoked', 'xms_install qa.cat.old', 'xms_install qa.cat.vtblock',
            'xms_install qa.cat.vtlive', 'xms_install qa.cat.vtparts', 'xms_install qa.cat.wdnew', 'xms_install qa.cat.wdrawn',
            'xms_install qa.cat.wdrevoked', 'xms_catalog vt qa.cat.upd2')
        $log = Invoke-Engine -Phase 'catalog-ordinary' -Catalog -Commands (@('xms_protect', 'xms_catalog refresh') +
            @($after | ForEach-Object { "xms_catalog after $_" })) -WaitFor @(
            'PROTECTQA: DONE', 'qa\.cat\.good: 1\.0\.0 staged for installation', 'qa\.cat\.dllname: installation refused',
            'qa\.cat\.pehidden: installation refused', 'qa\.cat\.unsigned: installation of 1\.0\.0 refused',
            'qa\.cat\.wrongkey: installation of 1\.0\.0 refused', 'qa\.cat\.revoked cannot be installed',
            'qa\.cat\.old: installation of 1\.1\.0 refused', 'qa\.cat\.vtblock cannot be installed',
            'qa\.cat\.vtlive cannot be installed', 'qa\.cat\.vtparts cannot be installed', 'qa\.cat\.wdnew cannot be installed',
            'qa\.cat\.wdrawn: 1\.0\.0 staged for installation', 'qa\.cat\.wdrevoked: 1\.0\.0 staged for installation',
            'VirusTotal for \[qa\.cat\.upd2\]')
        Assert-That 'without a catalog module the game is not protected' ($log -match 'protected mode off \(no module from the mod catalog\)')
        Assert-That 'the signed catalog is accepted' ($log -match '\[catalog\] serial 5 from http://127\.0\.0\.1:\d+/catalog/: 15 module\(s\)')
        Assert-That 'a card signed by its author is used' ($log -match '\*   qa\.cat\.good 1\.0\.0 card=signed')
        Assert-That 'an unsigned card is not' ($log -match 'the card of \[qa\.cat\.unsigned\] is not used: not signed')
        Assert-That 'a card signed by another key is not' ($log -match 'the card of \[qa\.cat\.wrongkey\] is not used: signed by key')
        Assert-That 'the thumbnail is fetched, checked and published' ($log -match ('qa\.cat\.good 1\.0\.0 card=signed thumbnail=xms_thumbs\\' + $script:catalogEnv.thumbnail_sha))
        Assert-That 'the reviews are read' ($log -match '2 review\(s\) of \[qa\.cat\.good\]')
        Assert-That 'VirusTotal: a lone no-name detection is noise' (($log -match
                'VirusTotal for \[qa\.cat\.good\]: 4 engine\(s\), 0 trusted malicious, 0 trusted suspicious, 1 other malicious') -and
            ($log -match '\*   qa\.cat\.good .* vt=clean\(live\)'))
        Assert-That 'VirusTotal: a trusted engine blocks' (($log -match 'VirusTotal for \[qa\.cat\.vtlive\]: 3 engine\(s\), 1 trusted malicious') -and
            ($log -match 'qa\.cat\.vtlive cannot be installed: VirusTotal: flagged by Kaspersky'))
        Assert-That 'the catalog''s VirusTotal verdict blocks too' ($log -match 'qa\.cat\.vtblock cannot be installed: VirusTotal: flagged by Microsoft')
        Assert-That 'a DLL in a release is refused before a byte is fetched' (($log -match 'qa\.cat\.dllname: installation refused - bin/evil\.dll: ') -and
            -not (Test-Path -LiteralPath (Join-Path $qaStaged 'qa.cat.dllname')))
        Assert-That 'a program under an innocent name is refused by its bytes' (($log -match
                'qa\.cat\.pehidden: installation refused - gamedata/configs/looks_like_config\.ltx: ') -and
            -not (Test-Path -LiteralPath (Join-Path $qaStaged 'qa.cat.pehidden')))
        Assert-That 'an unsigned release is refused' ($log -match 'qa\.cat\.unsigned: installation of 1\.0\.0 refused - not signed by the author''s key the catalog names \(not signed\)')
        Assert-That 'a release signed by another key is refused' ($log -match 'qa\.cat\.wrongkey: installation of 1\.0\.0 refused - not signed by the author''s key the catalog names \(signed by key')
        Assert-That 'a revoked module is refused' ($log -match 'qa\.cat\.revoked cannot be installed: withdrawn from the mod catalog: QA revocation')
        Assert-That 'a release below the listed version is refused' ($log -match 'qa\.cat\.old: installation of 1\.1\.0 refused - below the lowest version the catalog lists \(1\.2\.0\)')
        Assert-That 'the disclaimer is logged with an installation' ($log -match 'Modules are made by third parties\. The author of Dead Air: Refined is not responsible')

        # withdrawn by its author before this game ever saw it, a stale card of it still served
        Assert-That 'a malformed withdrawal is skipped, the index kept' ($log -match '! XMS catalog: withdrawal \[qa\.cat\.wdbad\] is malformed - skipped')
        Assert-That 'the console names what its author withdrew' ($log -match '\*   qa\.cat\.wdnew withdrawn by its author \(QaOwner/qa-cat-wdnew\)')
        Assert-That 'the browser has no entry for it, card or not' ($log -notmatch '\*   qa\.cat\.wdnew \S+ card=')
        Assert-That 'and it cannot be installed, before a byte is fetched' (($log -match
                'qa\.cat\.wdnew cannot be installed: its author withdrew it from the Mod Browser') -and
            -not (Select-String -LiteralPath $script:mockLog -Pattern 'qa-cat-wdnew' -Quiet))

        # a release in two packages, the second one flagged
        Assert-That 'the catalog''s verdict on a split release is its worst package''s' ($log -match
            '(?m)^\*   qa\.cat\.vtparts 1\.0\.0 card=signed .* vt=warn\r?$')
        $parts = @($script:catalogEnv.package_parts.'qa.cat.vtparts')
        $asked = @($parts | Where-Object { Select-String -LiteralPath $script:mockLog -Pattern "GET /vt/api/v3/files/$_" -Quiet })
        Assert-That 'the player''s lookup asks VirusTotal about every package' ($asked.Count -eq 2 -and $parts.Count -eq 2 -and
            ($log -match 'VirusTotal for \[qa\.cat\.vtparts\] package 1 of 2: 3 engine\(s\), 0 trusted malicious') -and
            ($log -match 'VirusTotal for \[qa\.cat\.vtparts\] package 2 of 2: 3 engine\(s\), 1 trusted malicious')) "$($asked.Count) of $($parts.Count) package(s) asked"
        Assert-That 'and the flagged one decides' (($log -match 'VirusTotal for \[qa\.cat\.vtparts\]: block, decided by package 2 of 2') -and
            ($log -match '\*   qa\.cat\.vtparts 1\.0\.0 .* vt=block\(live\)') -and
            ($log -match 'qa\.cat\.vtparts cannot be installed: VirusTotal: flagged by Kaspersky'))
        # the free API takes 4 requests a minute: no five of them inside 60 seconds
        $vtTimes = @(Get-Content -LiteralPath $script:mockLog | ForEach-Object {
                if ($_ -match '^(\d+\.\d+) GET /vt/api/v3/files/') { [double]$Matches[1] } })
        $closest = [double]::MaxValue
        for ($i = 0; $i + 4 -lt $vtTimes.Count; ++$i) { $closest = [Math]::Min($closest, $vtTimes[$i + 4] - $vtTimes[$i]) }
        Assert-That 'VirusTotal is asked at most four times a minute' ($vtTimes.Count -ge 5 -and $closest -ge 60 -and
            ($log -match 'VirusTotal takes 4 requests a minute: the next one in \d+ s')) ("{0} request(s), every five {1:N1} s apart or more" -f
            $vtTimes.Count, $closest)
        $ready = Join-Path $qaStaged 'qa.cat.good\ready.ltx'
        Assert-That 'the installation is staged, not applied' ((Test-Path -LiteralPath $ready) -and
            (Select-String -LiteralPath $ready -Pattern 'action = install' -Quiet) -and
            -not (Test-Path -LiteralPath (Join-Path $qaModules 'qa.cat.good')))
        $ordinary = Get-ProbeResults $log
        $real = @($escapes | Where-Object { $ordinary[$_] -eq 'yes' })
        Assert-That 'the probe proves every escape is real in an ordinary game' ($real.Count -eq $escapes.Count) ("real: {0} of {1}; not: {2}" -f
            $real.Count, $escapes.Count, (@($escapes | Where-Object { $ordinary[$_] -ne 'yes' }) -join ', '))
        # what the ordinary run could not take back (a writer luabind never handed over) must not
        # count against protection
        foreach ($leftover in @('protect_probe.dll', 'protect_probe_writer.dll', 'protect_probe.bat', 'protect_probe_cfg.ltx')) {
            Remove-Item -LiteralPath (Join-Path $qaRoot $leftover) -Force -ErrorAction SilentlyContinue
        }
        Remove-Item -LiteralPath (Join-Path $qaGameData 'scripts\protect_probe_planted.script') -Force -ErrorAction SilentlyContinue

        # ---- the installation lands, protection comes on, the probe hits every wall
        $log = Invoke-Engine -Phase 'catalog-protected' -Catalog -Commands @('xms_protect', 'xms_catalog refresh',
            'xms_catalog after run_string qa_protect_probe.run()', 'xms_catalog after run_string qa_protect_probe.console()',
            'xms_catalog after xms_update all') -WaitFor @('PROTECTQA: DONE', 'PROTECTQA: CONSOLE DONE',
            'xms_disable qa\.cat\.upd2 refused', 'qa\.cat\.upd: update 1\.1\.0 refused', 'qa\.cat\.upd2: 1\.1\.0 staged')
        Assert-That 'the browser installation is moved in by the next start' (($log -match 'module \[qa\.cat\.good\] 1\.0\.0 installed') -and
            (Test-Path -LiteralPath (Join-Path $qaModules 'qa.cat.good\mod.ltx')))
        Assert-That 'and remembered as the browser''s' ((Test-Path -LiteralPath $state) -and (Select-String -LiteralPath $state -Pattern '^qa\.cat\.good = ' -Quiet))
        Assert-That 'so are the two its authors withdraw later' (($log -match 'module \[qa\.cat\.wdrawn\] 1\.0\.0 installed') -and
            ($log -match 'module \[qa\.cat\.wdrevoked\] 1\.0\.0 installed') -and
            (Select-String -LiteralPath $state -Pattern '^qa\.cat\.wdrawn = ' -Quiet))
        Assert-That 'a catalog module switches protection on' (($log -match 'protected mode ON \(module \[qa\.cat\.[a-z0-9]+\] is from the mod catalog\)') -and
            ($log -match 'protected mode applied to the script state'))
        Assert-That 'a revoked installed module is switched off at the start' ($log -match 'module \[qa\.cat\.revinst\] disabled: withdrawn from the mod catalog: QA revocation of an installed module')
        $protected = Get-ProbeResults $log
        $open = @($escapes | Where-Object { $protected[$_] -ne 'no' })
        Assert-That 'protected mode stops every escape' (-not $open.Count) ("still open: {0}" -f ($open -join ', '))
        Assert-That 'planted bytecode is not loaded' ($protected['bytecode_loads'] -eq 'no' -and $protected['marshal_loads_planted_functions'] -eq 'no') ("bytecode {0}, marshal {1}" -f
            $protected['bytecode_loads'], $protected['marshal_loads_planted_functions'])
        Assert-That 'the game''s data folder stays writable for plain text' ($protected['write_appdata_text'] -eq 'yes')
        Assert-That 'reads through a network path are refused' ($protected['unc_read'] -eq 'no') "ordinary game: $($ordinary['unc_read'])"
        $located = [regex]::Matches($log, '! \[protect\] .+ refused: .+ \(.*qa_protect_probe\.script:\d+\)').Count
        Assert-That 'every refusal names its script and line' ($located -ge 15) "$located line(s)"
        Assert-That 'nothing was planted' (-not (Test-Path -LiteralPath (Join-Path $qaRoot 'protect_probe.dll')) -and
            -not (Test-Path -LiteralPath (Join-Path $qaRoot 'protect_probe_writer.dll')) -and
            -not (Test-Path -LiteralPath (Join-Path $qaRoot 'protect_probe.bat')) -and
            -not (Test-Path -LiteralPath (Join-Path $qaRoot 'protect_probe_cfg.ltx')) -and
            -not (Test-Path -LiteralPath (Join-Path $qaGameData 'scripts\protect_probe_planted.script')) -and
            (Get-FileHash -LiteralPath (Join-Path $qaRoot 'fsgame.ltx') -Algorithm SHA256).Hash -eq $fsgameHash)

        # the console: a script's command, a deferred one, a line break, a file of commands three ways
        $player = 'only the player may install, remove, switch or update modules'
        Assert-That 'a script may not switch modules' ($log -match "! \[protect\] xms_disable qa\.cat\.upd refused: $player \(.*qa_protect_probe\.script:\d+\)")
        Assert-That 'nor post reviews in the player''s name' ($log -match "! \[protect\] xms_catalog review qa\.cat\.good 5 .* refused: $player")
        Assert-That 'nor slip a command in for later' ($log -match "! \[protect\] xms_disable qa\.cat\.upd2 refused: $player")
        Assert-That 'nor hide one behind a line break' ($log -match '! \[protect\] flush\\nxms_disable qa\.cat\.good refused: a console command is one line')
        $fromFile = [regex]::Matches($log, "! \[protect\] xms_disable qa\.legacy refused: $player").Count
        Assert-That 'nor run one from a file of commands' ($fromFile -eq 3) "$fromFile of 3 route(s)"
        Assert-That 'a file of commands by its full path in the game still loads' ($log -match 'protect_probe_cmds\.ltx\] successfully loaded')
        Assert-That 'one from outside the game does not' ($log -match '! \[protect\] cfg_load C:\\Windows\\win refused: outside the game and its data folder')
        Assert-That 'a save may not be given a path' ($log -match '! \[protect\] save \.\.\\\.\.\\protect_probe_save refused: only the player may give this command a path')
        Assert-That 'the game may not be sent to another computer' ($log -match
            '! \[protect\] start server\(protect_probe/single/alife/new\) client\(192\.0\.2\.1\) refused: only the player may connect the game to another computer')
        $disabled = Join-Path $qaModules 'disabled.ltx'
        $switched = if (Test-Path -LiteralPath $disabled) { @(Select-String -LiteralPath $disabled -Pattern '^(qa\.cat\.upd2?|qa\.cat\.good|qa\.legacy)\b').Count } else { 0 }
        Assert-That 'and no module was switched by any of them' (-not $switched) "$switched module(s) in disabled.ltx"

        Assert-That 'an unsigned update of a catalog module is refused' ($log -match 'qa\.cat\.upd: update 1\.1\.0 refused - not signed by the author''s key the catalog names \(not signed\)')
        Assert-That 'a signed one is taken' ($log -match 'qa\.cat\.upd2: 1\.1\.0 staged')

        # ---- the Mods window, driven the way the mouse drives it (xms_mods)
        $log = Invoke-Engine -Phase 'catalog-ui' -Catalog -Commands @('xms_catalog refresh', 'xms_mods view browser',
            'xms_mods wait 4000', 'xms_mods state', 'xms_mods search dlname', 'xms_mods wait 300', 'xms_mods state',
            'xms_mods clear', 'xms_mods sort name', 'xms_mods tag quests', 'xms_mods wait 300', 'xms_mods state',
            'xms_mods select qa.cat.dllname', 'xms_mods press primary', 'xms_mods wait 300', 'xms_mods state',
            'xms_mods answer yes', 'xms_mods wait 4000', 'xms_mods state',
            'xms_mods select qa.cat.vtparts', 'xms_mods wait 300', 'xms_mods state', 'xms_mods press vt_check', 'xms_mods wait 3000',
            'xms_mods state',
            'xms_mods view installed', 'xms_mods select qa.cat.good', 'xms_mods press remove', 'xms_mods wait 300', 'xms_mods state',
            'xms_mods answer no', 'xms_mods press review', 'xms_mods wait 500', 'xms_mods state', 'xms_mods mark ui_done') -WaitFor @(
            '\[mods-ui\] mark ui_done')
        $views = [regex]::Matches($log, '\[mods-ui\] view (\w+), (\d+) item\(s\), sort (\w+), query "([^"]*)", tags (\S+)')
        $viewText = @($views | ForEach-Object { $_.Value }) -join ' | '
        Assert-That 'the browser lists what can be installed' ($views.Count -ge 1 -and $views[0].Groups[2].Value -eq '9') $viewText
        Assert-That 'with the thumbnail the catalog published' ($log -match '\[mods-ui\] row qa\.cat\.good "[^"]*" badge=\S+ rating=4\.5 thumbnail=yes')
        Assert-That 'a word with a typo still finds the mod' ($views.Count -ge 2 -and $views[1].Groups[2].Value -eq '1' -and
            $views[1].Groups[4].Value -eq 'dlname' -and $log -match '\[mods-ui\] row qa\.cat\.dllname ')
        $sorted = [regex]::Match($log, '(?s)sort name, query "", tags quests[^\n]*\n\* \[mods-ui\] row (\S+) ')
        Assert-That 'a tag filters and the name sorts' ($views.Count -ge 3 -and $views[2].Groups[2].Value -eq '9' -and
            $sorted.Success -and $sorted.Groups[1].Value -eq 'qa.cat.dllname') "first row: $($sorted.Groups[1].Value)"
        Assert-That 'Install asks first, with the disclaimer' ($log -match '\[mods-ui\] prompt install:qa\.cat\.dllname ')
        Assert-That 'and the answer starts the installation the catalog then refuses' ($log -match
            'qa\.cat\.dllname: installation refused - bin/evil\.dll' -and $log -match '\[mods-ui\] row qa\.cat\.dllname "[^"]*" badge=(?!-)')
        Assert-That 'Remove asks, naming the mod' ($log -match '\[mods-ui\] prompt remove:qa\.cat\.good ')
        Assert-That 'and No keeps it' (-not (Test-Path -LiteralPath (Join-Path $qaStaged 'qa.cat.good')))
        Assert-That 'Write a review opens the review' ($log -match '\[mods-ui\] review dialog for qa\.cat\.good')
        # the split release: the catalog's warning first, then what Check found in its second package
        $uiLog = Read-PhaseLog 'catalog-ui'
        $cards = [regex]::Matches($uiLog, '\[mods-ui\] card qa\.cat\.vtparts: status "[^"]*", primary "[^"]*"(?<off> \(off\))?, vt "(?<vt>[^"]*)"')
        Assert-That 'the window shows the catalog''s warning of a split release' ($cards.Count -eq 2 -and
            (Test-ModsText $cards[0].Groups['vt'].Value 'st_mods_vt_warn' 'ESET-NOD32')) $(if ($cards.Count) { $cards[0].Groups['vt'].Value })
        $badge = [regex]::Matches($uiLog, '\[mods-ui\] row qa\.cat\.vtparts "[^"]*" badge=(\S+) ')
        Assert-That 'and Check turns it into the block its second package earns' ($cards.Count -eq 2 -and
            (Test-ModsText $cards[1].Groups['vt'].Value 'st_mods_vt_block' 'Kaspersky') -and $cards[1].Groups['off'].Success -and
            $badge.Count -and (Test-ModsText $badge[$badge.Count - 1].Groups[1].Value 'st_mods_badge_blocked')) $(if ($cards.Count -eq 2) {
                "$($cards[1].Groups['vt'].Value), badge $($badge[$badge.Count - 1].Groups[1].Value)" })

        # ---- a mirror serving an older publication, then a forged one
        Copy-Item -LiteralPath (Join-Path $catalogAssets 'index_serial4.ltx') -Destination (Join-Path $catalogAssets 'catalog\index.ltx') -Force
        $log = Invoke-Engine -Phase 'catalog-rollback' -Catalog -Commands @('xms_catalog refresh', 'xms_catalog after xms_catalog') -WaitFor @(
            '\[catalog\] (offline|failed|ready), serial')
        Assert-That 'an older serial is refused' ($log -match 'serial 4 is older than the accepted 5')
        Assert-That 'and the accepted catalog is kept' (($log -match '\[catalog\] offline, serial 5') -and
            (Select-String -LiteralPath $state -Pattern '^serial = 5$' -Quiet))
        Copy-Item -LiteralPath (Join-Path $catalogAssets 'index_tampered.ltx') -Destination (Join-Path $catalogAssets 'catalog\index.ltx') -Force
        $log = Invoke-Engine -Phase 'catalog-tampered' -Catalog -Commands @('xms_catalog refresh', 'xms_catalog after xms_catalog') -WaitFor @(
            '\[catalog\] (offline|failed|ready), serial')
        Assert-That 'a changed index is refused' (($log -match 'index\.ltx: signature does not match') -and ($log -match '\[catalog\] offline, serial 5'))
        # signed and newer, but it names one module both listed and withdrawn
        Copy-Item -LiteralPath (Join-Path $catalogAssets 'index_duplicate.ltx') -Destination (Join-Path $catalogAssets 'catalog\index.ltx') -Force
        $log = Invoke-Engine -Phase 'catalog-duplicate' -Catalog -Commands @('xms_catalog refresh', 'xms_catalog after xms_catalog') -WaitFor @(
            '\[catalog\] (offline|failed|ready), serial')
        Assert-That 'a module both listed and withdrawn makes the index invalid' (($log -notmatch '\[catalog\] serial 9 from') -and
            ($log -match 'index\.ltx: a module is listed twice') -and ($log -match '\[catalog\] offline, serial 5'))
        Copy-Item -LiteralPath (Join-Path $catalogAssets 'index_serial5.ltx') -Destination (Join-Path $catalogAssets 'catalog\index.ltx') -Force

        # ---- a review: posted, then verified the way the catalog's workflow will
        $text = 'Проверка отзыва из QA: доставка работает, заказы приходят, карта показывает метку.'
        $log = Invoke-Engine -Phase 'catalog-review' -Catalog -Commands @('xms_catalog refresh',
            "xms_catalog after xms_catalog review qa.cat.good 4 $text", 'xms_catalog after xms_catalog review qa.cat.good 5 short') -WaitFor @(
            'review of \[qa\.cat\.good\] 1\.0\.0 sent', 'review of \[qa\.cat\.good\] not sent')
        Assert-That 'a review shorter than 30 characters is refused' ($log -match 'review of \[qa\.cat\.good\] not sent: a review is 30 to 2000 characters long')
        $inbox = & python $catalogTool inbox --assets $catalogAssets | ConvertFrom-Json
        $posted = @($inbox.reviews)
        Assert-That 'the review reached the inbox, signed and paid for' ($inbox.count -eq 1 -and $posted.Count -eq 1 -and $posted[0].ok) ($posted | ConvertTo-Json -Compress)
        Assert-That 'with its stars, module, version and text' ($posted.Count -eq 1 -and $posted[0].mod -eq 'qa.cat.good' -and
            $posted[0].stars -eq 4 -and $posted[0].version -eq '1.0.0' -and $posted[0].text -eq $text)

        # ---- two authors withdraw modules the browser installed: qa.cat.wdrawn publishes 1.1.0 after
        # that, qa.cat.wdrevoked is revoked after that
        Copy-Item -LiteralPath (Join-Path $catalogAssets 'index_withdrawn.ltx') -Destination (Join-Path $catalogAssets 'catalog\index.ltx') -Force
        Copy-Item -LiteralPath (Join-Path $catalogAssets 'withdraw\qa.cat.wdrawn.update.ltx') -Force `
            -Destination (Join-Path $catalogAssets 'github\QaOwner\qa-cat-wdrawn\qa.cat.wdrawn.update.ltx')
        $log = Invoke-Engine -Phase 'catalog-withdraw' -Catalog -Commands @('xms_catalog refresh', 'xms_catalog after xms_catalog',
            'xms_catalog after xms_update all') -WaitFor @('\[catalog\] ready, serial 7', 'qa\.cat\.wdrawn: update 1\.1\.0 refused')
        Assert-That 'a withdrawal arrives in the signed index' (($log -match '\[catalog\] serial 7 from http://127\.0\.0\.1:\d+/catalog/: 13 module\(s\)') -and
            ($log -match '\*   qa\.cat\.wdrawn withdrawn by its author') -and ($log -match '\*   qa\.cat\.wdrevoked withdrawn by its author'))
        Assert-That 'an update of a module its author withdrew is refused for that' ($log -match
            'qa\.cat\.wdrawn: update 1\.1\.0 refused - its author withdrew it from the Mod Browser')
        Assert-That 'and not a byte of it is downloaded' (-not (Select-String -LiteralPath $script:mockLog -Pattern 'qa\.cat\.wdrawn-1\.1\.0\.zip' -Quiet) -and
            -not (Test-Path -LiteralPath (Join-Path $qaStaged 'qa.cat.wdrawn')))

        # the next start decides with the withdrawals and the revocation cached
        $log = Invoke-Engine -Phase 'catalog-withdrawn' -Catalog -Commands @('xms_catalog refresh', 'xms_mods view installed',
            'xms_mods wait 4000', 'xms_mods select qa.cat.wdrawn', 'xms_mods wait 300', 'xms_mods state', 'xms_mods select qa.cat.wdrevoked',
            'xms_mods wait 300', 'xms_mods state', 'xms_mods view browser', 'xms_mods wait 1000', 'xms_mods state',
            'xms_mods mark withdrawn_done') -WaitFor @('qa\.cat\.wdrawn: update 1\.1\.0 refused', '\[mods-ui\] mark withdrawn_done')
        Assert-That 'a module its author withdrew stays enabled' (($log -match '\* XMS: \[\d+\] qa\.cat\.wdrawn 1\.0\.0 \(') -and
            ($log -notmatch 'module \[qa\.cat\.wdrawn\] disabled'))
        Assert-That 'a revocation still switches a withdrawn module off' ($log -match
            'module \[qa\.cat\.wdrevoked\] disabled: withdrawn from the mod catalog: QA revocation after its withdrawal')
        Assert-That 'the withdrawn module''s updates stay refused' ($log -match
            'qa\.cat\.wdrawn: update 1\.1\.0 refused - its author withdrew it from the Mod Browser')
        $uiLog = Read-PhaseLog 'catalog-withdrawn'
        $cardStatus = @{}
        foreach ($card in [regex]::Matches($uiLog, '\[mods-ui\] card (\S+): status "([^"]*)"')) { $cardStatus[$card.Groups[1].Value] = $card.Groups[2].Value }
        Assert-That 'the Mods window says its author withdrew it' (Test-ModsText $cardStatus['qa.cat.wdrawn'] 'st_mods_status_author_withdrawn') `
            $cardStatus['qa.cat.wdrawn']
        Assert-That 'as news, not as a refusal' ($uiLog -match '\[mods-ui\] row qa\.cat\.wdrawn "[^"]*" badge=- ')
        Assert-That 'a revocation''s own text wins over the withdrawal' (Test-ModsText $cardStatus['qa.cat.wdrevoked'] 'st_mods_status_withdrawn' `
                'QA revocation after its withdrawal') $cardStatus['qa.cat.wdrevoked']
        # every row of the browser's dump, whatever other lines a worker wrote between them
        $browserAt = $uiLog.IndexOf('[mods-ui] view browser,')
        $doneAt = $uiLog.IndexOf('[mods-ui] mark withdrawn_done')
        $browserRows = @(if ($browserAt -ge 0 -and $doneAt -gt $browserAt) {
                [regex]::Matches($uiLog.Substring($browserAt, $doneAt - $browserAt), '\[mods-ui\] row (\S+) ') | ForEach-Object { $_.Groups[1].Value } })
        Assert-That 'the browser offers no withdrawn module' ($browserRows.Count -gt 0 -and
            -not @($browserRows | Where-Object { $_ -like 'qa.cat.wd*' }).Count) ($browserRows -join ', ')

        # ---- removal, and protection that stays on
        $log = Invoke-Engine -Phase 'catalog-remove' -Catalog -Commands @('xms_remove qa.cat.good', 'xms_remove qa.cat.upd',
            'xms_remove qa.cat.upd2', 'xms_remove qa.cat.wdrawn', 'xms_remove qa.cat.wdrevoked', 'xms_remove qa.cat.revinst') -WaitFor @(
            'qa\.cat\.revinst: removed by the next start')
        $removal = Join-Path $qaStaged 'qa.cat.good\ready.ltx'
        Assert-That 'a removal is staged' ((Test-Path -LiteralPath $removal) -and (Select-String -LiteralPath $removal -Pattern 'action = remove' -Quiet))
        $log = Invoke-Engine -Phase 'catalog-removed' -Catalog -Commands @('xms_protect') -WaitFor @('XMS: protected mode')
        Assert-That 'the removal is applied by the next start' (($log -match 'module \[qa\.cat\.good\] removed') -and
            -not (Test-Path -LiteralPath (Join-Path $qaModules 'qa.cat.good')) -and -not (Test-Path -LiteralPath (Join-Path $qaModules 'qa.cat.upd')) -and
            -not (Test-Path -LiteralPath (Join-Path $qaModules 'qa.cat.wdrawn')))
        Assert-That 'and forgotten by the browser''s list' (-not (Select-String -LiteralPath $state -Pattern '^qa\.cat\.good = ' -Quiet))
        Assert-That 'protection stays on without catalog modules' ($log -match 'protected mode ON \(switched on in the Mods window, or a catalog module was enabled before\)')
    }
    finally {
        if ($mockProcess -and -not $mockProcess.HasExited) { Stop-Process -Id $mockProcess.Id -Force -ErrorAction SilentlyContinue }
    }
}

function Invoke-AdoptScenario {
    # MOD_CATALOG.md 14.1: modules copied in by hand, held against the catalog once one is accepted.
    # One start: no catalog cached, the first check takes nothing, the browser view loads it.
    Assert-NoTargetEngine
    New-QaRoot
    if (Test-Path -LiteralPath $qaModules) { Remove-Item -LiteralPath $qaModules -Recurse -Force }
    New-Item -ItemType Directory -Path $qaModules -Force | Out-Null
    $catalogAssets = Join-Path $qaRoot 'catalog-assets'
    $catalogTool = Join-Path $PSScriptRoot 'catalog_qa.py'
    Write-Host '== generating the adoption fixture'
    $fixtureArgs = @($catalogTool, 'adopt', '--modules', $qaModules, '--assets', $catalogAssets, '--port', $Port)
    if ($RealRelease) { $fixtureArgs += @('--real', $RealRelease) }
    & python @fixtureArgs
    if ($LASTEXITCODE) { throw 'the adoption fixture could not be made' }
    $script:catalogEnv = Get-Content -LiteralPath (Join-Path $catalogAssets 'env.json') -Raw | ConvertFrom-Json
    $script:mockLog = Join-Path $resultRoot 'mock.log'
    $mockProcess = Start-Process python -ArgumentList @("`"$catalogTool`"", 'serve', '--assets', "`"$catalogAssets`"", '--port', $Port,
        '--log', "`"$($script:mockLog)`"") -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 2
    $state = Join-Path $qaModules '.catalog\state.ltx'

    try {
        # a card is drawn on the frame after it is selected: each "state" waits for one
        $commands = @('xms_mods', 'xms_mods wait 3000', 'xms_mods view browser', 'xms_mods wait 5000', 'xms_mods state',
            'xms_mods view installed')
        foreach ($id in 'qa.adopt.old', 'qa.adopt.same', 'qa.adopt.wd', 'qa.adopt.extra', 'qa.adopt.foreign') {
            $commands += "xms_mods select $id", 'xms_mods wait 300', 'xms_mods state'
        }
        $log = Invoke-Engine -Phase 'adopt' -Catalog -Commands ($commands + 'xms_mods mark done') -WaitFor @('\[mods-ui\] mark done')
        $first = $log.IndexOf('modules installed by hand are held against it')
        Assert-That 'without a catalog the first check takes nothing' ($first -gt 0 -and
            $log.Substring(0, $first) -notmatch 'installed by hand')
        foreach ($taken in @(@('qa.adopt.old', 'v1\.0\.0'), @('qa.adopt.same', 'the latest release'), @('qa.adopt.wd', 'v1\.0\.0'))) {
            Assert-That "$($taken[0]) is taken for a browser installation" ($log -match
                ('\[mods\] {0}: installed by hand, release 1\.0\.0 of the Mod Browser byte for byte \({1}\)' -f [regex]::Escape($taken[0]), $taken[1]))
        }
        foreach ($refused in @(@('qa.adopt.edited', 'its files are not those of release 1\.0\.0'),
                @('qa.adopt.extra', 'its files are not those of release 1\.0\.0'), @('qa.adopt.git', 'it holds \.git'),
                @('qa.adopt.dev', 'version 1\.2\.0 is not published'), @('qa.adopt.unsigned', 'release 1\.0\.0 is not signed by the author'),
                @('qa.adopt.floor', 'version 1\.0\.0 is below the lowest the catalog lists'))) {
            Assert-That "$($refused[0]) is no release" ($log -match
                ('\[mods\] {0}: installed by hand, not a release of the Mod Browser - {1}' -f [regex]::Escape($refused[0]), $refused[1]))
        }
        Assert-That 'a held release is not taken' ($log -match
            '\[mods\] qa\.adopt\.held: installed by hand, a release the Mod Browser does not take - the catalog withdrew release 1\.0\.0')
        Assert-That 'another author''s module under a listed id is never looked at' ($log -notmatch 'qa\.adopt\.foreign: installed by hand')
        Assert-That 'the adopted older release is offered its update' ($log -match '\[mods\] qa\.adopt\.old: about \d+ of \d+ byte\(s\) to download' -and
            $log -notmatch 'qa\.adopt\.old: update 1\.1\.0 refused')
        Assert-That 'the adopted withdrawn module takes no update' ($log -match 'qa\.adopt\.wd: update 1\.1\.0 refused - its author withdrew it')
        Assert-That 'an edited copy is not offered its own version as a fix' (
            $log.Substring([Math]::Max($first, 0)) -notmatch 'qa\.adopt\.edited: 1\.0\.0 was published again')
        Assert-That 'the browser does not call another author''s module installed' ($log -match '\[mods-ui\] row qa\.adopt\.foreign "[^"]*" badge=- ')
        $stateText = if (Test-Path -LiteralPath $state) { Get-Content -LiteralPath $state -Raw } else { '' }
        Assert-That 'state.ltx records exactly the adoptions' (($stateText -match '(?m)^qa\.adopt\.old = ') -and
            ($stateText -match '(?m)^qa\.adopt\.same = ') -and ($stateText -match '(?m)^qa\.adopt\.wd = ') -and
            ($stateText -notmatch '(?m)^qa\.adopt\.(edited|extra|git|dev|unsigned|floor|held|foreign) = '))
        if ($RealRelease) {
            Assert-That 'the real published release is taken on its real signature' ($log -match
                '\[mods\] darf-overnight-delivery: installed by hand, release [0-9.]+ of the Mod Browser byte for byte')
        }
        Assert-That 'no file of a module was changed' (-not (Test-Path -LiteralPath $qaStaged) -and
            (Test-Path -LiteralPath (Join-Path $qaModules 'qa.adopt.git\.git\HEAD')) -and
            (Test-Path -LiteralPath (Join-Path $qaModules 'qa.adopt.extra\gamedata\configs\mine.ltx')))
    }
    finally {
        if ($mockProcess -and -not $mockProcess.HasExited) { Stop-Process -Id $mockProcess.Id -Force -ErrorAction SilentlyContinue }
    }
}


# ---------------------------------------------------------------------------- run
if ($Scenario -eq 'Catalog') {
    try { Invoke-CatalogScenario }
    finally { Clear-QaEnvironment }
    Complete-QaRun
}
if ($Scenario -eq 'Adopt') {
    try { Invoke-AdoptScenario }
    finally { Clear-QaEnvironment }
    Complete-QaRun
}

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
    Clear-QaEnvironment
}
Complete-QaRun
