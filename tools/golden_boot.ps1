# GOLDEN-BOOT regression harness (adopted 2026-07-01 reassessment).
#
# Runs the default boot for a fixed window, extracts a milestone vector from the
# stderr log, and diffs it against the checked-in golden file. Run after EVERY
# relift/rebuild; a mismatch = investigate before committing.
#
#   .\tools\golden_boot.ps1              # run + compare vs scratch\golden_boot.txt
#   .\tools\golden_boot.ps1 -Update      # run + overwrite the golden (after a
#                                        #   verified intentional milestone change)
#   .\tools\golden_boot.ps1 -Seconds 90  # longer window (default 60)
#
# The vector is deliberately coarse (counts + deepest-import), so timing jitter
# doesn't flap it; tighten fields only if they prove stable across runs.
param(
    [switch]$Update,
    [int]$Seconds = 60,
    [string]$Golden = "scratch\golden_boot.txt",
    [string]$Log = "scratch\golden_boot_run.err"
)

$exe = ".\yakuza\build\yakuza_recomp.exe"
if (-not (Test-Path $exe)) { Write-Error "build first: $exe missing"; exit 2 }

Write-Host "[golden] booting $Seconds s ..."
$p = Start-Process -FilePath $exe -ArgumentList "game\EBOOT.elf" `
        -WorkingDirectory (Get-Location) -PassThru -WindowStyle Hidden `
        -RedirectStandardError $Log -RedirectStandardOutput "$Log.out"
Start-Sleep -Seconds $Seconds
$exited = $p.HasExited
if (-not $exited) { Stop-Process -Id $p.Id -Force -Confirm:$false }

$e = Get-Content $Log
# ---- milestone vector ------------------------------------------------------
$imports    = $e | Select-String "\[import\] call (\S+)" | ForEach-Object { $_.Matches[0].Groups[1].Value }
$vec = [ordered]@{
    exited_early     = if ($exited) { "YES" } else { "no" }
    exceptions       = ($e | Select-String "EXCEPTION|exception code|crash handler").Count
    unknown_branch   = ($e | Select-String "unknown branch").Count
    esrch_fails      = ($e | Select-String "-> ESRCH").Count
    distinct_imports = ($imports | Sort-Object -Unique).Count
    deepest_import   = $imports[-1]
    reached_audio    = if ($imports -contains "cellAudioGetPortConfig") { "YES" } else { "no" }
    applier_fires    = ($e | Select-String "applied deferred release").Count -gt 0
    flowctl_forces   = ($e | Select-String "\[flowctl\]|\[flipadv\] FORCED").Count -gt 0
    vblank_alive     = ($e | Select-String "\[vbl\] tick").Count -gt 10
}
# ----------------------------------------------------------------------------
$text = ($vec.GetEnumerator() | ForEach-Object { "{0}={1}" -f $_.Key, $_.Value }) -join "`n"
Write-Host "[golden] milestone vector:`n$text"

if ($Update -or -not (Test-Path $Golden)) {
    $text | Set-Content -Encoding utf8 $Golden
    Write-Host "[golden] golden written -> $Golden"
    exit 0
}
$gold = (Get-Content $Golden) -join "`n"
if ($gold -eq $text) {
    Write-Host "[golden] PASS - matches golden"
    exit 0
}
Write-Host "[golden] FAIL - vector differs from golden:" -ForegroundColor Red
Compare-Object ($gold -split "`n") ($text -split "`n") | ForEach-Object {
    $tag = if ($_.SideIndicator -eq "<=") { "golden" } else { "  this" }
    Write-Host ("  {0}: {1}" -f $tag, $_.InputObject)
}
exit 1
