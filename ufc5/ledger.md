# UFC 5 / KytyPS5 ledger

Last updated: 2026-09-11 session 3 — **THE BLACK ROUND IS A PRESENT BUG, NOT A CULLING BUG.** The fight
scene renders fully into `0x1168360000` (1920x1080 HDR, 98.6% nonzero) every frame and never reaches the
scanout (8.6%, HUD only). The "scene RT is empty" evidence that drove days of occlusion/wave64 work came
from a surface dump with a **hardcoded address list that never contained the scene buffer**. Wave64
Stage 2 is no longer justified by the black round. See "THE BLACK ROUND IS A COMPOSITE/PRESENT BUG".

Also this session: **IN-FIGHT 1.0 -> 5.0 FPS.** Two independent wins: the 3 pixel
ubershaders now structurize on the Legacy path (1.0 -> 2.9), and `BufferCache` no longer garbage-collects
against memory it does not own (2.7 -> 4.6 median, 5.0-5.4 observed). Details below.

Previously in this session: **1.0 -> 2.9 FPS from the ubershader fix.**
The 3 pixel ubershaders now structurize on the Legacy path (privatise shared `Return` blocks). Per-draw GPU
cost **21,157 us -> 95-262 us (~80-220x)**; graphics GPU **847 -> 96-191 ms/frame**; census **672 legacy,
0 dispatch**; 0 device losses, 0 spirv-val failures. **The GPU is no longer the wall** (187 ms busy in a
366 ms frame). An SRT flat evaluator (`KYTY_SRT_LINEAR=1`) cuts `getprog` **28.0 -> 19.3 us/draw (-31%)**
but does **not** show end-to-end. Transfer queue re-tested: **NEUTRAL**. GC critical-threshold raise:
**a transient, not a fix** — VRAM climbs to whatever the threshold is and pins there. **The wall is now
`drawprep` (~60%), descriptor work in draw recording, and the unbounded growth of GPU-dirty buffers.**
See "2026-09-11 (session 3)".

Previously: 2026-09-11 session 2 — **in-fight wall identified: `IT_DISPATCH_INDIRECT` host arg read, ~45-50% of the frame**; CPU/GPU measured ~50/50 and serialised (per-draw track floors at ~2.1 fps); `vkCmdDispatchIndirect` implemented behind `KYTY_INDIRECT_DISPATCH`, default off, blocked on a depth-alias-sampling bug. See "2026-09-11 (session 2)".

Previously: 2026-09-10 (FPS work: intra-command-buffer EOP-wait skip — menu ~24→~33 fps, in-match cp_rest/finish collapsed but drawprep_ms now the wall; frame instrumentation added; commits 745d4d8 / 8ee5cc8 / aa794cd on fork)

### 2026-09-11 (session 3) — the übershaders structurize; the cause was a SHARED RETURN BLOCK

**FIX (uncommitted): `SplitSharedTerminalBlocks` in `ShaderCFG.cpp`, wired into `StructurizeLegacy`'s
retry ladder after the routing loop.** `legacy=0 -> legacy=1` for all three ubershader fixtures.

**The diagnosis in session 2 was aimed one step short of the cause.** The dumped CFG is:
```
16: cond -> true=192(Return) false=17      192: Return, preds=[16,191]
17: cond -> true=191         false=18      191: -> 192,   preds=[17]
18: -> 19                                  19:  join,     preds=[15,18]
```
i.e. `if (a) return; if (b) return; <join>` where **both early exits land on ONE shared return block**.
Because 192 is reached from 16 *and* from 191, it is dominated by 16, not by 17. `FindSelectionMerge(16)`
then takes the `false_reaches_true` branch at **:1322** (17 reaches 192, 192 does not reach 17) and returns
**192** — stretching 16's construct to the function exit, after which block 18's edge to the shared join 19
is an unstructured exit. **It never reaches the `global_merge == UINT32_MAX` gate at :1325 that the previous
session identified as "the gap".** That gate is real but is not on this path.

**Why privatising the return block is the right fix, not widening a merge gate.** SPIR-V allows leaving a
construct by returning, but *branching to a block that returns* is only legal if that block is inside the
construct. Sharing the return block converts a legal return into an illegal branch-out. Give 191 its own
clone and everything falls out of the existing code with no merge-selection change at all:
`FindSelectionMerge`'s `HasLinearPathToTerminal` rule now picks **17** for block 16 and **18** for block 17,
the tight merges. **This is why it does not disturb phis** — the constraint session 2 identified. Terminal
blocks have no successors, so cloning one adds no phi sites anywhere.
- Implementation: clone with the existing `AppendClonedSemanticBlock` (keeps `inst_begin/end`, so the
  epilogue is re-executed, which is correct — exactly one copy runs per invocation); first predecessor keeps
  the original. Only `Branch`/`ConditionalBranch` preds are retargeted (an `IndirectBranch`/`DispatchSwitch`
  keeps its own target tables and cannot be retargeted by id).
- Cost on these shaders: **+1 block** (193 -> 194). Not a code-size event.

**Verified, in this order:**
- `shader_cfg_tests` green, including new `TestUfc5PsLegacyStructurizedSpirv` which runs **all three**
  fixtures through the legacy strategy and gates on `spirv-val` + phi-parent validity + `OpSwitch == 0`.
  `1bcc68ff` 164,996 words / 1,981 phis, `b808b388` 164,526, `f7030726` 164,488 — all `switches=0 fallback=0`.
- `resource_materialization_tests`, `scalar_provenance_tests`, `resource_tracking_tests`: pass (the latter
  still logs the known pre-existing invalid-mip-range message). `shader_recompiler_compute_tests` fails at
  `renderContext.cpp:49 m_gpu == nullptr` — **confirmed pre-existing by stashing the change and re-running**,
  unrelated to CFG.
- **Menu run with `--shader-validation true`: 153 shaders, `[CFG-STRUCT] strategy=legacy` ×153,
  0 `CFG-STRUCT-FAIL`, 0 spirv-val failures, ~30-32 fps (unchanged).** No repeat of attempt 5's device loss.

**SIZE IS NOT THE METRIC — this corrects session 2.** The ledger attributed the `5,432 GCN dwords -> 166,690
SPIR-V words = 30.7x` expansion to the dispatcher. It is not: the **structured** module is the same size
(164,996 vs 166,203). The 30x is **wave64 lane-pair duplication**, not control flow. The dispatcher's cost is
the 161-case `OpSwitch` in a per-pixel loop where divergent lanes never reconverge — an algorithmic blowup in
wave utilisation, invisible to word count. The new test therefore asserts `OpSwitch == 0`, not a size bound.
A size assertion was written first and **failed**, which is what surfaced this.

**COMPILE TIME (asked directly).** `ShaderCompile: ... ms=` is the recompiler; `GfxPipeline: ms=` is the
driver. Per übershader: **structurize ~83 ms + SPIR-V emit ~50 ms ≈ 135 ms**, so **~400 ms for the three**,
once, at first draw. Driver pipeline creation is negligible (menu max **5.7 ms**) — the second is ours, not
NVIDIA's. **The fix does not add to this: baseline legacy took the same ~83 ms and then FAILED**, after which
DispatcherFull ran on top. Measured both ways by stashing the change.
- **A 4x structurize speedup exists and was deliberately NOT taken.** Moving the terminal-split retry
  *before* the routing loop (which copies and re-structurizes the whole graph once per block, ~190 times)
  gives **83 ms -> 20 ms**. It regresses three tests that assert a terminal epilogue is *not* duplicated when
  routing can repair the shape without duplication (`...AlternatingSharedReturns`,
  `...NestedEarlyExitSharedTerminal`, `...OverlappingEarlyExitLadder`). Duplicating code in shaders that
  already work, to save 250 ms of one-off compile, is the wrong trade. A gate that picks the split early only
  for the shared-terminal failure shape would get both; unproven, and probably does not separate those three.

**HARNESS: `shader_cfg_tests` now reports ALL failures instead of aborting on the first.** `Check` throws
`CheckFailed`; every test in `main` runs through `RUN(...)`, failures are collected and printed as a summary,
exit code 1. This paid for itself immediately — the retry-reorder experiment above showed all three
regressions in one run instead of one per rebuild.

**NEW DIAGNOSTIC: `KYTY_SKIP_PS_HASH=<hex>[,...]` (uncommitted, `renderDraw.cpp`, default off).** Drops the
`vkCmdDraw*` for draws binding those pixel shaders and *keeps every bit of host CPU work* (pipeline,
descriptors, render targets) — so the delta is GPU time only. Purpose: the "3 shaders = 79% of the frame"
claim rests on per-draw timestamp brackets, which is **attribution, not causation** — begin/end timestamps
around a draw on a pipelined GPU can still absorb work from other draws in flight, and this ledger has
already shipped one wrong number from exactly that (the 96% wave64 figure). Not issuing the draws is the
causal control, and it gives the **upper bound on what any fix to these shaders can ever be worth**.
- Decision table for the next fight session: fix-run fps jumps → done. Fix flat + skip jumps → the shaders
  are the cost but the structured version is still slow. **Fix flat + skip flat → the 79% attribution is
  wrong, stop working on these shaders.** The fix-run alone cannot separate the last two.
- **Mechanism verified on menu shaders** (the ubershaders never compile at the menu): skipping
  `0x69dadabfa1cfd7db,0x0e28dfb7b7c0b292,0xba0aad8a10da19a9` took them from 5.1-7.2 us/draw to
  **0.0 us/draw** in `GpuDraws`, `watched=12 draws 0.00ms`, no crash, no device loss. The env parsing and the
  skip path both work, so a fight run cannot be wasted on a typo.
- The frame is **not correct** with this set — draws are missing. Measurement tool only.

**STABILITY: 5m40s menu soak on the fixed build, `--shader-validation true`.** Survived (stopped by the
harness, not a crash); **0 crashes, 0 `ErrorDeviceLost`, 0 Vulkan validation errors, 0 spirv-val failures,
153/153 `strategy=legacy`, 0 `CFG-STRUCT-FAIL`**, fps flat at 34-36 for the whole run. Repeated after the
`KYTY_SKIP_PS_HASH` change with the flag unset: identical. **Caveat: none of the three ubershaders compile at
the menu** (grepped all three hashes, 0 hits) — this proves no regression in the 153 shaders the menu uses
and that the build is stable, NOT that the ubershaders execute correctly on the GPU.

### MEASURED IN-FIGHT: 1.0 -> 2.9 fps. The mechanism was right.

Log `KytyLog-PPSA03541-fight-fix.txt` (fix on, xfer off), real fight, HUD correct, round still black.

| measure | baseline (session 2) | after | change |
|---|---|---|---|
| `ps=0x1bcc68ffb7b0469e` per draw | **21,157 us** | **95-262 us** | **~80-220x** |
| graphics GPU / frame | 847 ms | 137-191 ms | ~5x |
| total GPU busy / frame | 924 ms | 240-325 ms | ~3x |
| **GPU time per draw** | **236 us** | **14-19 us** | **~13-16x** |
| strategy census | 645 legacy, 3 dispatch | **672 legacy, 0 dispatch** | — |
| device losses / spirv-val | — | **0 / 0** | — |
| fps | ~1.0 heavy scenes | **2.9** | — |

`GpuBusy` windows are exactly one frame (`NoteFrame`: Arm -> Recording -> Reading across two boundaries), so
these are directly comparable. **Per-draw GPU time is the honest headline** — the fix run's scene carried
7,434-10,044 draws/frame vs the baseline's ~3,700, so the graphics-time and fps rows *understate* the win.

**THE GPU IS NO LONGER THE WALL.** 187 ms busy in a 366 ms frame (~51%). `process_ms ≈ wall` again, for real
this time. Every remaining lever is CPU-side or shape.

**`op0x16` COLLAPSED ON ITS OWN — and this kills the `vkCmdDispatchIndirect` project.**
`IndirectArgs: total=10.8ms slow=5 slow_total=8.5ms` vs session 2's `total=1666ms slow=17` at ~98 ms each.
Per-call `op0x16` **11-22 ms -> 1.5-1.7 ms**. Session 2 hypothesised the stall was the CPU blocking on a
saturated GPU rather than a PM4 problem; **confirmed**. The remaining `op0x16` cost is dispatch recording,
not the arg read. The depth-alias-sampling blocker no longer needs solving *for this reason* (it is still a
real bug).

### FAILED: `KYTY_XFER_QUEUE=1` re-tested in-fight and is NEUTRAL. Do not retry.

Log `KytyLog-PPSA03541-fight-xfer.txt`. It did everything it claims — `XferReadback on_transfer_queue=256
(100.0%)`, `finish` count 56 -> 15, **`finish_ms` 85 -> 17 ms/frame, `gc_ms` 46 -> 10 ms/frame, ~103 ms/frame
removed**. And the frame did not get faster.

| | finish | gc | draws/frame | ms/frame | **us per draw** |
|---|---|---|---|---|---|
| xfer off | 85 | 46 | 2,478 | 344 | **139** |
| xfer on | 17 | 10 | 2,657 | 366 | **138** |

**COUNTER-READING TRAP (cost this session an hour of wrong emphasis): every `FrameProfile` field accumulates
over the whole window and is reset together; `frames=N` is the window length.** So `draws=7971 frames=3` is
**2,657 draws/frame, not 7,971**. Same for every `_ms` field. Divide by `frames` before quoting anything.
`GpuBusy`/`GpuDraws` are different - those windows are exactly one frame (see `NoteFrame`).

**Identical per draw.** The apparent fps drop (2.9 -> 2.7) is a 7% heavier scene, not a regression. The 103 ms
came back in compute dispatch recording — `Pm4Ops` same window: `op0x15 (IT_DISPATCH_DIRECT)` **0.112 ->
0.146 ms per call (+31%)**, `op0x16` 1.58 -> 1.71, while `op0x27` (draws) was **unchanged per call**
(0.0780 -> 0.0782). This is the ledger's own warning firing verbatim ("with the transfer queue on, that GPU
wait hides inside `op0x16`/`cp_rest`") and the same "time relocated, it did not vanish" that killed
`KYTY_DEFER_READBACK`. **Method note: the 443 -> 35 ms `finish_ms` figure from session 2 was read as a win.
It is not one. Quote fps normalised by draw count, never a bucket ratio.**

### THE WALL NOW: `drawprep` 60%, at ~138 us of host CPU PER DRAW

Per frame at 2.7-2.9 fps (~366 ms, **~2,650 draws**). These buckets nest and double-count — do not sum them.

| bucket | ms/frame | share |
|---|---|---|
| **drawprep** | **220** | **60%** |
| ├ draw recording (`DrawPhase bindings` 27-30 us/draw) | 92 | 25% |
| └ getprog / materialize | 73 | 20% |
| `cp_rest` | 85 | 23% |
| dispatch | 39 | 11% |
| finish (xfer on) | 17 | 5% |
| gc (xfer on) | 10 | 3% |

Sub-counters: `BindPhase findbuffers=10.4-11.0us rebind=5.2-5.5us textures=2.5us`;
`Materialize snapshot=16.7us/call`; `SrtEval sources=9.75us srtreads=4.97us cfg=1.24us`.

### PER-DRAW COST CHAIN, MEASURED THE SAME WAY EACH TIME (in-fight medians, us of wall per draw)

**Method that made these comparable:** fps alone is useless here - scenes vary 2,400-4,100 draws/frame, so
two runs of the "same" fight differ by 70%. Take every `FrameProfile`, divide by `frames`, drop the first
third (warm-up) and frames under 1,500 draws (menu), and quote the **median us/draw**. A single sample said
the GC change was worth 36%; the median said 14%. **Always use the median over the run, never one line.**

| run | getprog | finish | gc | **total us/draw** |
|---|---|---|---|---|
| ubershader CFG fix only | 28.0 | 35.0 | 17.5 | **141.9** |
| + SRT flat evaluator | **19.3** | 31.3 | 18.5 | **140.2** |

**The only solid per-draw win beyond the ubershader fix is `getprog` 28.0 -> 19.3 (-31%), and it did NOT
show end-to-end.** A third row claiming 105.4 us/draw from the GC threshold change was withdrawn - it was
measured before that run reached steady state; see the GC section below. **The 1.0 -> 2.9 fps from the
ubershader fix remains the session's real result.**

### SRT FLAT EVALUATOR (`KYTY_SRT_LINEAR=1`, uncommitted, default OFF) - bucket win real, frame win NOT

Compiles the SRT value DAG once per shader into a topologically ordered op array
(`ResourcePlan::srt_linear`, `LinearCompiler` in `SrtWalker.cpp`); per draw it is a flat loop over a slot
array - no recursion, no `unordered_map` find + `insert_or_assign` per node, no visiting-stack linear scan,
and memoisation falls out free because each `Inst` appears once.
- **Isolated cost: 14.5-15.2us -> 2.9-3.0us per call (~5x).** Coverage 57-78% of calls depending on scene
  (gated to programs with no CFG-conditional sources and no clean/specialization slots).
