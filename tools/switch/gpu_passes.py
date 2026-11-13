#!/usr/bin/env python3
"""Adds up a log's LO_GPU_PASS_TRACE lines: where a frame's GPU time goes, per render target (docs/SWITCH-GPU.md).

    python3 tools/switch/gpu_passes.py log.txt [--from-frame N] [--to-frame N] [--sequence [FRAME]] [--merge] [--top N] [--shaders [--spv-dir DIR]]

A "gpass:" line is one stretch of a traced command list: the draws into one set of render targets, a clear, a copy.
Its time (us) is the GPU time between the end of the stretch before and its own end, at the bottom of the pipe. Lists:
S a renderer batch, P the presentation list. Fields: t target size, c / d colour and depth formats, dr draws,
cl clears, bar full/fragment/primitive barriers, inv barriers that drop the texture cache, pb / sb / dw pipeline
binds, state binds and descriptor writes, sp samples passed, fr / vt fragment and vertex shader invocations (with
LO_GPU_PASS_COUNTERS=1), n a note, rc / rd / src what the colour, depth and source textures are to the renderer
(rt:<EDRAM base>:<Xenos format>:<pitch>x<height>:<c|d> a render target, rs:<address>:<format> a resolved
surface, swap the swapchain).

Output: the traced frames' totals, the time per group of stretches (mean per traced frame), and with --sequence
one frame's stretches in order (the frame nearest the median total unless one is named).
"""
import argparse
import re
import statistics
import sys
from collections import OrderedDict, defaultdict

FIELD = re.compile(r"(\w+)=(\S+)")


def parse(path, first, last):
    frames = OrderedDict()   # frame -> {"segs": [...], "ends": [...]}
    with open(path, errors="replace") as f:
        for line in f:
            at = line.find("gpass")
            if at < 0:
                continue
            end = line.startswith("gpass end:", at)
            fields = dict(FIELD.findall(line[at:]))
            if "f" not in fields:
                continue
            frame = int(fields["f"])
            if (first and frame < first) or (last and frame > last):
                continue
            entry = frames.setdefault(frame, {"segs": [], "ends": []})
            if end:
                entry["ends"].append({"l": fields["l"], "b": int(fields["b"]), "segs": int(fields["segs"]),
                                      "trace_us": float(fields["trace_us"]), "batch_us": float(fields["batch_us"]),
                                      "truncated": fields["truncated"] == "1", "counters": fields.get("counters") == "1"})
                continue
            bar = [int(v) for v in fields["bar"].split("/")]
            w, h = (int(v) for v in fields["t"].split("x"))
            entry["segs"].append({
                "l": fields["l"], "b": int(fields["b"]), "i": int(fields["i"]), "k": fields["k"], "us": float(fields["us"]),
                "w": w, "h": h, "c": fields["c"], "d": fields["d"], "dr": int(fields["dr"]), "cl": int(fields["cl"]),
                "bf": bar[0], "bg": bar[1], "bp": bar[2], "inv": int(fields["inv"]), "pb": int(fields["pb"]),
                "sb": int(fields["sb"]), "dw": int(fields["dw"]), "sp": int(fields["sp"]), "fr": int(fields["fr"]),
                "vt": int(fields["vt"]), "n": fields["n"], "rc": fields["rc"], "rd": fields["rd"], "src": fields["src"],
                "by": int(fields.get("by", 0)), "in": fields.get("in", "-"), "el": int(fields.get("el", 0)),
                "vs": fields.get("vs", "0" * 16), "ps": fields.get("ps", "0" * 16), "st": int(fields.get("st", "0"), 16),
            })
    for entry in frames.values():
        entry["segs"].sort(key=lambda s: (s["l"] == "P", s["b"], s["i"]))
    return frames


def role(name):
    """A texture's name without what changes from frame to frame."""
    return name


