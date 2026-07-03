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

# NATIVE stderr/stdout redirection via cmd (kernel-level file handles).
# PowerShell's -RedirectStandardError pumps the pipe through .NET on its own
# schedule; under print-heavy boots the pipe fills, every guest thread then
# SERIALIZES on the CRT stderr lock (measured 2026-07-03: t1 blocked 48 s
# inside one fprintf; the whole vsync ecosystem throttled to ~3 Hz and a
# "shader-build crawl" was entirely this artifact). cmd's 2> is a plain file
# handle -- writers never block on a reader.
$cmdline = "yakuza\build\yakuza_recomp.exe game\EBOOT.elf 2>`"$err`" 1>`"$out`""
$shell = Start-Process -FilePath "cmd.exe" -ArgumentList "/c", $cmdline `
        -PassThru -WindowStyle Hidden
# Track the real guest process (cmd's child) for liveness + kill.
$p = $null
for ($w = 0; $w -lt 50 -and -not $p; $w++) {
    Start-Sleep -Milliseconds 100
    $p = Get-Process yakuza_recomp -ErrorAction SilentlyContinue |
         Sort-Object StartTime -Descending | Select-Object -First 1
}
if (-not $p) { Write-Host "[boot_until] guest process never appeared"; exit 2 }

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$matched = $false
$hits = 0
$lastLine = 0
try {
    while ($sw.Elapsed.TotalSeconds -lt $Timeout) {
        Start-Sleep -Milliseconds 400
        if ($p.HasExited) { Write-Host "[boot_until] process exited at +$([int]$sw.Elapsed.TotalSeconds)s"; break }
        if ($Pattern -eq '') { continue }
        # Incremental scan of lines added since the last poll. HOLD BACK the
        # final line: it may be mid-flush (no trailing newline yet), and a
        # partial line that fails the match must be re-read next tick, not
        # skipped forever (2026-07-02: this exact race silently misclassified
        # five matched boots as NO MATCH).
        $lines = @(Get-Content $err -ErrorAction SilentlyContinue)
        if ($lines.Count -gt $lastLine + 1) {
            for ($i = $lastLine; $i -lt $lines.Count - 1; $i++) {
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
            if (-not $matched) { $lastLine = $lines.Count - 1 }
        }
        if ($matched) { break }
    }
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}

if (-not $matched -and $Pattern -ne '') {
    # Final authoritative pass over the settled file (the incremental scanner
    # can never have seen the very tail; the process is dead now, so the file
    # is complete).
    Start-Sleep -Milliseconds 200
    $lines = @(Get-Content $err -ErrorAction SilentlyContinue)
    for ($i = $lastLine; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match $Pattern) {
            $hits++
            if ($hits -ge $After) {
                Write-Host "[boot_until] MATCH #$hits on final pass (line $($i+1)) -- matched before kill:"
                $lo = [Math]::Max(0, $i - 1)
                $hi = [Math]::Min($lines.Count - 1, $i + $Context)
                $lines[$lo..$hi] | ForEach-Object { Write-Host "    $_" }
                $matched = $true
                break
            }
        }
    }
}

if (-not $matched -and $Pattern -ne '') {
    Write-Host "[boot_until] NO MATCH for '$Pattern' in ${Timeout}s ($hits/$After hits; log: $err)"
    exit 1
}
Write-Host "[boot_until] logs: $err / $out"
exit 0
