<#
  ppu_diverge.ps1 -- one command to find the next PPU lift divergence vs RPCS3.

  Mirrors tools\diverge.ps1 (the SPU-side driver) for the PPU trace-diff.
  Two paths:

    - Without -SkipBoot: arm YZ_PPU_TRACE* env vars, launch the game with
      NATIVE stderr/stdout redirection (avoids the CRT stderr serialization
      artifact under print volume), watch scratch\ppu_trace.txt
      until its size is stable for 60s (or a 20-min timeout), kill the game.
    - With -SkipBoot: use the existing scratch\ppu_trace.txt as-is (no boot).

  Either way it then runs tools\tracediff.py against -Ref and prints the
  result. On divergence it pulls the divergent PC and shows tools\show_func.py
  --both truncated to +-30 lines, so the report lands with disasm context in
  one command.

  The RPCS3-side reference capture is manual for v1 (an instrumented
  interpreter-mode RPCS3 run, aligned to the same arm PC and tid); if -Ref is
  missing or doesn't exist, this script says so and stops, it does not
  attempt to capture one.

  Usage:
    .\tools\ppu_diverge.ps1 -Tid 11 -Arm 0xF00E80 -Ref scratch\rpcs3_ppu_trace3.txt
    .\tools\ppu_diverge.ps1 -Tid 11 -Arm 0xF00E80 -SkipBoot -Ref scratch\rpcs3_ppu_trace3.txt
    .\tools\ppu_diverge.ps1 -Tid 11 -Arm 0xF00E80 -N 2000000 `
        -AlignPc F00E80 -PcRange 10000:1310768 `
        -Flags 'YZ_AUDIO_FORCE=1;YZ_VMGUARD=1;YZ_VMGUARD_SURVIVE=1' -Ref scratch\rpcs3_ppu_trace3.txt
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][int]$Tid,
  [Parameter(Mandatory = $true)][string]$Arm,
  [int]   $N          = 2000000,
  [string]$Ref        = '',
  [string]$AlignPc    = '',
  [string]$PcRange    = '10000:1310768',
  [string]$Flags      = '',
  [string]$OursTrace  = 'scratch\ppu_trace.txt',
  [int]   $StableSecs = 60,     # trace file size must hold steady this long...
  [int]   $TimeoutSecs = 1200,  # ...or this overall timeout fires (20 min)
  [switch]$SkipBoot
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo
function Stage($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }

# Default -AlignPc to -Arm (the PPU workflow always starts tracing at the arm
# PC, so aligning the diff there is the common case; override if needed).
if ($AlignPc -eq '') { $AlignPc = $Arm.TrimStart('0', 'x', 'X') }
if ($AlignPc -eq '') { $AlignPc = $Arm }
$armHex = $Arm -replace '^0[xX]', ''

# -Ref is required for the diff step (v1: RPCS3-side capture stays manual).
if ($Ref -eq '' -or -not (Test-Path $Ref)) {
  Write-Host "`n[ppu_diverge] No usable -Ref reference trace given (got: '$Ref')." -ForegroundColor Yellow
  Write-Host "Capture one first, from an instrumented interpreter-mode RPCS3 run" -ForegroundColor Yellow
  Write-Host "(RPCS3 interpreter-mode instrumentation, aligned to the same arm PC/tid)." -ForegroundColor Yellow
  exit 2
}

