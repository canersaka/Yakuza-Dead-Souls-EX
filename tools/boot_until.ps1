# boot_until.ps1 -- event-driven boot: run the recomp, watch stderr live for a
# pattern, kill the process the moment the question is answered (or a timeout
# proves it won't be). Replaces the fixed 100-115s Start-Sleep iteration loop
# (most probe questions resolve by ~25s; this cuts wall-clock per run ~in half).
#
# Usage:
#   .\tools\boot_until.ps1 -Pattern 'throw_event'                  # match & stop
#   .\tools\boot_until.ps1 -Pattern 'codec-peek.*@63D61400' -Timeout 60
#   .\tools\boot_until.ps1 -Pattern 'sendsig' -After 3             # 3rd match
#   .\tools\boot_until.ps1 -Timeout 60                             # plain timed run
#   -Tag names the scratch output pair (scratch\<tag>.err/.out); default 'bu'.
#   -Context N prints N trailing log lines around the match (default 6).
#   Env flags: set $env:YZ_* before invoking as usual; they inherit.
#
# Exit code 0 = pattern matched, 1 = timeout without match.
param(
    [string]$Pattern = '',
    [int]$Timeout = 120,
    [int]$After = 1,
    [string]$Tag = 'bu',
    [int]$Context = 6
)

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$err = Join-Path $root "scratch\$Tag.err"
$out = Join-Path $root "scratch\$Tag.out"
foreach ($f in @($err, $out)) { if (Test-Path $f) { Clear-Content $f } }

$p = Start-Process -FilePath ".\yakuza\build\yakuza_recomp.exe" -ArgumentList "game\EBOOT.elf" `
        -PassThru -WindowStyle Hidden -RedirectStandardError $err -RedirectStandardOutput $out

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$matched = $false
$hits = 0
$lastLine = 0
try {
    while ($sw.Elapsed.TotalSeconds -lt $Timeout) {
        Start-Sleep -Milliseconds 400
        if ($p.HasExited) { Write-Host "[boot_until] process exited at +$([int]$sw.Elapsed.TotalSeconds)s"; break }
        if ($Pattern -eq '') { continue }
        # incremental scan: only lines added since the last poll
        $lines = @(Get-Content $err -ErrorAction SilentlyContinue)
        if ($lines.Count -gt $lastLine) {
            for ($i = $lastLine; $i -lt $lines.Count; $i++) {
                if ($lines[$i] -match $Pattern) {
                    $hits++
                    if ($hits -ge $After) {
                        $t = [int]$sw.Elapsed.TotalSeconds
                        Write-Host "[boot_until] MATCH #$hits at +${t}s (line $($i+1)):"
                        $lo = [Math]::Max(0, $i - 1)
                        $hi = [Math]::Min($lines.Count - 1, $i + $Context)
                        $lines[$lo..$hi] | ForEach-Object { Write-Host "    $_" }
                        $matched = $true
                        break
                    }
                }
            }
            $lastLine = $lines.Count
        }
        if ($matched) { break }
    }
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}

if (-not $matched -and $Pattern -ne '') {
    Write-Host "[boot_until] NO MATCH for '$Pattern' in ${Timeout}s ($hits/$After hits; log: $err)"
    exit 1
}
Write-Host "[boot_until] logs: $err / $out"
exit 0
