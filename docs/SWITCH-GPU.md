# Where the GPU time goes on the Switch

[Switch port](SWITCH.md) · [Benchmarks](SWITCH-BENCHMARKS.md) · [PGO](SWITCH-PGO.md)

A study of the deko3d back end's GPU time in two scenes, made on 2026-11-13 on the test console: handheld, 960x540,
stock clocks (CPU 1020 / GPU 461 / memory 1331 MHz), release `env.txt`, the 1.2.1 code (`f23f4005`) with the
tracer of this document. Town is `uhra-street` at a 60 FPS target (Uhra station square, 1,180 draws), battle is
`new-game-battle` (the first battle's command menu, about 1,260 draws).

Every number here is measured unless it says estimated. GPU times come from timestamp reports the GPU writes
itself; a repeat of the same town run gave the same scene time within 0.01 ms (20.950 and 20.953 ms).

## The tools

Two traces, off unless `env.txt` turns them on (`LostOdysseyRecomp/gpu/gpu_trace.h`):

| Switch | What it gives | Read it with |
|---|---|---|
| `LO_GPU_TIMELINE=1` | one `gtl:` line per frame: when the render thread submitted each command list, when the GPU began and finished it, every wait of the render thread, pairs of the GPU's and the CPU's clock | `tools/switch/gpu_timeline.py log.txt --from-swap N` |
| `LO_GPU_PASS_TRACE=N` | every Nth frame, one `gpass:` line per stretch of work (the draws into one framebuffer, a clear, a copy) with its GPU time, its targets and what it samples | `tools/switch/gpu_passes.py log.txt --from-frame N [--sequence --merge]` |
| `LO_GPU_PASS_COUNTERS=1` | with the pass trace: samples passed, fragment and vertex shader invocations per stretch | the same |
| `LO_GPU_PASS_DRAWS=1` | with the pass trace: every draw a stretch, with its vertex and pixel shader | `gpu_passes.py --shaders --spv-dir <the pack's .spv files>` |

A stretch's time is the time between its timestamp report and the one before, both written when the work before
them has left the pipe. The reports add no wait: the traced town frame's batch took 21.20 ms against 20.95 ms
untraced (counters on, 170 reports of four counters each).

The hooks are in code that runs once per command list or per frame, and the per-stretch marks in a command list
class of their own (`Deko3DTracedCommandList`) that the queue only makes while a trace is asked for. A first
version hooked the command processor's packet loop and the renderer's batch functions: with both traces off the
town lost 1 FPS, because GCC drops the profile of every function whose flow graph changes.

`LO_GPU_EXPERIMENT=<bits>` sizes single costs and is not for playing: 1 the final pass without the game's gamma
ramp, 16 no descriptor or shader cache invalidation before draws, 32 that invalidation without the shader caches
(16 and 32 only in traced lists).

## Two things to know before measuring anything

**`gpu_ms` leaves the presentation out.** The `perf:` line's GPU time is the renderer's batches only. The
presentation list (the scaling to 1280x720 with the game's gamma ramp) and deko3d's decompression of the swapchain
image run after them and were never counted: 3.3 ms in town, 2.5 ms in battle. Town's "85% busy" GPU was a GPU
that is busy all the time.

**A PGO build must be made at the path the profile was trained at.** GCC keys the profile of every function with
internal linkage (static, anonymous namespace: the whole renderer and command processor) by a hash that includes
the source file's path as the compiler was given it (`coverage_compute_profile_id`). A USE build of the same
sources in another worktree finds no profile for those functions, says nothing (`-Wno-coverage-mismatch`), and
comes out slower: town 39.8 FPS instead of 40.25, `GPU CmdProc` 93% of a core instead of 81%, the NRO 213 KB
larger. Mount the worktree at the training build's path in the container
(`-v "$PWD:/Users/.../.claude/worktrees/merge-34-38"` for `out/pgo-v121`). Any A/B between two builds made at
different paths measured this, not the change.

## Is the GPU ever idle?

| GPU per frame | Town, ms | Battle, ms |
|---|---:|---:|
| renderer batch (what `gpu_ms` reports) | 20.95 | 32.77 |
| presentation list | 3.31 | 2.10 |
| after the presentation, before the next batch starts (swapchain decompression, fence) | 0.10 to 0.31 | 0.36 |
| waiting for the render thread | 0.9 (see below) | 0.08 |
| frame | 25.3 | 35.4 |

Battle: the GPU is never idle (0.08 ms of 35.4). The render thread waits 6.4 ms a frame for the presentation's
fence, so the CPU has room; every millisecond off the GPU is a millisecond off the frame.

Town: these town numbers are from a build made at the wrong path (above), whose render thread needed 25 ms a
frame and so limited the frame itself: the 0.9 ms the GPU waited was for the render thread recording commands,
not for the present path. The release build's render thread needs about 20 ms (81% of 24.85 ms) and waits 3.9 ms
in the present, which is the GPU-bound pattern of the battle. Still to do with a build made at the right path:
the same timeline, to show the wait is the presentation's fence.

Neither scene shows a wait for the swapchain image or between the batch and the presentation (0.02 ms).

## A frame, phase by phase

Lost Odyssey is Unreal Engine 3: a depth prepass, the opaque scene, shadow maps projected into a light
attenuation buffer, lights and translucency, post-processing, the UI, with EDRAM resolves in between. Mean of 60
traced town frames and 29 battle frames (`gpu_passes.py`, "by phase"):

| Phase | Town ms | % | Battle ms | % |
|---|---:|---:|---:|---:|
| opaque scene (339 / 303 draws) | 6.51 | 26.7 | 7.86 | 22.3 |
| lights and translucency (97 / 413 draws) | 2.50 | 10.2 | 9.76 | 27.7 |
| depth prepass (505 / 419 draws) | 1.33 | 5.4 | 2.94 | 8.3 |
| shadow maps | 0.73 | 3.0 | 0.48 | 1.4 |
| shadow projection, single depth draws | 2.12 | 8.7 | 1.99 | 5.6 |
| resolves, colour (10 / 12 full-target draws) | 3.19 | 13.1 | 2.94 | 8.3 |
| resolves, depth and shadow maps to R32F | 0.73 | 3.0 | 0.51 | 1.4 |
| post-processing, full-screen passes | 1.65 | 6.8 | 4.33 | 12.3 |
| post-processing, bloom and depth of field chain | 1.01 | 4.1 | 0.98 | 2.8 |
| UI and 2D | 0.41 | 1.7 | 0.52 | 1.5 |
| clears (44 / 35 calls) | 0.72 | 2.9 | 0.64 | 1.8 |
| copy engine (3 cropped copies of resolved surfaces) | 0.30 | 1.2 | 0.30 | 0.9 |
| presentation (one 1280x720 draw) | 3.15 | 12.9 | 2.03 | 5.7 |
| all traced | 24.35 | 100 | 35.27 | 100 |

Vertices shaded per frame: 275,000 in town, 1,312,000 in battle (380,000 in the prepass, 397,000 in the opaque
pass, 489,000 in the light passes). The battle's prepass has no pixel shader: 2.94 ms for 380,000 vertices is 7.7
ns a vertex, so the battle spends about 10 ms of its 35 on vertices.

Targets: the scene colour target is 960x552 `RGBA16F` (8 bytes a pixel) with a `D32F S8` depth target (8 bytes a
pixel, compressed); every colour resolve of it is a 960x540 `RGBA16F` full-target draw of 0.35 to 0.69 ms, the
`RGBA8` ones 0.10 to 0.36 ms.

## What single switches showed (town, scene batch 20.95 ms, repeat 20.953)

| Switch | Batch ms | Presentation ms | Reading |
|---|---:|---:|---|
| `LO_GPU_EXPERIMENT=1` (no gamma ramp; changes the image) | 20.85 | **0.52** | the ramp is 2.8 ms of the 3.3 ms final pass |
| `LO_DEKO3D_COLOR_COMPRESSION=0` | **25.31** | 3.31 | colour compression is worth 4.4 ms: keep it |
| `LO_RESOLVE_SHARE=1 LO_RESOLVE_FOLD_TRANSFER=1` | **20.52** | 3.34 | -0.43 ms; never measured before |
| `LO_GPU_EXPERIMENT=16` (no cache invalidation before draws; unsafe) | 20.85 | 3.32 | -0.10 ms at most: not worth making safe |
| `LO_GPU_EXPERIMENT=32` (descriptor caches only) | 20.81 | 3.32 | -0.14 ms |
| swapchain images without compression (a switch of its own pull request) | 20.88 | 3.32 | nothing in a frame the CPU limits; the 0.36 ms after the presentation in battle is the target |

## The gamma ramp: why it costs 2.8 ms

The final pass (`pixelLinear`, `gpu/presentation.cpp`) scales the 960x540 frontbuffer to 1280x720 and applies the
game's display gamma ramp: per channel two entries of a 256-entry constant buffer table and a `lerp`. Compiled by
uam that is six constant buffer loads with a register index (`ld c1[%r+off]`) among about 80 instructions. Without
the ramp the pass has 30 instructions and takes 0.52 ms for 921,600 pixels; with it, 3.3 ms. Fifty more
instructions at the same rate would be 0.8 ms: the six indexed loads cost about 2 ms, some twenty times an
arithmetic instruction each. (The battle's skinned vertex shaders index their bone matrices the same way,
`XeConstants.c[5 + k]`: whether those loads are as slow there is not known. The prepass's 7.7 ns a vertex fits
the instruction count alone, which suggests not; the per-draw trace can settle it.)

The same table as a 256x1 `RGBA32F` texture can be read with one gather per channel: a gather whose position is the
border between texels i and i+1 returns both, unfiltered. uam compiles it to three `texgather` and the same
`sub` / `mad` as before (`out/shader-lab` translates and compiles host shaders on the PC with `UAM_IR_DEBUG=1`).
That change is a pull request of its own, stacked on the one that brings these tools.

## Ranked candidates

| # | Change | Saves (ms a frame) | Basis | Risk to the image | How to prove it |
|---|---|---|---|---|---|
| 1 | Gamma ramp from a texture | town about 2.3, battle about 1.2 | estimated from the measured 2.8 ms and the pass without the ramp (0.52 ms) plus three gathers | none if the gather returns the two texels in the order expected: same floats, same arithmetic | `LO_PRESENT_RAMP_CHECK=1` with the pass trace's counters: the `rampcheck` stretch's samples are the differing pixels |
| 2 | The full barrier before the bloom chain | 0.6 to 0.7 in both scenes | measured: one depth-only draw (640x640, 230,000 samples) takes 0.68 / 0.73 ms where the same draw elsewhere takes 0.03 to 0.14; it carries a full barrier (wait for idle, no prefetch) | none if the barrier is a copy-stage transition with no copy behind it | `LO_DEKO3D_BARRIER_TRACE` names the caller; then the image is untouched by construction |
| 3 | `LO_RESOLVE_SHARE`, `LO_RESOLVE_FOLD_TRANSFER` on by default | 0.43 in town (together) | measured once | exact by design (a repeated resolve shows the previous image; a transfer is drawn straight into the resolve's destination) | each alone and both, ABBA; screenshots; a check pass like the ramp's would be stronger |
| 4 | Swapchain images without compression | 0.36 in battle | the gap after the presentation, measured; the saving is estimated | none: compression is lossless either way | ABBA in battle |
| 5 | Cropped copies of resolved surfaces through the blit shader instead of the copy engine | about 0.2 | three copies of 321x182 with a full barrier each take 0.08 to 0.11 ms; a 59,000-pixel draw takes 0.03 | none: the same texels (as `LO_RESOLVE_BLIT`) | ABBA; screenshots |
| 6 | Clears of the small views at EDRAM base 720 that only receive 4 to 8 pixels | up to 0.3 | measured (640x640, 660x648 `RGBA16F` clears, 0.04 to 0.05 ms each, eight a frame) | needs care: a view may be read later | not started |
| 7 | 32-bit colour targets for the 10-bit classes (`LO_RT32`) | resolves and post passes are about half bandwidth: perhaps 2 to 3 | estimated; an earlier test at 38 ms a frame found none | changes rounding (10-bit unorm instead of FP16) and the 2-bit alpha: **not** the same image | measure it only to know; not a candidate under the no-trade-off rule |
| 8 | Bone matrices without indexed constant buffer loads | unknown: 0 to several ms in battle | only the pixel-stage measurement above | none if exact | first the per-draw trace: ns per vertex of skinned against unskinned vertex shaders |

Not worth doing, with data: skipping the cache invalidation before draws (0.10 to 0.14 ms), turning colour
compression off (+4.4 ms).

Battle needs 1.45 ms off the GPU to hold 30 FPS (35.4 ms to 33.3). Candidates 1, 2 and 4 add up to about 2.2 ms.

## Still to measure

- The town timeline and both pass traces again with a build made at the right path (the GPU numbers above do not
  depend on it; the render thread's do).
- The sampled targets per stretch (`in=`): which resolves are read before the next one overwrites them. The battle
  resolves the scene twice into `9fa0000` around one draw that writes no pixel.
- The per-draw trace in both scenes: the hottest pixel shaders by ns per fragment and fragments, the vertex shaders
  by ns per vertex.
- `LO_DEKO3D_BARRIER_TRACE` for candidate 2.