- **`getprog` 28.0 -> 19.3 us/draw (-31%)**, over 4,000+ samples. The bucket moved exactly as predicted.
- **BUT end-to-end it was ~flat: 141.9 -> 140.2 us/draw.** A projection of "~46ms/frame = 11%" was made from
  the isolated per-call saving and **was wrong** - the frame is not bound by that bucket alone. **Size a
  change by the total us/draw before and after, never by its own bucket.**
- Correctness: `KYTY_SRT_LINEAR=verify` runs both evaluators and compares. **0 mismatches over ~573,000
  calls across 579 shader hashes, in-fight.** All three unit suites green with the path on and off.
- **TRAP THAT PRODUCED A FALSE CLEAN: the verify comparison shared a bug with the code it was checking.**
  `DescriptorSource::dwords` is `std::array<Value, 8>` (image/sampler descriptors are 8 dwords); both the
  flat path and the comparison used `d < 4`, so it validated only the half that was correct and reported
  `mismatched=0`. The truncation surfaced instead as `unsupported storage texture ... dwords=...,00000000,
  00000000,00000000,00000000` at `descriptors.cpp:406`. **A validator that shares an assumption with the
  code under test cannot check that assumption.**
- Second bug, caught by `TestSrtWalkerRealSmemTranslation` before it ever ran in-game: the raw-read address
  aligns `base`, the `memory_info` immediate and the runtime offset **each** down to 4 bytes independently -
  not sum-then-align.
- Wiring gotcha: `ExtractResourcePlan` rebuilds every `Inst` into its own `value_storage`, so a program
  compiled against the source `Program`'s pointers is useless to the extracted copy. It must be compiled on
  the object that is evaluated per draw - hence `BuildSrtLinearProgram()` called at the end of
  `ExtractResourcePlan`. Getting this wrong showed up as `SrtLinear: calls=0`.

### THE GC WAS DOING SPECULATIVE READBACK IT DID NOT NEED TO - the session's clearest win

`RunGarbageCollector` skips dirty buffers entirely unless `aggressive`
(`m_total_used_memory >= m_critical_gc_memory`); in aggressive mode it downloads **every GPU-dirty range of
the whole buffer** - 10 MB in one copy for `0x113d000000`. New `BufferGc/128` counter:
```
BufferGc/128: aggressive=128/128 used=5287MB trigger=2611MB critical=5222MB
```
**Every GC run was aggressive, and `used` was sitting ~1% (30-65 MB) over `critical`, permanently.** A
knife-edge: 65 MB the other way and none of that download traffic happens.
- **RAISING THE THRESHOLD IS A TRANSIENT, NOT A FIX. VRAM DOES NOT PLATEAU - it climbs until it reaches
  whatever `critical` is set to, then aggressive GC pins it there.** Full-run evidence:

  | critical | VRAM first -> last | aggressive windows |
  |---|---|---|
  | 5222 (default) | 4,439 -> 5,760 MB | 183/246 (74%) |
  | **6400** | 4,427 -> **6,496 MB** | **1,928/2,547 (76%)** |
  | 6000 | 4,410 -> 6,237 MB | 290/555 (52%) |

  The fraction of a run spent below the threshold is a function of **run length**, not threshold value -
  a longer run spends more of itself at equilibrium. Steady state always pays.
- **A -25% figure was published here and then withdrawn.** It came from reading `aggressive=0/128` and
  `total=105.4 us/draw` **early in the 6400 run**, while the run was still filling VRAM; the same run's
  full-length median is **127.6 us/draw** with 76% aggressive windows. The early window was not
  representative. This is the exact trap documented two sections above - **take the median over the whole
  run, and check the run actually reached steady state before quoting it.**
- What IS established: `gc` us/draw tracks the aggressive fraction (3.7 at 52% aggressive, 14.0-16.6 at
  74-76%), so the download really is the cost. The lever is just not the threshold.
- **Correctness is unaffected either way:** below the threshold the GC *keeps* dirty buffers resident
  instead of downloading-and-evicting them; genuine guest reads still fault and download on demand through
  `ReadMemoryOnGpu`. The aggressive path downloads buffers the guest may never read, purely to evict them.
### ROOT CAUSE FOUND AND MEASURED: the texture GC can never evict a GPU-modified TILED image

**It is not the buffer cache.** `BufferGc/128` census: the buffer cache holds a **flat ~500-540 MB** with no
trend (`dirty 27/439MB clean 451/97MB`) while total device memory climbs to `critical` and pins there.
`GetDeviceMemoryUsage()` is **total VMA, shared with the texture cache** - so `used` rising was never
evidence that buffers were growing, which this ledger had been assuming.

**`TexGc/128` with skip-reason counters names it exactly:**
```
used=6245MB critical=5222MB | images=4471 deleted=0/128runs
cand=7680 skip: unreg=0 tiled=7680 unpressured=0 dlfail=0
```
**100% of LRU candidates are GPU-modified + safe-to-download + tiled, and tiled images are skipped**
(`textureCache.cpp` `if (safe && owner->info.IsTiled()) continue;` - correct in itself: downloading one
would write detiled data into guest memory). The texture GC therefore **deletes literally zero images**
while sitting 1 GB over its critical threshold holding 4,471 images.

**The chain, end to end:** textures can never be evicted -> total VRAM pins at `critical` ->
`BufferCache::RunGarbageCollector` sees critical pressure permanently -> `aggressive=128/128` -> it
downloads dirty buffers forever, **in response to pressure it does not own and cannot relieve.**

**TWO FIXES TRIED, BOTH NET REGRESSIONS. Flag-gated, default OFF, do not enable without re-measuring.**

| variant | total us/draw | draw | finish | gc | fps |
|---|---|---|---|---|---|
| baseline | **133.7** | 37.9 | 32.9 | 17.5 | 2.65-2.83 |
| `KYTY_TEXGC_AGE_FIX=1` + budget | 153.3 | **60.7** | 29.9 | 21.3 | - |
| `KYTY_TEXGC_BUDGET_FIX=1` alone | **342.6** | 79.8 | 33.8 | 0.6 | 1.72-1.82 |

1. **Age-order inversion** (`aggressive ? 16 : pressured ? 80 : 160`, reversing the original). It *worked*
   as intended - `deleted` 0 -> 200-1,185/128 runs, images 4,500 -> 500-1,200, `used` 6,250 -> ~5,200,
   and **`BufferGc aggressive` fell 128/128 -> 12-30/128 on its own**, confirming the chain above. But
   `draw` went **37.9 -> 60.7 us/draw**: it evicts images last used ~16 ticks ≈ 0.1 frames ago, i.e. the
   current frame, and the game immediately re-uploads them. **The original `160` is deliberate anti-thrash,
   not a bug.** The working set really is ~4,500 images.
2. **Budget accounting alone** - scan past unevictable candidates (`scan_limit = deletions*16`) and charge
   the deletion budget only for an image actually freed. Logically correct, catastrophic in practice:
   `GcSplit texgc` **3.0ms -> 50.8-71.2ms** per 512 GC calls. **The GC runs ~175x per FRAME**, so a
   16x-deeper scan means ~650 candidates x 175 runs = ~114k `SafeToDownload()` calls per frame.

**THE FIX, AND IT IS THE SESSION'S SECOND BIG WIN: gate `BufferCache` on the memory it actually owns.**
`RunGarbageCollector` now requires **both** total pressure **and** this cache holding a material share of
the budget (25%, ~1.3 GB) before going aggressive. It holds ~500-760 MB, so it stays non-aggressive and
stops downloading dirty buffers entirely. `KYTY_BUFGC_OWN_SHARE=0` restores the old behaviour.

**Measured, median over the whole run (n=138), not a single window:**

| | before | after | |
|---|---|---|---|
| **total** | 133.7 | **83.8 us/draw** | **-37%** |
| `finish` | 32.9 | 14.9 | -55% |
| `gc` | 17.5 | 4.4 | -75% |
| `draw` | 37.9 | 22.4 | -41% |
| **fps median** | **2.725** | **4.615** | **+69%** |

User observed **5.0-5.4 fps consistently** in-fight. `aggressive=0/128`, `downloaded=0MB`, 0 device losses.
`draw` falling 41% was not predicted - fewer GPU drains means less stalling inside draw recording too.
- Implementation note: the cache's footprint is a **running counter** maintained in the symmetric
  `ChangeRegister<insert>` hook, NOT a walk of `m_slot_buffers`. The GC runs ~175x/frame; walking it there
  is exactly what made the texture-GC budget fix 20x worse.
- A drift check compares the counter against the full walk each census window. **Test it with a tolerance,
  not equality** - the walk skips `is_deleted` buffers while the counter tracks register/unregister, so
  they legitimately differ by bytes in flight. Exact equality produced 769 false warnings.
- **WATCH: the buffer cache grew ~500 -> ~760 MB and plateaued** (dirty ~658 MB), and total VRAM now
  oscillates **6,463-6,782 MB against a 6,528 MB budget** - i.e. at or slightly over. Not fatal here (no
  device loss, no allocation failure, fps stable) but it is the thing this change could break in a heavier
  scene. If it bites, lower `KYTY_BUFGC_OWN_SHARE` so the cache goes aggressive sooner.

This was rejected earlier in the session as "hacking around the root cause". The root cause is now measured
and it is **not** in the buffer cache - so removing a false signal was the principled fix after all.
- If the texture GC is ever revisited: **the ~175 GC runs per frame is the thing to fix first.** At once
  per frame a deep scan would be affordable.
- Still unexplained and worth a look: why are ~4,500 images resident and all of them GPU-modified+tiled?
- Ruled out by reading the code, not guessing: the demand path clips to a 512 KiB window
  (`RangeSet::ForEachIntersection` does clip), and `AppendSmallDirtyDownloads` caps seeds at 64 KiB and
  extra at 4 MiB. Neither can emit a 10 MB copy. Only the GC path can.

### SETTLED BY MEASUREMENT: GPU CULLING CANNOT REDUCE THE DRAW COUNT. Wave64 Stage 2 is a CORRECTNESS fix only.

The plan "un-stub the occlusion CS -> most draws disappear -> fps" is **dead**. Three independent lines:
1. **190 sampled `BufferDownload` addresses: ZERO fall inside either visibility buffer's address range**
   (`buffer[9]` 0x1163920000+18,874,368; `buffer[10]` 0x1170776e00+37,748,736).
2. **The new `BufferDlCensus/256` bounds it.** 8th-place cutoff is 53.4 MB of readback in a ~13-frame window.
   For `buffer[9]` (18.9 MB/read) to hide below that it could have been read **at most 2x**; `buffer[10]`
   (37.7 MB) **at most 1x**. A guest deciding draws from visibility reads it *every frame* - tens of reads.
   By comparison `0x113d000000` shows **164x**.
3. **99.9% of draws are direct** (below). Even for an indirect draw the host still records it, so the host-side
   count would not drop regardless.