def group_of(s):
    """The group a stretch is added up in: what it does and where."""
    if s["l"] == "P":
        return f"present: {s['k']} {s['w']}x{s['h']} {s['c']}"
    if s["k"] == "draws":
        if s["c"] == "-":
            return f"draws depth only {s['w']}x{s['h']} {s['d']} [{s['rd']}]"
        depth = f"+{s['d']}" if s["d"] != "-" else ""
        return f"draws {s['w']}x{s['h']} {s['c']}{depth} [{s['rc']}]"
    if s["k"] == "clear":
        what = s["c"] if s["c"] != "-" else s["d"]
        return f"clear {s['w']}x{s['h']} {what} [{s['rc'] if s['c'] != '-' else s['rd']}]"
    src = s["src"].split(":")
    return f"{s['k']} {s['w']}x{s['h']} {src[2] if len(src) > 2 else '?'}->{s['c']} [{':'.join(src[3:]) or '-'} -> {s['rc']}]"


def category_of(s):
    """A coarse split for the summary."""
    if s["l"] == "P":
        return "presentation list (final scaling, overlays)"
    if s["k"] in ("copy", "resolve", "blit2d", "readback"):
        return "copy engine / 2D engine (resolves, copies)"
    if s["k"] in ("upload", "bufcopy"):
        return "uploads"
    if s["k"] == "clear":
        return "clears"
    if s["k"] == "compute":
        return "compute"
    if s["c"] == "-":
        return "draws, depth only (shadow maps, depth passes)"
    if s["rc"].startswith("rs:") or s["rc"].startswith("rsview:"):
        return "draws into resolved surfaces (blit resolves, transfers)"
    if s["d"] == "-":
        return "draws, colour only (post-processing, 2D, UI)"
    return "draws, colour + depth (scene)"


def phases_of(segs):
    """Lost Odyssey's frame, stretch by stretch: the phase each one belongs to. The game is Unreal Engine 3: a depth
    prepass, the opaque scene, shadow maps and their projection into a light attenuation buffer, lights and
    translucency, resolves in between, post-processing, the UI. The scene colour target is the colour + depth one
    with the most draws; everything else is told from the targets and the order."""
    scene = defaultdict(int)
    for s in segs:
        if s["l"] == "S" and s["k"] == "draws" and s["c"] != "-" and s["d"] != "-":
            scene[s["rc"]] += s["dr"]
    scene_target = max(scene, key=scene.get) if scene else None
    out, seen_opaque = [], False
    for s in segs:
        if s["l"] == "P":
            phase = "presentation: scaling to the screen, gamma ramp"
        elif s["k"] in ("copy", "resolve", "blit2d", "readback", "upload", "bufcopy", "compute"):
            phase = "copy engine: cropped copies of resolved surfaces, uploads"
        elif s["k"] == "clear":
            phase = "clears"
        elif s["rc"].startswith(("rs:", "rsview:")):
            phase = "resolves: depth and shadow maps to R32F" if s["c"] == "r32f" else "resolves: colour (EDRAM to texture, as a full-target draw)"
        elif s["c"] == "-":
            if ":shadow" in s["rd"] or max(s["w"], s["h"]) <= 400:
                phase = "shadow maps"
            elif not seen_opaque and s["dr"] > 4:
                phase = "depth prepass"
            else:
                phase = "shadow projection and other single depth draws"
        elif s["d"] != "-":
            if s["rc"] == scene_target:
                phase = "lights and translucency" if seen_opaque else "opaque scene"
                seen_opaque = True
            else:
                phase = "shadow projection and other single depth draws"
        elif max(s["w"], s["h"]) <= 400:
            phase = "post-processing: bloom and depth of field chain (small targets)"
        elif s["dr"] >= 8:
            phase = "UI and 2D"
        else:
            phase = "post-processing: full-screen passes"
        out.append(phase)
    return out


