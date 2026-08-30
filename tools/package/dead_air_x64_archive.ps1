# Generic X-Ray archive builder, shared by the compatibility archive and the content bundles.
# Dot-source this file, then call New-XdbArchive.
#
# The compatibility archive and a content bundle are the same kind of file built from the same
# tool; only the staging differs. Everything compat-specific - the core script overrides and the
# cp1251 text fix-ups - lives in New-DeadAirCompatibilityArchive and must never run over a
# texture bundle.

function Get-XdbFileHashes {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Root
    )

    # Get-FileHash per file was written for ~170 files. A content bundle holds tens of
    # thousands, where the per-call overhead dominates and the pass takes minutes; hash in
    # parallel through .NET instead.
    $prefixLength = $Root.Length + 1
    $files = Get-ChildItem -LiteralPath $Root -Recurse -File
    $pairs = $files | ForEach-Object -ThrottleLimit ([Environment]::ProcessorCount) -Parallel {
        $stream = [IO.File]::OpenRead($_.FullName)
        try {
            $hash = [Security.Cryptography.SHA256]::Create().ComputeHash($stream)
        }
        finally {
            $stream.Dispose()
        }
        [pscustomobject]@{
            Path = $_.FullName.Substring($using:prefixLength).Replace('\', '/').ToLowerInvariant()
            Hash = [Convert]::ToHexString($hash).ToLowerInvariant()
            Size = $_.Length
        }
    }

    $map = @{}
    foreach ($pair in $pairs) { $map[$pair.Path] = $pair }
    return $map
}

function New-XdbArchive {
    [CmdletBinding()]
    param(
        # A prepared gamedata tree. Its contents become the archive root.
        [Parameter(Mandatory)][string]$StageRoot,
        [Parameter(Mandatory)][string]$OutputPath,
        [Parameter(Mandatory)][string]$UserDataPath,
        [Parameter(Mandatory)][string]$ConverterPath,
        [Parameter(Mandatory)][string]$VerifyRoot,
        # Skip the unpack-and-compare pass. Only for callers that verified the tree themselves.
        [switch]$SkipVerify
    )

    $ErrorActionPreference = "Stop"

    $converter = [IO.Path]::GetFullPath($ConverterPath)
    if (-not (Test-Path -LiteralPath $converter -PathType Leaf)) {
        throw "AXRToolset converter was not found: $converter"
    }
    if (-not (Test-Path -LiteralPath $StageRoot -PathType Container)) {
        throw "Stage root was not found: $StageRoot"
    }
    if (-not (Test-Path -LiteralPath $UserDataPath -PathType Leaf)) {
        throw "Archive user data was not found: $UserDataPath"
    }

    New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) -Force | Out-Null
    if (Test-Path -LiteralPath $OutputPath) {
        Remove-Item -LiteralPath $OutputPath -Force
    }

    & $converter -pack -xdb -xdb_ud $UserDataPath -out $OutputPath $StageRoot
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $OutputPath -PathType Leaf)) {
        throw "Archive build failed: $OutputPath"
    }

    if ($SkipVerify) {
        return
    }

    if (Test-Path -LiteralPath $VerifyRoot) {
        Remove-Item -LiteralPath $VerifyRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $VerifyRoot -Force | Out-Null
    & $converter -unpack -xdb -dir $VerifyRoot $OutputPath
    if ($LASTEXITCODE -ne 0) {
        throw "Archive verification unpack failed: $OutputPath"
    }

    $sourceHashes = Get-XdbFileHashes -Root $StageRoot
    $verifiedHashes = Get-XdbFileHashes -Root $VerifyRoot
    if ($sourceHashes.Count -ne $verifiedHashes.Count) {
        throw "Archive file count is invalid ($($sourceHashes.Count) packed, $($verifiedHashes.Count) read back): $OutputPath"
    }
    foreach ($relativePath in $sourceHashes.Keys) {
        $verified = $verifiedHashes[$relativePath]
        if (-not $verified -or $verified.Hash -ne $sourceHashes[$relativePath].Hash) {
            throw "Archive verification failed for $relativePath in $OutputPath"
        }
    }
}