**The guest CPU cannot act on data it never reads.** The visibility output is consumed GPU-side - `0x1163920000`
is bound as `CS texture[3/4/6]`, read-only sampled, 1068x600, i.e. a Hi-Z pyramid for other compute passes.
- This is a **bound, not a proof of zero**. But it is far below what a per-frame dependency requires.
- **Wave64 Stage 2 remains the fix for the BLACK ROUND (issue #0/#1) and should still be done for that.**
  Do not expect fps from it. Budget it as correctness work.
- New counter `BufferDlCensus/256` (`bufferCache.cpp`, ungated): accumulates bytes+count per download address
  over *every* copy and dumps the top 8. **Gotcha: the first version gated on
  `Config::GraphicsDebugDumpEnabled()`, which needs `--graphics-debug-dump` that no other counter requires, so
  it printed nothing.** The pre-existing `BufferDownload #N` line only logs `copies.front()` and samples
  1-in-64, and batches carry 3-16 copies, so it cannot answer "is address X ever read back".
- **NEXT LEAD (chase after the per-draw work): `0x113d000000 = 164x / 1,679,360 KB` in one census window**
  = one 10 MB buffer re-downloaded **~12x per frame, ~120 MB/frame**. The ledger already named this buffer in
  2026-09-10. Re-downloading the same 10 MB a dozen times a frame smells like missing dirty-tracking, and each
  one is a stall. Small investigation, possibly a bug rather than a cost.

**THE DRAWS ARE ~99.9% DIRECT, WHICH UNDERMINES THE "FEWER DRAWS" PLAN.** `Pm4Ops` opcode mix in-fight:
```
op0x27 IT_DRAW_INDEX_2      15,498   (direct indexed)
op0x2d IT_DRAW_INDEX_AUTO      369   (direct)
op0x24 IT_DRAW_INDIRECT         21   <- twenty-one
```
The guest CPU issues essentially every draw itself. So "un-stub the occlusion CS and most draws disappear"
is **an assumption, not a measurement** — it requires the guest's own draw decisions to depend on CS output
it reads back, and the buffer sizes alone do not show that. The ledger's description of `buffer[9]`/
`buffer[10]` as "indirect-draw data" is contradicted by 21 indirect draws per window. **Do not start
multi-day wave64 Stage 2 work on this justification until the dependency is proven.**

**THE REAL NUMBER IS ~138 us OF HOST CPU PER DRAW.** That is the anomaly: shadPS4 runs the same engine family
at 30-58 fps on ONE core with no threaded rasterizer, so its per-draw cost must be single-digit us. Kyty is
~20-50x off, and 60% of it is `drawprep`. **Cost per draw, not count of draws, is where the multiple is.**
The two sub-targets are already measured and already designed:
- `getprog`/materialize 73 ms/frame — the **flat/bytecode linearisation of the SRT value DAG** (designed in
  detail above; NOT the Xbyak JIT, which buys ~4%). Sized at ~44 ms/frame.
- draw recording 92 ms/frame — `DrawPhase bindings` 27-30 us/draw, of which `findbuffers` 10.4 us and
  `rebind` 5.2 us. Descriptor reuse across draws has never been seriously attempted.

## Replicating the 5 fps configuration — START HERE

```
D:\PS5\tools\run_ufc5.bat                 # the 5 fps configuration
D:\PS5\tools\run_ufc5.bat -Baseline       # optional wins OFF, for A/B
D:\PS5\tools\run_ufc5.bat -Validate       # + --shader-validation
D:\PS5\tools\run_ufc5.bat -Verify         # SRT equivalence check (slow, fps meaningless)

python D:\PS5\tools\frame_stats.py D:\PS5\KytyLog-PPSA03541-run.txt
```
`run_ufc5.ps1` is the real script; the `.bat` is a wrapper. It kills any running
emulator first, clears every env var known to be neutral or a regression (so a value left in
your shell cannot silently change the run), and prints what it set.

**Measured 2026-09-11: `fps median 5.00`, `TOTAL 79.0 us/draw` over 137 in-fight samples**
(baseline the same day: `fps median 2.69`, `137.7 us/draw`).

**What is ON, and why:**

| setting | why |
|---|---|
| `KYTY_SKIP_CS_HASH=0xea0aceac518ec52d` | **Required to reach a fight at all.** The occlusion CS is wave64 and TDRs on a wave32-only RTX 3070. This is why the round renders **BLACK** — expected, not a regression. |
| `KYTY_GPU_TIMESTAMPS=1` | Real GPU timing. The 3 ubershader hashes are already the default watch list. |
| `KYTY_SRT_LINEAR=1` | Flat SRT evaluator. `getprog` -31%; verified equivalent (0 mismatches / 573k calls). |
| *(no env var)* | The two big wins are **in the code, always on**: the ubershader structurizer fix, and BufferCache no longer GC'ing against memory it does not own. |

**What is deliberately OFF — all measured, do not re-enable without re-measuring:**

| setting | result |
|---|---|
| `KYTY_XFER_QUEUE` | **Neutral.** `finish_ms` 85 -> 17 but identical us/draw; time relocates into dispatch recording. |
| `KYTY_GC_CRITICAL_MB` | **Transient.** VRAM climbs to whatever line you set, then pins there. |
| `KYTY_TEXGC_AGE_FIX` | **+14 us/draw** of texture re-upload thrash. The original age of 160 is deliberate. |
| `KYTY_TEXGC_BUDGET_FIX` | **`texgc` 3ms -> 71ms**, fps 2.7 -> 1.8. The GC runs ~175x per frame. |
| `KYTY_BUFGC_OWN_SHARE=0` | Restores the pre-fix BufferCache behaviour (i.e. undoes a 37% win). |
| `KYTY_SKIP_PS_HASH` | Diagnostic only — drops draws, so the frame is incomplete. |

**HOW TO READ A RUN — this is not optional.** `fps` from a single `FrameProfile` line is
worthless: scenes vary 2,400-4,100 draws/frame, so two runs of the "same" fight differ by 70%.
**Use `frame_stats.py`: it normalises by draws, drops the first third as warm-up, and takes the
median.** Two changes were called wins this session and withdrawn — the transfer queue (judged
on a bucket ratio) and the GC threshold (judged on an early window before steady state). Both
were caught by this method. Also: every `FrameProfile` field accumulates over `frames=N`, so
`draws=7971 frames=3` is **2,657 draws/frame**, not 7,971. `GpuBusy`/`GpuDraws` windows *are*
one frame.

**`--printf-direction File` is not optional either** — `printf_direction` defaults to `Silent`
and `graphics_debug_dump_enabled()` keys off it, so without it every counter is silently
discarded and the log looks fine but contains no measurements. The script always passes it.

### THE BLACK ROUND IS A COMPOSITE/PRESENT BUG. THE SCENE RENDERS FINE. (2026-09-11)

**The fight scene is fully rendered every frame and never reaches the scanout.**

```
frame 1703, in-fight, screen is black:
  rt68360000  0x1168360000  1920x1080 fmt=122 (B10G11R11_UFLOAT)  nonzero 2,044,948/2,073,600 = 98.6%
  present     0x111a800000  1920x1080                              nonzero   177,654/2,073,600 =  8.6%  (HUD only)
```
Brightness distribution of that HDR buffer (8.5% black, 28% 1-3, 22% 4-15, **37% 16-63**, 4% 64-191,
0.2% 192+) matches a known-good scene, and an ASCII render of it shows obvious structure - a lit figure,
floor, gradients. It is a real image, not noise.

**HOW THIS WAS MISSED FOR DAYS, AND THE LESSON.** `swapchain.cpp`'s surface dump used a **hardcoded
address list** that never contained `0x1168360000`; it contained the neighbours `0x1168270000` and
`0x1168260000`. So every in-fight dump found only unrelated buffers, and this ledger's
*"scene RT `0x1162c00000` collapses to 400x225 empty"* was measuring a **quarter-res buffer that reuses
that address** (1600x900 / 4 = 400x225). That one observation was the entire evidence for "the stubbed
occlusion CS starves the fight", and it was an artifact of looking at the wrong surface.
**A hardcoded list of addresses in a diagnostic is a trap: it silently answers a different question.**
`KYTY_DUMP_SURFACES=<hex>[,...]` now overrides it, and the in-fight targets are in the default list.

**How to find the right surfaces:** `WatchedDrawTarget` (`renderDraw.cpp`, rate-limited to 48 lines, keyed
off the shared `IsWatchedDrawPixelShader` watch list) logs the bound colour attachments for the ubershader
draws. In-fight they render to `0x1168360000` (1920x1080) and `0x1166f40000` (1600x904), both fmt=122.

**WHAT THIS INVALIDATES:**
- **The black round is NOT blocked on the occlusion CS, therefore NOT on wave64 Stage 2.** Stage 2's whole
  justification was "the only way to make the round render". That is now false.
- The ubershaders are not doing expensive work for nothing - they are drawing **the actual scene**.
- The buffer-zeroing experiment below was aimed at the wrong layer entirely.

**NEXT: why does the game's tonemap/composite pass not get `0x1168360000` into the scanout?** That is in
the presentation path - code this fork has already worked in - not in wave64 emulation.

### FAILED, AND IT CLOSES OPTION (a) FOR THE BLACK ROUND: zeroing the occlusion CS's two big buffers

`KYTY_STUB_CLEAR_BUFFERS=1` (`renderCompute.cpp`, default OFF, kept only so this is not retried).
Extends the existing stub — which already clears the CS's written *images* to 0 — to also zero its
written *buffers*, i.e. `0x1163920000` (18 MB) and `0x1170776e00` (36 MB).

**Hypothesis, and why it looked good:** the image clear uses "0 = nothing occludes, reverse-Z far
plane", and that is exactly what made the intro and corner scenes render. The long-standing objection
to filling the buffers — "sizes don't reveal the layout; filling blind risks a GPU hang from a wrong
value in an indirect-args buffer" — had been weakened by measurement this session: `0x1163920000` is
bound as **`CS texture[3/4/6]`, read-only sampled, 1068x600** (a Hi-Z pyramid consumed by *other*
compute shaders, not draw args), and the whole frame issues only **~21 `IT_DRAW_INDIRECT`**.

**Result: refuted, and a regression.** `cleared_images=2 cleared_buffers=2` over 450 firings, so it
did clear exactly the two intended buffers. The fight round **stayed black**, and the **HUD became
corrupted** (health bars scrambled; clean with the flag off). The walkout still rendered.

**What this establishes:** those buffers carry structured state the rest of the frame consumes, not a
visibility mask that a constant satisfies. Zero is not an "all visible" value, and nothing suggests
another single constant would be. **Option (a) in "Next steps" — extend the stub to fill the buffers —
is closed.** The black round needs the real CS running, i.e. wave64 Stage 2, or a hand-written
replacement of the occlusion algorithm.
- Cost note if anything like this is tried again: the fill ran per stub firing, 54 MB x 450 = ~24 GB
  of `vkCmdFillBuffer` in one session. Any future experiment here should fill once, not per dispatch.

### Upstream sync — what was hand-woven in, and the trap next to it (2026-09-11)

**Policy: do NOT merge or cherry-pick from upstream. Hand-apply what is relevant.** Our branch has
diverged across **46 files that upstream also changed** — `ShaderCFG.cpp` (ours +995/-26 vs theirs
+48/-87, refactors that *delete* code we build on), `spirvEmitterMemory.cpp` (+104/-111 rewritten),
`ShaderRecompilerComputeTests.cpp` (theirs +1278/-101). A rebase is a large, risky change and there is
no reason to take it while the 5 fps is this fresh.

**Woven in (commit `aad25ff`), all measured NEUTRAL:**
- Wave-wide `VCCZ`/`EXECZ` **operand** reads (`Translator::MaskIsZero`). They test whether the whole
  mask word is zero, not the current lane's bit. Upstream fixed one site (`1af19ea`, `ReadRawU32`);
  **our tree had the same bug in the U1 operand path too**, so both are fixed.
- `S_ORN2_SAVEEXEC_B32` (upstream `32ea086`) and `S_WQM_B32` (`c354657`) — we had B64 only.
- Neutral confirmed by fair A/B, same build, validation off, comparable scenes (2,439 vs 2,536
  draws/frame): **76.2 vs 79.0 us/draw**, buckets within noise. Expected: `unsupported=0` before and
  after means no UFC5 shader used either opcode, and `MaskIsZero` only fires on a *data operand* read.
- **Method note: an earlier reading of 90.3 us/draw for these was `--shader-validation` plus a 47%
  heavier scene.** Always A/B with the same flags and check `draws/frame` before believing a delta.

**DO NOT "FIX" `AddBranchCondition` (`Translate.cpp` ~line 801).** It branches `S_CBRANCH_EXECZ`/`VCCZ`
on the invocation-local EXEC/VCC boolean, which looks like exactly the per-lane bug fixed above. It is
**deliberate** — the comment above it says so: Kyty models each lane as its own invocation, so branching
on the lane's own bit lets inactive invocations leave the region without reconstructing a host-subgroup
mask. Upstream's merged fix does not touch this site either; PR #470, which would, is **open and
conflicting**. Changing it is a semantic change to the hottest control-flow path and needs its own
measurement. This was nearly "fixed" on the assumption it was the same bug.

**Upstream work worth revisiting later (inspected 2026-09-11, none pulled):**
- Wave64/EXEC: merged `c913951` (RDNA2 subvector loop mask/branch semantics); open **#470** (model EXEC
  and VCC as subgroup ballots) — the right foundation for Stage 2, but conflicts with our Stage 1
  `ThreadBit` constant-folding. Upstream's `ThreadBit` is the wave-wide `(word >> lane) & 1` form using
  `program.wave_size`; ours folds compile-time-constant masks using `current_wave_size`. **Ours is an
  optimisation that belongs on top of theirs**, not an alternative.
- **Duplicated work:** upstream `e04007a` implements `DS_INC_RTN_U32`/`DS_DEC_RTN_U32`, which this fork
  implemented independently across 5 files. Theirs ships 228 lines of tests. Adopting theirs and
  dropping ours would cut divergence in files that already conflict.
- Perf PRs in our exact area, unevaluated: **#506** (FPS overhead in GPU scheduling and resource
  caching), **#473** (query host readability once per region during the SRT walk), **#562** (BDA upload
  from CPU-dirty hints), **#537** (publish linear storage images before CPU access).
- Possibly relevant to open correctness bugs: `a305a6c`/`fde550e`/`23e7df2` (shared texture channel
  layouts and **swizzle decoding** — hypotheses 2 and 3 for the skin corruption, issue 4b);
  `0b4e78c` + PR **#545** (DB_RENDER_OVERRIDE depth/stencil copies, stencil sync — our depth-alias bugs).
- **DO NOT PULL #483** (skip GPU sync when unmapping non-GPU memory): already tried here, **livelocked**
  the guest CPU on a stale value. #484 (SRT scratch reuse) is already in the tree as `a1fc490`.

## FPS profiling (2026-09-10) — read this before more perf work

`FrameProfile:` now carries `process_ms` (whole `GuestGpu::Process(submission)` on the GPU worker thread — the single thread that does PM4 decode + Vulkan record + stalls + GC), `drawprep_ms` (`RenderExecutor::DrawIndex/DrawAuto` incl. pipeline lookup + `AcquireRenderTargets` + descriptor resolve + `hw_check`; nests `draw_ms`), `gc_ms`, `faultbuf_ms`, `flush_ms`, `sendcmd_ms`, `cp_rest` (= `process_ms` − drawprep − dispatch − submit − gc − flush − sendcmd = **raw PM4 decode + SET_*_REG / WAIT / EVENT handlers + per-`Process()` boilerplate**), and `process`/`gc`/`faultbuf` counts. `FlushAndWait` now counts as `Finish`.

**GPU worker thread is 100% saturated** (`process_ms/frame` ≈ wall) both on menu and in-match — the bottleneck is CPU-side command processing, not GPU or guest-thread idle.

After the EOP-wait skip (`aa794cd`), remaining in-match cost (heavy close-up scene, ~8-9k draws/frame, ~1.9 fps): **`drawprep_ms` ≈ 400 ms/frame is now the wall** (30 µs/draw prep × 8-9k), then `draw_ms` ~120, `dispatch_ms` ~40, `finish_ms` ~25, `cp_rest` ~45, `gc_ms` ~4. A second in-match regime (~7.4k draws, ~1.1 fps) still shows `finish_ms` ~450 ms/frame — residual **cross-queue** WAIT_REG_MEM (async-compute label, graphics waits) that the intra-buffer skip can't touch.

Menu (~33 fps now): `drawprep_ms` ~11 ms/frame, `cp_rest` ~5, rest small.

### FPS work log (2026-09-10, continued)

