# UFC 5 (PPSA03541) work

Fork-specific notes and tooling for getting **EA Sports UFC 5** running in KytyPS5. Kept in its
own directory so the fork's additions stay separate from upstream and rebases onto
`KytyPS5/KytyPS5` stay clean.

## Start here

**[`ledger.md`](ledger.md)** is the source of truth. It records what was measured, what was
tried and failed, and *why* — including several changes that looked like wins and were
withdrawn. Read the top section before doing any performance work; it will save you from
repeating experiments that are already ruled out.

Current state: **1.0 → 5.0 fps in-fight** (2026-09-11). The fight round renders **black** —
that is the skipped occlusion compute shader, a known and separate problem.

## Tools

| | |
|---|---|
| `tools/run_ufc5.bat` | Launch with the configuration that measured 5 fps. Wrapper over the `.ps1`. |
| `tools/run_ufc5.ps1` | The real script. `-Baseline` / `-Validate` / `-Verify` variants for A/B. |
| `tools/frame_stats.py` | **Use this to judge any change.** Median µs/draw over a whole run. |
| `tools/devenv.ps1` | MSVC/clang-cl build environment. |
| `tools/drive_ufc.py`, `ufc5_pad.py`, `ufc5_keys.py`, `*.route` | Virtual pad menu navigation (vgamepad + ViGEmBus). |
| `tools/capture-kyty.ps1` | Log capture helper. |

```
ufc5\tools\run_ufc5.bat
python ufc5\tools\frame_stats.py D:\PS5\KytyLog-PPSA03541-run.txt
```

## Judging a change

`fps` from a single `FrameProfile` line is worthless — scenes vary 2,400–4,100 draws/frame, so
two runs of the "same" fight differ by 70%. `frame_stats.py` normalises by draw count, drops
warm-up, and reports the median. Two changes were called wins during this work and had to be
withdrawn; both were caught by this method and by nothing else.

Also: every `FrameProfile` field accumulates over its window, and `frames=N` is the window
length — `draws=7971 frames=3` is **2,657 draws/frame**, not 7,971. `GpuBusy` and `GpuDraws`
windows *are* one frame.

## Paths

These scripts assume a specific layout and hardcode it as defaults:

```
D:\PS5\Games\UFC5              game dump
D:\PS5\Emulators\KytyPS5-Bin   deployed emulator
D:\PS5\                        log output
```

`run_ufc5.ps1` takes `-GameDir`, `-BinDir` and `-LogDir` to override them. `ledger.md` quotes
absolute paths throughout because it is a working log, not documentation — treat the paths as
belonging to the machine it was written on.
