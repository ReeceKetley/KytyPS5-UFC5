#!/usr/bin/env python3
"""Read out a single-launch A-B from the present-rate log.

WHY THIS EXISTS. The single-run present-rate variance floor on this project is about 8%
(EXP-0027), and that noise is dominated by scene variation between paused fights rather than by
the sampler itself: three local optimizations returned at most 1.7% and could not be resolved end
to end at all (EXP-0019). So relaunching the emulator for each arm of an A-B cannot settle a
question near 10% -- which is where EXP-0029 was left. The single-launch harness in
renderer/abControl.h flips the feature inside ONE paused fight instead, holding the scene fixed,
and the sampler in swapchain.cpp tags every logged window with the variant in force and resets
its window on a flip, so nothing straddles the boundary.

This script does the arithmetic that discipline implies:

  * groups the log's windows into RUNS -- maximal stretches on one variant -- because the A-B-A
    pattern's whole point is that a variant appearing twice should agree with itself. Drift
    between the two A runs is the honest estimate of what the method cannot resolve, and it is
    reported as such rather than being averaged away;
  * pools windows per variant and reports median, spread and a bootstrap confidence interval on
    the median ratio, so the answer is an interval and not a point;
  * compares that interval against the drift observed between repeats of the same variant, and
    refuses to call a difference real when the interval is not clear of it.

Usage:
    python analyze-ab-fps.py [--log D:/PS5/fps-pressure-test.txt] [--since-bytes N]
                             [--tail N] [--label 0=eager --label 1=lazy]
"""

from __future__ import annotations

import argparse
import random
import re
import statistics
import sys
from dataclasses import dataclass, field

# Floor on the resolution band. The drift between repeats of one variant is the honest estimate of
# what this method can see, but it is computed from very few run medians, so a lucky pair can read
# absurdly low. Nothing below this is called a win regardless.
MIN_BAND = 0.02

LINE = re.compile(
    r"PresentRate:\s+pressure=(?P<pressure>\d+)"
    r"(?:\s+variant=(?P<variant>-?\d+))?"
    r"\s+frames=(?P<frames>\d+)\s+seconds=(?P<seconds>[\d.]+)\s+fps=(?P<fps>[\d.]+)"
)


@dataclass
class Window:
    index: int
    pressure: int
    variant: int | None
    frames: int
    seconds: float
    fps: float


@dataclass
class Run:
    variant: int | None
    windows: list[Window] = field(default_factory=list)

    @property
    def fps(self) -> list[float]:
        return [w.fps for w in self.windows]


def parse(path: str, since_bytes: int, tail: int | None,
          until_bytes: int | None = None) -> list[Window]:
    """Read windows from a byte range of the log.

    until_bytes matters more than it looks. The log is append-only and shared by every run, so
    --since-bytes alone reads to end-of-file and silently swallows every LATER run too. Re-reading
    a historical window without an upper bound produced a confident, wrong answer here once
    already: two different experiments came back with identical tail pairs because both had
    absorbed everything after them.
    """
    with open(path, "rb") as handle:
        if since_bytes:
            handle.seek(since_bytes)
        if until_bytes is not None:
            span = max(until_bytes - since_bytes, 0)
            text = handle.read(span).decode("utf-8", "replace")
        else:
            text = handle.read().decode("utf-8", "replace")
    windows: list[Window] = []
    for line in text.splitlines():
        match = LINE.search(line)
        if match is None:
            continue
        variant = match.group("variant")
        windows.append(
            Window(
                index=len(windows),
                pressure=int(match.group("pressure")),
                variant=None if variant is None else int(variant),
                frames=int(match.group("frames")),
                seconds=float(match.group("seconds")),
                fps=float(match.group("fps")),
            )
        )
    if tail is not None:
        windows = windows[-tail:]
    return windows


def split_runs(windows: list[Window]) -> list[Run]:
    runs: list[Run] = []
    for window in windows:
        if not runs or runs[-1].variant != window.variant:
            runs.append(Run(variant=window.variant))
        runs[-1].windows.append(window)
    return runs


