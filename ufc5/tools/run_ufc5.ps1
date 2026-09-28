# Launch EA Sports UFC 5 (PPSA03541) in KytyPS5 with the configuration that
# measured 5.0-5.4 fps in-fight on 2026-09-11.
#
#   .\run_ufc5.ps1                 # the 5 fps configuration
#   .\run_ufc5.ps1 -Tag myrun      # same, log to KytyLog-PPSA03541-myrun.txt
#   .\run_ufc5.ps1 -Baseline       # everything optional turned OFF, for A/B
#   .\run_ufc5.ps1 -Validate       # add --shader-validation (names a bad shader
#                                  # at compile time instead of a device loss)
#   .\run_ufc5.ps1 -Verify         # KYTY_SRT_LINEAR=verify: run both SRT
#                                  # evaluators and compare. SLOW - correctness
#                                  # check only, the fps from this run is
#                                  # meaningless because it does the work twice.
#   .\run_ufc5.ps1 -RunHangCs      # no-op now (CS runs by default); use -Baseline
#                                  # to revert to the old skip-CS behaviour
#   .\run_ufc5.ps1 -ForceScene     # now the DEFAULT - force 3D scene, suppress HUD
#                                  # (KYTY_UFC_FORCE_SCENE is unset -> true by default)
#
# Reading the result: NEVER quote fps from a single FrameProfile line. Scenes
# vary 2,400-4,100 draws/frame, so two runs of the "same" fight differ by 70%.
# Take the median us/draw over the whole run - see "Replicating the 5 fps
# configuration" in D:\PS5\ledger.md.

[CmdletBinding()]
param(
    [string] $Tag       = 'run',
    [switch] $Baseline,
    [switch] $Validate,
    [switch] $Verify,
    [switch] $RunHangCs,
    [switch] $Timestamps,
    [switch] $ForceScene,
    [string] $GameDir   = 'D:\PS5\Games\UFC5',
    [string] $BinDir    = 'D:\PS5\Emulators\KytyPS5-Bin',
    [string] $LogDir    = 'D:\PS5'
)

$ErrorActionPreference = 'Stop'

$exe = Join-Path $BinDir 'kyty_emulator.exe'
if (-not (Test-Path $exe))     { throw "emulator not found: $exe (build + deploy first)" }
if (-not (Test-Path $GameDir)) { throw "game dump not found: $GameDir" }

# Always close the running emulator first - copying over a live exe fails, and
# two instances fight over the same log file.
Get-Process kyty_emulator -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

# --- The configuration -------------------------------------------------------
# KYTY_SKIP_CS_HASH   Default: unset. Compute shaders run by default (wave32 lowering
#                     is implemented). Set to the hang-CS hash to skip it for A/B
#                     testing. -RunHangCs is now a no-op; -Baseline sets this.
# KYTY_SRT_LINEAR     Flat SRT evaluator. getprog -31%; does not show
#                     end-to-end, but it is verified equivalent and free to run.
$env:KYTY_SRT_LINEAR     = '1'
# KYTY_UFC_FORCE_SCENE  Default: ON (unset -> force_scene=true). The 3D scene
#                       (octagon, fighters) is always selected as the presentation
#                       source; the HUD/interface layer is never the source. If no
#                       scene is found, the frame is cleared instead of blitting the
#                       HUD-only buffer. Set to 0 or use -Baseline to disable.
# KYTY_GPU_TIMESTAMPS is NOT on by default. GpuTimestamps::Arm() resets the query pool from the
# HOST while command buffers from the previous window can still be in flight, so a slot can be
# written twice with only one intervening reset - Vulkan validation reports
# "vkCmdWriteTimestamp2(): query N: query not reset", which is undefined behaviour and a
# plausible cause of the intermittent ErrorDeviceLost seen on 2026-09-11. Opt in with
# -Timestamps when you actually need per-draw/per-dispatch GPU attribution; frame_stats.py
# reads FrameProfile and does not need it. Opted in AFTER the clear loop below.

