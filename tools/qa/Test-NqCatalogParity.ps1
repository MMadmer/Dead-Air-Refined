<#
.SYNOPSIS
    Every kind the NQ catalog declares has an implementation, and every
    implementation is declared.

.DESCRIPTION
    The catalog is the contract the editor reads and the runtime executes. A
    kind in one and not the other is the failure that only shows up as a quest
    refusing to load in somebody's game: the editor offers a node the runtime
    cannot run, or the runtime carries code nothing can reach.

    Checked statically, so it costs nothing and cannot be skipped.
#>
[CmdletBinding()]
param(
    [string] $Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)

$ErrorActionPreference = 'Stop'

$stage = Join-Path $Root 'packaging\dead-air-x64\compatibility\gamedata'
$catalogPath = Join-Path $stage 'configs\nq\catalog.ltx'
if (-not (Test-Path $catalogPath)) { throw "no catalog at $catalogPath" }

$catalog = [System.Collections.Generic.HashSet[string]]::new()
foreach ($line in Get-Content -LiteralPath $catalogPath) {
    if ($line -match '^\[nq\.([a-z_.]+)\]') {
        if ($Matches[1] -ne 'catalog') { [void]$catalog.Add($Matches[1]) }
    }
}

$implemented = [System.Collections.Generic.HashSet[string]]::new()
foreach ($script in Get-ChildItem -LiteralPath (Join-Path $stage 'scripts') -Filter 'xms_nq*.script') {
    foreach ($line in Get-Content -LiteralPath $script.FullName) {
        if ($line -match '^K\["([a-z_.]+)"\]') { [void]$implemented.Add($Matches[1]) }
    }
}

$missing = @($catalog | Where-Object { -not $implemented.Contains($_) } | Sort-Object)
$orphan = @($implemented | Where-Object { -not $catalog.Contains($_) } | Sort-Object)

Write-Host ("catalog kinds: {0}, implementations: {1}" -f $catalog.Count, $implemented.Count)

if ($missing.Count) {
    Write-Host 'declared but not implemented:' -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
}
if ($orphan.Count) {
    Write-Host 'implemented but not declared:' -ForegroundColor Red
    $orphan | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
}

if ($missing.Count -or $orphan.Count) {
    Write-Host 'NQ CATALOG PARITY: FAILED' -ForegroundColor Red
    exit 1
}

Write-Host ("NQ CATALOG PARITY: OK ({0} kinds)" -f $catalog.Count) -ForegroundColor Green
exit 0