def bootstrap_ratio(a: list[float], b: list[float], iterations: int = 20000) -> tuple[float, float]:
    """Percentile interval for median(b) / median(a). Resamples windows, so it answers
    'how much of this gap is the luck of which windows landed where', and nothing more."""
    rng = random.Random(20261008)
    ratios = []
    for _ in range(iterations):
        ra = statistics.median(rng.choices(a, k=len(a)))
        rb = statistics.median(rng.choices(b, k=len(b)))
        if ra > 0:
            ratios.append(rb / ra)
    ratios.sort()
    if not ratios:
        return (float("nan"), float("nan"))
    low = ratios[int(0.025 * (len(ratios) - 1))]
    high = ratios[int(0.975 * (len(ratios) - 1))]
    return (low, high)



def paired_adjacent(runs, lo, hi):
    """Effect estimated from ADJACENT arm pairs, which is what makes a rapid-alternation run
    worth doing.

    A long-arm A-B-A-B loads any drift in the machine onto whichever variant held the floor at
    the time: in one observed arm the rate climbed 11.18 -> 11.70 fps with no flip in it, a 4%
    trend across a single arm, which is larger than the effect being chased. Adjacent arms are
    only ~30 s apart, so the drift between one arm and the next is small and mostly common to
    both. Taking the ratio within each pair cancels it; taking the median across pairs then
    resists the odd bad window.
    """
    ratios = []
    for first, second in zip(runs, runs[1:]):
        if first.variant == second.variant or not first.fps or not second.fps:
            continue
        a, b = first, second
        if a.variant == hi:
            a, b = b, a          # a is always the `lo` arm, b the `hi` arm
        ratios.append(statistics.median(b.fps) / statistics.median(a.fps))
    return ratios


def trend_adjusted(runs, lo, hi):
    """Least-squares fit of fps against (variant, window index), as a cross-check on the pairing.

    Window index stands in for time. If the drift really is roughly linear over the run, the
    variant coefficient is the effect with the trend taken out. Reported alongside the paired
    estimate rather than instead of it: when two methods that fail differently agree, the answer
    is probably real; when they disagree, neither should be quoted.
    """
    xs, ds, ys = [], [], []
    index = 0
    for run in runs:
        for value in run.fps:
            if run.variant in (lo, hi):
                xs.append(float(index))
                ds.append(1.0 if run.variant == hi else 0.0)
                ys.append(value)
            index += 1
    n = len(ys)
    if n < 6 or len(set(ds)) < 2:
        return None
    # Normal equations for y = b0 + b1*x + b2*d, solved by Gaussian elimination on a 3x3.
    cols = [[1.0] * n, xs, ds]
    a = [[sum(ci[k] * cj[k] for k in range(n)) for cj in cols] for ci in cols]
    rhs = [sum(ci[k] * ys[k] for k in range(n)) for ci in cols]
    for i in range(3):
        pivot = max(range(i, 3), key=lambda r: abs(a[r][i]))
        if abs(a[pivot][i]) < 1e-12:
            return None
        a[i], a[pivot] = a[pivot], a[i]
        rhs[i], rhs[pivot] = rhs[pivot], rhs[i]
        for r in range(i + 1, 3):
            factor = a[r][i] / a[i][i]
            for c in range(i, 3):
                a[r][c] -= factor * a[i][c]
            rhs[r] -= factor * rhs[i]
    beta = [0.0, 0.0, 0.0]
    for i in reversed(range(3)):
        beta[i] = (rhs[i] - sum(a[i][c] * beta[c] for c in range(i + 1, 3))) / a[i][i]
    base = statistics.fmean(v for v, d in zip(ys, ds) if d == 0.0)
    if base <= 0:
        return None
    return {'effect': beta[2] / base, 'drift_per_window': beta[1]}


