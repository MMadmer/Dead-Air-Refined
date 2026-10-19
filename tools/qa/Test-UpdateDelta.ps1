# End-to-end check of the update delta, through the real game and the real updater.
#
# Builds a rig from the worktree's build (database, shader cache and settings borrowed from an
# existing installation, the database by hard link), then a release 1.6.1 whose Update.zip is the
# rig's own payload with one file changed, one added and one dropped. The release mock serves it the
# way GitHub does - the release list, a redirect to storage, ranged downloads - and the game, started
# with -qa_update -qa_update_auto, has to find the release, read the archive's directory and
# manifest by range, fetch only the two records it lacks, write the patch, quit into the updater
# and come back up on 1.6.1's file set.
#
# The maintenance program in both payloads is a do-nothing stub, so no installer runs and nothing
# outside the rig is touched. One game launch, about a minute.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Updater,
    [string]$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    # An installation of the same version whose database, content state, settings and shader cache
    # the rig borrows. Nothing in it is written.
    [string]$Source = "D:\Games\Dead Air",
    [string]$Rig = "D:\Games\Dead Air\_qa\updelta",
    [int]$Port = 8791,
    [int]$TimeoutSeconds = 240
)

$ErrorActionPreference = "Stop"
$sevenZip = (Get-Command 7z.exe -ErrorAction Stop).Source
$work = "$Rig-work"
$failures = [Collections.Generic.List[string]]::new()

function Assert-Case {
    param([string]$Name, [bool]$Condition, [string]$Detail = "")
    if ($Condition) { Write-Host "  ok   $Name" }
    else {
        Write-Host "  FAIL $Name $Detail" -ForegroundColor Red
        $script:failures.Add($Name)
    }
}