def resolve_report(frames):
    """Who reads each resolve. A stretch writes a resolved surface when the surface is its colour target (a resolve
    drawn with the blit shader, a transfer) or its copy's destination; a stretch reads it when one of its draws has a
    shader binding for it. A write nobody reads before the next write of the same surface, or before the frame ends,
    did nothing for this frame's picture (the presentation's read of the frontbuffer is in list P)."""
    totals = defaultdict(lambda: [0, 0, 0.0, 0.0])   # surface -> writes, unread writes, ms, unread ms
    count = len(frames)
    example = None
    for frame, e in frames.items():
        segs = e["segs"]
        rows = []
        for i, s in enumerate(segs):
            name = s["rc"]
            if not name.startswith(("rs:", "rsview:")):
                continue
            readers = []
            for later in segs[i + 1:]:
                if later["rc"] == name and later is not s:
                    break
                if name in later["in"].split(","):
                    readers.append(later)
            t = totals[name]
            t[0] += 1; t[2] += s["us"]
            if not readers:
                t[1] += 1; t[3] += s["us"]
            rows.append((s, readers))
        if example is None:
            example = (frame, rows)
    print()
    print(f"Resolved surfaces, per traced frame ({count} frames)")
    table([[name, f"{t[0] / count:.1f}", f"{t[2] / count / 1000:.3f}", f"{t[1] / count:.1f}", f"{t[3] / count / 1000:.3f}"]
           for name, t in sorted(totals.items(), key=lambda kv: -kv[1][2])],
          ["surface", "writes", "ms", "writes nobody reads", "ms"], "lrrrr")
    frame, rows = example
    print()
    print(f"Frame {frame}: each write and its readers")
    out = []
    for s, readers in rows:
        who = defaultdict(int)
        for r in readers:
            who[f"{r['l']}{r['i']}:{r['w']}x{r['h']}>{r['rc']}"] += r["dr"]
        text = ", ".join(f"{k} ({v} draws)" for k, v in list(who.items())[:4]) + (" ..." if len(who) > 4 else "")
        out.append([f"{s['l']}{s['i']}", s["k"], f"{s['us'] / 1000:.3f}", f"{s['w']}x{s['h']}", s["rc"],
                    s["in"] if s["k"] == "draws" else s["src"].split(":", 3)[-1], text or "nobody"])
    table(out, ["at", "kind", "ms", "size", "writes", "from", "read by (until its next write)"], "llrllll")


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def shader_names(spv_dir):
    """hash (as the back end makes it: FNV-1a 64 of the SPIR-V bytes) -> file name without .spv."""
    import os
    names = {}
    if not spv_dir:
        return names
    for name in os.listdir(spv_dir):
        if name.endswith(".spv"):
            with open(os.path.join(spv_dir, name), "rb") as f:
                names[f"{fnv1a64(f.read()):016x}"] = name[:-4]
    return names


