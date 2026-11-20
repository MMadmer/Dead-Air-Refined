<#
.SYNOPSIS
    Every module of the archive of old modules in today's game: a module built by any earlier release of the
    authoring tool keeps working (PROJECT_RULES.md, the XMS contract).

.DESCRIPTION
    The archive is tools/qa/module/old/<tool version>/<module id>: modules as a released tool built them, frozen
    - never rebuilt, never edited -, each with <module id>.txt beside it saying what made it. Each one goes through
    Run-ModuleInGameQa.ps1 as it is: one start and a new game a module (about two minutes each), judged by the
    log and the probe - the module mounts, the module system refuses nothing of it, its items are there and its
    first-person motions play. A game change that touches what a module ships runs this before it is deployed;
    a release of the authoring tool adds its own reference module (the tool's
    tools/qa/freeze_reference_module.py) and never replaces one that is there.

    -Only takes the archive entries whose path contains the text. The rest of the parameters are the runner's.
#>
[CmdletBinding()]
param(
    [string]$GameRoot = 'D:\Games\Dead Air',
    [string]$BuildRoot,
    [string]$Only,
    [switch]$MenuOnly,
    [ValidateRange(60, 3500)] [int]$TimeoutSeconds = 420
)
$ErrorActionPreference = 'Stop'

$archive = Join-Path $PSScriptRoot 'old'
$runner = Join-Path $PSScriptRoot 'Run-ModuleInGameQa.ps1'
$modules = @(Get-ChildItem -LiteralPath $archive -Directory | Sort-Object Name | ForEach-Object {
        Get-ChildItem -LiteralPath $_.FullName -Directory | Sort-Object Name
    } | Where-Object { -not $Only -or $_.FullName -like "*$Only*" })
if ($modules.Count -eq 0) {
    Write-Host "== no old module in $archive$(if ($Only) { " matching '$Only'" })"
    exit 1
}

$failed = @()
foreach ($module in $modules) {
    $name = "$($module.Parent.Name)/$($module.Name)"
    Write-Host "== old module $name"
    $arguments = @{ Module = $module.FullName; GameRoot = $GameRoot; TimeoutSeconds = $TimeoutSeconds }
    if ($BuildRoot) { $arguments.BuildRoot = $BuildRoot }
    if ($MenuOnly) { $arguments.MenuOnly = $true }
    & $runner @arguments
    if ($LASTEXITCODE -ne 0) { $failed += $name }
}
Write-Host "== old modules: $($modules.Count), failed: $($failed.Count)$(if ($failed) { ' - ' + ($failed -join ', ') })"
exit $failed.Count
