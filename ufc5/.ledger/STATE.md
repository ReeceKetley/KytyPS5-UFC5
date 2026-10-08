# Current State

## Architecture

UFC 5 is being developed on the `ufc5-v2` Kyty fork, which renders correctly; the only remaining
problem is frame rate (FACT-0005). The current path uses native 4K output and sparse BDA. Bounded
Function LDS, the CMask impossible-clear skip, compiled SRT and dirty-only BDA are now ALL default
on in code, with per-feature opt-outs (`KYTY_FUNCTION_LDS_BOUND=0`, `KYTY_CMASK_SKIP_IMPOSSIBLE`,
`KYTY_COMPILED_SRT=0`, `KYTY_BDA_SYNC=0`). Still record exact flags per run. The repository has
extensive uncommitted work; do not discard it.

## Working

- Paused-fight present rate is **~10.2 fps on a plain launch**, up from 6.28 (EXP-0009): compiled
  SRT + dirty BDA default-on took it to 9.26 (EXP-0012), barrier coalescing added ~10% (EXP-0013),
  and lazy submission a further +10-19% (EXP-0027). Live gameplay is at least as fast as paused
  (FACT-0014), so the paused fight is a CONSERVATIVE proxy.
- **MENUS now run 54-60 fps, up from 40-41** (EXP-0027). Lazy submission helps far more there than
  in the fight, because submission cost is per-submission regardless of the work it carries.
- Lazy submission is default-on (`KYTY_SUBMIT_DRAW_THRESHOLD=0` opts out, default 64 draws per
  submit). It cut queue submits 340 -> 46/epoch and foreground waits 52.73 -> 39.61 ms/epoch.
- Global-barrier coalescing is on by default (`KYTY_BARRIER_COALESCE=0` opts out). It cut
  `EmitGlobalBarrier` by 68% per dispatch and barrier resource records by 33% (EXP-0013).
- Bounded LDS already paid a large dividend (FACT-0009): epoch wall 736 -> 347 ms, waits 465 -> 33
  ms, detile 413.2 -> 4.33 ms/epoch. It was never a neutral result; the gain was absorbed by the
  next bottleneck.
- Compiled SRT and dirty-only BDA are default-on and validated: 27919 + 1093 shadow checks, zero
  mismatches (EXP-0011, FACT-0013). They cut recording 230.8 -> 129.8 ms/epoch.
- The paused-fight measurement is reproducible to within ~1% across captures, so A-B comparisons
  are now meaningful. Use `tools/profile-production.ps1`, not the legacy `run_ufc5.ps1` (WARN-0008).
- `KYTY_DISPATCH_RANGES_CSV=<path>` logs per-dispatch thread groups and every buffer/image range
  read or written (tagged bw/br/iw/ir). Off by default; diagnostic only.

## Broken or Blocked

- ~10.4 fps is still ~2.9x short of the 30 fps floor (DEC-0003). Not playable yet.
- The bottleneck is host CPU command recording, not GPU work and no longer readback waits
  (EXP-0009). Readback waits fell to 9.6% of wall; do not port the old branch's transfer-queue
  work until attribution says otherwise (FACT-0008).
- **Four lines of attack are closed; do not re-walk them.** Compute dispatch merging is blocked by
  scattered, differently-sized writes (FACT-0021). Barrier elision fired on 17.9% of dispatches for
  no gain, reverted (EXP-0017). Whole-binding reuse measures a 1% hit rate (FACT-0022). The draw
  commit worker thread HALVES the frame rate (EXP-0020) -- never enable `KYTY_DRAW_COMMIT_THREAD`.
- **Recurring trap, hit three times:** the recording stream is not partitioned the way the logical
  phases suggest, because every stage reaches the one shared command buffer through
  `CommandBuffer::Handle()`. Instrumented adjacency does not prove nothing was recorded between two
  points (FACT-0019), and hazard-freedom does not imply recording-adjacency (EXP-0017).
- Remaining per-draw cost sits in `draw_state_cpu` and `draw_bindings_cpu`: ~123 ms/epoch of a
  ~246 ms wall (FACT-0015), still the largest single item -- but **local optimization is close to
  exhausted**: three attempts returned at most 1.7% (EXP-0019). Scene variation between paused
  fights exceeds that, so sub-2% changes cannot be measured end to end.
- Frame composition has shifted: recording ~48%, GPU batch ~30%, waits ~16% (FACT-0015). Pure CPU
  optimization cannot reach 30 fps alone; zeroing all instrumented CPU work still leaves ~84 ms.
- Waits rose as CPU work fell, so further CPU wins buy less than the early ones did.
- 1080p override breaks the scene/HUD. BDA suballocation crashed at round start.
- Format-83 R16G16_SFLOAT CMask clear decoding remains a correctness gap. LDS-bound output has
  visual observation but no pixel-matched proof (WARN-0007).

## Current Milestone

Frame time is attributed (EXP-0009, EXP-0010, FACT-0011) and the cheap wins are banked. Now reduce
per-draw CPU work in `draw_state_cpu` / `draw_bindings_cpu` and the graphics draw count, measured
end to end against the reproducible ~246 ms/epoch paused-fight baseline, toward the 30 fps floor.

## Next Work

Getting to 30 FPS needs ARCHITECTURE, not more local tuning. The two candidates, both
multi-session, and they likely have to be combined:

1. **Cut the 5570 graphics draws per epoch (TASK-0012)** -- the untested lever with the most
   headroom. Each draw removed saves per-draw CPU, GPU batch time and barrier records, so it pays
   three times. Start by instrumenting consecutive graphics draws the way the compute path was.
2. **Parallel recording via secondary command buffers (TASK-0011)** -- the one threading approach
   EXP-0020 does not rule out. Real scope is making the texture/buffer/pipeline caches concurrent,
   not just splitting the buffer. Ideal 4-way is ~37% wall, roughly 14-16 fps, so not sufficient
   alone.
3. Close the pixel-matched proof gap on bounded LDS (WARN-0007, TASK-0003).

Last updated 2026-10-08 after EXP-0017. Original `ledger.md` content is DEPRECATED historical
notes; its process IDs and live-mode claims are snapshots, not current state.
