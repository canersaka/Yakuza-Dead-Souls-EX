[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Capture,

    [string]$Stream = "",
    [string]$OutDir = "",
    [switch]$Hardware,
    [int]$MaxDraw = -1
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$capturePath = (Resolve-Path $Capture).Path
$stem = [System.IO.Path]::GetFileName($capturePath) -replace '(?i)\.rrc(?:\.gz)?$', ''

if (-not $Stream) {
    $Stream = Join-Path $repoRoot ("scratch\replay\{0}.rxs" -f $stem)
}
if (-not [System.IO.Path]::IsPathRooted($Stream)) {
    $Stream = Join-Path $repoRoot $Stream
}

if (-not $OutDir) {
    $OutDir = Join-Path $repoRoot ("scratch\replay\{0}" -f $stem)
}
if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $repoRoot $OutDir
}

$streamParent = Split-Path -Parent $Stream
New-Item -ItemType Directory -Force -Path $streamParent | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Write-Host "[rrc] exporting $capturePath"
& py -3 (Join-Path $repoRoot "tools\rrc_export.py") $capturePath -o $Stream
if ($LASTEXITCODE -ne 0) {
    throw "rrc_export.py failed with exit code $LASTEXITCODE"
}

$testsDir = Join-Path $repoRoot "libs\video\tests"
Push-Location $testsDir
try {
    Write-Host "[rrc] building replay harness"
    & cmd /c build_replay.cmd
    if ($LASTEXITCODE -ne 0) {
        throw "build_replay.cmd failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

$savedMaxDraw = $env:RSX_MAX_DRAW
try {
    if ($MaxDraw -ge 0) {
        $env:RSX_MAX_DRAW = [string]$MaxDraw
    }
    else {
        Remove-Item Env:RSX_MAX_DRAW -ErrorAction SilentlyContinue
    }

    $replayArgs = @($Stream)
    if ($Hardware) {
        $replayArgs += "--hw"
    }
    $replayArgs += @("--out", $OutDir)

    Write-Host "[rrc] replaying to $OutDir"
    & (Join-Path $testsDir "replay.exe") @replayArgs
    if ($LASTEXITCODE -ne 0) {
        throw "replay.exe failed with exit code $LASTEXITCODE"
    }
}
finally {
    if ($null -eq $savedMaxDraw) {
        Remove-Item Env:RSX_MAX_DRAW -ErrorAction SilentlyContinue
    }
    else {
        $env:RSX_MAX_DRAW = $savedMaxDraw
    }
}

Write-Host "[rrc] stream: $Stream"
Write-Host "[rrc] output: $OutDir"