if (-not $SkipBoot) {
  Stage "1/3  Arm YZ_PPU_TRACE* + launch (NATIVE redirection)"

  $env:YZ_PPU_TRACE     = '1'
  $env:YZ_PPU_TRACE_TID = "$Tid"
  $env:YZ_PPU_TRACE_ARM = $armHex
  $env:YZ_PPU_TRACE_N   = "$N"

  # -Flags 'A=1;B=2' -> set each as an env var (house convention: semicolon-
  # separated KEY=VALUE pairs).
  if ($Flags -ne '') {
    foreach ($kv in ($Flags -split ';')) {
      if ($kv -match '^\s*([^=]+)=(.*)$') {
        $k = $matches[1].Trim(); $v = $matches[2].Trim()
        Set-Item -Path "env:$k" -Value $v
        Write-Host "    +env $k=$v"
      }
    }
  }

  if (Test-Path $OursTrace) { Remove-Item $OursTrace -Force }
  $errLog = 'scratch\ppu_diverge_run.err'; $outLog = 'scratch\ppu_diverge_run.out'
  # NATIVE redirection: cmd's 2>/1> are plain kernel file handles;
  # PowerShell's -RedirectStandardError pumps a .NET pipe that serializes every
  # guest thread on the CRT stderr lock under print volume -- same fix as
  # boot_until.ps1 / diverge.ps1.
  $cmdline = "yakuza\build\yakuza_recomp.exe game\EBOOT.elf 2>`"$errLog`" 1>`"$outLog`""
  Start-Process -FilePath "cmd.exe" -ArgumentList "/c", $cmdline -WindowStyle Hidden | Out-Null
  $p = $null
  for ($w = 0; $w -lt 50 -and -not $p; $w++) {
    Start-Sleep -Milliseconds 100
    $p = Get-Process yakuza_recomp -ErrorAction SilentlyContinue |
         Sort-Object StartTime -Descending | Select-Object -First 1
  }
  if (-not $p) { throw "guest process never appeared" }

  Stage "2/3  Watch $OursTrace until stable for ${StableSecs}s (timeout ${TimeoutSecs}s)"
  $last = -1; $stableFor = 0; $sw = [System.Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt $TimeoutSecs) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) { Write-Host "    process exited at +$([int]$sw.Elapsed.TotalSeconds)s"; break }
    $sz = if (Test-Path $OursTrace) { (Get-Item $OursTrace).Length } else { 0 }
    if ($sz -gt 0 -and $sz -eq $last) { $stableFor++ } else { $stableFor = 0 }
    $last = $sz
    if ($stableFor -ge $StableSecs) {
      Write-Host ("    stable at {0:N0} bytes for {1}s -- done" -f $sz, $StableSecs)
      break
    }
  }
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  if (-not (Test-Path $OursTrace) -or (Get-Item $OursTrace).Length -eq 0) { throw "no trace produced at $OursTrace" }
  Write-Host ("    trace: {0:N0} bytes ({1})" -f (Get-Item $OursTrace).Length, $OursTrace)
} else {
  Stage "1-2/3  -SkipBoot: reusing existing $OursTrace"
  if (-not (Test-Path $OursTrace)) { throw "-SkipBoot given but $OursTrace does not exist" }
  Write-Host ("    trace: {0:N0} bytes ({1})" -f (Get-Item $OursTrace).Length, $OursTrace)
}

Stage "3/3  Diff vs RPCS3 ($Ref)"
# tracediff.py parses --pc-range as HEX (not decimal) -- pass PcRange through verbatim.
$diffArgs = @($OursTrace, $Ref, '--align-pc', $AlignPc, '--pc-only', '--pc-range', $PcRange)
$txt = (& py -3 tools\tracediff.py @diffArgs) | Out-String
Write-Host $txt

# Pull the divergent PC out of either message shape tracediff.py emits:
#   "compute diverged at PC 0x...:"   or   "PC diverged: ours=0x... ref=0x..."
$m = [regex]::Match($txt, 'diverged at PC 0x([0-9A-Fa-f]+)')
if (-not $m.Success) { $m = [regex]::Match($txt, 'ours=0x([0-9A-Fa-f]+)') }
if ($m.Success) {
  $hex = $m.Groups[1].Value
  Stage "DIVERGENT PC 0x$hex -- disassembly context (+-30 lines)"
  & py -3 tools\show_func.py $hex --both --max 60
  Write-Host "`nFix the lift at 0x$hex, then re-run this script." -ForegroundColor Green
  exit 1
} else {
  Write-Host "`nNo divergence in the compared window -- traces AGREE." -ForegroundColor Green
  exit 0
}
