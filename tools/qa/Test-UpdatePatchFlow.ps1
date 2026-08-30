# End-to-end check of the patch update path, run entirely on a synthetic installation.
#
# Builds a "1.0.0" install, then a 1.0.1 release in both shapes - the full archive and the
# patch that carries only what changed - and drives the real DeadAirUpdater.exe over each.
# What it proves: a patch lands the installation in EXACTLY the state the full archive would,
# leaves the untouched files alone, deletes files the new version dropped, and refuses (with
# the marker the client reads) when a file it expected to find locally has been altered.
#
# The maintenance program is stubbed by a do-nothing executable that the manifest declares
# like any other file, so no installer runs and nothing outside the temporary tree is touched.
[CmdletBinding()]
param(
    [string]$Updater = (Join-Path $PSScriptRoot "..\..\build\installer\DeadAirUpdater.exe"),
    [string]$WorkRoot = (Join-Path $env:TEMP "dar-update-patch-test")
)

$ErrorActionPreference = "Stop"
$sevenZip = (Get-Command 7z.exe -ErrorAction Stop).Source
$Updater = (Resolve-Path $Updater).Path

$script:failures = 0
function Assert-That {
    param([Parameter(Mandatory)][bool]$Condition, [Parameter(Mandatory)][string]$Because)
    if ($Condition) {
        Write-Host "  ok   $Because"
    }
    else {
        Write-Host "  FAIL $Because" -ForegroundColor Red
        $script:failures++
    }
}

function New-Payload {
    param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][hashtable]$Files)
    foreach ($relative in $Files.Keys) {
        $path = Join-Path $Root $relative.Replace('/', '\')
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllText($path, $Files[$relative], [Text.UTF8Encoding]::new($false))
    }
}

function Write-Manifest {
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Version,
        [string]$BaseVersion,
        [hashtable]$FullFileSet
    )
    $lines = [Collections.Generic.List[string]]::new()
    if ($BaseVersion) {
        $lines.Add("schema=dead-air-refined.update/2")
        $lines.Add("version=$Version")
        $lines.Add("kind=patch")
        $lines.Add("base=$BaseVersion")
    }
    else {
        $lines.Add("schema=dead-air-refined.update/1")
        $lines.Add("version=$Version")
    }
    foreach ($relative in ($FullFileSet.Keys | Sort-Object)) {
        $lines.Add($FullFileSet[$relative])
    }
    [IO.File]::WriteAllText((Join-Path $Root "update-manifest.txt"),
        [string]::Join("`n", $lines) + "`n", [Text.UTF8Encoding]::new($false))
}

function Get-ManifestRows {
    param([Parameter(Mandatory)][string]$Root)
    $rows = @{}
    Get-ChildItem -LiteralPath $Root -Recurse -File |
        Where-Object { $_.FullName.Substring($Root.Length + 1) -ne "update-manifest.txt" } |
        ForEach-Object {
            $relative = $_.FullName.Substring($Root.Length + 1).Replace('\', '/')
            $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            $rows[$relative] = "$hash`t$($_.Length)`t$relative"
        }
    return $rows
}

function New-Archive {
    param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$ArchivePath)
    if (Test-Path -LiteralPath $ArchivePath) { Remove-Item -LiteralPath $ArchivePath -Force }
    Push-Location $Root
    try {
        & $sevenZip a -tzip -mx=1 $ArchivePath "*" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7-Zip failed: $LASTEXITCODE" }
    }
    finally { Pop-Location }
    return "sha256:" + (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-Updater {
    param(
        [Parameter(Mandatory)][string]$GameDir,
        [Parameter(Mandatory)][string]$Archive,
        [Parameter(Mandatory)][string]$Version,
        [Parameter(Mandatory)][string]$Digest
    )
    # The finish stage wipes the directory the archive sits in, and its parent - mirror the
    # real layout (<game>/.dead-air-x64/update-cache/<version>/) so it deletes only that.
    $cache = Join-Path $GameDir ".dead-air-x64\update-cache\$Version"
    New-Item -ItemType Directory -Path $cache -Force | Out-Null
    $staged = Join-Path $cache (Split-Path -Leaf $Archive)
    Copy-Item -LiteralPath $Archive -Destination $staged -Force
    $Archive = $staged

    # A restart command that does nothing: the finish stage relaunches it after cleanup.
    $restart = Join-Path (Split-Path -Parent $Archive) "restart-command.bin"
    $command = "$env:ComSpec /c exit"
    [IO.File]::WriteAllBytes($restart, [Text.Encoding]::Unicode.GetBytes($command + "`0"))

    # --wait-pid wants a process that is already gone; our own pid would deadlock the test.
    $ghost = Start-Process -FilePath $env:ComSpec -ArgumentList "/c exit" -PassThru -WindowStyle Hidden
    $ghost.WaitForExit()

    $arguments = @(
        "--game-dir", $GameDir, "--archive", $Archive, "--version", $Version,
        "--digest", $Digest, "--wait-pid", $ghost.Id, "--restart-command", $restart
    )
    $process = Start-Process -FilePath $Updater -ArgumentList $arguments -PassThru -Wait -WindowStyle Hidden
    return $process.ExitCode
}

function New-Installation {
    param([Parameter(Mandatory)][string]$GameDir, [Parameter(Mandatory)][hashtable]$Files)
    if (Test-Path -LiteralPath $GameDir) { Remove-Item -LiteralPath $GameDir -Recurse -Force }
    New-Item -ItemType Directory -Path $GameDir -Force | Out-Null
    New-Payload -Root $GameDir -Files $Files
    $control = Join-Path $GameDir ".dead-air-x64"
    New-Item -ItemType Directory -Path $control -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $control "port-version.txt"), "1.0.0")
    # The list of files the installation owns. Without it the applier has no scope to prune,
    # so a file dropped by the new version would linger - exactly as a real install ships it.
    [IO.File]::WriteAllText((Join-Path $control "runtime-files.txt"),
        [string]::Join("`r`n", ($Files.Keys | Sort-Object)) + "`r`n")
    Copy-Item -LiteralPath $Updater -Destination (Join-Path $GameDir "DeadAirUpdater.exe") -Force
}

