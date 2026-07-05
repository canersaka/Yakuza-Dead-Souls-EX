<#
  diverge.ps1 -- one command to find the next SPU lift divergence vs RPCS3.

  Loop: fix the lifter -> run this -> it relifts (with --trace) + rebuilds + runs
  + diffs against the CACHED RPCS3 reference trace + prints the first divergence
  AND the lifted C at that PC. Fix that instruction, run it again.

  The RPCS3 reference trace is captured ONCE (an instrumented interpreter-mode run) and reused every
  loop -- only OUR side rebuilds, so each iteration is one relift + one ninja.

  Usage:
    powershell -ExecutionPolicy Bypass -File tools\diverge.ps1
    ... -DiffOnly           # skip relift+build+run, just re-diff the last trace
    ... -RefTrace <path> -AlignPc 3050 -PcRange 3000:8000 -Registers

  Defaults target gs_task (SPU image 0).
#>
[CmdletBinding()]
param(
  [string]$Elf       = 'scratch\spu_imgs\spu_0003_at_0126A580.elf',
  [string]$RegName   = 'spu_recomp_register_gstask',
  [string]$Source    = 'gs_task.c',
  [string]$Header    = 'gs_task.h',
  [string]$RefTrace  = 'scratch\rpcs3_gstask_trace.txt',
  [string]$OursTrace = 'scratch\spu_trace.txt',
  [int]   $TraceImg  = 0,
  [string]$AlignPc   = '3050',
  [string]$PcRange   = '3000:8000',
  [switch]$Registers,          # register-level diff (default is control-flow / --pc-only)
  [int]   $RunSeconds = 45,    # gs_task livelocks; capture the bounded trace then kill
  [switch]$DiffOnly
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo
function Stage($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }

$gsc = "recomp_prx\$Source"; $gsh = "recomp_prx\$Header"
$bakDir = 'scratch\diverge_bak'

if (-not $DiffOnly) {
  Stage "1/5  Back up + relift $Source WITH --trace (picks up your lifter fix)"
  New-Item -ItemType Directory -Force $bakDir | Out-Null
  Copy-Item $gsc "$bakDir\" -Force; Copy-Item $gsh "$bakDir\" -Force
  py -3 tools\spu_lifter.py --auto-functions $Elf --seed-all `
      --register-name $RegName --output recomp_prx `
      --source-name $Source --header-name $Header --trace
  if ($LASTEXITCODE -ne 0) { throw "relift failed" }

  Stage "2/5  Build (vcvars64 env import + ninja)"
  $vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
  if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars -- edit the path" }
  cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
  }
  ninja -C yakuza\build yakuza_recomp
  if ($LASTEXITCODE -ne 0) { throw "ninja build failed" }

  Stage "3/5  Run faithful, trace image $TraceImg -> $OursTrace  (kills after $RunSeconds s)"
  if (Test-Path $OursTrace) { Remove-Item $OursTrace -Force }
  # (faithful mode is the DEFAULT since the band-aid retirement 2026-07-02; no env needed)
  $env:YZ_SPU_TRACE = '1'; $env:YZ_SPU_TRACE_IMG = "$TraceImg"
  # NATIVE redirection: the PS pipe pump serializes guest threads on the CRT
  # stderr lock under print volume -- same fix as boot_until/golden.
  $cmdline = "yakuza\build\yakuza_recomp.exe game\EBOOT.elf 2>`"scratch\diverge_run.err`" 1>`"scratch\diverge_run.out`""
  Start-Process -FilePath "cmd.exe" -ArgumentList "/c", $cmdline -WindowStyle Hidden | Out-Null
  $p = $null
  for ($w = 0; $w -lt 50 -and -not $p; $w++) {
    Start-Sleep -Milliseconds 100
    $p = Get-Process yakuza_recomp -ErrorAction SilentlyContinue |
         Sort-Object StartTime -Descending | Select-Object -First 1
  }
  if (-not $p) { throw "guest process never appeared" }
  # Wait until the bounded trace stops growing (budget filled) or the timeout.
  $last = -1; $stable = 0
  for ($t = 0; $t -lt $RunSeconds; $t++) {
    Start-Sleep 1
    $sz = if (Test-Path $OursTrace) { (Get-Item $OursTrace).Length } else { 0 }
    if ($sz -gt 0 -and $sz -eq $last) { $stable++ } else { $stable = 0 }
    $last = $sz
    if ($stable -ge 4) { break }   # 4s with no growth = trace complete
  }
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  if (-not (Test-Path $OursTrace) -or (Get-Item $OursTrace).Length -eq 0) { throw "no trace produced" }
  Write-Host ("    trace: {0:N0} bytes" -f (Get-Item $OursTrace).Length)

  Stage "5/5  Restore $Source to non-trace form"
  Copy-Item "$bakDir\$Source" $gsc -Force; Copy-Item "$bakDir\$Header" $gsh -Force
}

Stage "4/5  Diff vs RPCS3 ($RefTrace)"
$mode = @('--align-pc', $AlignPc, '--pc-range', $PcRange)
if (-not $Registers) { $mode += '--pc-only' }
$txt = (& py -3 tools\tracediff.py $OursTrace $RefTrace @mode --context 20) | Out-String
Write-Host $txt

# Pull the divergence PC and show the lifted C for it (the instruction to fix).
$m = [regex]::Match($txt, 'diverged at PC 0x([0-9A-Fa-f]+)')
if (-not $m.Success) { $m = [regex]::Match($txt, 'ours=0x([0-9A-Fa-f]+)') }
if ($m.Success) {
  $hex = $m.Groups[1].Value
  $fn  = "spu_func_{0:X8}" -f [Convert]::ToInt32($hex, 16)
  Stage "NEXT INSTRUCTION TO FIX  (LS 0x$hex -> $fn in recomp_prx\$Source)"
  Select-String -Path $gsc -Pattern "void $fn\(" -Context 0,2 |
    ForEach-Object { $_.Line; $_.Context.PostContext } | Write-Host
  Write-Host "`nFix it in the lifter, then re-run:  powershell -File tools\diverge.ps1" -ForegroundColor Green
} else {
  Write-Host "`nNo divergence in the compared window -- traces AGREE over it." -ForegroundColor Green
}
exit 0   # a found divergence is a successful run, not a script failure
