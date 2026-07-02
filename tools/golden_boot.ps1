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
    # 90 s default (2026-07-03): with the SPU idle-poll backoff the boot's
    # phase timing wobbles by tens of seconds run-to-run (codec-taskset launch
    # measured at ~19 s and >60 s across two max-power runs); 60 s put the
    # audio milestones on the window edge and flapped the vector.
    [int]$Seconds = 90,
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
# STABLE fields are compared against the golden; INFO fields are printed but
# not diffed (2026-07-03: the boot's phase pacing wobbles by tens of seconds
# run-to-run, so window-edge milestones -- deepest import, distinct-import
# count, the image-5 overlay's single unknown-branch -- flap between adjacent
# values on identical builds and flapped the vector 3 ways in 3 runs).
$imports    = $e | Select-String "\[import\] call (\S+)" | ForEach-Object { $_.Matches[0].Groups[1].Value }
$vec = [ordered]@{
    exited_early     = if ($exited) { "YES" } else { "no" }
    exceptions       = ($e | Select-String "EXCEPTION|exception code|crash handler").Count
    unknown_branch_gt1 = if (($e | Select-String "unknown branch").Count -gt 1) { "YES" } else { "no" }
    esrch_fails      = ($e | Select-String "-> ESRCH").Count
    reached_audio    = if ($imports -contains "cellAudioGetPortConfig") { "YES" } else { "no" }
    applier_fires    = ($e | Select-String "applied deferred release").Count -gt 0
    vblank_alive     = ($e | Select-String "\[vbl\] tick").Count -gt 10
}
$info = [ordered]@{
    unknown_branch   = ($e | Select-String "unknown branch").Count
    distinct_imports = ($imports | Sort-Object -Unique).Count
    deepest_import   = $imports[-1]
    flowctl_forces   = ($e | Select-String "\[flowctl\]|\[flipadv\] FORCED").Count -gt 0
}
# ----------------------------------------------------------------------------
$text = ($vec.GetEnumerator() | ForEach-Object { "{0}={1}" -f $_.Key, $_.Value }) -join "`n"
$itext = ($info.GetEnumerator() | ForEach-Object { "{0}={1}" -f $_.Key, $_.Value }) -join "`n"
Write-Host "[golden] milestone vector (compared):`n$text"
Write-Host "[golden] info (not compared):`n$itext"

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
