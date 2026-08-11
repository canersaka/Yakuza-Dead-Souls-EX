# cycle.ps1 -- the whole validated iteration pipeline as ONE command, writing
# ONE summary (scratch\cycle_summary.txt). A session becomes: read the summary,
# think, make the one edit that matters, rerun. Replaces the token-hungry
# supervise-every-step chain.
#
#   .\tools\cycle.ps1                      # build + golden (the default loop)
#   .\tools\cycle.ps1 -Relift              # + conformance gate + PPU relift chain (game + 3 PRX + regen)
#   .\tools\cycle.ps1 -SpuImages           # + batch SPU image lift/table regen
#   .\tools\cycle.ps1 -Pattern 'codec-put' # + a boot_until probe run after golden
#   .\tools\cycle.ps1 -Relift -Pattern 'throw_event' -Timeout 90
#
# Any $env:YZ_* set before invoking inherits into the probe run.
# Exit 0 = everything passed (and pattern matched, if given).
param(
    [switch]$Relift,
    [switch]$SpuImages,
    [string]$Pattern = '',
    [int]$Timeout = 90,
    [switch]$SkipGolden,
    [string]$GameInputDir = ''
)

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$sum = Join-Path $root 'scratch\cycle_summary.txt'
"cycle @ $(Get-Date -Format 'HH:mm:ss')  relift=$Relift spuimages=$SpuImages pattern='$Pattern'" | Set-Content $sum
$fail = $false

if ($GameInputDir -eq '') {
    # In a linked worktree the decrypted inputs remain in the primary
    # worktree. Derive that location from git's common directory rather than
    # recording a machine-specific absolute path.
    $gitCommon = (& git rev-parse --path-format=absolute --git-common-dir).Trim()
    if ($LASTEXITCODE -ne 0 -or $gitCommon -eq '') {
        throw 'Could not derive GameInputDir from git; pass -GameInputDir.'
    }
    $GameInputDir = Split-Path -Parent $gitCommon
}
$gameElf = Join-Path $GameInputDir 'game\EBOOT.elf'
$firmwareInput = Join-Path $GameInputDir 'recomp_prx'

function Step($name, [scriptblock]$body) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Host "[cycle] $name..."
    $out = & $body 2>&1
    $t = [int]$sw.Elapsed.TotalSeconds
    $ok = ($LASTEXITCODE -eq 0 -or $null -eq $LASTEXITCODE)
    $mark = if ($ok) { 'OK  ' } else { 'FAIL' }
    "[$mark] $name (${t}s)" | Add-Content $script:sum
    if (-not $ok) {
        $out | Select-Object -Last 15 | Add-Content $script:sum
        $script:fail = $true
    }
    return $ok
}

if ($Relift) {
    if (-not (Step 'conformance suite' { py -3 tools\test_ppu_lift.py })) {
        Write-Host '[cycle] conformance FAILED -- aborting before the expensive relift'
        Get-Content $sum | Write-Host; exit 1
    }
    Step 'relift game' { py -3 -u tools\ppu_lifter.py $gameElf --output recomp\ --functions functions.json --override-funcs yakuza\movie_overrides.txt } | Out-Null
    Step 'relift libsre oracle' { py -3 -u tools\ppu_lifter.py (Join-Path $firmwareInput 'libsre_image.bin') --raw --base 0x02000000 --toc 0x02039AB0 --functions (Join-Path $firmwareInput 'libsre_functions.json') --output recomp\prx --header-name libsre_recomp.h --source-name libsre_recomp --table-name libsre_function_table --extern-funcs recomp\ppu_recomp.h } | Out-Null
    Step 'relift libgcm oracle' { py -3 -u tools\ppu_lifter.py (Join-Path $firmwareInput 'libgcm_sys_image.bin') --raw --base 0x02100000 --toc 0x02114000 --functions (Join-Path $firmwareInput 'libgcm_sys_functions.json') --output recomp\prx --header-name libgcm_sys_recomp.h --source-name libgcm_sys_recomp --table-name libgcm_function_table --extern-funcs recomp\ppu_recomp.h } | Out-Null
    Step 'relift pxd_shader' { py -3 -u tools\ppu_lifter.py (Join-Path $firmwareInput 'ogrez_shader_ps3.ppu_image.bin') --raw --base 0x02200000 --toc 0x02673020 --functions (Join-Path $firmwareInput 'ogrez_shader_ps3.ppu_functions.json') --output recomp\prx --header-name pxd_shader_recomp.h --source-name pxd_shader_recomp --table-name pxd_shader_function_table --extern-funcs recomp\ppu_recomp.h } | Out-Null
    Step 'gen_func_table' { py -3 yakuza\gen_func_table.py } | Out-Null
    Step 'audit native imports' { py -3 yakuza\gen_imports.py --spurs-backend native --elf $gameElf --firmware-dir $firmwareInput --output scratch\import_bridges_native_audit.cpp --audit } | Out-Null
}
if ($SpuImages) {
    Step 'batch SPU image lift + table' { py -3 tools\gen_spu_images.py } | Out-Null
}

Step 'build' { cmd /c scratch\dobuild2.bat } | Out-Null

if (-not $SkipGolden -and -not $fail) {
    $g = & .\tools\golden_boot.ps1 2>&1
    $gs = if ($LASTEXITCODE -eq 0) { 'OK  ' } else { 'FAIL' }
    "[$gs] golden boot" | Add-Content $sum
    $g | Where-Object { $_ -match '^\S|this:|golden:' } | Add-Content $sum
    if ($LASTEXITCODE -ne 0) { $fail = $true }
}

if ($Pattern -ne '' -and -not $fail) {
    $b = & .\tools\boot_until.ps1 -Pattern $Pattern -Timeout $Timeout -Tag cycle 2>&1
    $bs = if ($LASTEXITCODE -eq 0) { 'OK  ' } else { 'FAIL' }
    "[$bs] probe boot: '$Pattern'" | Add-Content $sum
    $b | Add-Content $sum
    if ($LASTEXITCODE -ne 0) { $fail = $true }
}

'' | Add-Content $sum
Get-Content $sum | Write-Host
if ($fail) { exit 1 } else { exit 0 }
