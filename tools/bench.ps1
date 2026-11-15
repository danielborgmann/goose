# Times several compiler variants back to back under one machine claim, at High priority.
# Each case is "label|exe|workdir|arg arg ..."; the cases run in blocks (one warm-up, then
# -Runs timed runs each), so a variant whose C differs does not rebuild the other's split units.
#   pwsh tools/bench.ps1 -Runs 2 -Cases @("pin|build/pinned/goose.exe|build/work/nr/engine|check src/viewer.goose", ...)
# Claim first (claim.mjs exclusive), release after; this script takes no claim of its own.
# -CaseFile: the cases one per line instead (from bash, where @(...) does not pass).
param([string[]]$Cases, [string]$CaseFile, [int]$Runs = 2, [switch]$NoWarmup)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
if ($CaseFile) { $Cases = Get-Content $CaseFile | Where-Object { $_.Trim() -and -not $_.StartsWith('#') } }
$log = Join-Path ([IO.Path]::GetTempPath()) "goose_bench_$PID"
foreach ($c in $Cases) {
    $label, $exe, $dir, $argline = $c -split '\|', 4
    $exe = [IO.Path]::GetFullPath($exe, $root); $dir = [IO.Path]::GetFullPath($dir, $root)
    $times = @()
    foreach ($r in $(if ($NoWarmup) { 1..$Runs } else { 0..$Runs })) {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $p = Start-Process $exe -ArgumentList $argline -WorkingDirectory $dir -NoNewWindow -PassThru `
            -RedirectStandardOutput "$log.out" -RedirectStandardError "$log.err"
        $p.PriorityClass = 'High'; $p.WaitForExit()
        if ($p.ExitCode -ne 0) { "{0}: exit {1}" -f $label, $p.ExitCode }
        if ($r -gt 0) { $times += [math]::Round($sw.Elapsed.TotalSeconds, 2) }
    }
    "{0,-14} {1}" -f $label, ($times -join "  ")
}
Remove-Item "$log.out", "$log.err" -ErrorAction SilentlyContinue