def report_paired(runs, pooled, name_of):
    usable = sorted((v for v in pooled if v is not None))
    if len(usable) != 2:
        return
    lo, hi = usable
    ratios = paired_adjacent(runs, lo, hi)
    if len(ratios) < 2:
        print()
        print("Not enough alternations for a paired estimate: it needs at least two adjacent")
        print("arm pairs. Run the variants in short alternating arms rather than long blocks.")
        return
    point = statistics.median(ratios)
    rng = random.Random(20261008)
    draws = sorted(statistics.median(rng.choices(ratios, k=len(ratios))) for _ in range(20000))
    low = draws[int(0.025 * (len(draws) - 1))]
    high = draws[int(0.975 * (len(draws) - 1))]
    print()
    print(f"Paired across {len(ratios)} adjacent arm pairs (drift cancels within a pair):")
    print(f"  {name_of(hi)} vs {name_of(lo)}: {point - 1:+.1%} "
          f"(95% CI {low - 1:+.1%} .. {high - 1:+.1%})")
    print("  per-pair: " + ", ".join(f"{r - 1:+.1%}" for r in ratios))
    fit = trend_adjusted(runs, lo, hi)
    if fit is not None:
        print(f"  cross-check, least squares with a linear time term: {fit['effect']:+.1%} "
              f"(drift {fit['drift_per_window']:+.4f} fps per window)")
        if abs(fit['effect'] - (point - 1)) > 0.02:
            print("  THE TWO METHODS DISAGREE by more than 2 points. Do not quote either;")
            print("  the drift is not behaving linearly and more alternations are needed.")
    if low > 1.0 and high > 1.0:
        print(f"  VERDICT: {name_of(hi)} is faster; the interval excludes zero.")
    elif low < 1.0 and high < 1.0:
        print(f"  VERDICT: {name_of(hi)} is slower; the interval excludes zero.")
    else:
        print("  VERDICT: UNRESOLVED -- the interval spans zero. More alternations would narrow it.")