- **`aa794cd` intra-command-buffer EOP-wait skip.** `WAIT_REG_MEM` on a label an EOP earlier in the *same* command buffer writes no longer does `BufferFlushAndWait()` (submit + full CPU-blocking GPU idle); GPU command order already serialises it. **Menu ~24 → ~33 fps.** In-match: removed one wall, revealed the next.
- **`e46defd` cross-queue EOP-wait skip.** Pending-label map moved from per-CommandProcessor to file-static (shared across graphics + async-compute CPs). A graphics `WAIT_REG_MEM` on an async-compute EOP label: `PopPendingOperations()` to drain retired submissions, re-test the actual guest value; if the write landed, skip. **One in-match scene ~1.1 → ~2.1 fps.** `cp_rest` ~435 → ~50 ms/frame; the stall cost relocated into `gc_ms`/`finish_ms`.
- **`0811b71` cached hot-path getenv/config polls** (`GraphicsRunDebugDumpEnabled`, `graphics_debug_dump_enabled`, `EnvListContainsHash`). Neutral in-fight, small menu help.
- **`hw_check` — measured ~0 ms.** The `drawprep_ms` cost is `getprog_ms` (`PrepareProgram`: shader-map lookup + heap alloc + resource-decl parse, VS+PS) ~100-130 ms/frame in-match. A "same as last draw" memo (register-struct + context memcmp) was **0.02 % hit rate in a fight** (per-object data goes through shader SGPR user_data) — reverted. Menu-only, dropped.
- **THE REMAINING WALL: synchronous `bufdl` buffer-download `Finish`.** `SchedFinish/200: bufdl=200/2740ms` = 100 % buffer-download stalls. ~17/frame, ~13.6 ms each (full GPU idle to drain a buffer's producing work before the CPU copies it back). Guest CPU reading GPU-computed data (a recurring 10 MB buffer at `0x113d000000` ×105, a 4-byte counter cluster `0x1140008204` ×397, etc.). **~230-550 ms/frame.**

### 2026-09-10 late session — root-caused the device losses, and the readback wall fell

**`431672f` USE-AFTER-FREE ON GC'd BUFFERS — this was causing every `ErrorDeviceLost`.**
`BufferCache::RunGarbageCollector()` retired dirty buffers with an immediate
`m_slot_buffers.erase(id)` (destroying the `VkBuffer`) directly after
`DownloadBufferMemory()` had recorded GPU->staging copies against those same buffers into
the still-recording command buffer. Validation says it exactly:
`vkCmdPipelineBarrier(): ... VkBuffer 0x... was destroyed` /
`VUID-vkCmdPipelineBarrier-commandBuffer-recording`, then the submit returns
`ErrorDeviceLost`. `DeleteBuffer()` already retires via `CommandScheduler::DeferOperation`;
the GC path just did not use it. Fixed by doing the same.
- 5 device losses across 6 sessions before; **0 "was destroyed" errors and no device loss after.**
- **Pre-existing upstream bug, not caused by this session's work.** The synchronous readback
  path masked it: `Finish()` drained and `BeginNext()` opened a fresh command buffer before
  the erase. `KYTY_DEFER_READBACK` removes that accidental drain, so it became frequent.
  **This fix is a prerequisite for deferred readback being safe.**
- **Method note: three hypotheses were wrong** (`DiscardMemory`/unmap-discard, deferred
  readback itself, the stubbed CS) — each disproven by experiment. `--vulkan-validation true`
  found it in one run. For intermittent device loss, go to validation immediately.

**`KYTY_DEFER_READBACK=all` — the `bufdl` wall is gone.** `finish_ms` **450 -> 8-20 ms/frame**
(56x); `finish` count 24/frame -> ~10. Contradicts the shadPS4 prior above (prefetch fails)
because this is not a prefetch: the GPU->staging copy still rides the demand fault, only the
staging->guest writeback is deferred a frame.
- **But fps only moved 2.20 -> 2.40 at ~9k draws/frame (+8%).** The time relocated, it did not
  vanish. Quote the fps, not the `finish_ms` ratio.
- Not yet default. Needs `431672f` first (it does).

**Corrected frame budget (in-match, ~8k draws/frame, ~2.4 fps, deferral on):**
`drawprep_ms` ~300 ms/frame is the wall. Per draw: **getprog/materialize 17.2 us (45%)**,
draw recording 13.7 us (36%), rtresolve 1.4, other 5.5. `finish` now ~2% of frame.
- **Earlier "80% CPU / readback only 24%" claim was from a light 2.4 fps scene and was wrong
  for a real fight** (there it was 62% readback). Always sample a real fight frame.
- `gc_ms` is NOT a lever — `GcSplit/512: fault=0.0 dlimg=0.1 texgc=0.0 bufgc=0.0`. The
  apparent 192 ms was `Finish` nested inside GC being double-counted.

**`MaterializeResources` — measured to the sub-phase.** It re-walks the SRT graph from guest
memory on *every* draw even on a full program-cache hit. `ProgLookup/8192` shows the rest of
the cache-hit path is free: `key=0.1ms materialize=117ms permscan=0.0ms`.
`Materialize/8192: snapshot=15us specialize=0.4us avgbuf=7 avgimg=1 avgsmp=0.4` — the cost is
fixed per call, NOT proportional to the ~8 bound resources.
`SrtEval/8192: setup=0.15us cfg=2.22us sources=7.89us srtreads=4.55us | avg_cfg=42 avg_srtreads=64 avg_srcs=8.1`
- **CFG BFS (15%) is prunable** — walks all ~42 blocks every draw.
- **`srt_reads` (31%) is NOT prunable by dependency closure** (the idea suggested externally):
  `flattened_srt` is uploaded as a GPU buffer and indexed *dynamically* by the shader
  (`spirvEmitterMemory.cpp:1129`), so the host cannot know statically which entries are read.
  **Safe variant instead:** shaders with no `IR::DescriptorBindingKind::FlattenedSrt` binding
  never read that buffer — for those the whole 4.5 us table build is dead. Needs a per-entry
  flag in `ProgramCache::SourceEntry`; measure what fraction of UFC5 shaders qualify first.
- `sources` (54%) is irreducible — that IS the bound-descriptor evaluation.

**Instrumentation added this session** (all gated behind `--printf-direction`, cheap counters):
`FinishSplit/200` (submit/gpuwait/post — proved 91% of `Finish` is real GPU execution, not
bookkeeping), `GcSplit/512`, `ProgLookup/8192`, `Materialize/8192`, `SrtEval/8192`,
`SchedFlushAndWait/200`. Keep these; they are what made the above findable.

**GOTCHA THAT COST TWO BOOTS: `LOGF` goes nowhere by default.** `printf_direction` defaults to
`Silent` and `graphics_debug_dump_enabled()` keys off it, so every counter is silently
disabled. **Always launch with `--printf-direction File --printf-output-file <path>`.**
Shell-redirecting stdout only captures `::printf`, not `LOGF`, and is block-buffered.

Still uncommitted / unproven: `DiscardMemory` + unmap fast-path (`bufferCache`,
`gpuResourceManager`) — suspected of the crashes, disproven, and never shown to help. Lean
toward reverting.

Logs: `KytyLog-PPSA03541-valid.txt` (validation caught the UAF), `-uaffix-val.txt` (clean
after fix), `-deferall.txt` / `-uaffix.txt` (deferral), `-srteval.txt`, `-finsplit.txt`.

### 2026-09-11 — transfer-queue readback landed; parallelism plan and its blocker

**`847e20f` READBACK NOW RUNS ON A DEDICATED TRANSFER QUEUE.** The GPU->staging copy used to
be recorded into the graphics command buffer, so waiting for it drained every draw queued
ahead of it (`FinishSplit` proved 91% of a `Finish()` was `m_master.Wait` - real GPU
execution, ~8ms each, ~24/frame). Now:
- device creation enumerates *all* queue families (it used to `return` on the first match, so
  nothing else was visible). RTX 3070 exposes family 1 `{Transfer|SparseBinding}` = a real DMA
  engine; falls back to a second queue of the universal family, then to the old path.
- buffers become `SharingMode::eConcurrent` across the two families (one place:
  `Buffer::Buffer`), avoiding ownership-transfer barriers.
- `Buffer::NoteGpuWrite(CurrentTick())` in `ObtainBuffer(is_written)` gives each buffer its
  producer tick.
- `CommandScheduler::SubmitTransferReadback` waits on the *graphics timeline at the producer
  tick*, signals its own timeline; the CPU waits only on that.
- **`finish_ms` 443 -> 35 ms/frame, `finish` 47 -> 10 per 2 frames, WITHOUT deferral** - so
  guest memory stays current, no 1-frame staleness. `XferReadback` shows a **100% hit rate**:
  the producing submission had essentially always been submitted already.
- Opt-in behind `KYTY_XFER_QUEUE=1`; every failure path falls back. Not default yet.
- **Design note: neither half works alone.** Producer-tick waiting alone is useless (single
  in-order queue - the copy is recorded *after* the draws, so you cannot wait for it without
  waiting for them). A transfer queue alone is useless (it would still wait on "all graphics
  work so far"). Only the pair works.
- Graphics corruption in-match is unchanged by this (confirmed by the user) - it is the
  pre-existing issue, not a readback regression.

**`b8e0bdd` per-phase counters** - `Pm4Ops/262144`, `ProgLookup/8192`, `Materialize/8192`,
`SrtEval/8192`, `DrawPhase/8192` (+ `GcSplit`, `FinishSplit`, `SchedFlushAndWait`,
`XferReadback`). Attribution by subtraction (`cp_rest`) sent this work down two wrong paths;
measure directly.

**Measured in-match budget, ~2.7k draws/frame, transfer queue on: 184 us of host CPU PER
DRAW.**

| bucket | ms/frame | us/draw |
|---|---|---|
| draw recording | 144 | 53 |
| getprog (MaterializeResources) | 96 | 35 |
| cp_rest (PM4 decode + reg handlers) | 117 | 43 |
| dispatch | 60 | 22 |
| finish (readback) | 35 | 13 |
| gc + rest | 48 | 18 |

`DrawPhase/8192: bindings=12.3us vbuf=1.2us rendertargets=1.4us pipeline=0.6us` - vertex
buffers, render targets and the pipeline-cache lookup are all fine. Descriptor work dominates:
~12us in `PrepareGraphicsBindings` plus ~38us after it (`CommitBindings`/`CommitVertexBuffers`).
**Descriptors ~50us + materialize ~35us = 46% of all CPU time, both recomputed from scratch
every draw.**

`MaterializeResources` mechanism (confirmed, still unfixed): every **4-byte** descriptor dword
read goes through `TryReadGpuCleanBacking`, which calls `TextureCache::IsRegionGpuModified`
(takes a lock, runs `FindImagesInRegion`, allocates a vector) and `HasGpuDirtyBytes`. ~48 such
reads per draw per stage. **A per-read page memo was tried and REVERTED** - it regressed
(12.2 -> 19.4us on identical menu shaders), because memoising inside the per-read callback pays
TLS + scan costs 48x to avoid a cost that should be *hoisted out* of the read path. The right
fix is one clean/dirty check per descriptor range, or use the existing
`read_specialization_block` batch reader. That is a `SrtWalker` change, not a `memory.cpp` one.

**THE BLOCKER FOR ANY PARALLELISM: `BufferCache` has ZERO synchronisation.** Not one mutex. It
is safe only because exactly one thread touches it, and it owns the page table, LRU, memory
tracker and GPU-dirty range sets - every draw hits it several times (`ObtainBuffer`,
`FindBuffer`, `SynchronizeBuffer`). By contrast `TextureCache` has `TrackingSpinLock m_lock`,
`PipelineCache`/`SamplerCache` have mutexes, and the CP context globals
(`g_current_processor`, `g_current_execution`, `g_gpu_thread`) are already `thread_local`.

`process_ms` has equalled wall time in **every** sample, menu and in-match: one GPU worker
thread does all PM4 decode, draw prep and Vulkan recording; 7 cores idle; the GPU is mostly
idle waiting for work. The GPU is not the saturated resource - the CPU thread feeding it is.

**Parallelism plan, and why it is ordered this way:**
0. **Reduce per-draw work first** (descriptor reuse, materialize). ~46% of CPU, no
   architectural risk, and it shrinks whatever has to be parallelised later. Parallelising
   redundant work just burns 8 cores on the same waste.
1. Async-compute queue on its own thread - **not free**, compute dispatches bind buffers too,
   so it also needs a thread-safe `BufferCache`.
2. Pipeline decode vs record - **gains little**: the decode thread must keep all cache access,
   so the record thread only gets the cheap `vkCmd*` calls.
3. Parallel draw preparation into secondary command buffers - **where the 5x is, prerequisite
   is thread-safe `BufferCache`**, the largest and riskiest change in the hot path. Every draw
   touches it several times, so it may end up contention-bound well under the theoretical 8x.

Realistic ceiling: eliminating *both* known hot spots entirely is ~3 fps, not 10. 10 fps needs
stage 3.

### 2026-09-11 (later) — fourth failure, and the tools that were there all along

4. **Per-slot flat cache in the evaluator** (`EvaluateSlot`: evaluate an SRT slot once, then
   read it back by index - the direct analogue of shadPS4's `flattened_ud_buf`/`ReadUdSharp`).
   **Regression: `Materialize snapshot` 15-16 -> 19.5-24.4us, `SrtEval sources` 7.9 -> 9.7-13.5us.**
   Reverted.
   **Why:** `EvaluatorScratch::cache` already memoises by `Inst*`, and a `ReadConst` node *is*
   an `Inst`, so a repeat hits `m_cache.find` and returns before reaching the slot cache. It
   only helps when *different* `ReadConst` nodes share a slot, which is rare here. Added bounds
   checks + stamp compare + store to a path that already short-circuited.

**Four optimisation attempts, four failures, one repeated error: assuming a mechanism is
expensive without confirming the branch is hot.** Three of the four were already
short-circuiting or already memoised. Do not add another cache to this function without a
profile first.

**TOOLS THAT WERE ALREADY IN THE TREE AND WENT UNUSED:**
- **Tracy is vendored and built** (`3rdparty/tracy`, `_Build/windows/3rdparty/tracy`), the hot
  functions already carry `KYTY_PROFILER_FUNCTION()` (`PrepareBindings`, `FindBuffers`,
  `RebindBuffers`, `CommitBindings`), and `--profiler-direction Network` starts the client.
  **Use this before writing another counter.** Needs the Tracy server GUI to connect.
- **Xbyak AND Zydis are already fetched** (`3rdparty/CMakeLists.txt`, `FetchContent_MakeAvailable(xbyak zydis)`)
  and **Kyty already JITs x86 with Xbyak in `src/loader/redZonePatcher.cpp`**
  (`Xbyak::CodeGenerator patch_gen/trampoline_gen`, plus `<Zydis/Zydis.h>` for decoding).
  An earlier note in this ledger said Xbyak was not vendored - **that was wrong**. Both
  dependencies shadPS4's SRT walker needs are present, with a working in-tree example of
  emitting and installing generated code. The JIT port is therefore much smaller than assumed;
  shadPS4's `flatten_extended_userdata_pass.cpp` is 328 lines to follow.
  - Note their JIT chases raw pointer chains that can fault, handled by a signal handler that
    decodes the faulting insn with Zydis and self-patches `mov rdi,[rdi+off]` -> `xor rdi,rdi`.
    On Windows that is a vectored exception handler. Kyty's interpreter gets this free via
    `TryReadBacking` returning false.

Suggested order from here: **profile with Tracy first**, then port the walker JIT if the
profile confirms interpretation overhead.

### 2026-09-11 (session 2) — the real in-fight wall is ONE PM4 OPCODE, and the CPU/GPU split

**`cp_rest` is not "PM4 decode + reg handlers". In a heavy fight it is `IT_DISPATCH_INDIRECT` (op 0x16).**
`Pm4Ops` in-fight: `op0x16=2314-3323ms / 136-204 calls` = **11-22 ms PER CALL**, ~30 calls/frame =
**~420-545 ms/frame, ~45-50% of the frame** - more than all ~3,500 draws combined (op0x27 ≈ 345 ms/frame).
It is invisible to every bucket: not `Finish`, not `dispatch`, not `gc`, so it lands in `cp_rest`.

**Mechanism, proved not assumed.** `CommandProcessor::DispatchIndirect` (`graphicsRun.cpp:1272`)
dereferences the GPU-written arg block straight out of guest memory. New `IndirectArgs/128` counter
times *only* that dereference: `total=1666ms slow=17 slow_total=1663ms` - **17 of 128 reads block, ~98 ms
each**; the other 111 cost ~27 us total. Per-call average matches `op0x16` exactly (13 vs 14.2 ms), so
the dereference IS the opcode cost. `EnsureCurrentForCpu` is a no-op (deferred readback off) and
`DispatchDirect`'s unscoped prologue (`CheckBuffer`) is innocent - both were candidates, both cleared.

**CPU or GPU? Both, ~50/50, and serialised.** `FinishSplit/200: submit=7.7ms gpuwait=2392.1ms post=674.3ms`
- **78% of a blocking Finish is real GPU execution.** Frame at ~1,100 ms: **~630 ms CPU busy / ~470 ms
blocked on GPU**. `process_ms ≈ wall` was read as "CPU-bound, GPU idle" - wrong: a thread blocked in
`vkWaitSemaphore` is still inside `Process()`. Both sides are ~50-100x too slow (a 3070 should do 3,500
draws + 500 dispatches in <10 ms). **Consequence: the entire per-draw track (SRT JIT, bda, descriptors)
has a hard floor at the GPU's ~470 ms ≈ 2.1 fps.** Past that it must be GPU-side, i.e. wave64→wave32.

**Transfer-queue A/B (same fight, same binary).** `op0x16` per call 13.8-14.2 (on) vs 11.3-13.6 (off) -
**the transfer queue does NOT cause or amplify this stall**; an early hypothesis that `847e20f` was
responsible was wrong. What it *does* do is confirmed strongly: `finish` 25→4/frame, `finish_ms`
603→5, `gc_ms` 100→24. The indirect-arg stall is immune because its producer is the compute shader
that just ran (the µs lead-time pattern); the wait is genuine GPU execution, not scheduling slack.
With the queue off the stall appears as `finish_ms`, with it on as `cp_rest`. Same cost, different column.

**`PrepareBda` - the `findbuffers` bucket was mismeasured.** The `BindPhase` timer at
`descriptors.cpp:1075` spans `FindBuffers` ×2 **plus `PrepareBda()`**, which walks every mapped range ×
every buffer in the cache (`ForEachUploadRange` = spinlock + bitmap scan per tracker region) on every
draw whose shader has `uses_dma`. New `BdaSplit` counter: in-fight `bda=8-9us/draw, bda_draws≈1000/8192`
(12% of draws, ~75 us/call) = **~33 ms/frame, ~2%**; menu ~1 ms/call but ~0.5 calls/frame. Real but small.
`findbuffers` proper is **0.7-0.9 us**, `clamp` ~0.15 us/call - so `ClampRangeSize`'s global kernel
mutex and `FindBuffer` are both innocent, contrary to the earlier note. Upstream shadPS4 `origin/main`
has the identical unguarded per-draw sweep; there is no `!fault_process_pending` guard to port.

**`vkCmdDispatchIndirect` - implemented behind `KYTY_INDIRECT_DISPATCH=1`, DEFAULT OFF, BLOCKED.**
Kyty uses **zero** Vulkan indirect commands anywhere in `host_gpu/` - every indirect draw and dispatch
is CPU-converted. shadPS4 issues `cmdbuf.dispatchIndirect` / `drawIndexedIndirect` / `drawIndirectCount`
(`vk_rasterizer.cpp:1364-1377,1497`) and never reads the args. Cache buffers already carry
`eIndirectBuffer` usage (`streamBuffer.h:33`, `bufferCache.cpp:779`) and the existing post-dispatch
`ShaderAccessBarrier` (dst `eMemoryRead` at `eAllCommands`) already covers `eIndirectCommandRead`, so
neither staging nor a new barrier is needed. Legal only when `mode & 0x20` (USE_THREAD_DIMENSIONS) is
clear - that bit folds the counts into the compute shader's specialization key.
- **BLOCKER: the host still binds descriptors for dispatches that turn out to be zero-group.** The
  zero-check (`renderCompute.cpp:579`) currently skips binding entirely for GPU-driven passes that
  resolve to "nothing to do"; the indirect path cannot know. CS `0x45cde74926a779f8` logs
  `groups=0x1x1` (**thread_group_x = 0**) and is skipped for free today. Going indirect binds its 15
  textures and dies: `invalid image view: image_format=130 view_format=43 aspect=0x1` - a **D32_SFLOAT_S8_UINT
  image sampled through an R8G8B8A8_SRGB descriptor**. Pre-existing latent bug, same family as issue #3
  but on the compute *sampling* path, only ever masked by that lucky early-out.
- Narrowing the gate to "only when the read would actually stall"
  (`HasGpuDirtyBytes(args_addr, 12)`, ~12-14% of calls carrying ~99% of the cost) **did not help** -
  that dispatch's args are GPU-dirty too. Kept anyway: strictly better than the broad gate.
- **FAILED FIX, REVERTED: `ResolveDepthOverlap` Texture rule.** Adding
  `recreate |= cached.info.IsDepth() && !requested.IsDepth()` (the rule Storage/RenderTarget/VideoOut
  already have) fixes the view crash but replaces images that are simultaneously the draw's bound depth
  target → `depth target changed after render-state discovery` (`renderDraw.cpp:553`) on the next draw.
  Texture bindings legitimately alias the live depth target - that is what depth-feedback support is for.
  Comment left in place at the site so it is not retried.
- **Next idea, untested:** promote per-CS-hash - first indirect dispatch of a hash takes the direct path
  and records whether its counts were non-zero; only hashes known to do real work go indirect. Would
  spare `0x45cde74926a779f8` (zero both times it appears per run). Still a heuristic; a
  sometimes-zero shader would reopen the hole. The principled fix is the depth-alias-sampling bug.

**Instrumentation added (keep):** `IndirectArgs/128`, `IndirectEligible/128`, `BdaSplit/8192`
(bda / bda_draws / clamp / lookup / descriptors-per-draw), `SrtShape/8192`. Cheap counters, gated like the rest.

**WAVE64 IS NOT THE PERFORMANCE CEILING - MEASURED, AND IT OVERTURNS THIS LEDGER'S ASSUMPTION.**
`GpuDispatchTime` (real GPU timestamps, `KYTY_GPU_TIMESTAMPS=1`, two per dispatch) in-fight:
`total=13.9-47.6ms over 281-954 dispatches | wave64=3.4-40.6ms (24-85%)`.
**Total GPU compute is 14-48 ms/frame in a ~1,000 ms frame = 1.4-4.8%. Wave64 compute is 0.3-4%.**
Stage 2 is worth **at most ~4%**, not the ceiling. It remains the fix for the **black fight round**
(correctness, unaffected) - do it for that, but it is the *weakest* remaining perf lever, not the
strongest. The earlier "~470 ms/frame of GPU time, wave64 is the ceiling" reasoning in this ledger was
inference from CPU wait times plus the 62% wave64 *invocation* share, and it was wrong.
- **METHOD WARNING that nearly shipped a 96% figure.** The first implementation wrote ONE timestamp per
  dispatch and diffed consecutive ones. That measures **gap + execution**: GPU idle waiting for the CPU
  gets charged to whichever dispatch preceded it, and since >90% of dispatches are wave64, wave64
  collected the idle by construction. It reported `total=1078ms, wave64=96%` **for a frame that takes
  550-620 ms** - the frame-time contradiction is the only thing that caught it. Always bracket each
  region with its own begin/end pair, and always sanity-check a GPU total against wall time.
- Implementation: `GpuDispatchTimer` in `renderCompute.cpp`, `KYTY_GPU_TIMESTAMPS=1`, default off.
  Uses `hostQueryReset` (added to `RequiredVulkan12Features`) so the pool is reset from the host only
  while nothing references it - avoids ordering `vkCmdResetQueryPool` against the command buffers the
  scheduler opens and closes mid-frame, which is the lifetime-bug class that caused the device losses.
  Results polled without the WAIT bit; a frame that is not ready simply retries.

**CONFIRMED WITH EXACT SIZING: 3 SHADERS OF 648 OWN ~79% OF THE FRAME.**
Watched-shader brackets (always on for the 3 ubershader hashes, not sampled):
```
graphics=847.1ms of 924.3ms total GPU busy
ps=0x1bcc68ffb7b0469e  719.33ms over 34 draws (21156.7us EACH)
ps=0xb808b3887f76ff0c    6.72ms over  2 draws ( 3360.8us each)
ps=0x0000000000000000    0.32ms over 344 draws (   0.9us each)   <- healthy
```
A single draw was measured at **408.70 ms**. Strategy census the same run: **645 legacy, 3 dispatch**.
Ubershaders = 726 ms of 847 ms graphics = **86% of graphics, ~79% of the whole frame, from 0.5% of
the shaders.** Menu control: 6 us/draw.

**MECHANISM, read straight out of `PS_1bcc68ffb7b0469e.cfg.txt`:**
```
block_1  condition=dispatch_done  merge=412  continue=411  loop_header=1
block_2  successors=[3..163,410]  indirect_targets=[3..163]  selector_values=[3..163]
```
i.e. `loop { switch (state) { 161 cases } if (dispatch_done) break; }` - every pixel runs a 161-case
switch inside a loop, one iteration per basic block visited. SPIR-V expansion **5,432 GCN dwords ->
166,690 words = 30.7x**. Three compounding costs, the third being the 5,000x:
1. no structured control flow - the driver cannot unroll or reconverge;
2. register pressure - every value live across a state transition survives the switch, so occupancy
   collapses and there is no latency hiding;
3. **divergence never reconverges** - lanes sit in different states, so each iteration the wave
   executes the union of every case any lane is in. With 161 cases that is an algorithmic blowup in
   wave utilisation, not a constant-factor codegen loss.

**THE FIX IS NOT "improve DispatcherFull"** - the dispatcher is inherently this shape. It is to stop
these three shaders needing it. This ledger already records exactly why Legacy fails on them:
`block %82 exits the selection headed by %80, but not via a structured exit` - a fall-through block
jumping to a grandparent selection merge, past the discard-pad merges of two enclosing selections.
**One specific edge pattern**, and Legacy already has related machinery (shared-merge splits, clone
retry for external selection tails / the 16->177 shape). Teach it to handle a fall-through targeting a
grandparent merge (clone the tail, or thread it through the intervening merges), with
`ValidateStructuredExits` + `spirv-val` as the correctness gate and `tests/shaderCfgTests.cpp` as the
harness. **`ValidateStructuredExits` stays** - it is what stops the invalid-SPIR-V GPU hangs; we are
removing the *need* for the fallback, not the safety net.

**THE CFG TEST SUITE IS GREEN AGAIN, AND IT ALREADY CONTAINS THE UBERSHADER.**
`shader_cfg_tests` loads `_Shaders\cfg_fail\PS_1bcc68ffb7b0469e.bin` (the real 5,432-word shader),
runs BOTH strategies and prints the failure reason:
`UFC PS strategy compare: legacy=0 blocks=193 reason=block 18 exits the construct headed by 16 to 19
without a structured exit (merge 192) | dispatch=1 blocks=413 cases=161 synthetic=252`.
**The whole problem is iterable in-process in seconds - no emulator, no navigating into a fight.**
That was the dominant cost on this all day. `TestUfc5PsDispatcherFullOracle`, tests/shaderCfgTests.cpp.
- Fixed two **stale** tests that pinned `mode=dispatcher` for patterns legacy now handles, so they
  failed as an *improvement* and (harness aborts on first failure) hid every later test:
  `...NestedLoopNonlocalExitDispatcher` and the mixed continue/nonmerge case. Both now assert the
  property that matters - **valid SPIR-V** - plus mode/control-flow consistency.
  The other three `mode=dispatcher` assertions were LEFT ALONE: irreducible CFGs and `S_SETPC_B64`
  jump tables genuinely cannot be structured, so pinning the dispatcher there stays correct.
- Learned: **a structured module may legitimately contain `OpSwitch`** - `RouteSharedSelectionArm`
  introduces routing variables that lower to one. "structured implies switch-free" is false.
- **TODO: make the harness continue after a failed `Check` and report all failures.** It aborts on the
  first one, so a regression elsewhere hides the UFC result you are trying to measure.

**THE CFG SHAPE AT THE FAILURE (dumped from the FAILED graph, not a fresh BuildGraph - the ids in the
reason refer to the post-split 193-block graph; 177 and 192 are synthetic and absent from the
161-block original):**
```
block_16  preds=[11]      succs=[17,192]  cond: true=192 false=17   doms=[0,11,16]
block_17  preds=[16]      succs=[18,191]  cond: true=191 false=18   doms=[0,11,16,17]
block_18  preds=[17]      succs=[19]                                doms=[0,11,16,17,18]
block_19  preds=[15,18]   succs=[20,57]                             doms=[0,11,19]  <- NOT dom by 16
block_192 preds=[16,191]  succs=[]        Return
```
Block 16 is `if (c) return; else ...`. `FindSelectionMerge` returns the nearest common post-dominator,
which is **192, the function's return block**, stretching the construct to the function exit. Block 19
is a join shared with block 15 (reached under block 11 by a different path), so `18 -> 19` is an exit
from 16's construct that is neither the merge nor a loop break/continue.

**FAILED ATTEMPT 6, REVERTED - widen the terminal-merge gate.** `FindSelectionMerge` ALREADY has the
right special cases, commented *"a return can leave a selection without reaching its merge"*
(ShaderCFG.cpp:1334-1343), but they are gated on `global_merge == UINT32_MAX` so they never fire when
a merge IS found and simply *is* a return block. **That gate at :1325 is the gap.** Widening it to also
fire when the merge block is terminal (`successors.empty()`) **regressed a phi test**
("consecutive typed Phis were not emitted as two native OpPhi instructions") - merge placement and phi
emission are coupled - and the harness aborted before reaching the UFC oracle, so it is still unknown
whether it fixes the ubershader. Reverted; suite green.
- **The real constraint: a fix must move the merge WITHOUT disturbing phi placement.** That likely
  means choosing the tighter merge *only* when the wider one would produce an illegal exit -
  information `FindSelectionMerge` does not currently have, since it runs before
  `ValidateStructuredExits`. Consider computing the exit check first, or retrying with a tighter
  merge only for constructs the validator rejects.

**THE EXACT STRUCTURIZER FAILURE (same for all 3 ubershaders):**
```
blocks=193 loops=12 failure=StructuredControlFlow block=18
reason=block 18 exits the construct headed by 16 to 19 without a structured exit (merge 192)
first_pass=selection header block 13 has externally entered region block 177
           preds=[13,15,16] merge=14 true=177 false=14
```
`FindSelectionMerge(16)` returns **192** of 193 blocks - the arms only reconverge at the function end -
and block 18 inside that construct branches to 19, which is neither the merge nor dominated by 16.

**FAILED ATTEMPT 5, REVERTED - dominance-based external-entry test.** `SplitOneSelectionMerge` repairs
regions defined by **reachability** (`SelectionRegion`, ShaderCFG.cpp:1571 - walks successors from the
arms to the merge) while `ValidateStructuredExits` validates regions defined by **dominance**
(`DominatedBlocks`, :1050). Those are not equivalent, so "no external entry into the reachable region"
does not imply "no unstructured exit from the dominated region". Added `!graph.Dominates(block_id,
member)` to the external-entry test at :1811 so non-dominated members get a private clone.
**Result: the 3 ubershaders STILL routed to dispatcher (`578 legacy, 3 dispatch`), AND it caused
`ErrorDeviceLost` on a COMPUTE dispatch** (`debug_op=0 args=32768,1,1,97`) - the extra cloning
miscompiled some *other* shader into invalid SPIR-V. Reverted; menu back to 30 fps.
- The inconsistency is real, but it is **not** what blocks these shaders. The blocker is upstream:
  either `SplitExternalSelectionEntry` declines this shape, or the 16->19 exit survives cloning.
- **`ValidateStructuredExits` did not catch the bad module it produced** - the validator is necessary
  but not sufficient. **Run any structurizer experiment with `--shader-validation true`** so spirv-val
  names the offending shader at compile time instead of learning about it from a device loss.
- **The CFG test suite is RED and cannot gate this work.** `shader_cfg_tests` fails at
  `TestNewShaderRecompilerCfgNestedLoopNonlocalExitDispatcher` ("did not select dispatcher fallback") -
  a **stale** test asserting the dispatcher should win a pattern Legacy now handles. Pre-existing
  (confirmed via `git status`), and the harness stops at the first failure so everything after it is
  unverified. **Fix that test first** - no structurizer change is safe without this net.
- Next attempt needs the actual CFG shape, not the error string: dump the pre-structurization graph
  around blocks 13-19, 16, 19, 177, 192 and trace what `SplitExternalSelectionEntry` decides for each.

**Superseded first-pass numbers (1-in-16 sampling, kept for the method lesson):**
`GpuDraws` (sampled 1-in-16 draw brackets, `KYTY_GPU_TIMESTAMP_DRAWS`):
`sampled=483 every 16 | total=13.35ms avg=27.6us max=11.18ms`, and by pixel shader:
```
ps=0x1bcc68ffb7b0469e  11.58ms over   2 sampled draws (5788.2us each)
ps=0xb808b3887f76ff0c   0.42ms over   2 sampled draws ( 212.5us each)
ps=0x0000000000000000   0.37ms over 369 sampled draws (   1.0us each)   <- depth/shadow, healthy
```
Another frame recorded a **single draw at 37.47 ms**. Menu control: 6 us/draw.
**Both top entries are the übershaders this ledger already names** (`0x1bcc68ffb7b0469e`,
`0xb808b3887f76ff0c`, `0xf7030726b9470dd8` - 5,400+ GCN words, 161 blocks, 12 loops). The
distribution is NOT "all draws are slow": 369/483 sampled draws cost 1.0 us. It is a handful of
draws at ~5,000x the cost of a normal one.

**The chain, end to end:**
```
3 übershaders fail Legacy structurization (invalid SPIR-V -> real GPU hangs)
  -> ValidateStructuredExits routes them to DispatcherFull
    -> dispatcher codegen = a state-machine loop, per pixel, at 1080p
      -> 5.8-37 ms per draw
        -> graphics 513-746 ms/frame, GPU saturated
          -> the CPU's indirect-arg read blocks ~98 ms -> op0x16 ~50% of frame
```
`ValidateStructuredExits` was correct and necessary - it stopped real hangs. It traded a hang for a
5,000x slowdown on three shaders, and nothing measured the second half of that trade until now.
**The fix is to make Legacy structurization handle these three shapes** (or to improve DispatcherFull
codegen) - not any of the CPU-side levers. This outranks everything else in this ledger.
- Sizing caveat: per-draw costs are measured directly and solid; the *total* share is noisy because
  1-in-16 sampling on draws this rare extrapolates to anywhere from 214 ms to 1,284 ms per frame.
  **Next: always bracket draws binding those three hashes** instead of blind sampling.
- Also refuted here: "no culling -> overdraw" does not fit. Overdraw would raise cost uniformly;
  369/483 sampled draws are 1 us. And the stubbed CS makes the round *black*, i.e. draws render
  nothing - which should be fast, not slow.

**ANSWERED: THE `op0x16` STALL IS THE CPU WAITING FOR A SATURATED GPU, AND THE GPU IS BUSY DRAWING.**
`GpuBusy` brackets whole command buffers (draws included), not just dispatches:
`cmdbuffers=958.1ms over 391 buffers | dispatches=79.4ms over 1362 (wave64=71.4ms) | graphics=878.7ms`
(range across scenes: 90 -> 958 ms, matching the two regimes).
- **Graphics is 92% of GPU time; compute 8%; wave64 7.5%.**
- GPU busy **958 ms in a ~920 ms frame**, and `process_ms` is 911 ms/frame: **CPU and GPU are BOTH
  ~100% saturated at the same time.** Neither is "the" bottleneck; the `op0x16` stall is simply the CPU
  blocking on a GPU that is genuinely flat out.
- **~236 us of GPU time PER DRAW** (878 ms / 3,728 draws). A 3070 should retire a draw in single-digit
  us - this is 50-100x off and is now **the largest single item in the system**, larger than `op0x16`
  (which is mostly this seen from the CPU side), larger than `drawprep`.
- Candidate causes, compounding: (1) the **3 pixel übershaders** (5,400+ GCN words, 161 blocks, 12 loops)
  that fail Legacy structurization and fall back to **`DispatcherFull`** - a state-machine loop, which
  is catastrophic codegen for a per-pixel shader at 1080p; (2) **no culling** - the occlusion CS is
  stubbed so everything is drawn, i.e. overdraw through those shaders.
- **Next: attribute graphics GPU time per pipeline/shader** the same way dispatches were attributed
  (`GpuTimestamps::BeginDispatch/EndDispatch` generalises), and check what fraction of draws bind a
  dispatcher-structurized übershader. That names the target instead of guessing between (1) and (2).

**Two collector bugs that produced silence rather than errors** (both fixed, both worth remembering):
dropping still-open regions at the window boundary left permanently unwritten slots, and
`getQueryPoolResults` returns `eNotReady` for the **entire range** if any single query is unavailable -
so the collector wedged in Reading forever and printed nothing at all. Fixed with
`eWithAvailability` (skip holes per query) plus a poll limit so one straggler cannot stall a window.

**SUPERSEDED: what is the `op0x16` stall actually waiting for?** Frame ~1,000 ms, `process_ms` ~1,001 (worker
100% busy), `cp_rest` 502 ms (50%), `drawprep` 368 ms (37%), dispatch recording 93 ms, rest 55 ms. If GPU
compute is only ~48 ms, the ~450 ms the CPU blocks on when reading indirect args is **not** the compute
that produces them. Candidates: (1) **graphics work** - the readback drains the whole queue and the
timestamps cover dispatches ONLY; 3,700 draws through the 5,400-GCN-word pixel übershaders at 1080p are
unmeasured; (2) CPU-side cost in the fault/download path, not GPU wait at all; (3) queue/semaphore
stalls. **Next measurement: bracket whole command buffers with two timestamps** for total GPU busy
including draws; subtracting the compute figure isolates graphics. Cheap, same machinery.

**DO NOT PORT shadPS4's SRT JIT. Build a flat program in plain C++ instead.** `SrtReads/8192` splits
the walk into interpreter dispatch vs the guest reads themselves:
`SrtEval setup=0.23 cfg=0.44 sources=11.76 srtreads=1.57` (14.0us/call) vs
`SrtReads guest_read=0.55us/call reads=22.2/call ns_per_read=25`.
**The guest reads are 3.9% of the walk; 96% is interpreter dispatch.** A flat/bytecode rewrite keeps
`read_memory` and addresses the 96%. An Xbyak walker would additionally save 3.9% - **not worth a
vectored exception handler, Zydis decoding and self-patching generated code.** No Xbyak, no Zydis, no
fault machinery, no platform-specific code.
- **Why shadPS4 needed the JIT and we do not:** their walker is *already* a minimal pointer chase, so
  their residual cost genuinely is the reads (hence codegen + signal handler). Kyty's cost is a
  tree-walking interpreter layered on top. Different bottleneck; porting their fix buys ~4%.
- `avg_srtreads=22.0` vs `reads=22.2/call`: ~one guest read per SRT read and **none** from the
  `sources` evaluation - yet `sources` is 11.76us, the largest bucket. Descriptor-source evaluation
  touches no memory at all; it is pure graph walking plus `unordered_map` cache hits over values
  already resolved. The purest possible case for linearisation.
- **Design:** at ResourcePlan build time (`ResourceMaterialization.cpp:954`, where `srt_reads` and
  `descriptor_sources` are cloned) linearise the value DAG into a topologically ordered op array -
  one op per `Inst`, operands as slot indices - then per draw evaluate ops in order into a reused
  `slots` scratch. Kills traversal, hash lookups and `Arg()` recursion; memoisation falls out for free
  since each Inst appears once. Bail to the interpreter for `Phi`, `ReadFirstLane`, `ReadConstBuffer`,
  any clean_flat_slot, and anything outside the ~18 hot opcodes (98% of traffic - see SrtOps census).
  Gate behind an env flag and run both paths comparing results to prove equivalence on real workloads.
- **Validation harness now exists:** `resource_materialization_tests`, `resource_tracking_tests` and
  `scalar_provenance_tests` could not link (undefined `Common::Timer::QueryPerformanceCounter`,
  `Log::Write`) because they compile `SrtWalker.cpp` - which carries the profiling counters - but only
  linked `fmt::fmt`. Added `common` to all three. They are `EXCLUDE_FROM_ALL`, so this had been broken
  invisibly since the counters landed. `resource_materialization_tests` passes and validates this work
  without a navigation cycle.
- **Pre-existing failure now visible:** `resource_tracking_tests` fails with
  `shader resource specialization failed: storage image descriptor 1 has an invalid mip range`.
  Not caused by this session (only `SrtWalker.cpp` counters were touched among that target's sources).
  Possibly the same bug as issue #5's `vkCreateImage mipLevels exceeds extent-derived max`, with a unit
  test already written for it.

**WAVE CENSUS - `op0x16` AND WAVE64 ARE THE SAME PROBLEM FROM TWO ENDS.** `WaveCensus/16f` in-fight:
`wave64=539 disp/f (22.7M inv/f) wave32=120 disp/f (14.0M inv/f) share=61.8%`; menu `82 disp/f, 24M inv/f`.
So **~660 compute dispatches and ~36.7M invocations per frame in a fight, 62% wave64-emulated** - wave64
is in play for the compute that actually runs, not just the skipped occlusion CS. 36.7M invocations
should be single-digit ms on a 3070; heavy scenes spend ~450 ms/frame waiting on compute. ~100x off.
**The thing the CP blocks on in `op0x16` IS compute output** (the indirect dispatch args): slow wave64
compute → args arrive late → the host read of the args blocks ~98 ms → `op0x16` ≈ 50% of the heavy
frame. `vkCmdDispatchIndirect` stops the CPU *waiting*; wave64 lowering makes the GPU *finish sooner*.
They compound rather than compete.
- Caveat: 62% of *invocations* is a prior, not proof the GPU time is inside those shaders (could be the
  wave32 passes or the pixel übershaders). **GPU timestamp queries would name them - do that before Stage 2.**
- Also measured: the census sits after the early-outs and sees ~112 real dispatches/frame where
  `FrameProfile` counts ~606, so **~80% of dispatches never reach a pipeline** (meta-clear, image-clear,
  UI-mask fallback, zero-size). `dispatch_ms` ~96 ms/frame is spent by the ~112, not the 606.
- Counter gotcha: the first version logged every 64 frames. At ~1 fps that is over a minute, so in-fight
  lines still showed menu data and looked like a stuck counter. Now 16 frames, frame number stamped.
- **Two regimes confirmed, and they matter for prioritising.** ~2.1 fps scenes: CPU-bound, `op0x16` 7-18%,
  `finish_ms` ~18 ms/f. ~1.0 fps scenes: `cp_rest`/`op0x16` ~50%, i.e. **the indirect-dispatch fix matters
  most exactly where the framerate is worst.** With the transfer queue on, that GPU wait hides inside
  `op0x16`/`cp_rest`, not `finish_ms` - do not read a small `finish_ms` as "the GPU is keeping up".

**THE COST IS SPREAD - there is no single wall left.** Measured frame, 2.07 fps, 483 ms, ~2,500 draws:

| bucket | ms/frame | share |
|---|---|---|
| draw recording (`draw_ms`) | 101 | 21% |
| `cp_rest` (incl. `op0x16` ~42 in this scene) | 88 | 18% |
| `getprog` / materialize | 67 | 14% |
| dispatch | 46 | 10% |
| finish (GPU wait) | 29 | 6% |
| rtresolve / gc / submit / flush / present | 25 | 5% |

**No CPU lever exceeds ~21%.** `op0x16` was ~48% in one heavy scene but ~7-18% in others - it is spiky,
not universal. This explains the whole history in this ledger: every single optimisation has returned
~10% because **~10% is the size of the individual items.** Stacking all known CPU levers (SRT JIT,
op0x16, PrepareBda, descriptor reuse) lands near 2x total ≈ 3 fps, matching the earlier estimate.
A bigger multiple must change the *shape*: fewer draws (the occlusion CS is stubbed, so nothing is
culled), the GPU side (wave64 lowering), or parallelism (deprioritised - shadPS4 does this on one core).
The first two are both Option A.

**SRT walker JIT - scoped and GREEN, sized at ~10%.** `SrtShape/8192` in-fight:
`cfg=1890 clean=0 jittable=6302` = **77% compilable in-fight, 96% in menu**, and **`clean=0` always** -
the dual clean/specialization evaluator, one of the two things making Kyty's walker harder to compile
than shadPS4's, never executes. Sizing: materialize is 67 ms/frame, `sources+srtreads` = 86% of it,
× 77% coverage = **~44 ms/frame → 2.07 → ~2.28 fps**. Real, steady, present in every scene, but not
transformative; it costs a code generator. Insertion points:
| shadPS4 | Kyty |
|---|---|
| `GenerateSrtProgram` (compile-time x86 emit) | `BuildSrtPlan(Program&)` - `SrtWalker.cpp:1155` |
| `Info::RefreshFlatBuf()` (per draw, one native call) | `EvaluateRuntimeSourcesImpl` - `SrtWalker.cpp:1027` |
| `ReadUdSharp` (array index) | `flattened_srt` - already built, not used as the read path |
Xbyak + Zydis are fetched in `3rdparty/CMakeLists.txt`; `src/loader/redZonePatcher.cpp` already emits
and installs generated x86 with `Xbyak::CodeGenerator` - a working in-tree example of the mechanic.
**Correction to the earlier note: this is NOT a 328-line copy.** That is shadPS4's pass against *their*
IR; Kyty's `SrtWalker.cpp` is 1,214 lines with CFG-conditional source activation they do not have.
Shape: JIT the simple case, keep the interpreter as fallback for the 23%.
Also measured: `Materialize/8192 snapshot=16.5us specialize=0.5us` - `BuildResourceSpecialization` is
free, it is all the walk. `SrtEval cfg=1.53us` (13%) is not worth chasing even if halved (~1% of frame).

### 2026-09-11 — what the per-draw cost is NOT (three failed optimisations)

All three were measured and reverted. Recorded so they are not retried.

1. **Skip the flattened-SRT build for shaders with no `FlattenedSrt` binding.** Safe and
   correct, but `FlatSkip/8192` showed only **4.4%** of draws qualify - ~0.2us of a 15us call.
   Not worth the plumbing. (The broader "prune srt_reads by dependency closure" idea is
   *unsound*: `flattened_srt` is a GPU buffer the shader indexes **dynamically**
   (`spirvEmitterMemory.cpp:1129`), so the host cannot know statically which entries are read.)

2. **Memoise the GPU-clean check per page in `thread_local` storage** (inside
   `TryReadGpuCleanBacking`). Regression: identical menu shader mix went **12.2 -> 19.4us**.

3. **Same memo, but on the stack**, passed via `SrtRuntime::userdata` (already plumbed, was
   unused) to kill the TLS cost. Still a regression: **15-16 -> 16.9-20.7us**, `SrtEval
   sources` **7.9 -> 9.3-10.6us**.

**Why 2 and 3 both failed - the hypothesis was wrong, not the implementation.**
`TryReadGpuCleanBacking` only enters its expensive branch (texture-cache lock +
`FindImagesInRegion` + vector alloc) when `IsGpuAddressRange(vaddr, size)` is true. Shader
descriptor tables mostly live in ordinary guest memory, so that branch rarely runs and the
probe was already near-free. Both caches added a scan + store to an already-cheap path.
**Check whether a branch is actually taken before optimising it.**

**By elimination, the ~35us of `MaterializeResources` is the IR-graph interpretation itself,
not memory access**: `SrtEval` splits it as sources ~7.9us + srtreads ~4.6us + CFG walk ~2.2us
per call, x2 stages per draw. That is ~250ns per evaluated node - i.e. the cost of a
tree-walking interpreter. It cannot be cached away, because the inputs genuinely change every
draw (same reason the `GetGraphicsPrograms` memo hit 0.02%).

**The only approach with evidence behind it is shadPS4's: compile the walk instead of
interpreting it.** `flatten_extended_userdata_pass.cpp` emits x86 with Xbyak
(`static Xbyak::CodeGenerator g_srt_codegen(32_MB)`, `RegisterWalkerCode`) once per shader;
per draw `Info::RefreshFlatBuf()` makes **one** native call to flatten the SRT, after which
every descriptor read is `ReadUdSharp` = a plain array index. Kyty already *computes*
`flattened_srt` per draw but does not use it as the read path.

Suggested order: **(B)** restructure so the existing `flattened_srt` is the read path for
descriptor sources (no JIT, tests the premise cheaply), then **(A)** vendor Xbyak (header-only,
not currently in `3rdparty/`) and emit a native walker in `BuildSrtPlan`.

The same "per-resource cache lookup per draw" shape is the other two costs, from
`BindPhase/8192`: `ResolveTexture` **10.7us** (texture-cache lock + lookup per image) and
`FindBuffers` **9.6us** (`ClampRangeSize` + `FindBuffer` per buffer); samplers 0.4, shaderdata
0.4, rebind 1.9, commit 3.1. shadPS4 does all of this from a flat buffer + push descriptors
(Kyty already uses push descriptors where the pipeline supports them - no gap there).

**shadPS4 has NO threaded rasterizer** (no `std::thread` in `vk_rasterizer.cpp`) and reaches
30-58 fps on UFC4. A comparable emulator does this job on one core, so Kyty's problem is
constant factor, not parallelism - **deprioritise the `BufferCache` thread-safety project.**

### Readback: what the shadPS4 `ufc4-research` ledger already established (`D:\PS4\src\shadPS4`, branch `ufc4-research`)

Same engine family. Their conclusions transfer:
- **Prefetch / eager frame-boundary sweep (= `KYTY_DEFER_READBACK`'s family) does NOT work for this workload.** Lead time between GPU-marks-page and CPU-reads-it is **microseconds** — "the game reads what the GPU just produced, no window exists." Every prefetch variant delivered the broken-30fps result. Plus **amplification**: a blanket sweep moves every page the GPU *marks* vs. today only pages the CPU *reads* — multiples more data + GPU work.
- **What worked: `SHADPS4_UFC4_DEP_SUBMIT=2` — producer-aware early flush.** Stamp writable backing buffers with their scheduler producer tick; when a consumer draw/dispatch that reads *or writes* the buffer is recorded and its producer is still in the current **unsubmitted** command buffer → `EndRendering()` + `Flush()` right then. Genuine CPU read faults keep the synchronous path. The GPU work is submitted earlier → done by the time the CPU reads it → **the read doesn't fault → no synchronous download.** Result: **30 → 56-60 fps in-fight**, mostly intact geometry, occasional accepted 30 Hz dips. Became the CUSA14209 default.
  - **Fragile.** "Broad mode 2 submits far too often (~15-49k submissions/character-select). All narrower discriminators (DEPNARROW, DEPWRITESET, DEPONCE, SCANPAGE) LOST CORRECTNESS. Do not ship as a general solution." Took many failed iterations.
- Also: GETENV (one cached getenv → 43 % off draw pipeline lookup — the finding behind `0811b71`).

### FPS work log — round 3 (2026-09-10, evening)

- **`0811b71` cached hot-path getenv/config polls** — neutral in-fight, small menu help.
- **`a1fc490` ported upstream KytyPS5 PR #484** (SRT evaluator scratch reuse) — neutral on UFC5 `getprog_ms`; Kyty's per-draw shader-prep cost isn't in the SRT walker like shadPS4's. Kept as a harmless upstream port.
- **KytyPS5 PR #483 (skip GPU sync when unmapping non-GPU memory) — TRIED, LIVELOCKED.** Hit **60 fps** in-fight then the guest CPU livelocked spinning on a stale value (`handle=1 / data=0x7ec55f320` flood; `draws=0 dispatch=0`). Its author's own caveat fired: skipping `InvalidateMemory` on unmap left stale buffer-cache entries. **Reverted.** BUT it proved the point — kill the readback and it's 60 fps; the readback IS 100% of the in-match cost.
- **Owner's GC frame-retention change — TRIED, net negative, reverted.** Moved readback out of the GC (`gc_ms` 155→10) but 5×'d the per-Finish `bufdl` cost (13.7→66 ms) — delaying readbacks just concentrates them.
- **DEP_SUBMIT-style producer-aware early flush (`KYTY_DEP_SUBMIT=2`) — IMPLEMENTED, NO EFFECT ON UFC5, reverted.** Stamped every GPU-written `Buffer` with `CommandScheduler::CurrentTick()`; in `ObtainBuffer`, when a consumer (read+write, mode 2) saw its producer still in the unsubmitted command buffer → `EndRendering()`+`Flush()`. 102k flushes fired in-fight (incl. the 10 MB `0x113d000000` buffer). `finish_ms` / `bufdl` **unchanged**. Why it worked on UFC4 but not here: UFC4's producer is early-frame and the CPU read is much later, so early-submit buys a CPU/GPU overlap window; **UFC5 reads each buffer immediately after the GPU produces it** (the µs lead-time the shadPS4 ledger measured) — the flush submits the producer earlier but it's still executing when the CPU read faults. Also confirmed: **Kyty has no speculative over-reading** (no `GuestHasData`-style scan) — every one of the ~17 `bufdl`/frame is a genuine guest CPU read.

### Where the readback wall stands

`SchedFinish/200: bufdl=200/~4000ms` = 100 % buffer-download Finish, ~13-20 ms each, **~230-450 ms/frame**. It does not yield to: prefetch/defer (µs lead time), producer-early-flush (read follows write too tightly), scan removal (no scan), GC tuning (delay = concentrate). What's left is **architecture**:

1. **Async readback servicing thread** — render thread records the GPU→staging copy + a fence and moves on; a dedicated thread waits the fence and does the staging→guest memcpy. Removes the render-thread block without reducing GPU work. Multi-day, real risk, but the only remaining shape that fits UFC5's tight read-after-write pattern.
2. **Completion-gated 1-frame deferred readback** — on the CPU fault, wait only the *specific* producing tick instead of a full pipeline drain. `KYTY_DEFER_READBACK` infra is a start; shadPS4 tried 1-frame latency and got "30 fps, broken fighters" — likely a correctness dead end here too.

### Committed FPS wins this session (net)

- Menu **~22 → ~34 fps** (EOP intra-buffer skip `aa794cd` + getenv cache).
- In-match: one close-up scene **~1.1 → ~2.1** (cross-queue EOP skip `e46defd`); other scenes ~1.0-1.9. The `bufdl` readback wall is unbroken.

### Non-readback next levers (smaller)

- Draw / dispatch record time (~95 / ~55 ms/frame in-fight) — the actual Vulkan command emission.
- `getprog_ms` ~100 ms/frame — `PrepareProgram` (shader-map lookup + `ShaderGetStaticInputInfo*` parse), still uncached per draw; a shader-addr-keyed memo of the *static* input layout (not the per-draw resource snapshot) could work where the full-result memo (0.02 % hit) didn't.

Goal: get **EA Sports UFC 5** (`PPSA03541`, dump `D:\PS5\Games\UFC5`) drawing in KytyPS5. UFC-only. Real kernel / FS / GPU. Stub PSN, network, and trophies. Do not commit unless asked.

Source: `D:\PS5\src\KytyPS5`
Binary: `D:\PS5\Emulators\KytyPS5-Bin\kyty_emulator.exe`
Build: Ninja + clang-cl Release at `D:\PS5\src\KytyPS5\_Build\windows`
Helper: `D:\PS5\tools\devenv.ps1`
If devenv fails with "input line is too long", reset PATH to Machine+User and clear `INCLUDE` / `LIB` / `LIBPATH` first. Always close `kyty_emulator` before build/copy.
Incremental build is ~7 s. Deploy = copy `_Build\windows\kyty_emulator.exe` to `Emulators\KytyPS5-Bin\`.

Launch from `D:\PS5\Emulators\KytyPS5-Bin`:

```
.\kyty_emulator.exe --game "D:\PS5\Games\UFC5" --printf-direction File --printf-output-file "D:\PS5\KytyLog-PPSA03541-….txt"
```

`EXIT()` is code **321**. FPS is in `FrameProfile:` lines. Default CFG strategy is **Auto**. **F9** dumps GPU surfaces.

GPU: **NVIDIA GeForce RTX 3070**. `Vulkan subgroup: default=32 min=32 max=32 size_control=false wave64=false` — wave32-only, no way to get a 64-wide subgroup. This is the root of the hang-CS problem.

All of this work is uncommitted on `main` (`b847135-dirty`).

---

## Fixes applied

### Boot / platform

- POSIX `truncate` / `KernelTruncate` (and `KernelFtruncate` NID wiring) so the dump can size files.
- `sceNetResolverAbort`.
- NP entitlement / partner libs return signed-out instead of unresolved NIDs (`NpEntitlementAccessPft`, `NpPartner001`).
- Cap noisy `nanosleep` / equeue wait logs (first 16 only) so printf files stay usable.

### Presentation

- Swapchain / scanout / compositor path so the **title screen** actually shows in the window.
- UFC surface dumps on present; **F9** GPU dump.
- Host input: F8/F9/F11 not eaten as game keys.

### Host GPU

- Texture cache: compute image clears, DCC fill tracking, download / view work needed by UFC compute.
- Pipeline cache compile timing logs (`ShaderCompile:`, `CsPipeline:`, `GfxPipeline:`).
- Compute dispatch logging (groups, local size, buffers, textures).
- Diagnostic skip: `KYTY_SKIP_CS_HASH` (not a product fix).
- Hang-CS GDS **chunked dispatch** (hash `0xea0aceac518ec52d`): windows of `KYTY_GDS_CHUNK` (default 4) with `Finish()` between submits. **Not a playable fix.** `KYTY_GDS_LIMIT_CAP` caps items for diagnosis.
- **`FindImage` depth-as-color (2026-09-09):** when a `RenderTarget` desc resolves to a depth-associated image (`info.IsDepth()` or `depth_id` set), drop it and `InsertImage` a fresh color image instead of returning the depth image. `FindRenderTarget` fatally rejects any depth-aliased image ("color target requires rediscovery before final acquisition"), and `AcquireRenderTargets`' re-resolve guard was missing the `depth_id` case. Fixes the crash on UFC surface `0x1163740000` (a 512 KB depth surface the engine reuses as a color target). Also enriched the `FindRenderTarget` EXIT message with `registered/depth_id/needs_rebind/addr/format/tile/type`.
- **`AcquireRenderTargets` (renderDraw.cpp):** added `old_image->depth_id` to the re-resolve condition.

### Shader recompiler / SPIR-V

- **`ShaderInfoCollection` PS-input tolerance (2026-09-09):** a `GetAttribute` / `GetInterpolationParameter` that references a parameter slot `>= SPI_PS_IN_CONTROL.NUM_INTERP` no longer aborts the frame — it logs once (`PS param past NUM_INTERP`) and continues; `EmitAttribute` already yields constant 0 for an unbound slot. Still hard-fails if the index is `>= 32` (would OOB the 32-slot arrays). **Diagnostic finding:** PS `0x4c22cd8526171dbb` gets `attr=0, input_num=0` — `SPI_PS_IN_CONTROL` reads as **0** at compile time. Real bug still open (see Current issues #2).
- **`spirvEmitterFlow.cpp` null guard (2026-09-09):** `EmitInterpolationParameter` dereferenced `InputBindingForParameter(...)` without a null check (unlike `EmitAttribute`). Guarded.
- **`vulkanWindow.cpp` debug messenger (2026-09-09):** validation-layer errors are now logged as `[Vulkan][E][..][VALIDATION-ERROR]` instead of `EXIT`, so `--vulkan-validation true` runs past Kyty's own benign errors (e.g. `vkCreateFramebuffer renderPass VK_NULL_HANDLE`) and captures the full error sequence before a device-lost. DIAGNOSTIC — consider gating behind a flag before committing.
- **`textureCache.cpp` `FindTexture` packed-float alias reinterpret (2026-09-09):** the encoding-mismatch block (`!ViewEncodingCompatible && SameTexelBlockSize`) previously *always* forced `view_info.format = image.backing.format` — right for UFC's RGBA8 title compositor bound through an 11-11-10 descriptor, wrong for the reverse (GPU wrote a `B10G11R11_UFLOAT` / `E5B9G9R9_UFLOAT` HDR surface, shader binds it as `A2R10G10B10_SNORM` #98 etc.) which produced rainbow marbling on skin + garbage HUD panels. Now: when backing IS packed-float, descriptor is NOT, and `ImageViewOps::FormatsCompatible(backing, descriptor)` holds, we KEEP the descriptor format and let the mutable-format image serve a true bitcast reinterpret view. All other mismatches keep the old force-to-backing. Kill-switch: `KYTY_NO_ALIAS_REINTERPRET=1`. Log: `TextureCache: reinterpret packed-float alias: ...` vs `... sampling backing encoding: ...`.
  - **RESULT (2026-09-09, tested in-game):** fix fires as designed — `TextureCache: reinterpret packed-float alias: backing format 122 vs descriptor format 98 addr=0x0000001156990000` (the skin/probe surface), path taken ~every sample after warmup. **No regression** (menus/title unchanged) but **no visible improvement** either — rainbow skin / corruption looks identical. So a naive bitcast `B10G11R11_UFLOAT → A2R10G10B10_SNORM` view is NOT what the shader wants here; the real mismatch is elsewhere (wrong *source* image entirely, tiling/swizzle, or the surface genuinely never gets the right data written because of the stubbed occlusion CS). Fix is landed + inert; leave it (harmless, correct in principle) and look upstream next.
  - Note: a *separate* pre-existing diagnostic line `TextureCache: sampling GPU-written alias fmt=122 instead of req_fmt=98 addr=0x...1156990000` also fires at the same address from another code site — both paths touch this surface.
  - `KYTY_NO_ALIAS_REINTERPRET` getenv is now read once into a `static const bool` (was per-texture-bind per-draw).

- **Menu FPS regression — round 1 (2026-09-09):** `EnsureCurrentForCpu()` (added at the 5 indirect draw/dispatch/count sites in `graphicsRun.cpp` to protect deferred readback) unconditionally did a `ReadMemory` → `SendCommandSync` GPU round-trip + `DrainDeferredReadbacks` on **every indirect draw**. Fix: now a **no-op unless `BufferCache::DeferredReadbackEnabled()`** (`KYTY_DEFER_READBACK` set). `KYTY_DEFER_READBACK` parsing factored into cached `BufferCache::DeferMinCopyBytes()` / `DeferredReadbackEnabled()`. **Did NOT recover menu fps** (still ~21 at "press any button") — so this was real overhead but not the main cause.
- **Menu FPS regression — round 2, THE cause (2026-09-09):** `FrameProfile` on the "press any button" screen showed ~23fps / 43ms per frame but only ~13ms accounted (draw+dispatch+submit+finish+present); ~30ms/frame missing. Log analysis: **821k `cmd = 0x…` / `address = 0x…` lines + 191k `acb[N] = …` lines** — `src/libs/agc.cpp` has **~79 ungated `LOGF(...)` diagnostic dumps** (pointer/param traces), including in the hot per-frame AGC template-patch path: `AgcWaitRegMemPatchAddress`, `AgcWaitRegMemPatchReference`, `AgcQueueEndOfPipeActionPatch{Address,Data}`, `AgcCondExecPatchSet*`, `AgcJumpPatchSetTarget`, `submit_acb` (8-dword acb dump per compute submit), `AgcDriverSubmit*`, `AgcDcb/AcbDispatchIndirect`, `AgcDcbEventWrite`, `AgcDcbAcquireMem`, `AgcCbReleaseMem`, `AgcDcbCopyData`, predication setters. Frostbite pre-builds command buffers and re-patches GPU addresses **every frame** → hundreds of synchronous `LOGF`→file writes per frame on the submit thread. These are pre-existing upstream (`147188b`), not added this session — the earlier ~60fps run likely predated `--printf-direction File` being standard. **Fix: blanket `s/LOGF(/AgcTrace(/` in `agc.cpp`** (all except `AgcTrace`'s own body, line 50). `AgcTrace` already gates on `Config::GraphicsDebugDumpEnabled()` (false unless `--graphics-debug-dump true`); every `LOGF` in that file was a trace dump, none were warnings/errors. **RESULT: did NOT move menu fps** (still ~21 on the "press any button" attract screen — Kyty's logger is buffered/async, so those writes weren't on the critical path). Kept anyway (disk + log readability). The ~30 ms/frame gap is the guest thread (Frostbite genuinely rebuilds ~2700 GPU commands/frame for that real-time-lit attract screen) + ~242 vkQueueSubmit/frame. Likely the "~60 fps menu" memory was the earlier EA/Frostbite logo screens, not this one.

- **Log-spam gating round 2 (2026-09-10):** an in-match run produced a **425 MB** log (~8.7M lines). Sources, all ungated per-frame / per-audio-callback `LOGF`: `ajm.cpp` AJM decoder param dumps (~5.9M `instance = …`), `graphicsRun.cpp` `WriteReferenceClock` `copy_data reference clock` (~1.8M), `sync.cpp` `EndOfPipe Signal!!!` LOGF_COLOR (~0.9M), `audio.cpp` AudioOut param dumps (~0.96M), `faultManager.cpp` `Accessed non-GPU cached memory` (~102k). Fixes: `ajm.cpp` blanket `LOGF→AjmTrace` (new helper gated on the file's `PRINT_NAME_ENABLED`, always false there); deleted the `EndOfPipe Signal!!!` and `copy_data reference clock` debug lines; rate-limited `faultManager` to 64. `audio.cpp` left (has two `LIB_NAME` blocks, structurally awkward; 10× smaller than ajm). Built + deployed as `-quiet2`.

- **Mip-chain over-declaration clamp (2026-09-10):** the user's session added `ImageMipTrace`/`TextureMipTrace` diagnostics which caught UFC 5 creating storage mip chains **one level over the Vulkan max** (`mipLevels > floor(log2(max_dim))+1` — e.g. a 512² image asking for 11 levels, a 256² asking for 10). The extra level has undefined contents → RGB speckle when sampled. Fix (3 points): `image.cpp` ctor clamps `backing.mip_levels` to the complete chain; `imageView.cpp FindView` folds a descriptor that still points at the dropped level back into the real range (instead of `EXIT`); `image.cpp GetBarriers` clamps the transition range + uses `full_levels = min(guest_levels, backing.mip_levels)` so no barrier is emitted for a level the image no longer has (would have been device-lost). **RESULT: clamp verified firing (`… (clamped)` in log, no crash) but NO visible change** — the walkout-floor speckle is not this. It's downstream of the stubbed occlusion CS (same root as the black match / rainbow skin). Clamp kept as a correctness fix (Vulkan spec violation + latent crash class).

- **Present path — native-scanout rework (user session, 2026-09-10, uncommitted):** `swapchain.cpp PrepareFrame` no longer *always* substitutes the flip-alias / last-color / `0x1162c00000` compositor. It now presents the game's real scanout directly when `native_scanout = scanout.SafeToDownload() && (scanout.usage.storage || scanout.usage.render_target)`, keeping the old fallbacks only when that's false. **Effect: in-match the real composited frame now shows (walkout scene renders — black from stubbed CS + entrance-stage light strip + speckled floor); menu + HUD broke** (the `native_scanout` gate is too permissive on menu paths, presents the wrong image). Follow-up: also require the scanout was GPU-written *this* frame / matches flip dims, or restrict to gameplay context. Also in this session: `textureCache.cpp CopyImage` new `crosses_1d` path (1D↔2D copy via buffer, "UFC reuses a 1D R32 image as a 2D row"); `vulkanWindow.cpp` enables `dualSrcBlend` when supported.
  - **Committed as checkpoint `745d4d8`** (menu + pre-fight + walkout render, HUD present). Also folded in that commit: the mip-chain clamp, the round-2 log gating, the UFC UI-presence-mask fallback (`KYTY_UFC_UI_MASK_FALLBACK`, fills the exact 1080p mask kernel with full coverage — CMask metadata is empty because Vulkan colour draws don't write it), full dual-source-blend support (pipelineCache detect Src1* factors → `ps_dual_source_blend`; spirvEmitterModule emits Location 0 / Index 0|1).

- **VideoOut flip-buffer format pin — ROOT-CAUSE FIX (`8ee5cc8`, 2026-09-10):** the A2R/A2B scanout mismatch. UFC double-buffers the scanout; the game registers both flip buffers as A2R10G10B10 (`attribute=58`), but `TextureCache::FindImage` stamps `backing.format` from whichever binding creates the cache image first, and the paths disagree — the VideoOut resolve decodes the registered token (→ **58 A2R**), a CS storage write maps `k10_10_10_2 → A2B` unconditionally (**64**). Whichever wins the `FindImage` race labels the image; `0x111a800000` (VideoOut first) got 58, `0x111b800000` (CS storage first) got 64. Bits are identical (CS packs raw); only the label diverged, so `PrepareFrame`/`CopyFrom` misread the 64-labelled buffer. Fix: `VideoOutRegisterBuffers2` (which already computes the authoritative `ImageInfo` and discarded it) now calls new `TextureCache::RegisterVideoOutSurface(addr,size,fmt)`; `InsertImage` forces `info.pixel_format` to the registered format for any image created inside a flip range, *before* the VkImage is made → one allocation, display-format label, all binding paths agree. Mutable-format lets the CS still view it as A2B for its own stores. Removed the now-dead `swapchain.cpp` per-present `IsPacked10Unorm` `frame_format` override (the user's present-time workaround). **Verified:** `pinning VideoOut surface 0x111b800000 to registered format 58 (was 64)` once; `VideoOut present format mismatch` count ~10/session → **0**; both buffers `backing=58`; splash + menu colours correct.
  - Kept (different axis, still legitimately variable): `Frame::CopyFrom`'s `IsPacked10Unorm && IsPacked10Unorm` verbatim-copy branch (scanout backing vs *swapchain image* format).

### Wave64 lowering — Option A (in progress, 2026-09-09)

Goal: stop the wave64-on-wave32 emulation from being ~1000× too slow. Disassembly of the hang CS (`strategy=legacy`, clean structurize) showed the cost is: **1771 `OpSelect`** (exec-mask predication — every `v_cmpx` region lowered to "compute both sides, `result = active ? new : old`"), **× `lane_count == 2`** scalar lane-pair packing (`SpirvEmitter.cpp:326`), plus 279 paired subgroup ops. `OpIAdd`+`OpFAdd` only 704 — real arithmetic is modest.

- **Stage 1 (LANDED + MEASURED): fold a compile-time-uniform exec mask.** `Translator::ThreadBit` (`Translate.cpp`) returns immediate `true`/`false` when the mask is an all-ones / all-zero compile-time constant. `s_mov_b64 exec, -1` (emitted after every `v_cmpx` region) lowered to `SetExec(ThreadBit({0xFFFFFFFF,0xFFFFFFFF}))` → runtime `(0xFFFFFFFF >> laneid) & 1` chain constant-prop couldn't fold → every downstream `Select(exec,new,old)` survived. Now: SsaRewrite forwards immediate `true`, `FoldSelect` collapses them, DCE drops the old-value loads.
  - **Measured on the hang CS** (in-match dump): `OpSelect` **1771 → 1309 (−26%)**, OpPhi 1021→895, OpLoad 524→498, SPIR-V 320KB→294KB (−8%), `OpLabel` 1209→1209 (structure intact), `spirv-val` clean, menu byte-identical.
  - Remaining 1309 selects: the hang CS also restores exec via `s_mov exec, <saved-sgpr>` (×12, `s_and_saveexec`), `s_mov exec, vcc` (×9), `s_andn2 exec` (×7) — not compile-time constant, so still per-lane. Stage 1 alone ~10-15% fewer instrs — **not enough to beat the TDR**.
  - Pre-Stage-1 dump saved: `shader-dumps/CS_ea0aceac518ec52d.spv.pre-stage1`; post: `.spv` + `CS_stage1_dis.txt`.

- **Occlusion-CS clear-to-zero stub (LANDED, `renderCompute.cpp`):** when `KYTY_SKIP_CS_HASH` matches, instead of dropping the dispatch, clear the CS's read-write images to 0 ("nothing occludes" — 0 is what the real shader IMAGE_STOREs into an empty tile; reverse-Z far plane). Logs `stubbing watched compute shader ... cleared_images=N`.
  - **Result: intro + pre-round corner scenes now RENDER** (octagon, crowd, cage, lighting, cornerman+bucket — full 3D). `display nonzero` 99.8%. No crash. ~1.2-1.7 fps. **This proves the whole engine works** — CFG fix + Stage 1 + depth-alias + PS-input fixes all compounding.
  - **The actual fight round is still black.** The CS also writes two big read-write buffers — `buffer[9]` `0x1163920000` (4718592×4 = 18 MB), `buffer[10]` `0x1170776e00` (9437184×4 = 36 MB, exactly 2× buffer[9]) — visibility / indirect-draw data the fight round consumes. The stub leaves them stale → fight-scene draws culled → black. Sizes don't reveal the layout; filling blind risks a GPU hang (wrong value in an indirect-args buffer). Corner scene renders because it doesn't consume these.
- **Stage 2 (TODO): `lane_count = 1`.** 256 GCN threads → 256 invocations (8 subgroups) instead of 128 scalar lane-pairs. Removes the `%X`/`%X+1` op duplication. Only 23 `lane_count` refs to audit (mostly `spirvEmitterProgram.cpp`/`Mesh`/`Module`); the `state.wave_size == 64` cross-lane helpers (`spirvEmitterHelpers.cpp:213`, `spirvEmitterFlow.cpp`) must bridge 2 subgroups via LDS instead of assuming pair-packing.
- **Stage 3 (TODO): native subgroup arithmetic.** Declare `GroupNonUniformArithmetic`; manual shuffle-tree reductions → `OpGroupNonUniformIAdd`/`FAdd`.

### Tooling (2026-09-09)

- **`D:\PS5\tools\drive_ufc.py`** — virtual Xbox pad via `vgamepad` + ViGEmBus (both installed). Navigates menus into a match: `python drive_ufc.py --seq "A:6, A:2, A:6, START:8, A:8"` (BUTTON:seconds-after). space→A, enter→START. Confirmed Kyty picks up the pad (`Controller axis: 0 …` in log); XInput is not focus-gated so no window juggling. Pad timing is non-deterministic — menu load times vary, so the same seq can land in different scenes (an `A,A,A,START,A` run hit a `draws=8443` scene vs the manual path's `~5084`).
- **GPU surface dump on demand:** `touch D:/PS5/dumps/DUMP_NOW` → next present dumps `display`/`present` + 9 RT surfaces as BMP to `D:/PS5/dumps/`, each with a `nonzero=X/Y` count in the log. (Also frames 1/30/90/180/360/720/1500, or F9.)

### Shader CFG

- `StructurizeWithStrategy` with Legacy vs DispatcherFull; env `KYTY_SHADER_CFG_STRATEGY` (default Auto). Auto = try Legacy, fall back to DispatcherFull `if (!success && IsStructuralPathology(graph))`.
- Shared-merge splits and **clone retry** for UFC pixel CFGs (external selection tails / 16→177 shape).
- SRT / resource tracking import onto rewritten IR.
- SPIR-V emit / SSA updates for dispatcher state.
- Tests in `tests/shaderCfgTests.cpp` plus fixtures under `tests/shaders/`.
- **`ValidateStructuredExits()` (2026-09-09):** after `StructurizeImpl` assigns merge blocks, walk every construct region (`DominatedBlocks(header, merge)`) and verify no member block's successor leaves the construct except via its own merge, or a break/continue of an *enclosing loop*. On violation → `SetFailure(FailureKind::StructuredControlFlow)` + `return false`, which routes through the existing Auto fallback to DispatcherFull for that one shader.
  - **Why:** `StructurizeImpl` assigned each selection a merge but never checked the interior edges. The 3 UFC pixel übershaders (`0x1bcc68ffb7b0469e`, `0xb808b3887f76ff0c`, `0xf7030726b9470dd8` — 5400+ GCN words, 161 blocks / 12 loops) got Legacy-structurized into **invalid SPIR-V**: `block %82 exits the selection headed by %80, but not via a structured exit` (a fall-through block jumping to a grandparent selection merge, past the discard-pad merges of two enclosing selections). NVIDIA accepted the module (validation is advisory; `--shader-validation` was off by default) and miscompiled the malformed control flow into a **GPU hang** — this was the `commandScheduler.cpp:401` / `ErrorDeviceLost` device-lost we were chasing (the EA engine wrote its own `_hang.st.dat` too).
  - **Verified:** with the fix, `3 [CFG-STRUCT-FAIL] strategy=legacy` → `3 [CFG-STRUCT] strategy=dispatch` for the übershaders, `546 [CFG-STRUCT] strategy=legacy` for everything else (no over-trigger), and `0` spirv-val failures. Run now clears the übershaders and reaches the hang CS.

### DS / GDS (hang CS family)

- Decoder + translate + SPIR-V for **wrap** `DS_INC` / `DEC` / `RSUB` (with and without return), LDS and GDS.
  - ISA: `tmp = MEM; MEM = (tmp >= DATA) ? 0 : tmp+1; return tmp`.
  - UFC encoding: `ds_inc_rtn_u32 v3, v2, v3 offset:4 gds` (`0xd88e0004 0x03000302`).
- **M0 GDS window** on regular GDS DS ops (`Translator::ApplyGdsWindow`). UFC uses size-only `m0=0xC000` (base 0).
- Host **GDS probe** on the watched hang CS: GPU copy of GDS[0..7] through the download buffer, then `Finish()`, then log `limit[0]` / `counter[1]`.
- GPU tests: `shader_recompiler_compute_tests.exe --ds-inc-only` (wrap semantics + captured UFC GDS inc + M0 base + 144-WG GDS contention).
- Wrap-INC CAS loop memory semantics (SPIR-V valid): load Acquire, CmpXchg equal AcquireRelease, unequal Acquire, then barrier.
- Diagnostic `KYTY_GDS_LIMIT_CAP` and `KYTY_GDS_CHUNK`.

---

## Current issues

### 1. Hang CS TDR (blocking) — cap=1 no longer survives

Compute shader **`hash=0xea0aceac518ec52d`**, addr `0x11409ec000`, 1612 GCN words → ~80,080 SPIR-V words. Wave64, local **16×16×1**, **144** groups. Frostbite GPU software occlusion culling: persistent threads claim 16×16 screen tiles via `ds_inc` on GDS[1] vs limit GDS[0] (~5500–5700 items), each rasterizes an occluder Hi-Z tile and `IMAGE_STORE`s to a 1600×904 target.

- **Root cause:** RTX 3070 is wave32-only. The shader is wave64. Kyty emulates wave64 as **2 GCN lanes per host invocation** (256 GCN threads → 128 invocations), every ALU op doubled, every ballot/shuffle/readlane done twice + recombined through LDS/barriers, `exec` mask emulated as data. For this ballot-saturated 6-nested-loop shader that is ~1000× too slow — **~0.67 s per 16×16 tile**.
- Full 144-group dispatch → instant TDR (`vkQueueSubmit` → `ErrorDeviceLost`).
- **As of 2026-09-09:** with the CFG + RT-depth fixes clearing everything in front of it, the run reaches **frame ~571**, then `KYTY_GDS_LIMIT_CAP=1` — running a *single* tile — **still TDRs** (`debug_op=0` DispatchDirect, `args=144,1,1,65,0x11409ec000`, `original=5700`). One tile + the `Finish()` serialization now exceeds the 2 s TDR window this deep into the scene. Chunking and capping are exhausted.
- `KYTY_SKIP_CS_HASH=0xea0aceac518ec52d` (full skip) still gets past — used for fast iteration on later blockers. Loses occlusion culling (worse 3D-scene draw count, but correct).
- Dumps: `D:\PS5\shader-dumps\CS_ea0aceac518ec52d.{bin,rdna2,cfg.txt,ir.txt,spv}`

**This is now the sole hard blocker. The remaining work is the wave64→wave32 lowering rework (Option A) — see "Next steps" #1.**

**Render state with the CS skipped (2026-09-09):** `KYTY_SKIP_CS_HASH` gets past the TDR and into a match (HUD/health-bars/banner composite fine — presentation path verified good), but the **3D scene is black + a garbage RGB band** — the GPU-driven scene submits ~5000 draws that render nothing because the visibility/HZB data the skipped CS produces is missing. Confirmed render bug not presentation: `present` in-match ~3.7% non-zero, scene RT `0x1162c00000` collapses to 400×225 empty. Getting a correct scene needs the CS actually working (Option A). A heavier auto-navigated scene (`draws=8443`) in this broken state hit `ErrorDeviceLost` (debug_op=3) once — not cleanly attributed (Stage 1 change + different scene); the reverted A/B build was clean where it reached but didn't get to the same scene.

### 2. Pixel `input_num=0` (worked around; real bug open)

PS `0x4c22cd8526171dbb` (116 words): `SPI_PS_IN_CONTROL & 0x3f` decodes to **0**, so `input_num=0`, but the shader reads `GetAttribute(0, ...)`. Worked around in `ShaderInfoCollection` (log + continue; `EmitAttribute` → 0). Real fix: find why that PS is compiled before `SPI_PS_IN_CONTROL` is written — register-capture ordering in the PM4 → pipeline-key path. Reachable with `KYTY_SKIP_CS_HASH` set.

### 3. depth-surface-as-color-target (FIXED 2026-09-09)

`0x1163740000`, 512 KB, depth format, bound as a color RT. Fixed in `FindImage` / `AcquireRenderTargets`.

### 4. CFG structurizer invalid SPIR-V (FIXED 2026-09-09)

3 pixel übershaders → invalid structured control flow → GPU hang. Fixed by `ValidateStructuredExits()` + Auto fallback to DispatcherFull.

### 4b. Skin corruption — rainbow oil-slick on all exposed skin (open, isolated 2026-09-09)

With the intro/corner scenes rendering, the visible "graphical issues": **all exposed skin** (fighters' arms/faces/torsos, foreground cornerman arm) shows a psychedelic rainbow marbled pattern that follows surface curvature; hair has green+magenta speckle. Clothing, cage, crowd, arena all correct.

Isolated to a **texture format alias**: 40× `TextureCache: sampling backing format 122 instead of descriptor format 98 addr=0x0000001156990000` (and `sampling GPU-written alias fmt=122 instead of req_fmt=98`). `0x1156990000` is a **256×256, 9-mip** render target written as `fmt=0x6 nfmt=0x7` (→ `VK_FORMAT_B10G11R11_UFLOAT_PACK32` = 122, an HDR/prefiltered-cube style buffer); the skin shader samples that same memory with a descriptor for `VK_FORMAT_A2R10G10B10_SNORM_PACK32` = 98 (signed packed — normal/SSS style). Both 32-bit so the cache aliases and `FindTexture` overrides the view to the backing format (`textureCache.cpp:1623`), and the `FindImage` `encoding_mismatch && same_block` branch (`~1417`, comment "UFC 5 draws the title as RGBA8, composites with 11-11-10") deliberately samples the written encoding as-is. That's right for the title screen but wrong for SNORM-vs-UFLOAT skin: the shader wants the 32 bits *reinterpreted*, not resampled as float. Likely fix: for this case create a real format-reinterpret `ImageView` (mutable format, both in the 32-bit compatibility class) with the descriptor's format instead of forcing the backing format. Or check whether the guest descriptor→vk 98 decode is even correct (256×256×9-mip suggests a cubemap/prefilter, which shouldn't be SNORM).

**UPDATE 2026-09-09 — reinterpret-view fix landed, DID NOT help.** `FindTexture` now serves a true bitcast `ImageView` in the descriptor format (122→98) when backing is packed-float and `FormatsCompatible` (see "Fixes applied → Shader recompiler" entry). Confirmed firing in-game on `0x1156990000`; **skin looks identical, no regression.** So the bug is NOT a view-encoding issue at the sample site. Revised hypotheses for next session, in order:
  1. **Wrong source image / stale contents.** `0x1156990000` may be getting bound when the shader actually wants a *different* surface, or this RT is never correctly populated because it's downstream of the stubbed occlusion CS. Check what writes `0x1156990000` and when, relative to the skin draw.
  2. **Tiling / swizzle mismatch** — the 256×256×9-mip surface may be macro-tiled on PS5 and we're reading it linear (or wrong micro-tile mode). Dump the raw image and eyeball for tile-block scrambling vs pure colour-space wrongness.
  3. **Guest descriptor decode** — verify the `nfmt`/`dfmt` → vk-98 path; a 9-mip 256² buffer smells like a prefiltered cube (should be a UFLOAT/half, not SNORM). If the *descriptor* decode is wrong, 98 is a red herring.
  4. It may simply be a **lighting/SSS term that reads garbage from a stubbed CS buffer** — i.e. same root cause as the black round, just a codepath that survives. Lowest priority to chase independently.

### 5. Other Vulkan validation errors (open — real, but NOT the hang)

From `--vulkan-validation true` (`KytyLog-PPSA03541-valid2.txt`):

- **`dstColorBlendFactor VK_BLEND_FACTOR_SRC1_ALPHA` but `dualSrcBlend` feature never enabled.** One-liner: `device_features.dualSrcBlend = supported_features2.features.dualSrcBlend;` in `vulkanWindow.cpp` (~line 707). Also the fragment shader emits `out_mrt_1` at **Location 1** instead of dual-source **Location 0 / Index 1** — needs an `Index` decoration in the SPIR-V backend when the blend state uses SRC1_* factors.
- **`image_10` compute sampler wants UINT, bound descriptor format is `VK_FORMAT_B10G11R11_UFLOAT_PACK32`** — type-confused image read (near `TextureCache: sampling GPU-written alias fmt=122 instead of req_fmt=98`).
- `vkCmdCopyImage` srcImage type 1D ≠ dstImage type 2D.
- `vkCreateImage mipLevels (9/10/11)` exceeds the extent-derived max (8/9/10) — creation fails, null image used later.

### 6. 3D slideshow

Menus ~20–60 fps. In-match / 3D scene ~**1 fps** with **8k–10k draws** after compiles finish. Made worse by skipping the occlusion-cull CS. Separate from the hang CS.

---

## Next steps

0. **Fight round still black — the occlusion CS's two big buffers.** Either (a) figure out `buffer[9]` (`0x1163920000`, 18 MB) / `buffer[10]` (`0x1170776e00`, 36 MB) layout — trace the consumer shader that reads them, or RenderDoc a real PS5 frame — and extend the `renderCompute.cpp` stub to fill them with a real "all visible" value, or (b) get the real CS running (step 2 / hand-written replacement). Intro + corner scenes already render, so everything else is sound.
1. **Skin corruption** (issue 4b) — the `fmt 98↔122` reinterpret-view fix is landed but **did not change the visuals**. Not a sample-site encoding bug. Next: chase the 4 revised hypotheses in issue 4b (wrong/stale source image → tiling → descriptor decode → stubbed-CS garbage). Start by tracing what writes `0x1156990000`.
2. **Wave64 → wave32 lowering rework (Option A).** Stage 1 landed (−26% OpSelect, measured). Next: **Stage 1b** — at structurize time mark the structured-`if`-body entry exec as identity so the `s_and_saveexec`/`s_mov exec,<saved>` restores fold too (catches most of the remaining 1309 selects); **Stage 2** — `lane_count = 1` (kill the ~2× lane-pair scalar duplication on everything, incl. the 279 `OpGroupNonUniform`). Target model: map **1 GCN wave64 → a 64-invocation workgroup = 2 native subgroups of 32**.
   - Per-lane ALU: stop the 2×-per-invocation packing → 1 GCN thread = 1 SPIR-V invocation (kills the doubling on most of the shader).
   - Cross-lane ops: one `combine(subgroup0, subgroup1)` per wave through a single LDS slot + one barrier, computed once per loop edge — not twice per use. `readlane`→`subgroupShuffle`/LDS; `readfirstlane`→`subgroupBroadcastFirst`+LDS hop; `ballot`→two `subgroupBallot`+LDS halves; wave reduce→`subgroupAdd`/etc.
   - **Declare `GroupNonUniformArithmetic`** (currently not declared — reductions are hand-rolled 6-step shuffle trees ×2).
   - `exec` mask: recognize the `v_cmpx / s_cbranch_execz / s_mov_b64 exec,-1` idiom as a plain SPIR-V `if` and let the driver handle divergence; only emulate `exec` for the genuinely weird cases (`s_not_b64 exec` then continue, cross-mask value reuse).
   - Pin with `VK_EXT_subgroup_size_control` + `requiredSubgroupSize=32` + `FULL_SUBGROUPS`.
   - Expected: ~3–6× for this shader (tile ~0.67 s → ~0.1–0.2 s). Might survive with chunking; helps every compute-heavy Frostbite shader. If not enough, the fallback is a hand-written wave32 replacement of the occlusion-cull algorithm (detect the hash, substitute the pipeline) or an AMD wave64 GPU.
2. **`input_num=0`** — fix at the source (PS `0x4c22cd8526171dbb` register-capture timing).
3. **dualSrcBlend** — enable the feature; emit the dual-source PS output at Location 0 / Index 1.
4. **Slideshow** — draw/submit cost on the 3D scene once the GPU stays alive.

## Diagnostic env / flags

```
$env:KYTY_SKIP_CS_HASH       = '0xea0aceac518ec52d'   # skip hang CS entirely — fast iteration (cap=1 no longer survives)
$env:KYTY_SHADER_DUMP_DIR    = 'D:\PS5\shader-dumps'
$env:KYTY_DUMP_SHADER_HASH   = '0x…,0x…'              # dump .spv/.rdna2/.cfg.txt/.ir.txt for a hash (comma list)
$env:KYTY_SHADER_CFG_STRATEGY= 'auto'                 # legacy | dispatch | auto (default). auto now falls back to dispatch on invalid legacy output.
$env:KYTY_GDS_LIMIT_CAP      = '1'                    # cap hang-CS work items (no longer prevents the TDR as of frame ~571)
$env:KYTY_GDS_CHUNK          = '4'
```

CLI flags:
- `--shader-validation true` — run spvtools `Validate` on emitted SPIR-V; on failure dump `.spv` to the shader-log folder + print friendly disassembly, then `EXIT`. **Off by default** (this is why the übershader invalid SPIR-V shipped silently for so long).
- `--vulkan-validation true` — `VK_LAYER_KHRONOS_validation`; errors now non-fatal (logged `[VALIDATION-ERROR]`). `VK_LAYER_LUNARG_crash_diagnostic` is also installed if a device-lost needs pinpointing.
- `--gpu-assisted-validation true` — bounds-checks shader accesses on the GPU; very slow, implies `--vulkan-validation`.

Key logs this session: `KytyLog-PPSA03541-valid2.txt` (validation sweep), `KytyLog-PPSA03541-shval.txt` (übershader dump), `KytyLog-PPSA03541-dispatch.txt` (forced dispatch — reached frame 571), `KytyLog-PPSA03541-cfgfix.txt` (the `ValidateStructuredExits` fix — auto fallback working).