if (Test-Path -LiteralPath $WorkRoot) { Remove-Item -LiteralPath $WorkRoot -Recurse -Force }
New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null

# A no-op stand-in for the Inno maintenance program, declared in the manifest like any file.
$stubSource = Join-Path $WorkRoot "stub.cpp"
[IO.File]::WriteAllText($stubSource, "int __stdcall wWinMain(void*,void*,wchar_t*,int){return 0;}`n")
$stubExe = Join-Path $WorkRoot "maintenance-stub.exe"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerPrompt = Join-Path $visualStudio "Common7\Tools\VsDevCmd.bat"
$build = 'call "' + $developerPrompt + '" -arch=x64 -host_arch=x64 && cl.exe /nologo /O1 /MT ' +
    '/Fo:"' + (Join-Path $WorkRoot "stub.obj") + '" /Fe:"' + $stubExe + '" "' + $stubSource + '" ' +
    '/link /SUBSYSTEM:WINDOWS /ENTRY:wWinMain kernel32.lib'
& cmd.exe /d /s /c $build | Out-Null
if (-not (Test-Path -LiteralPath $stubExe)) { throw "Could not build the maintenance stub." }
$stubBytes = [Convert]::ToBase64String([IO.File]::ReadAllBytes($stubExe))

# ---- the two versions -------------------------------------------------------------------
# unchanged.txt is identical in both: it is what the patch must be able to leave out.
# dropped.txt exists only in 1.0.0: the patch must still cause its removal.
$common = @{
    "unchanged.txt" = "this file never changes between the two versions"
}
$v100 = $common + @{ "changed.txt" = "version 1.0.0 payload"; "dropped.txt" = "removed in 1.0.1" }
$v101 = $common + @{ "changed.txt" = "version 1.0.1 payload - different bytes"; "added.txt" = "new in 1.0.1" }

$fullRoot = Join-Path $WorkRoot "full-1.0.1"
New-Item -ItemType Directory -Path $fullRoot -Force | Out-Null
New-Payload -Root $fullRoot -Files $v101
New-Item -ItemType Directory -Path (Join-Path $fullRoot ".dead-air-x64") -Force | Out-Null
[IO.File]::WriteAllBytes((Join-Path $fullRoot ".dead-air-x64\Dead-Air-Refined-Maintenance.exe"),
    [Convert]::FromBase64String($stubBytes))
$rows = Get-ManifestRows -Root $fullRoot
Write-Manifest -Root $fullRoot -Version "1.0.1" -FullFileSet $rows
$fullArchive = Join-Path $WorkRoot "full-1.0.1.zip"
$fullDigest = New-Archive -Root $fullRoot -ArchivePath $fullArchive

# The patch: same manifest rows, but only the files whose bytes differ are packed. The
# maintenance stub is deliberately NOT packed, which exercises the fallback to the installed
# copy - the case a real release hits whenever that program does not change.
$patchRoot = Join-Path $WorkRoot "patch-1.0.1"
New-Item -ItemType Directory -Path $patchRoot -Force | Out-Null
New-Payload -Root $patchRoot -Files @{
    "changed.txt" = $v101["changed.txt"]
    "added.txt"   = $v101["added.txt"]
}
Write-Manifest -Root $patchRoot -Version "1.0.1" -BaseVersion "1.0.0" -FullFileSet $rows
$patchArchive = Join-Path $WorkRoot "patch-1.0.1.zip"
$patchDigest = New-Archive -Root $patchRoot -ArchivePath $patchArchive

Write-Host "patch carries $((Get-ChildItem -LiteralPath $patchRoot -Recurse -File).Count) entries against $($rows.Count) manifest rows"

