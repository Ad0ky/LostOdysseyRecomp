#!/usr/bin/env python3
"""Where the GPU's idle time goes, from a log with LO_GPU_TIMELINE=1 lines (docs/SWITCH-GPU.md).

    python3 tools/switch/gpu_timeline.py log.txt [--from-swap N] [--to-swap N] [--frames]

Each "gtl:" line is one presented frame of the render thread (GPU CmdProc):

    gtl: n=<swap> t=<us> S<id>:<start>:<dur>:<frame> P<id>:<start>:<dur>:<frame> G<id>:<begin>:<end>
         F<start>:<dur> Q.. A.. X.. Z.. I.. O<cpu>:<dur>:<gpu>

S and P are submissions (a renderer batch, the presentation list) with the CPU time of the submit call, G the GPU's
own timestamps at the start and end of a submission, the other letters waits of the render thread (F a renderer
fence, Q the presentation fence, A the swapchain image, X the present call, Z the pacing sleep, I waiting for the
game's commands) and O a reading of the GPU's clock between two of the CPU's. Times are microseconds.

The GPU is busy between a submission's two timestamps. A gap between two submissions is split in two:
  - until the render thread had submitted the next one: the GPU waited for the CPU. That part is given to whatever
    the render thread was doing then: one of its waits, or recording commands (the rest).
  - after that: the GPU had the commands and still did not start them. That is work the timestamps do not cover
    (deko3d's decompression of the swapchain image and fence flush at a present, the wait for the swapchain image
    before the presentation list) or the GPU channel's own latency.
"""
import argparse
import re
import statistics
import sys

WAITS = {
    "F": "renderer fence",
    "Q": "presentation fence",
    "A": "swapchain image (acquire)",
    "X": "present call",
    "Z": "pacing sleep",
    "I": "waiting for the game",
}


def parse(path, first, last):
    frames = []      # (swap, t_us)
    subs = {}        # id -> dict(kind, start, dur, frame)
    gpu = {}         # id -> (begin, end) on the GPU's clock
    waits = []       # (start, end, kind)
    clocks = []      # (cpu mid, cpu dur, gpu)
    line_re = re.compile(r"gtl: n=(\d+) t=(\d+)(.*)")
    with open(path, errors="replace") as f:
        for line in f:
            m = line_re.search(line)
            if not m:
                continue
            swap, t = int(m.group(1)), int(m.group(2))
            if (first and swap < first) or (last and swap > last):
                continue
            frames.append((swap, t))
            for field in m.group(3).split():
                kind, values = field[0], [int(v) for v in field[1:].split(":")]
                if kind in "SP":
                    subs[values[0]] = {"kind": kind, "start": values[1], "dur": values[2], "frame": values[3]}
                elif kind == "G":
                    gpu[values[0]] = (values[1], values[2])
                elif kind == "O":
                    clocks.append((values[0] + values[1] / 2, values[1], values[2]))
                elif kind in WAITS:
                    waits.append((values[0], values[0] + values[1], kind))
    return frames, subs, gpu, waits, clocks