def shader_report(frames, n, spv_dir, top):
    """Per-draw stretches (LO_GPU_PASS_DRAWS=1) added up by shader. A draw's time is the GPU time until its last
    pixel left the pipe, after the draw before it: vertex and pixel work together. Draws without a pixel shader
    (depth only) give each vertex shader's time per vertex; that time, by the vertices it shaded, is taken off the
    colour draws that use the same vertex shader (the mean of all depth-only draws when it has none), and what is
    left is the pixel shader's. The split is an estimate; the per-draw times and counts are measured."""
    names = shader_names(spv_dir)
    none = "0" * 16
    draws = [s for e in frames.values() for s in e["segs"] if s["l"] == "S" and s["k"] == "draws" and s["dr"] == 1]
    if not draws:
        print("\nno per-draw stretches (LO_GPU_PASS_DRAWS=1)")
        return
    depth_only = defaultdict(lambda: [0.0, 0, 0])
    for s in draws:
        if s["ps"] == none and s["vt"] > 0:
            d = depth_only[s["vs"]]
            d[0] += s["us"]; d[1] += s["vt"]; d[2] += 1
    all_us = sum(d[0] for d in depth_only.values())
    all_vt = sum(d[1] for d in depth_only.values())
    mean_ns = 1000 * all_us / all_vt if all_vt else 0.0
    vs_ns = {vs: 1000 * d[0] / d[1] for vs, d in depth_only.items() if d[1] >= 2000}
    vs_tot = defaultdict(lambda: defaultdict(float))
    ps_tot = defaultdict(lambda: defaultdict(float))
    for s in draws:
        v = vs_tot[s["vs"]]
        v["us"] += s["us"]; v["vt"] += s["vt"]; v["n"] += 1; v["el"] += s["el"]
        if s["ps"] == none:
            v["depth_us"] += s["us"]; v["depth_vt"] += s["vt"]
            continue
        vertex_us = s["vt"] * vs_ns.get(s["vs"], mean_ns) / 1000
        g = ps_tot[s["ps"]]
        g["us"] += s["us"]; g["pixel_us"] += max(0.0, s["us"] - vertex_us); g["fr"] += s["fr"]; g["sp"] += s["sp"]
        g["n"] += 1; g["vt"] += s["vt"]
    total = sum(s["us"] for s in draws)
    print()
    print(f"Per traced frame: {len(draws) / n:.0f} draws, {total / n / 1000:.2f} ms. Depth-only draws: "
          f"{all_us / n / 1000:.2f} ms for {all_vt / n / 1000:.0f}k vertices, {mean_ns:.1f} ns a vertex")
    print()
    print("Vertex shaders, by the time of the draws that use them")
    rows = []
    for vs, v in sorted(vs_tot.items(), key=lambda kv: -kv[1]["us"])[:top]:
        rows.append([names.get(vs, vs), f"{v['us'] / n / 1000:.3f}", f"{100 * v['us'] / total:.1f}", f"{v['n'] / n:.0f}",
                     f"{v['vt'] / n / 1000:.1f}", f"{v['el'] / n / 1000:.1f}",
                     f"{1000 * v['depth_us'] / v['depth_vt']:.1f}" if v["depth_vt"] >= 2000 else "-"])
    table(rows, ["vertex shader", "ms (its draws)", "%", "draws", "vertices k", "indices k", "ns/vertex (depth only)"], "lrrrrrr")
    print()
    print("Pixel shaders, by the time left after the vertices")
    rows = []
    for ps, g in sorted(ps_tot.items(), key=lambda kv: -kv[1]["pixel_us"])[:top]:
        rows.append([names.get(ps, ps), f"{g['pixel_us'] / n / 1000:.3f}", f"{100 * g['pixel_us'] / total:.1f}", f"{g['us'] / n / 1000:.3f}",
                     f"{g['n'] / n:.0f}", f"{g['fr'] / n / 1000:.1f}", f"{g['sp'] / n / 1000:.1f}",
                     f"{1000 * g['pixel_us'] / g['fr']:.1f}" if g["fr"] >= 1000 else "-"])
    table(rows, ["pixel shader", "ms (pixels)", "%", "ms (its draws)", "draws", "fragments k", "samples k", "ns/fragment"], "lrrrrrrr")


