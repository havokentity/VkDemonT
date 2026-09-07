# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
#
# Convert a timestamp between GitHub's UTC and local time.
#
# GitHub Actions stamps every log line in UTC (the trailing Z), and there is no
# runner or workflow setting that localises it. Converting in your head while
# reading a failing job is exactly when you get it wrong.
#
#   pwsh tools/ci/utc.ps1 2026-09-07T14:31:22Z   # -> local
#   pwsh tools/ci/utc.ps1 12:46                  # bare == UTC -> local
#   pwsh tools/ci/utc.ps1 20:15 -FromLocal       # -> UTC
#   pwsh tools/ci/utc.ps1                        # just show the offset

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Time,
    # Treat the input as LOCAL and report UTC, rather than the default
    # direction.
    [switch]$FromLocal
)

$ErrorActionPreference = 'Stop'
$inv    = [System.Globalization.CultureInfo]::InvariantCulture
$styles = [System.Globalization.DateTimeStyles]::None

if (-not $Time) {
    $now = Get-Date
    $off = [System.TimeZoneInfo]::Local.GetUtcOffset($now)
    $sign = if ($off.Ticks -ge 0) { '+' } else { '-' }
    Write-Host ("Local {0} = {1}Z   (UTC{2}{3:00}:{4:00}, {5})" -f `
                $now.ToString('HH:mm:ss'), $now.ToUniversalTime().ToString('HH:mm:ss'),
                $sign, [Math]::Abs($off.Hours), [Math]::Abs($off.Minutes),
                [System.TimeZoneInfo]::Local.Id)
    exit 0
}

$parsed = $null

# Only strings that CARRY a zone go down the DateTimeOffset path. TryParse is
# greedy: it accepts a bare "12:46" and silently assumes LOCAL, which would
# convert a UTC log time as though it were already local and report it
# unchanged -- the exact bug this guard exists to prevent.
$hasOffset = ($Time -match 'Z$') -or ($Time -match '[+-][0-9]{2}:?[0-9]{2}$')
$dto = [DateTimeOffset]::MinValue
if ($hasOffset -and [DateTimeOffset]::TryParse($Time, $inv,
        [System.Globalization.DateTimeStyles]::RoundtripKind, [ref]$dto)) {
    $parsed = $dto
} else {
    $t = [DateTime]::MinValue
    foreach ($fmt in @('HH:mm:ss', 'HH:mm', 'H:mm:ss', 'H:mm')) {
        if ([DateTime]::TryParseExact($Time, $fmt, $inv, $styles, [ref]$t)) { break }
    }
    if ($t -eq [DateTime]::MinValue) {
        Write-Host "Could not parse '$Time'." -ForegroundColor Red
        Write-Host "Try an ISO stamp (2026-09-07T14:31:22Z) or a bare HH:mm." -ForegroundColor Yellow
        exit 2
    }
    $stamp = (Get-Date).Date.AddHours($t.Hour).AddMinutes($t.Minute).AddSeconds($t.Second)
    # Unspecified kind is required: (Get-Date).Date returns a LOCAL-kind
    # DateTime, and DateTimeOffset's constructor rejects an explicit offset that
    # disagrees with the kind it was handed.
    $stamp = [DateTime]::SpecifyKind($stamp, [DateTimeKind]::Unspecified)
    if ($FromLocal) {
        $parsed = [DateTimeOffset]::new($stamp, [System.TimeZoneInfo]::Local.GetUtcOffset($stamp))
    } else {
        $parsed = [DateTimeOffset]::new($stamp, [TimeSpan]::Zero)   # bare == UTC
    }
}

Write-Host ""
Write-Host ("  UTC   (GitHub) : {0}" -f $parsed.ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss')) -ForegroundColor Cyan
Write-Host ("  Local ({0}) : {1}" -f [System.TimeZoneInfo]::Local.StandardName,
            $parsed.ToLocalTime().ToString('yyyy-MM-dd HH:mm:ss')) -ForegroundColor Green
Write-Host ""
