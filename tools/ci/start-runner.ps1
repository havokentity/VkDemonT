# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
#
# Start the self-hosted GitHub Actions runner for this repo, on demand.
#
# WHY ON-DEMAND RATHER THAN A SERVICE. A service is the usual advice, and it is
# the wrong advice for this machine: it is a workstation, not a build server,
# and it is not on 24/7. A service that starts at boot would also happily pick
# up a job and fire a ~7 minute GPU render burst while you are mid-game --
# which is the exact GPU contention that crashed this box once already. Running
# the runner when you want CI, and closing it when you do not, keeps that
# decision yours.
#
# The consequence, stated plainly: a PR opened while this is not running gets
# no GPU check until you start it. The workflow is triggered on pull_request,
# so GitHub queues the job and it runs whenever the runner next comes up. It is
# not lost, just deferred.
#
# Usage:
#   pwsh tools/ci/start-runner.ps1            # start it, Ctrl+C to stop
#   pwsh tools/ci/start-runner.ps1 -Status    # is it registered? online?
#   pwsh tools/ci/start-runner.ps1 -Reconfigure
#
# -Reconfigure re-registers the runner against the repo. Needed if the runner
# was removed on the GitHub side, if labels change, or after restoring this
# machine. It mints its own registration token via `gh`, because those tokens
# expire about an hour after they are issued and a hardcoded one is useless by
# the time you need it.

[CmdletBinding()]
param(
    [switch]$Status,
    [switch]$Reconfigure,
    [string]$RunnerDir = "$env:USERPROFILE\actions-runner",
    [string]$Repo      = "havokentity/VkDemonT",
    # Must match what the workflow's `runs-on` asks for. The GPU golden matrix
    # keys on `gpu`; `rtx5090` is there so a second machine could be added
    # later without the two competing for the same jobs.
    [string]$Labels    = "self-hosted,windows,x64,gpu,rtx5090",
    [string]$RunnerName = "rtx5090-win"
)

$ErrorActionPreference = 'Stop'

function Require-Gh {
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        throw "GitHub CLI (gh) not found on PATH. It is needed to mint a runner registration token."
    }
    # `gh auth status` exits non-zero when logged out; surface that clearly
    # rather than letting the token call fail with something cryptic.
    gh auth status *> $null
    if ($LASTEXITCODE -ne 0) { throw "gh is not authenticated. Run: gh auth login" }
}

function Get-RegistrationToken {
    Require-Gh
    $t = gh api -X POST "repos/$Repo/actions/runners/registration-token" --jq '.token'
    if ([string]::IsNullOrWhiteSpace($t)) {
        throw "Could not mint a registration token. Does your gh token carry the 'repo' scope?"
    }
    return $t
}

if (-not (Test-Path $RunnerDir)) {
    Write-Host "Runner not installed at $RunnerDir." -ForegroundColor Yellow
    Write-Host "Download it from https://github.com/actions/runner/releases (win-x64)," -ForegroundColor Yellow
    Write-Host "extract to that path, then re-run this script with -Reconfigure." -ForegroundColor Yellow
    exit 1
}

# ---- status -----------------------------------------------------------------
if ($Status) {
    $configured = Test-Path (Join-Path $RunnerDir '.runner')
    Write-Host "runner dir : $RunnerDir"
    Write-Host "configured : $configured"
    Require-Gh
    # Formatted in PowerShell rather than with `gh --jq`: a jq expression
    # containing double quotes gets mangled by PowerShell's native-argument
    # parsing and reaches gh as eight separate arguments.
    #
    # `offline` here is the NORMAL resting state -- it means this script is not
    # currently running, not that anything is broken.
    $runners = (gh api "repos/$Repo/actions/runners" | ConvertFrom-Json).runners
    if (-not $runners) {
        Write-Host "github     : no runners registered for $Repo"
    } else {
        foreach ($r in $runners) {
            $lbl = ($r.labels | ForEach-Object { $_.name }) -join ','
            Write-Host ("github     : {0} status={1} busy={2} labels={3}" -f `
                        $r.name, $r.status, $r.busy, $lbl)
        }
    }
    exit 0
}

# ---- (re)configure ----------------------------------------------------------
if ($Reconfigure -or -not (Test-Path (Join-Path $RunnerDir '.runner'))) {
    Write-Host "Registering runner '$RunnerName' against $Repo ..." -ForegroundColor Cyan
    $tok = Get-RegistrationToken
    Push-Location $RunnerDir
    try {
        # --replace so re-running this is idempotent rather than erroring out
        # or silently creating a second runner with the same name.
        & .\config.cmd --unattended `
            --url "https://github.com/$Repo" `
            --token $tok `
            --name $RunnerName `
            --labels $Labels `
            --work "_work" `
            --replace
        if ($LASTEXITCODE -ne 0) { throw "config.cmd failed with exit code $LASTEXITCODE" }
    } finally { Pop-Location }
    Write-Host "Registered." -ForegroundColor Green
}

# ---- run --------------------------------------------------------------------
# GitHub Actions stamps every log line in UTC (the trailing Z), both in the
# web UI and in this runner's console output, and that is not configurable --
# there is no runner or workflow setting that localises it. Printing the offset
# here beats doing the arithmetic in your head at 2am while reading a job log.
$now = Get-Date
$utc = $now.ToUniversalTime()
$off = [System.TimeZoneInfo]::Local.GetUtcOffset($now)
$sign = if ($off.Ticks -ge 0) { '+' } else { '-' }
Write-Host ""
Write-Host ("Timestamps: GitHub logs are UTC. Local {0} = {1}Z  (UTC{2}{3:00}:{4:00}, {5})" -f `
            $now.ToString('HH:mm'), $utc.ToString('HH:mm'), $sign,
            [Math]::Abs($off.Hours), [Math]::Abs($off.Minutes),
            [System.TimeZoneInfo]::Local.Id) -ForegroundColor Cyan
Write-Host ""
Write-Host "Starting runner '$RunnerName'. Ctrl+C to stop." -ForegroundColor Green
Write-Host "Queued PR jobs will start picking up now." -ForegroundColor Green
Write-Host "The GPU golden matrix runs on pull_request only, so ordinary pushes" -ForegroundColor DarkGray
Write-Host "will not spin the GPU up behind your back." -ForegroundColor DarkGray
Write-Host ""

Push-Location $RunnerDir
try { & .\run.cmd } finally { Pop-Location }