function Stop-RigProcesses {
    Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ($_.Path.StartsWith("$Rig\", [StringComparison]::OrdinalIgnoreCase) -or
        $_.Path.StartsWith("$work\", [StringComparison]::OrdinalIgnoreCase)) } |
        ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
}

function Get-Sha256 { param([string]$Path) (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

# ---- the rig ---------------------------------------------------------------------------------
Stop-RigProcesses
foreach ($path in $Rig, $work) {
    # The database holds hard links only; removing them leaves the source installation alone.
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
}
New-Item -ItemType Directory -Force -Path $Rig, $work, "$Rig\database", "$Rig\appdata", "$Rig\.dead-air-x64" | Out-Null

$runtime = Get-Content (Join-Path $Repo "packaging\dead-air-x64\installer\runtime-files.txt") | ForEach-Object Trim | Where-Object { $_ }
foreach ($file in $runtime) { Copy-Item -LiteralPath (Join-Path $Repo "bin\x64\Release\$file") -Destination $Rig }
Copy-Item -LiteralPath $Updater -Destination (Join-Path $Rig "DeadAirUpdater.exe")
Copy-Item -Path (Join-Path $Repo "packaging\dead-air-x64\compatibility\gamedata") -Destination (Join-Path $Rig "gamedata") -Recurse
Get-ChildItem -LiteralPath (Join-Path $Source "database") -File | ForEach-Object {
    New-Item -ItemType HardLink -Path (Join-Path $Rig "database\$($_.Name)") -Target $_.FullName | Out-Null
}
Copy-Item -LiteralPath (Join-Path $Source "fsgame.ltx") -Destination $Rig
Copy-Item -LiteralPath (Join-Path $Source "appdata\user.ltx") -Destination (Join-Path $Rig "appdata")
Copy-Item -LiteralPath (Join-Path $Source "appdata\shaders_cache_grizzly") -Destination (Join-Path $Rig "appdata") -Recurse
foreach ($file in "content-manifest.txt", "content-state.txt") {
    Copy-Item -LiteralPath (Join-Path $Source ".dead-air-x64\$file") -Destination (Join-Path $Rig ".dead-air-x64")
}
Copy-Item -LiteralPath (Join-Path $Repo "packaging\dead-air-x64\installer\runtime-files.txt") -Destination (Join-Path $Rig ".dead-air-x64")

# The maintenance stub: the updater runs whatever the payload calls the maintenance program.
$stubSource = Join-Path $work "stub.cpp"
[IO.File]::WriteAllText($stubSource, "int __stdcall wWinMain(void*,void*,wchar_t*,int){return 0;}`n")
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
& cmd.exe /d /s /c ('call "' + (Join-Path $vs "Common7\Tools\VsDevCmd.bat") + '" -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /O1 ' +
    '/Fo:"' + (Join-Path $work "stub.obj") + '" /Fe:"' + (Join-Path $Rig ".dead-air-x64\Dead-Air-Refined-Maintenance.exe") + '" "' +
    $stubSource + '" /link /SUBSYSTEM:WINDOWS >nul')
if ($LASTEXITCODE -ne 0) { throw "the maintenance stub did not build" }

[IO.File]::WriteAllText((Join-Path $Rig "qa-delta-changed.txt"), "before`n")
[IO.File]::WriteAllText((Join-Path $Rig "qa-delta-dropped.txt"), "only in 1.6.0`n")
[IO.File]::WriteAllText((Join-Path $Rig ".dead-air-x64\port-version.txt"), "1.6.0")

# The payload of 1.6.0 as the rig holds it: what managed-files.txt names.
$payloadFiles = @($runtime) + @("DeadAirUpdater.exe", "database/xtra_dead_air_x64.xdb0", ".dead-air-x64/content-manifest.txt",
    ".dead-air-x64/runtime-files.txt", ".dead-air-x64/Dead-Air-Refined-Maintenance.exe", "qa-delta-changed.txt", "qa-delta-dropped.txt")
[IO.File]::WriteAllLines((Join-Path $Rig ".dead-air-x64\managed-files.txt"), [string[]]($payloadFiles | ForEach-Object { $_.Replace('/', '\') }))

# ---- the release ------------------------------------------------------------------------------
$payload = Join-Path $work "payload"
New-Item -ItemType Directory -Force -Path $payload, "$payload\appdata\savedgames" | Out-Null
foreach ($file in $payloadFiles) {
    if ($file -eq "qa-delta-dropped.txt") { continue }
    $target = Join-Path $payload $file.Replace('/', '\')
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath (Join-Path $Rig $file.Replace('/', '\')) -Destination $target
}
[IO.File]::WriteAllText((Join-Path $payload "qa-delta-changed.txt"), "after, in 1.6.1`n")
[IO.File]::WriteAllText((Join-Path $payload "qa-delta-added.txt"), "new in 1.6.1`n")
# A third file at the other end of the archive: two separate ranges, so the second request goes to
# the storage URL of the first - which the mock lets expire after one use.
[IO.File]::WriteAllText((Join-Path $payload ".dead-air-x64\qa-delta-note.txt"), "new in 1.6.1, first in the archive`n")

$manifest = [Collections.Generic.List[string]]::new()
$manifest.Add("schema=dead-air-refined.update/1")
$manifest.Add("version=1.6.1")
Get-ChildItem -LiteralPath $payload -Recurse -File | Sort-Object { $_.FullName.Substring($payload.Length + 1) } | ForEach-Object {
    $relative = $_.FullName.Substring($payload.Length + 1).Replace('\', '/')
    $manifest.Add("$(Get-Sha256 $_.FullName)`t$($_.Length)`t$relative")
}
[IO.File]::WriteAllText((Join-Path $payload "update-manifest.txt"), [string]::Join("`n", $manifest) + "`n", [Text.UTF8Encoding]::new($false))

$release = Join-Path $work "releases\1.6.1"
New-Item -ItemType Directory -Force -Path $release | Out-Null
$archive = Join-Path $release "Dead-Air-Refined-1.6.1-Update.zip"
Push-Location $payload
try {
    & $sevenZip a -tzip -mx=9 $archive "*" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7-Zip failed: $LASTEXITCODE" }
}
finally { Pop-Location }
Copy-Item -LiteralPath (Join-Path $Rig ".dead-air-x64\content-manifest.txt") -Destination (Join-Path $release "Dead-Air-Refined-1.6.1-content-manifest.txt")
[IO.File]::WriteAllText((Join-Path $release "body.md"), "## EN`n`n## Changes`n`n* The delta test release.`n`n## RU`n`n## Изменения`n`n* Тестовый выпуск дельты.`n")
$archiveSize = (Get-Item -LiteralPath $archive).Length

# ---- the run ----------------------------------------------------------------------------------
$mockExe = Join-Path $work "release-mock.exe"
Push-Location (Join-Path $PSScriptRoot "release-mock")
try { & go build -o $mockExe .; if ($LASTEXITCODE -ne 0) { throw "the release mock did not build" } }
finally { Pop-Location }
$mockLog = Join-Path $work "mock.jsonl"
$mock = Start-Process -FilePath $mockExe -PassThru -WindowStyle Hidden -ArgumentList @(
    "-listen", "127.0.0.1:$Port", "-releases", "`"$(Join-Path $work 'releases')`"", "-log", "`"$mockLog`"", "-timeout", "15m",
    "-expire-after", "1")

$env:DAR_QA_UPDATE_API = "http://127.0.0.1:$Port/repos/MMadmer/Dead-Air-Refined/releases?per_page=30"
& (Join-Path $PSScriptRoot "Start-DetachedHiddenDesktopProcess.ps1") -FilePath (Join-Path $Rig "xrEngine.exe") `
    -Arguments "-qa_update -qa_update_auto -silent_error_mode -force_flushlog -always_active" `
    -ProcessIdFile (Join-Path $work "game.pid") -LauncherIdFile (Join-Path $work "launcher.pid") `
    -DesktopName "DeadAirUpdateDelta" -TimeoutSeconds $TimeoutSeconds | Out-Null

# The hidden desktop lives only as long as something holds it. The launcher lets go when the game
# quits into the updater, and a process the updater starts in the moment its parent exits finds no
# desktop to attach to and dies before its first line - which no player's desktop ever does. The
# test holds the desktop for the whole run.
Add-Type -Namespace QaDesktop -Name Native -MemberDefinition @'
[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern IntPtr OpenDesktop(string name, int flags, bool inherit, uint access);
[DllImport("user32.dll")] public static extern bool CloseDesktop(IntPtr desktop);
'@
$desktop = [IntPtr]::Zero
for ($attempt = 0; $attempt -lt 100 -and $desktop -eq [IntPtr]::Zero; $attempt++) {
    Start-Sleep -Milliseconds 100
    $desktop = [QaDesktop.Native]::OpenDesktop("DeadAirUpdateDelta", 0, $false, 0x10000000)
}

# Done when the update has landed: the added file is there and the update cache is gone.
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$landed = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    if ((Test-Path (Join-Path $Rig "qa-delta-added.txt")) -and -not (Test-Path (Join-Path $Rig ".dead-air-x64\update-cache"))) {
        $landed = $true
        break
    }
}
if (-not $landed) {
    # Whatever is still running under the rig, and what its windows say - an updater that failed
    # says so in a message box nobody can see on the hidden desktop.
    Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith("$Rig\", [StringComparison]::OrdinalIgnoreCase) } |
        ForEach-Object {
            Write-Host "  still running: $($_.ProcessId) $($_.CommandLine)"
            try {
                & (Join-Path $PSScriptRoot "Get-HiddenDesktopWindowInfo.ps1") -DesktopName "DeadAirUpdateDelta" -ProcessId $_.ProcessId |
                    ForEach-Object { Write-Host "    window: $_" }
            }
            catch { Write-Host "    (no window information: $($_.Exception.Message))" }
        }
}
# The game the updater relaunched checks again; the mock goes first so that check finds nothing.
Start-Sleep -Seconds 3
Stop-Process -Id $mock.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 5
Stop-RigProcesses
if ($desktop -ne [IntPtr]::Zero) { [QaDesktop.Native]::CloseDesktop($desktop) | Out-Null }

# ---- the verdict ------------------------------------------------------------------------------
$logs = @(Get-ChildItem -LiteralPath (Join-Path $Rig "appdata\logs") -Filter "grizzly_admin*" -ErrorAction SilentlyContinue |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -ErrorAction SilentlyContinue })
$deltaLine = $logs | Where-Object { $_ -match '\* Update delta: ' } | Select-Object -First 1
Write-Host "  log: $deltaLine"
Write-Host "  log: $($logs | Where-Object { $_ -match '\* Update delta written' } | Select-Object -First 1)"

Assert-Case "the update landed" $landed
Assert-Case "the delta named three files" ($deltaLine -match ' 3 of \d+ file\(s\) differ') $deltaLine
Assert-Case "the changed file is the new one" ((Test-Path (Join-Path $Rig "qa-delta-changed.txt")) -and
    ([IO.File]::ReadAllText((Join-Path $Rig "qa-delta-changed.txt")) -eq "after, in 1.6.1`n"))
Assert-Case "the added files arrived" ((Test-Path (Join-Path $Rig "qa-delta-added.txt")) -and (Test-Path (Join-Path $Rig ".dead-air-x64\qa-delta-note.txt")))
Assert-Case "the dropped file is gone" (-not (Test-Path (Join-Path $Rig "qa-delta-dropped.txt")))
$unchanged = $runtime | Where-Object { (Get-Sha256 (Join-Path $Rig $_)) -ne (Get-Sha256 (Join-Path $payload $_)) }
Assert-Case "every other file is the release's" (-not $unchanged) "$unchanged"

$served = 0L
$redirected = 0
$refused = 0
foreach ($line in Get-Content -LiteralPath $mockLog -ErrorAction SilentlyContinue) {
    $entry = $line | ConvertFrom-Json
    if ($entry.path -like "*Update.zip" -and $entry.status -in 200, 206) { $served += [long]$entry.bytes }
    if ($entry.path -like "*Update.zip" -and $entry.status -eq 302) { $redirected++ }
    if ($entry.path -like "*Update.zip" -and $entry.status -eq 403) { $refused++ }
}
Write-Host "  served $served of $archiveSize bytes of the archive, $redirected request(s) through the redirect"
Assert-Case "only a sliver of the archive was downloaded" ($served -gt 0 -and $served -lt $archiveSize / 50) "$served of $archiveSize"
# The plan's reads go the long way; the download reuses the storage URL after its first request,
# and goes back through the redirect when the mock has let that URL expire.
Assert-Case "an expired storage URL sent the download back through the redirect" ($refused -ge 1 -and $redirected -le 8) "$refused refusal(s), $redirected redirect(s)"

Write-Host ""
if ($failures.Count -eq 0) { Write-Host "update delta passed" -ForegroundColor Green }
else {
    Write-Host "FAILURES:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