# ---- case 1: the full archive still applies (schema /1 must not regress) ------------------
Write-Host "`ncase 1: full archive"
$gameDir = Join-Path $WorkRoot "game-full"
New-Installation -GameDir $gameDir -Files $v100
# The installed maintenance program has to exist for the fallback case later; put it here too.
[IO.File]::WriteAllBytes((Join-Path $gameDir ".dead-air-x64\Dead-Air-Refined-Maintenance.exe"),
    [Convert]::FromBase64String($stubBytes))
$code = Invoke-Updater -GameDir $gameDir -Archive $fullArchive -Version "1.0.1" -Digest $fullDigest
Assert-That ($code -eq 0) "updater exited 0 (got $code)"
Assert-That ((Get-Content (Join-Path $gameDir "changed.txt") -Raw).Trim() -eq $v101["changed.txt"]) "changed.txt updated"
Assert-That (Test-Path (Join-Path $gameDir "added.txt")) "added.txt created"
Assert-That (-not (Test-Path (Join-Path $gameDir "dropped.txt"))) "dropped.txt removed"

# ---- case 2: the patch reaches the identical state ----------------------------------------
Write-Host "`ncase 2: patch archive"
$patchDir = Join-Path $WorkRoot "game-patch"
New-Installation -GameDir $patchDir -Files $v100
[IO.File]::WriteAllBytes((Join-Path $patchDir ".dead-air-x64\Dead-Air-Refined-Maintenance.exe"),
    [Convert]::FromBase64String($stubBytes))
$untouchedBefore = (Get-Item (Join-Path $patchDir "unchanged.txt")).LastWriteTimeUtc
$code = Invoke-Updater -GameDir $patchDir -Archive $patchArchive -Version "1.0.1" -Digest $patchDigest
Assert-That ($code -eq 0) "updater exited 0 (got $code)"
Assert-That ((Get-Content (Join-Path $patchDir "changed.txt") -Raw).Trim() -eq $v101["changed.txt"]) "changed.txt updated from the patch"
Assert-That (Test-Path (Join-Path $patchDir "added.txt")) "added.txt created from the patch"
Assert-That (-not (Test-Path (Join-Path $patchDir "dropped.txt"))) "dropped.txt removed by the patch"
Assert-That ((Get-Item (Join-Path $patchDir "unchanged.txt")).LastWriteTimeUtc -eq $untouchedBefore) "unchanged.txt left untouched"

# The decisive comparison: both routes must produce byte-identical installations.
$compare = {
    param($root)
    Get-ChildItem -LiteralPath $root -Recurse -File |
        Where-Object { $_.FullName -notmatch '\\\.dead-air-x64\\backups\\' } |
        ForEach-Object { "$($_.FullName.Substring($root.Length + 1).Replace('\','/'))=$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)" } |
        Sort-Object
}
$fullState = & $compare $gameDir
$patchState = & $compare $patchDir
Assert-That (($fullState -join "`n") -eq ($patchState -join "`n")) "patch and full installs are byte-identical"

# ---- case 3: a patch that does not fit must refuse, and say so ----------------------------
Write-Host "`ncase 3: patch onto a modified installation"
$dirtyDir = Join-Path $WorkRoot "game-dirty"
New-Installation -GameDir $dirtyDir -Files $v100
[IO.File]::WriteAllBytes((Join-Path $dirtyDir ".dead-air-x64\Dead-Air-Refined-Maintenance.exe"),
    [Convert]::FromBase64String($stubBytes))
[IO.File]::WriteAllText((Join-Path $dirtyDir "unchanged.txt"), "a mod edited this file")
$code = Invoke-Updater -GameDir $dirtyDir -Archive $patchArchive -Version "1.0.1" -Digest $patchDigest
Assert-That ($code -eq 24) "updater reported the patch as unusable (got $code)"
$marker = Join-Path $dirtyDir ".dead-air-x64\patch-rejected.txt"
Assert-That (Test-Path $marker) "patch-rejected marker written"
if (Test-Path $marker) {
    Assert-That ((Get-Content $marker -Raw).Trim() -eq "1.0.1") "marker names the version"
}
Assert-That ((Get-Content (Join-Path $dirtyDir "changed.txt") -Raw).Trim() -eq $v100["changed.txt"]) "installation left untouched after refusal"

# ---- case 4: the full archive rescues that same installation ------------------------------
Write-Host "`ncase 4: full archive after a refused patch"
$code = Invoke-Updater -GameDir $dirtyDir -Archive $fullArchive -Version "1.0.1" -Digest $fullDigest
Assert-That ($code -eq 0) "updater exited 0 (got $code)"
Assert-That ((Get-Content (Join-Path $dirtyDir "changed.txt") -Raw).Trim() -eq $v101["changed.txt"]) "changed.txt updated"
Assert-That (-not (Test-Path $marker)) "marker cleared after a successful update"

Write-Host ""
if ($script:failures) {
    Write-Host "$($script:failures) check(s) FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "all checks passed" -ForegroundColor Green
