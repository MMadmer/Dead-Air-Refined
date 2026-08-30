# Serves a fake GitHub releases API on loopback so the in-game update flow can be exercised
# without publishing anything. The client only talks to it when started with -qa_update AND
# DAR_QA_UPDATE_API pointing at a 127.0.0.1/localhost address (UpdateService::qa_api_url), so
# this cannot affect a normal launch.
#
# Scenarios:
#   Patch  - one newer release inside the installed major line, offering both the full
#            archive and a patch cut against the installed version (the client must pick the
#            patch, because that is the smaller download).
#   Major  - one release from the next major line only (the client must announce it instead
#            of offering an update).
#   Both   - both of the above at once, which is the case the two dialogs must not confuse:
#            the major notice comes first, the ordinary offer waits behind it.
[CmdletBinding()]
param(
    [ValidateSet("Patch", "Major", "Both", "FullOnly", "Legacy")]
    [string]$Scenario = "Both",
    [Parameter(Mandatory)][string]$InstalledVersion,
    [int]$Port = 8791,
    [string]$AssetRoot = (Join-Path $env:TEMP "dar-update-mock-assets")
)

$ErrorActionPreference = "Stop"

function New-FakeAsset {
    param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][int]$SizeKb)
    New-Item -ItemType Directory -Path $AssetRoot -Force | Out-Null
    $path = Join-Path $AssetRoot $Name
    # Deterministic filler: the client checks size and SHA-256, never the contents.
    $bytes = [byte[]]::new($SizeKb * 1024)
    for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = [byte](($i * 31 + $Name.Length) % 251) }
    [IO.File]::WriteAllBytes($path, $bytes)
    [pscustomobject]@{
        Name = $Name
        Path = $path
        Size = $bytes.Length
        Digest = "sha256:" + (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Get-NextVersion {
    param([string]$Version, [string]$Part)
    $parts = $Version.Split('.') | ForEach-Object { [int]$_ }
    switch ($Part) {
        "minor" { "$($parts[0]).$($parts[1] + 1).0" }
        "major" { "$($parts[0] + 1).0.0" }
    }
}

$minorVersion = Get-NextVersion -Version $InstalledVersion -Part "minor"
$majorVersion = Get-NextVersion -Version $InstalledVersion -Part "major"
$prefix = "Dead-Air-Refined-"

$assets = @{}
$releases = [Collections.Generic.List[object]]::new()

$body = @"
## RU

## Изменения

* Проверочный пункт списка изменений.
* Второй пункт, чтобы список был виден.

## EN

## Changes

* Mock changelog entry.
"@

if ($Scenario -in @("Patch", "Both", "FullOnly", "Legacy")) {
    $release = @{ tag = $minorVersion; assets = [Collections.Generic.List[object]]::new() }
    if ($Scenario -eq "Legacy") {
        $release.assets.Add((New-FakeAsset -Name "$prefix$minorVersion-Update.zip" -SizeKb 900))
    }
    else {
        $release.assets.Add((New-FakeAsset -Name "$prefix$minorVersion-Setup_Manual.zip" -SizeKb 900))
    }
    if ($Scenario -in @("Patch", "Both")) {
        $release.assets.Add((New-FakeAsset -Name "$prefix$minorVersion-Update_Patch.zip" -SizeKb 120))
    }
    $releases.Add($release)
}

if ($Scenario -in @("Major", "Both")) {
    $release = @{ tag = $majorVersion; assets = [Collections.Generic.List[object]]::new() }
    $release.assets.Add((New-FakeAsset -Name "$prefix$majorVersion-Setup_Manual.zip" -SizeKb 1500))
    $releases.Add($release)
}

# The release the patch is cut against has to exist in the list: the client identifies the
# base as the newest release below the offered one.
$releases.Add(@{ tag = $InstalledVersion; assets = [Collections.Generic.List[object]]::new() })
$releases[$releases.Count - 1].assets.Add((New-FakeAsset -Name "$prefix$InstalledVersion-Setup_Manual.zip" -SizeKb 880))

$escapedBody = $body.Replace('\', '\\').Replace('"', '\"').Replace("`r", "").Replace("`n", '\n')
$json = [Collections.Generic.List[string]]::new()
foreach ($release in $releases) {
    $assetJson = [Collections.Generic.List[string]]::new()
    foreach ($asset in $release.assets) {
        $assets[$asset.Name] = $asset
        $assetJson.Add(('{{"name":"{0}","browser_download_url":"http://127.0.0.1:{1}/assets/{0}","digest":"{2}","size":{3}}}' -f
            $asset.Name, $Port, $asset.Digest, $asset.Size))
    }
    $json.Add(('{{"tag_name":"{0}","html_url":"https://github.com/MMadmer/Dead-Air-Refined/releases/tag/{0}","draft":false,"prerelease":false,"body":"{1}","assets":[{2}]}}' -f
        $release.tag, $escapedBody, [string]::Join(",", $assetJson)))
}
$payload = "[" + [string]::Join(",", $json) + "]"

$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
$listener.Start()
Write-Host "mock api: http://127.0.0.1:$Port/releases  scenario=$Scenario installed=$InstalledVersion"
Write-Host "releases: $(($releases | ForEach-Object { $_.tag }) -join ', ')"

try {
    while ($true) {
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $buffer = [byte[]]::new(8192)
            $read = $stream.Read($buffer, 0, $buffer.Length)
            if ($read -le 0) { continue }
            $request = [Text.Encoding]::ASCII.GetString($buffer, 0, $read)
            $target = ($request -split "`r`n")[0].Split(' ')[1]
            Write-Host "  <- $target"

            if ($target -like "/assets/*") {
                $name = [IO.Path]::GetFileName($target)
                $asset = $assets[$name]
                if ($asset) {
                    $bytes = [IO.File]::ReadAllBytes($asset.Path)
                    $head = [Text.Encoding]::ASCII.GetBytes(
                        "HTTP/1.1 200 OK`r`nContent-Type: application/zip`r`nContent-Length: $($bytes.Length)`r`nConnection: close`r`n`r`n")
                    $stream.Write($head, 0, $head.Length)
                    $stream.Write($bytes, 0, $bytes.Length)
                }
                else {
                    $head = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 404 Not Found`r`nContent-Length: 0`r`nConnection: close`r`n`r`n")
                    $stream.Write($head, 0, $head.Length)
                }
            }
            else {
                $bytes = [Text.Encoding]::UTF8.GetBytes($payload)
                $head = [Text.Encoding]::ASCII.GetBytes(
                    "HTTP/1.1 200 OK`r`nContent-Type: application/json`r`nContent-Length: $($bytes.Length)`r`nConnection: close`r`n`r`n")
                $stream.Write($head, 0, $head.Length)
                $stream.Write($bytes, 0, $bytes.Length)
            }
            $stream.Flush()
        }
        finally {
            $client.Close()
        }
    }
}
finally {
    $listener.Stop()
}
