# Current State

## Architecture

UFC 5 is being developed on the `ufc5-v2` Kyty fork, which renders correctly; the only remaining
problem is frame rate (FACT-0005). The current path uses native 4K output and sparse BDA. Bounded
Function LDS, the CMask impossible-clear skip, compiled SRT and dirty-only BDA are now ALL default
on in code, with per-feature opt-outs (`KYTY_FUNCTION_LDS_BOUND=0`, `KYTY_CMASK_SKIP_IMPOSSIBLE`,
`KYTY_COMPILED_SRT=0`, `KYTY_BDA_SYNC=0`). Still record exact flags per run. The working tree is
clean as of 2026-10-08; everything is committed on `ufc5-v2`.

## Working

- Paused-fight present rate is **10.2-11.6 fps depending on the pause**, up from 6.28 (EXP-0009):
  compiled SRT + dirty BDA default-on took it to 9.26 (EXP-0012), barrier coalescing added ~10%
  (EXP-0013), and lazy submission a further +10-19% (EXP-0027). Live gameplay is at least as fast
  as paused (FACT-0014), so the paused fight is a CONSERVATIVE proxy.
- **There is no single fps baseline, and quoting one is a mistake** (FACT-0033, WARN-0011). The
  same binary and flags read 10.2 at one pause and 11.13 at another on the same day: scene
  variation between fights is ~9%, within one fight 0.3-0.6%. Report A-B deltas measured inside
  ONE scene; when an absolute number is unavoidable, name the pause.
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
- **A-B must use SHORT ALTERNATING ARMS** (WARN-0013): long arms can report the WRONG SIGN,
  because the machine drifts non-monotonically on a 1-2 minute timescale -- one arm climbed
  11.18 -> 11.70 fps with no flip in it. Run 10 arms of 3 windows alternating; `analyze-ab-fps.py`
  then estimates from adjacent pairs, which cancels drift, and cross-checks with a trend fit.
  Always pass `--until-bytes` (the next launch's `fps_log_start_bytes`): the log is append-only
  and shared, so an unbounded read silently absorbs every later run.
- **A-B now happens inside one paused fight** (DEC-0005), which is what made the floor tractable.
  `KYTY_AB_CONTROL_FILE` holds one integer polled every 250 ms; the present-rate sampler reads the
  same file, tags every window `variant=<n>` and resets its window on a flip, so nothing straddles
  a boundary. Launch with `-Ab`, flip with `-Action AbSet -AbVariant <n>`, read out with
  `analyze-ab-fps.py`, and always bracket X-Y-X. Resolution is ~0.5%, about 15x finer than the old
  relaunch floor. Use `tools/profile-production.ps1`, not the legacy `run_ufc5.ps1` (WARN-0008).
- **The per-draw commit packet is gone, +5.4%** (EXP-0031, revised by NOTE-0055 from the pooled
  +4.3%; paired CI +4.1% to +6.5%, least-squares cross-check also +5.4%). A draw is recorded
  through a non-owning `DrawCommitView` instead of a 6792-byte `CommitWork` that was
  `make_shared`'d on every draw even on the inline path that never used it.
  `build_packet`/`view_over` remain for the deferred path only.
- **Only changed dynamic-state setters are issued, about +2%** (EXP-0032, paired +2.6% CI +1.2%
  to +4.4%, cross-check +1.5%). All fifteen were re-issued on every draw. The cache lives on
  CommandBuffer and is cleared in `Begin()`; colour-write is never cached and `blitHelper.cpp`
  must reset it -- see the invariant in `graphicsDynamicState.h` before adding any setter.
  `KYTY_DYNAMIC_STATE_CACHE=0` opts out.
- `KYTY_DISPATCH_RANGES_CSV=<path>` logs per-dispatch thread groups and every buffer/image range
  read or written (tagged bw/br/iw/ir). Off by default; diagnostic only.

## Broken or Blocked

- Still roughly 2.7-2.9x short of the 30 fps floor (DEC-0003), and the multiplier itself depends
  on which pause you measure (FACT-0033). Not playable yet.
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
  ~246 ms wall (FACT-0015), still the largest single item. **Local optimization is NOT exhausted
  after all.** EXP-0019 concluded it was, on three attempts returning at most 1.7%; EXP-0031 then
  got +4.3% from deleting a single per-draw allocation. Those three attempts were never shown to
  be small, only unmeasurable against the old 8% relaunch floor (FACT-0033). And analytic
  memory-cost estimates on this path underestimate by about 4x, because they miss cold-block
  read-for-ownership, working-set displacement and allocator cost (FACT-0034) -- so rule nothing
  out here without measuring it.
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
   EXP-0020 does not rule out. Stage 1 is **TASK-0016**, the ordered operation queue; its A-B
   prerequisite is discharged (EXP-0030). The view refactor and the dynamic-state split are
   landed. **The remaining blocker is binding lifetime, not payload size** (FACT-0036): queued
   draws held `PreparedBindings*` into the persistent `m_graphics_bindings`, which the next draw
   overwrites -- sound only because at most one draw was ever in flight. `CommitBindings` also
   issues image transitions from live cache state, and 98.7% of draws push descriptors
   (EXP-0028), so the writes must be deep-copied into the arena. Real order: make the binding
   result per-draw and cheap, then the arena and queue, then the worker.
3. Close the pixel-matched proof gap on bounded LDS (WARN-0007, TASK-0003).

Last updated 2026-10-08 after EXP-0032, FACT-0036 and WARN-0013. Original `ledger.md` content is DEPRECATED
historical notes; its process IDs and live-mode claims are snapshots, not current state.
