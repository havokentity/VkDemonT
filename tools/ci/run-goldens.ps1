# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
#
# Run the GPU golden matrix locally and publish the result to GitHub as a
# commit status, so a PR can be gated on it.
#
# WHY THIS EXISTS INSTEAD OF A SELF-HOSTED RUNNER.
#
# Every golden cell is a vulkan cell and GitHub-hosted runners have no GPU, so
# the matrix cannot run in CI. The obvious fix -- a self-hosted runner -- is the
# wrong one for a PUBLIC repository: a self-hosted runner executes workflow
# steps directly on a real machine, and anyone may fork, edit the workflow, and
# open a PR that then runs their code on it. GitHub documents that as the reason
# not to pair the two.
#
# This inverts the direction. Nothing from GitHub executes here. The matrix runs
# because YOU ran it, and the result is PUSHED out as a commit status. There is
# no inbound attack surface at all.
#
# WHAT THIS IS AND IS NOT. It is an ATTESTATION, not independent verification:
# it says "a machine with a GPU ran the matrix at this commit and here is what
# happened", signed by whoever holds the token. It is not proof to a third party,
# and an outside contributor cannot produce one. For a maintainer-gated project
# that is the right trade; for a project accepting untrusted GPU-affecting PRs it
# is not, and those need a real isolated runner.
#
# Usage:
#   pwsh tools/ci/run-goldens.ps1                  # run, print, write JSON
#   pwsh tools/ci/run-goldens.ps1 -Post            # ...and publish the status
#   pwsh tools/ci/run-goldens.ps1 -Post -Build     # build first, then run
#   pwsh tools/ci/run-goldens.ps1 -Sha <sha>       # attest a specific commit
#
# To make it a merge gate: Settings -> Branches -> branch protection rule for
# main -> Require status checks to pass -> add the context printed below
# (default: goldens/gpu). Until a status with that context arrives, the PR
# cannot merge.

[CmdletBinding()]
param(
    [switch]$Post,
    [switch]$Build,
    [string]$Sha,
    [string]$BuildDir = "build/win-clang-release",
    [string]$Repo     = "havokentity/VkDemonT",
    # The status context. Must match the branch-protection rule exactly.
    [string]$Context  = "goldens/gpu",
    [string]$JsonOut  = "golden-result.json"
)

$ErrorActionPreference = 'Stop'

function Enter-DevShell {
    $vsw = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vsw)) { throw "vswhere not found -- is Visual Studio installed?" }
    $inst = & $vsw -latest -prerelease -property installationPath
    Import-Module (Join-Path $inst "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell -VsInstallPath $inst -SkipAutomaticLocation `
        -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}

# --- what are we attesting? --------------------------------------------------
if (-not $Sha) { $Sha = (git rev-parse HEAD).Trim() }
$branch = (git rev-parse --abbrev-ref HEAD).Trim()

# A dirty tree means the binaries under test do not correspond to $Sha, which
# would make the attestation a lie. Refuse rather than publish something
# misleading; -Sha is the escape hatch for deliberate cases.
$dirty = (git status --porcelain) -ne $null
if ($dirty -and $Post) {
    Write-Host "Working tree is dirty. The result would not correspond to $($Sha.Substring(0,7))." -ForegroundColor Red
    Write-Host "Commit or stash first, or re-run without -Post to get the JSON only." -ForegroundColor Yellow
    exit 2
}

# GPU NAME: prefer the DISCRETE NVIDIA adapter explicitly. This machine also
# reports an AMD integrated adapter, and taking the first non-virtual entry
# picked THAT -- so the attestation named hardware the renderer never touched,
# which is worse than naming none. This engine is NVIDIA-exclusive, so an
# NVIDIA match is the right answer whenever one exists.
$adapters = @(Get-CimInstance Win32_VideoController |
              Where-Object { $_.Name -notmatch 'Basic|Remote|Virtual|Meta' } |
              Select-Object -ExpandProperty Name)
$gpu = ($adapters | Where-Object { $_ -match 'NVIDIA|GeForce|RTX' } | Select-Object -First 1)
if (-not $gpu) { $gpu = ($adapters | Select-Object -First 1) }
if (-not $gpu) { $gpu = 'unknown' }

Enter-DevShell

if ($Build) {
    Write-Host "Building..." -ForegroundColor Cyan
    cmake --build $BuildDir | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }
}

# --- run ---------------------------------------------------------------------
Write-Host "Running the golden matrix on $gpu ..." -ForegroundColor Cyan
$sw  = [System.Diagnostics.Stopwatch]::StartNew()
$out = & ctest --test-dir $BuildDir -R golden --output-on-failure 2>&1 | Out-String
$rc  = $LASTEXITCODE
$sw.Stop()

# ctest's summary line is the authoritative count; parsing individual result
# lines would miss cells that never ran.
$passed = 0; $total = 0
if ($out -match '(\d+)%\s+tests\s+passed,\s+(\d+)\s+tests?\s+failed\s+out\s+of\s+(\d+)') {
    $total  = [int]$matches[3]
    $passed = $total - [int]$matches[2]
} elseif ($out -match '100%\s+tests\s+passed,\s+0\s+tests\s+failed\s+out\s+of\s+(\d+)') {
    $total = [int]$matches[1]; $passed = $total
}
$failures = @()
foreach ($line in ($out -split "`n")) {
    if ($line -match '^\s*\d+\s*-\s*(\S+)\s+\(Failed\)') { $failures += $matches[1] }
}
$state = if ($rc -eq 0) { 'success' } else { 'failure' }

$result = [ordered]@{
    context       = $Context
    state         = $state
    sha           = $Sha
    branch        = $branch
    timestamp_utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    host          = $env:COMPUTERNAME
    gpu           = $gpu
    gpu_adapters  = $adapters
    total         = $total
    passed        = $passed
    failed        = ($total - $passed)
    failures      = $failures
    duration_s    = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    ctest_exit    = $rc
}
$result | ConvertTo-Json -Depth 4 | Set-Content -Path $JsonOut -Encoding utf8

Write-Host ""
Write-Host ("  {0}  {1}/{2} cells  {3}s  on {4}" -f $state.ToUpper(), $passed, $total,
            $result.duration_s, $gpu) -ForegroundColor $(if ($state -eq 'success') { 'Green' } else { 'Red' })
if ($failures.Count) { $failures | ForEach-Object { Write-Host "    FAILED: $_" -ForegroundColor Red } }
Write-Host "  JSON -> $JsonOut"

# --- publish -----------------------------------------------------------------
if ($Post) {
    # Description is capped at 140 chars by the API; keep it to the facts that
    # matter when it is read as one line in a PR's check list.
    $desc = "{0}/{1} cells on {2} in {3}s" -f $passed, $total, $gpu, $result.duration_s
    if ($desc.Length -gt 140) { $desc = $desc.Substring(0, 140) }

    gh api "repos/$Repo/statuses/$Sha" -X POST `
        -f state="$state" -f context="$Context" -f description="$desc" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "failed to publish status (is gh authenticated, with repo scope?)" }
    Write-Host "  published status '$Context' = $state on $($Sha.Substring(0,7))" -ForegroundColor Green
}

exit $rc