def table(rows, headers, aligns):
    widths = [max(len(str(r[i])) for r in [headers] + rows) for i in range(len(headers))]
    def fmt(r):
        return "  ".join(str(v).ljust(w) if a == "l" else str(v).rjust(w) for v, w, a in zip(r, widths, aligns))
    print(fmt(headers))
    print("  ".join("-" * w for w in widths))
    for r in rows:
        print(fmt(r))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--from-frame", type=int, default=0)
    ap.add_argument("--to-frame", type=int, default=0)
    ap.add_argument("--sequence", nargs="?", const=-1, type=int, default=None, help="print one frame's stretches in order")
    ap.add_argument("--merge", action="store_true", help="in --sequence, merge neighbours of the same group")
    ap.add_argument("--top", type=int, default=60, help="groups shown")
    ap.add_argument("--resolves", action="store_true",
                    help="every write of a resolved surface and the stretches that sample it before its next write")
    ap.add_argument("--shaders", action="store_true",
                    help="with LO_GPU_PASS_DRAWS=1 lines: the cost per vertex and pixel shader")
    ap.add_argument("--spv-dir", help="the pack's shaders as tools/deko3d/extract_spirv.py wrote them (vs_*.spv, ps_*.spv): "
                    "names the shaders of --shaders by file")
    args = ap.parse_args()

    frames = parse(args.log, args.from_frame, args.to_frame)
    frames = OrderedDict((f, e) for f, e in frames.items() if e["segs"] and e["ends"])
    if not frames:
        sys.exit("no gpass: lines (LO_GPU_PASS_TRACE=N) in the range")
    n = len(frames)
    counters = any(e["counters"] for entry in frames.values() for e in entry["ends"])

    totals = {f: sum(s["us"] for s in e["segs"]) for f, e in frames.items()}
    total_s = [sum(s["us"] for s in e["segs"] if s["l"] == "S") for e in frames.values()]
    total_p = [sum(s["us"] for s in e["segs"] if s["l"] == "P") for e in frames.values()]
    batch_s = [sum(x["batch_us"] for x in e["ends"] if x["l"] == "S") for e in frames.values()]
    batch_p = [sum(x["batch_us"] for x in e["ends"] if x["l"] == "P") for e in frames.values()]
    batches = [sum(1 for x in e["ends"] if x["l"] == "S") for e in frames.values()]
    segs = [len(e["segs"]) for e in frames.values()]
    truncated = sum(1 for e in frames.values() for x in e["ends"] if x["truncated"])
    mean = statistics.mean
    print(f"{n} traced frames ({min(frames)}..{max(frames)}), {mean(segs):.0f} stretches and {mean(batches):.2f} renderer batches a frame"
          + (f", {truncated} truncated lists" if truncated else ""))
    print(f"renderer batches: traced {mean(total_s) / 1000:.3f} ms a frame (min {min(total_s) / 1000:.3f}, max {max(total_s) / 1000:.3f}); "
          f"the batches' own begin-to-end time {mean(batch_s) / 1000:.3f} ms")
    print(f"presentation list: traced {mean(total_p) / 1000:.3f} ms a frame; its own begin-to-end time {mean(batch_p) / 1000:.3f} ms")
    frame_total = mean(total_s) + mean(total_p)
    print()

    # Categories.
    cats = defaultdict(lambda: defaultdict(float))
    groups = defaultdict(lambda: defaultdict(float))
    for e in frames.values():
        for s in e["segs"]:
            for key, into in ((category_of(s), cats), (group_of(s), groups)):
                g = into[key]
                g["us"] += s["us"]
                g["n"] += 1
                for field in ("dr", "cl", "bf", "bg", "bp", "inv", "pb", "sb", "dw", "sp", "fr", "vt"):
                    g[field] += s[field]
                g["area"] = s["w"] * s["h"]
    rows = []
    for key, g in sorted(cats.items(), key=lambda kv: -kv[1]["us"]):
        rows.append([key, f"{g['us'] / n / 1000:.3f}", f"{100 * g['us'] / n / frame_total:.1f}", f"{g['n'] / n:.1f}",
                     f"{g['dr'] / n:.0f}", f"{g['bf'] / n:.1f}"])
    print("Per traced frame, by kind of work")
    table(rows, ["what", "ms", "%", "stretches", "draws", "full barriers"], "lrrrrr")
    print()

    phases = defaultdict(lambda: defaultdict(float))
    for e in frames.values():
        for s, phase in zip(e["segs"], phases_of(e["segs"])):
            g = phases[phase]
            g["us"] += s["us"]
            g["n"] += 1
            for field in ("dr", "fr", "vt", "sp"):
                g[field] += s[field]
    rows = []
    for key, g in sorted(phases.items(), key=lambda kv: -kv[1]["us"]):
        row = [key, f"{g['us'] / n / 1000:.2f}", f"{100 * g['us'] / n / frame_total:.1f}", f"{g['n'] / n:.0f}", f"{g['dr'] / n:.0f}"]
        if counters:
            row += [f"{g['vt'] / n / 1000:.0f}", f"{g['fr'] / n / 1000:.0f}"]
        rows.append(row)
    rows.append(["all traced", f"{frame_total / 1000:.2f}", "100.0", f"{mean(segs):.0f}", f"{sum(g['dr'] for g in phases.values()) / n:.0f}"]
                + ([f"{sum(g['vt'] for g in phases.values()) / n / 1000:.0f}", f"{sum(g['fr'] for g in phases.values()) / n / 1000:.0f}"] if counters else []))
    print("Per traced frame, by phase of the frame")
    table(rows, ["phase", "ms", "%", "stretches", "draws"] + (["vertices k", "fragments k"] if counters else []),
          "lrrrr" + ("rr" if counters else ""))
    print()

    rows = []
    for key, g in sorted(groups.items(), key=lambda kv: -kv[1]["us"])[: args.top]:
        row = [key, f"{g['us'] / n / 1000:.3f}", f"{100 * g['us'] / n / frame_total:.1f}", f"{g['n'] / n:.1f}",
               f"{g['dr'] / n:.0f}", f"{g['cl'] / n:.1f}", f"{g['bf'] / n:.1f}", f"{g['pb'] / n:.0f}", f"{g['dw'] / n:.0f}"]
        if counters:
            area = g["area"] or 1
            row += [f"{g['sp'] / n / 1000:.0f}", f"{g['fr'] / n / 1000:.0f}", f"{g['fr'] / n / area:.2f}", f"{g['vt'] / n / 1000:.0f}",
                    f"{g['us'] * 1000 / g['fr']:.1f}" if g["fr"] else "-"]
        rows.append(row)
    headers = ["group", "ms", "%", "stretches", "draws", "clears", "fullbar", "pipe", "descr"]
    aligns = "lrrrrrrrr"
    if counters:
        headers += ["samples k", "frags k", "frags/px", "verts k", "ns/frag"]
        aligns += "rrrrr"
    print("Per traced frame, by group (what, target size, formats, [the renderer's name])")
    table(rows, headers, aligns)

    # Noted stretches (Deko3DPassTraceNote): how many, their time and their counters. LO_PRESENT_RAMP_CHECK's
    # "rampcheck" stretch draws only the pixels where the two final passes differ, so its samples count them.
    notes = defaultdict(lambda: defaultdict(float))
    for e in frames.values():
        for s in e["segs"]:
            if s["n"] != "-":
                g = notes[s["n"]]
                g["n"] += 1
                for field in ("us", "sp", "fr", "dr"):
                    g[field] += s[field]
                g["max_sp"] = max(g["max_sp"], s["sp"])
    if notes:
        print()
        print("Noted stretches (all traced frames)")
        table([[name, f"{g['n']:.0f}", f"{g['dr']:.0f}", f"{g['us'] / 1000:.3f}", f"{g['sp']:.0f}", f"{g['max_sp']:.0f}"]
               for name, g in sorted(notes.items())], ["note", "stretches", "draws", "ms in all", "samples in all", "most in one"], "lrrrrr")

    if args.shaders:
        shader_report(frames, n, args.spv_dir, args.top)
    if args.resolves:
        resolve_report(frames)

    if args.sequence is not None:
        if args.sequence > 0 and args.sequence in frames:
            pick = args.sequence
        else:
            median = statistics.median(totals.values())
            pick = min(totals, key=lambda f: abs(totals[f] - median))
        print()
        print(f"Frame {pick}: {totals[pick] / 1000:.3f} ms traced, in order")
        seq = []
        for s in frames[pick]["segs"]:
            g = group_of(s)
            if args.merge and seq and seq[-1]["g"] == g and seq[-1]["l"] == s["l"]:
                m = seq[-1]
                for field in ("us", "dr", "cl", "bf", "bg", "bp", "inv", "pb", "dw", "sp", "fr", "vt"):
                    m[field] += s[field]
                m["count"] += 1
                names = [x for x in m["in"].split(",") if x != "-"]
                names += [x for x in s["in"].split(",") if x != "-" and x not in names]
                m["in"] = ",".join(names) or "-"
            else:
                seq.append({**s, "g": g, "count": 1})
        rows = []
        at = 0.0
        for s in seq:
            at += s["us"]
            row = [s["l"], s["b"], s["i"], f"{s['us'] / 1000:.3f}", f"{at / 1000:.3f}", s["g"], s["count"], s["dr"], s["cl"],
                   f"{s['bf']}/{s['bg']}/{s['bp']}", s["pb"], s["dw"], s["in"]]
            if counters:
                row += [f"{s['sp'] / 1000:.0f}", f"{s['fr'] / 1000:.0f}", f"{s['vt'] / 1000:.0f}"]
            rows.append(row)
        headers = ["l", "b", "i", "ms", "at", "group", "n", "draws", "clr", "bar", "pipe", "descr", "samples from"]
        aligns = "lrrrrlrrrrrrl"
        if counters:
            headers += ["samples k", "frags k", "verts k"]
            aligns += "rrr"
        table(rows, headers, aligns)


if __name__ == "__main__":
    main()