def overlap(a0, a1, b0, b1):
    return max(0, min(a1, b1) - max(a0, b0))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--from-swap", type=int, default=0)
    ap.add_argument("--to-swap", type=int, default=0)
    ap.add_argument("--frames", action="store_true", help="print every frame's row")
    args = ap.parse_args()

    frames, subs, gpu, waits, clocks = parse(args.log, args.from_swap, args.to_swap)
    if len(frames) < 3:
        sys.exit("no gtl: lines (LO_GPU_TIMELINE=1) in the range")
    if not clocks:
        sys.exit("no clock pairs (O fields): the GPU's clock cannot be placed on the CPU's")
    # The GPU's clock minus the CPU's. The readings with the shortest kernel call are the best.
    best = sorted(clocks, key=lambda c: c[1])[: max(1, len(clocks) // 2)]
    offsets = [g - c for c, _, g in best]
    offset = statistics.median(offsets)
    drift = max(offsets) - min(offsets)

    # Busy intervals on the CPU's clock, in GPU order.
    busy = []
    for i, (b, e) in gpu.items():
        if i in subs and e >= b > 0:
            busy.append({"id": i, "begin": b - offset, "end": e - offset, **subs[i]})
    busy.sort(key=lambda x: x["begin"])
    if len(busy) < 4:
        sys.exit("too few timed submissions")
    waits.sort()

    # Frames are cut at presentation lists: a frame is everything after the previous P's end up to this P's end.
    rows = []
    cur = None
    prev = None
    for b in busy:
        if cur is None:
            cur = {"busy_S": 0.0, "busy_P": 0.0, "n_S": 0, "cpu_late": 0.0, "queued": {}, "late_by": {}, "start": None}
        if prev is not None:
            gap = b["begin"] - prev["end"]
            submitted = b["start"] + b["dur"]          # when the submit call returned
            late = max(0.0, min(b["begin"], submitted) - prev["end"]) if gap > 0 else 0.0
            queued = max(0.0, gap - late) if gap > 0 else 0.0
            if gap < 0:
                queued = gap                            # overlap: clock error, kept visible
            key = prev["kind"] + ">" + b["kind"]
            cur["queued"][key] = cur["queued"].get(key, 0.0) + queued
            if late > 0:
                cur["cpu_late"] += late
                w0, w1 = prev["end"], prev["end"] + late
                waited = 0.0
                for ws, we, kind in waits:
                    if we <= w0:
                        continue
                    if ws >= w1:
                        break
                    o = overlap(w0, w1, ws, we)
                    if o:
                        cur["late_by"][kind] = cur["late_by"].get(kind, 0.0) + o
                        waited += o
                cur["late_by"]["recording"] = cur["late_by"].get("recording", 0.0) + max(0.0, late - waited)
        cur["busy_" + b["kind"]] += b["end"] - b["begin"]
        if b["kind"] == "S":
            cur["n_S"] += 1
        if b["kind"] == "P":
            cur["end"] = b["end"]
            rows.append(cur)
            cur = None
        prev = b
    rows = rows[1:]  # the first has no frame before it
    if len(rows) < 2:
        sys.exit("too few complete frames")
    periods = [rows[i]["end"] - rows[i - 1]["end"] for i in range(1, len(rows))]
    rows = rows[1:]
    n = len(rows)

    def mean(values):
        return sum(values) / len(values) if values else 0.0

    period = mean(periods)
    print(f"{n} frames, swaps {frames[0][0]}-{frames[-1][0]}; GPU clock - CPU clock = {offset / 1e6:.6f} s "
          f"(spread of the best pairs {drift:.0f} us, {len(clocks)} pairs)")
    print(f"frame period on the GPU (presentation end to presentation end): {period / 1000:.3f} ms = {1e6 / period:.2f} FPS")
    print()
    print("GPU per frame                                   ms     % of frame")

    def out(label, value):
        print(f"  {label:<44}{value / 1000:7.3f}   {100 * value / period:6.2f}")

    busy_s, busy_p = mean([r["busy_S"] for r in rows]), mean([r["busy_P"] for r in rows])
    out(f"renderer batches ({mean([r['n_S'] for r in rows]):.2f} per frame)", busy_s)
    out("presentation list", busy_p)
    total = busy_s + busy_p
    keys = sorted({k for r in rows for k in r["queued"]})
    names = {"S>P": "between renderer batch and presentation", "P>S": "after presentation (decompress, fence)",
             "S>S": "between renderer batches", "P>P": "between presentations"}
    for k in keys:
        v = mean([r["queued"].get(k, 0.0) for r in rows])
        out("not started though submitted: " + k, v)
        total += v
    late = mean([r["cpu_late"] for r in rows])
    out("waiting for the render thread to submit", late)
    total += late
    out("sum", total)
    print()
    print("  not started though submitted:")
    for k in keys:
        print(f"    {k}: {names.get(k, k)}")
    print()
    print("While the GPU waited for the render thread, the render thread was        ms per frame")
    kinds = sorted({k for r in rows for k in r["late_by"]}, key=lambda k: -mean([r["late_by"].get(k, 0.0) for r in rows]))
    for k in kinds:
        v = mean([r["late_by"].get(k, 0.0) for r in rows])
        print(f"  {WAITS.get(k, 'recording commands (not waiting)'):<60}{v / 1000:7.3f}")
    print()

    # The render thread's own frame: waits by kind over the same span.
    t0, t1 = frames[0][1], frames[-1][1]
    span_frames = len(frames) - 1
    print(f"Render thread per frame over the {span_frames} frames between the first and last line "
          f"({(t1 - t0) / span_frames / 1000:.3f} ms a frame)")
    by = {}
    for ws, we, kind in waits:
        o = overlap(t0, t1, ws, we)
        if o:
            by[kind] = by.get(kind, 0.0) + o
    waited = 0.0
    for kind in sorted(by, key=lambda k: -by[k]):
        print(f"  {WAITS[kind]:<44}{by[kind] / span_frames / 1000:7.3f}")
        waited += by[kind]
    sub = sum(s["dur"] for s in subs.values() if t0 <= s["start"] <= t1)
    print(f"  {'in submit calls':<44}{sub / span_frames / 1000:7.3f}")
    print(f"  {'everything else (recording, present set-up)':<44}{(t1 - t0 - waited - sub) / span_frames / 1000:7.3f}")

    if args.frames:
        print()
        print("frame  busy_S  busy_P  " + "  ".join(keys) + "  cpu_late")
        for i, r in enumerate(rows):
            print(f"{i:5d} {r['busy_S'] / 1000:7.3f} {r['busy_P'] / 1000:7.3f}  " +
                  "  ".join(f"{r['queued'].get(k, 0.0) / 1000:6.3f}" for k in keys) + f"  {r['cpu_late'] / 1000:7.3f}")


if __name__ == "__main__":
    main()