# Cleared explicitly so a value left over in the shell cannot silently change
# the run. All of these are measured NEUTRAL or REGRESSIONS - see the ledger.
#   KYTY_XFER_QUEUE       neutral (time relocates into dispatch recording)
#   KYTY_GC_CRITICAL_MB   transient only; VRAM climbs to whatever line you set
#   KYTY_TEXGC_AGE_FIX    +14us/draw of texture re-upload thrash
#   KYTY_TEXGC_BUDGET_FIX texgc 3ms -> 71ms; the GC runs ~175x per frame
#   KYTY_BUFGC_OWN_SHARE  0 restores the pre-fix BufferCache behaviour
#   KYTY_SKIP_PS_HASH     diagnostic; drops draws, so the frame is incomplete
foreach ($name in @('KYTY_XFER_QUEUE','KYTY_GC_CRITICAL_MB','KYTY_GC_TRIGGER_MB',
                    'KYTY_TEXGC_AGE_FIX','KYTY_TEXGC_BUDGET_FIX',
                    'KYTY_BUFGC_OWN_SHARE','KYTY_SKIP_CS_HASH','KYTY_SKIP_PS_HASH',
                    'KYTY_GPU_TIMESTAMP_PS','KYTY_DEFER_READBACK','KYTY_GPU_TIMESTAMPS',
                     'KYTY_RUN_HANG_CS','KYTY_UFC_FORCE_SCENE')) {
    Remove-Item "Env:\$name" -ErrorAction SilentlyContinue
}

# Compute shaders run by default (wave32 lowering implemented). CS is no longer
# skipped; KYTY_SKIP_CS_HASH is only set in -Baseline mode for A/B comparison.
if ($RunHangCs) {
    $Tag = "$Tag-hang-cs"
}

if ($Timestamps) { $env:KYTY_GPU_TIMESTAMPS = '1' }

# ForceScene is now the DEFAULT. The -ForceScene switch is kept for backwards
# compatibility and adds a tag to the log for identification.
if ($ForceScene) { $Tag = "$Tag-force-scene" }

if ($Baseline) {
    # A/B control: the emulator as it behaves without this session's opt-ins.
    Remove-Item Env:\KYTY_SRT_LINEAR -ErrorAction SilentlyContinue
    $env:KYTY_BUFGC_OWN_SHARE = '0'
    # Revert to pre-fix behaviour: skip the hang CS and allow the HUD to be
    # selected as the presentation source.
    $env:KYTY_SKIP_CS_HASH = '0xea0aceac518ec52d'
    $env:KYTY_UFC_FORCE_SCENE = '0'
    $Tag = "$Tag-baseline"
}
if ($Verify) { $env:KYTY_SRT_LINEAR = 'verify'; $Tag = "$Tag-verify" }

$log  = Join-Path $LogDir "KytyLog-PPSA03541-$Tag.txt"
if (Test-Path $log) { Remove-Item $log -Force }

# --printf-direction File is NOT optional. printf_direction defaults to Silent
# and graphics_debug_dump_enabled() keys off it, so without this EVERY counter
# is silently discarded and the log looks fine but is empty of measurements.
$cliArgs = @('--game', $GameDir, '--printf-direction', 'File', '--printf-output-file', $log)
if ($Validate) { $cliArgs += @('--shader-validation', 'true') }

Write-Host ''
Write-Host 'KytyPS5 / UFC 5 (PPSA03541)' -ForegroundColor Cyan
Write-Host "  log      $log"
foreach ($n in @('KYTY_SKIP_CS_HASH','KYTY_RUN_HANG_CS','KYTY_GPU_TIMESTAMPS',
                  'KYTY_SRT_LINEAR','KYTY_BUFGC_OWN_SHARE','KYTY_UFC_FORCE_SCENE')) {
    $v = [Environment]::GetEnvironmentVariable($n)
    if ($v) { Write-Host ("  {0,-22} {1}" -f $n, $v) }
}
if ($Validate) { Write-Host '  --shader-validation    true' }
Write-Host ''
Write-Host 'Navigate into a fight, then let it run ~2 minutes for steady state.' -ForegroundColor Yellow
Write-Host 'Expect ~5 fps in-fight. The round renders with 3D fighters and arena.'
Write-Host ''
Write-Host 'Median us/draw over the whole run:' -ForegroundColor Cyan
Write-Host "  python `"$(Join-Path $PSScriptRoot 'frame_stats.py')`" `"$log`""
Write-Host ''

$proc = Start-Process -PassThru -FilePath $exe -ArgumentList $cliArgs -WorkingDirectory $BinDir
Write-Host "started pid=$($proc.Id)"