def describe(values: list[float]) -> str:
    if not values:
        return "n=0"
    median = statistics.median(values)
    spread = ""
    if len(values) >= 2:
        spread = f" sd={statistics.stdev(values):.3f}"
    return (
        f"n={len(values):2d} median={median:6.3f} mean={statistics.fmean(values):6.3f}"
        f" min={min(values):6.3f} max={max(values):6.3f}{spread}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--log", default="D:/PS5/fps-pressure-test.txt")
    parser.add_argument("--since-bytes", type=int, default=0,
                        help="byte offset to start from; the launch manifest records "
                             "fps_log_start_bytes for exactly this purpose")
    parser.add_argument("--until-bytes", type=int, default=None,
                        help="byte offset to stop at; without it the read runs to end of file and "
                             "absorbs every later run in this shared append-only log")
    parser.add_argument("--tail", type=int, default=None,
                        help="use only the last N windows")
    parser.add_argument("--label", action="append", default=[], metavar="N=NAME",
                        help="name a variant, e.g. --label 0=eager --label 1=lazy")
    parser.add_argument("--drop-first", type=int, default=0,
                        help="discard the first N windows of each run as settling time")
    args = parser.parse_args()

    labels = {}
    for item in args.label:
        key, _, name = item.partition("=")
        try:
            labels[int(key)] = name
        except ValueError:
            print(f"ignoring malformed --label {item!r}", file=sys.stderr)

    def name_of(variant: int | None) -> str:
        if variant is None:
            return "unlabelled(no control file)"
        return f"{variant}:{labels[variant]}" if variant in labels else str(variant)

    windows = parse(args.log, args.since_bytes, args.tail, args.until_bytes)
    if not windows:
        print("no PresentRate windows found -- is KYTY_VRAM_TEST_FPS or KYTY_AB_CONTROL_FILE set?")
        return 1

    runs = split_runs(windows)
    if args.drop_first:
        for run in runs:
            run.windows = run.windows[args.drop_first:]
        runs = [r for r in runs if r.windows]

    print(f"{len(windows)} windows, {len(runs)} runs, "
          f"{sum(w.seconds for w in windows):.0f} s of wall-clock sampling")
    if any(w.variant is None for w in windows):
        print("NOTE: some windows carry no variant tag. Either they predate the A-B harness or the")
        print("      run had no control file, in which case they are all one setting and cannot be")
        print("      compared against each other here.")
    if len({w.pressure for w in windows}) > 1:
        print("NOTE: the VRAM pressure trigger changed during this log; pressure is a separate A-B")
        print("      and mixing it with this one invalidates the comparison.")

    print()
    print("Runs in order (a variant appearing more than once should agree with itself):")
    for position, run in enumerate(runs, 1):
        print(f"  run {position} variant {name_of(run.variant):>24s}  {describe(run.fps)}")

    pooled: dict[int | None, list[float]] = {}
    for run in runs:
        pooled.setdefault(run.variant, []).extend(run.fps)

    print()
    print("Pooled per variant:")
    for variant in sorted(pooled, key=lambda v: (v is None, v)):
        print(f"  variant {name_of(variant):>24s}  {describe(pooled[variant])}")

    # Repeat-disagreement: the method's own resolution limit, measured in this very log.
    drift = 0.0
    for variant, group in pooled.items():
        medians = [statistics.median(r.fps) for r in runs if r.variant == variant and r.fps]
        if len(medians) >= 2:
            spread = (max(medians) - min(medians)) / statistics.fmean(medians)
            drift = max(drift, spread)
            print()
            print(f"Variant {name_of(variant)} was measured {len(medians)} times: "
                  f"run medians {', '.join(f'{m:.3f}' for m in medians)}")
            print(f"  -> it disagrees with itself by {spread * 100:.1f}%. Nothing smaller than that")
            print("     is resolvable in this log, whatever the pooled numbers suggest.")
    if drift == 0.0:
        print()
        print("NO variant was measured twice, so this log has no internal estimate of its own")
        print("resolution. Run an A-B-A -- variant X, variant Y, then variant X again -- or the")
        print("result cannot be distinguished from drift. Falling back to the project's 7.8%")
        print("single-run variance floor (EXP-0027) as the threshold.")
        drift = 0.078
    elif drift < MIN_BAND:
        # Two run medians agreeing closely is weak evidence that they always would; a single lucky
        # pair must not be allowed to manufacture confidence in a small effect.
        print()
        print(f"Measured drift {drift:.1%} is below the {MIN_BAND:.0%} floor this script applies,")
        print("because it rests on too few repeats to be trusted as the resolution limit. Using")
        print(f"{MIN_BAND:.0%}; add more A-B-A repeats to earn a tighter threshold.")
        drift = MIN_BAND

    # With several short alternating arms the paired estimate is the one to trust; the pooled
    # comparison below cannot tell an effect from a trend.
    if len(runs) >= 4:
        report_paired(runs, pooled, name_of)

    usable = sorted((v for v in pooled if v is not None), key=lambda v: v)
    if len(usable) < 2:
        print()
        print("Only one variant present; nothing to compare.")
        return 0

    print()
    print("Pairwise, against that resolution limit:")
    for i in range(len(usable)):
        for j in range(i + 1, len(usable)):
            a, b = usable[i], usable[j]
            ma, mb = statistics.median(pooled[a]), statistics.median(pooled[b])
            ratio = mb / ma
            low, high = bootstrap_ratio(pooled[a], pooled[b])
            print(f"  {name_of(b)} vs {name_of(a)}: "
                  f"median {mb:.3f} vs {ma:.3f} = {ratio - 1:+.1%} "
                  f"(95% CI {low - 1:+.1%} .. {high - 1:+.1%})")
            band = (1.0 - drift, 1.0 + drift)
            if low > band[1]:
                print(f"     VERDICT: {name_of(b)} is faster, clear of the {drift:.1%} limit.")
            elif high < band[0]:
                print(f"     VERDICT: {name_of(b)} is slower, clear of the {drift:.1%} limit.")
            elif low > 1.0 - drift and high < 1.0 + drift:
                print(f"     VERDICT: NEUTRAL. The whole interval sits inside +/-{drift:.1%}, so any")
                print(f"     real effect is smaller than this method can see. That is the strongest")
                print(f"     claim available: 'costs nothing measurable', not 'costs nothing'.")
            else:
                print(f"     VERDICT: UNRESOLVED. The interval straddles the {drift:.1%} limit.")
                print(f"     More windows per run, or more A-B-A repeats, would narrow it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
