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

No. Both scenes are limited by the GPU (build made at the profile's path, traces off: town 40.28 and 40.24 FPS,
the release's 40.25; `gpu_timeline.py` over 888 town and 440 battle frames):

| GPU per frame | Town, ms | % | Battle, ms | % |
|---|---:|---:|---:|---:|
| renderer batch (what `gpu_ms` reports) | 21.11 | 85.0 | 32.56 | 92.6 |
| presentation list | 3.32 | 13.4 | 2.11 | 6.0 |
| after the presentation, before the next batch starts (swapchain decompression, fence) | 0.31 | 1.3 | 0.37 | 1.1 |
| other gaps with the next list already submitted | 0.04 | 0.2 | 0.10 | 0.3 |
| waiting for the render thread | 0.04 | 0.15 | 0.03 | 0.07 |
| frame | 24.83 | | 35.17 | |

The render thread is never what the GPU waits for: it waits itself, 3.1 ms a frame in town and 10.2 ms in battle,
for the fence of the previous presentation. Nothing waits for the swapchain image, and the present call is 0.06
ms. So there are no bubbles to remove from the submission path: every millisecond taken off the GPU's work is a
millisecond off the frame, in both scenes.

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
arithmetic instruction each.

The same table as a 256x1 `RGBA32F` texture can be read with one gather per channel: a gather whose position is the
border between texels i and i+1 returns both, unfiltered. uam compiles it to three `texgather` and the same
`sub` / `mad` as before (`out/shader-lab` translates and compiles host shaders on the PC with `UAM_IR_DEBUG=1`).
The battle's skinned vertex shaders index their bone rows the same way (48 to 57 indexed loads among 355 to 549
instructions), but there the loads cost no more than any instruction (see Shaders below).

That change is a pull request of its own, stacked on the one that brings these tools.

## The resolves: who reads them

`gpu_passes.py --resolves` lists every write of a resolved surface and the stretches whose shaders sample it before
its next write (60 town frames, 30 battle frames; the same picture in every frame):

| Surface | Writes a frame | ms (town / battle) | Writes nobody reads | ms |
|---|---:|---:|---:|---:|
| `9fa0000`, scene colour, 960x540 `RGBA16F` | 3 | 1.66 / 1.20 | 2 | **1.09 / 0.84** |
| `b0d9000`, light attenuation and UI source, `RGBA8` | 2 / 3 | 0.53 / 0.51 | 0 | |
| `aac000`, `RGBA8`, written once before the UI | 1 | 0.35 / 0.25 | 1 (not read in the next frame either) | 0.35 / 0.25 |
| shadow map and depth to `R32F` | 3 / 2.6 | 0.74 / 0.52 | 0 | |
| `714000`, the frontbuffer | 1 | 0.43 / 0.33 | 0 (the presentation reads it) | |

Two of the three scene colour resolves are overwritten before anything samples them: the one after the opaque
pass and the one before the translucent pass. The game makes them because on the Xbox 360 the EDRAM is about to be
reused (the light attenuation buffer shares the scene colour's tiles) and it draws the copy back afterwards; here
the target keeps its image and that restore draw is skipped (`LO_SKIP_RESTORE`), so its source is never read. A
scene with refraction or heat haze would read them, so they can only be dropped after the fact: when the next
whole write of the same surface arrives in the same command list and nothing has read the surface in between.

(The cropped copies, `rsview`, and the small blur surfaces they come from read as unread in the tool: it only
follows render-target textures, and the cropped views are plain textures. They are read.)

## The full barrier before the bloom chain

One stretch in each frame is 0.6 to 0.7 ms longer than its work: in the per-target trace it is a single depth draw
(0.68 / 0.73 ms where the same draw elsewhere takes 0.03 to 0.14), in other runs the first cropped copy. The
barrier trace names it: a full barrier (wait for idle, no prefetch, texture cache dropped) made of
`960x552 -> COPY_SOURCE` and `960x540 -> COPY_DEST` from `Transition`. It is the fourth scene resolve of the frame,
a repeat of the third that `ResolveOnGpu` recognises (`consecutiveResolveCopies.CanReuse`, "copy_reused") and
does not copy: but its two copy-stage transitions are still recorded, for a copy that never happens. With
`LO_RESOLVE_BLIT` (the default) nothing in that branch needs them.

A town frame has six full barriers: that one, three for the cropped copies, one at the end of the renderer's batch
(the frontbuffer to `COPY_SOURCE` for the presentation) and one at the end of the presentation list.

## Shaders (per-draw trace, `gpu_passes.py --shaders`)

Hottest by the time of their draws, town (6 frames) and battle (5 frames):

| Shader | Scene | Draws | Fragments | Time | |
|---|---|---:|---:|---:|---|
| the resolve blit (host) | town / battle | 13 / 15 | 5.8M / 6.5M | 3.81 / 3.26 ms | 0.5 to 0.7 ns a fragment: bandwidth, 16 bytes read and written per `RGBA16F` pixel |
| the EDRAM transfer (host) | battle | 3 | 1.6M | 2.42 ms | three whole-target format conversions a frame; `LO_RESOLVE_FOLD_TRANSFER` is aimed at these |
| `ps_3e26f1f33692de6e` | battle | 54 | 70,700 shaded, 4,500 pass depth | 1.49 ms | 17 ns a fragment, 94% of them rejected after shading |
| `ps_c1ee5034271b3a4a` | town | 10 | 337,000 shaded, 75,000 pass | 1.21 ms | 78% rejected after shading |
| `ps_f708ca5d075481ab` | town | 72 | 204,000 | 1.07 ms (pixels) | 5.2 ns a fragment |
| `ps_30731f7aad7cb542` | town | 5 | 126,000 | 1.03 ms | 8.2 ns a fragment |
| `ps_7c260eacff1d681d` (blur, 360x360) | both | 1 | 57,400 | 0.62 to 0.66 ms | 11 ns a fragment |

Fragments shaded and then rejected by the depth test (`gpu_passes.py --rejected`, per-draw trace with each
draw's state, 6 battle and 8 town frames; the times are upper bounds, a draw's time includes its vertices):

| Draws, by what an early test would be | Battle: draws | ms | ms in rejected fragments | Town: draws | ms | ms in rejected fragments |
|---|---:|---:|---:|---:|---:|---:|
| already tested early | 572 | 17.56 | 0 | 406 | 12.28 | 0 |
| no depth or stencil test (full-screen passes) | 18 | 5.65 | 0 | 13 | 3.88 | 0 |
| not exact: the draw writes depth and its shader can discard | 164 | 3.49 | **2.80** | 200 | 2.21 | **1.15** |
| exact: tests without depth or stencil writes | 112 | 1.56 | 0.27 | 38 | 0.41 | 0.06 |
| not exact: stencil writes | 0 | | | 13 | 0.08 | 0.02 |

The loss is in the opaque pass's masked materials (`ps_3e26f1f33692de6e`, `ps_c1ee5034271b3a4a` and others with
a guest `clip()`): depth test `>=` with depth write. Tested before the shader, a fragment the shader then
discards would already have written its depth, and whatever is drawn behind the hole afterwards would fail the
test: not the same image. Only the draws that write neither depth nor stencil could take the early test exactly
(blended draws under a stencil mask here), and they hold 0.27 ms in battle and 0.06 ms in town at most. That is
within the noise of an ABBA: not worth a change.

Vertex shaders: the prepass's skinned shader `vs_31bde3e2770db187` (355 instructions, 48 of them indexed constant
loads for the bone rows) takes 4.0 ns a vertex over 354,000 vertices a frame. That is the instruction count at
the usual rate: indexed constant loads are not slow in the vertex stage, only in the final pass's pixel shader.
The battle's vertices cost about 1.3M x 4 to 6 ns, 6 to 8 ms.

## The battle's three transfers

The first battle has a full-screen effect the town has not, and it costs 2.4 ms of transfers a frame (per-draw
trace, six frames, each 960x552):

| Step | What the game does | Here | ms |
|---|---|---|---:|
| 1 | sets the scene's tiles to the 10-bit unorm format and resolves them to a `k_2_10_10_10` texture: the 7e3 float bits read as integers | transfer 7e3 to 10-bit into the other class's texture | 1.01 |
| | | then the resolve | 0.39 |
| 2 | draws its offsets into an 8-bit target on the same tiles (one small draw in this scene), resolves it | | |
| 3 | goes back to the 10-bit target and draws the effect over the whole target, from the two textures | transfer 8-bit to 10-bit (the tiles' last owner), which the draw then covers | 0.43 |
| 4 | goes back to the 7e3 format | transfer 10-bit to 7e3 | 0.98 |

The transfer shader is one program for every pair of classes: it branches on the two classes from a constant
buffer, and its 7e3 conversions branch three times a channel. Through 7e3 it takes 1.84 to 1.90 ns a fragment
against 0.81 for the 8-bit to 10-bit pair and 0.5 to 0.7 for a plain blit. Compiled with uam off the console
(`out/shader-lab`): the general shader is 320 words of code, the 7e3 to 10-bit pair alone 192, and written
without branches (the same integer arithmetic through selects) 128; the blit is 32. A shader per pair without
branches is the same arithmetic step for step, and the 65,536 half-float inputs can be checked one by one on the
CPU. Estimated from the 8-bit pair's rate: 0.3 to 0.45 ms off each 7e3 transfer.

`LO_RESOLVE_FOLD_TRANSFER` draws step 1's transfer straight into the resolved texture (the resolve's 0.39 ms);
whether the owed transfer into the target is then ever drawn decides if that is a gain in battle (step 3 takes the
tiles over from the 8-bit class before anything reads the 10-bit target).

## The clears around the bloom chain

The Xbox 360's `Clear()` is a rectangle drawn through one view of the EDRAM tiles; every other view of those tiles
is a texture of its own here, so the renderer repeats the rectangle in each of them at once (`colour clear rect
... replayed into base=0x2d0 pitch=1280 960x68 rows`, and the depth fill that wipes every colour view). Tile base
720 has eight such views (pitches 1280, 880, 640, 480, 240 in up to three classes), and the bloom chain clears its
small targets twice a frame: each time the other views are bound one after another, the whole clear each still
owed is carried out (0.01 to 0.05 ms each) and the rectangle is drawn into it. Per-draw trace (6 battle and 8
town frames): 18 draws a frame with the clear's shaders (`vs_760aacf6212e632c`, `ps_63c971f5e9d59913`, a few rows
each) take 0.075 ms in battle and 16 take 0.062 ms in town, and the whole clears carried out right before them,
14 and 13 a frame, take 0.33 and 0.40 ms: 0.41 and 0.46 ms a frame, mostly for views that are cleared again
before anything reads them.
Kept as owed clears (the renderer already owes whole and rectangle clears per target, `pending_clear`), they would
cost nothing until a view is drawn to or read, and an owed whole clear replaces what was owed before it. That
needs the rectangle's colour on the CPU. The clear's pixel shader writes its interpolated vertex colour, so the
colour is in the rectangle's vertices, and only a colour of all zeros is certain to come out of the interpolation
and the conversion to the target's format as the same bits a clear writes; an owed rectangle of the value a view's
owed whole clear already has costs nothing at all. Anything else keeps the replay.

## Ranked candidates

| # | Change | Saves (ms a frame) | Basis | Risk to the image | How to prove it |
|---|---|---|---|---|---|
| 1 | Gamma ramp from a texture | town about 2.3, battle about 1.2 | estimated from the measured 2.8 ms and the pass without the ramp (0.52 ms) plus three gathers | none if the gather returns the two texels in the order expected: same floats, same arithmetic | `LO_PRESENT_RAMP_CHECK=1` with the pass trace's counters: the `rampcheck` stretch's samples are the differing pixels |
| 2 | Early depth test on discarding draws that write neither depth nor stencil | battle 0.27, town 0.06 at most: **no-go** | measured with each draw's state: 2.80 / 1.15 ms of the rejected fragments are on draws that write depth, where the early test changes the image | none for the 0.27 / 0.06 ms part | not pursued |
| 3 | Scene resolves nobody reads | town 1.09, battle 0.84 | measured: two of three writes of `9fa0000` have no reader before the next write | none if a write is dropped only once the next whole write is recorded and no read came between | a check mode that fills a dropped resolve with magenta instead |
| 4 | The repeated resolve's copy-stage transitions | 0.6 to 0.7 in both scenes | measured, and named by the barrier trace | none: no copy follows them | ABBA; the barrier count per frame drops from 6 to 5 |
| 5 | `LO_RESOLVE_SHARE`, `LO_RESOLVE_FOLD_TRANSFER` on by default | 0.43 in town (together); in battle the first transfer's resolve is 0.39 | measured once in town | exact by design | each alone and both, ABBA; screenshots |
| 5b | A transfer shader per pair of classes, without branches | battle 0.6 to 0.9; town none (it has no transfers) | estimated from the measured rates of the three transfers and uam's code sizes | none: the same integer arithmetic, checked for every half-float input on the CPU | the exhaustive CPU check; ABBA in battle; screenshots |
| 6 | Swapchain images without compression | 0.3 to 0.4 in both scenes | the gap after the presentation, measured; the saving is estimated | none: compression is lossless either way | ABBA |
| 7 | Cropped copies of resolved surfaces through the blit shader instead of the copy engine | about 0.2 | three copies of 321x182 with a full barrier each take 0.08 to 0.11 ms; a 59,000-pixel draw takes 0.03 | none: the same texels (as `LO_RESOLVE_BLIT`) | ABBA; screenshots |
| 8 | Colour clears replayed into the other views of the same tiles, as owed clears | battle up to 0.41, town up to 0.46 | measured: the clear-shader draws and the whole clears carried out before them (see above); how many have a zero colour is not known yet | none for a colour of all zeros with no alpha test and a full write mask; anything else keeps the replay | a check mode that does both and counts differing pixels; ABBA |
| 9 | 32-bit colour targets for the 10-bit classes (`LO_RT32`) | resolves and post passes at half the bandwidth: perhaps 2 to 3 | estimated; an earlier test at 38 ms a frame found none | changes rounding (10-bit unorm instead of FP16) and the 2-bit alpha: **not** the same image | not a candidate under the no-trade-off rule |

Ruled out, with data: skipping the cache invalidation before draws (0.10 to 0.14 ms), turning colour compression
off (+4.4 ms), bone matrices without indexed constant loads (the loads cost what any instruction costs: 4.0 ns
a vertex for 355 instructions). Looked for and not found in the 22 hottest pixel shaders of each scene: constant
buffer loads with a computed index (none), and the per-sample format fix-ups the translator writes after every
texture read (39 branches a texture in the pack's SPIR-V): every textured draw of both scenes already runs a
variant specialised for its textures' formats (`texinfo spec ... specialised=100.0%`), where they fold away. The
full-screen game shaders take 0.6 to 1.9 ns a fragment, in step with their size.

Not pursued: the GPU shades 0.58 vertices per index in battle and 0.49 in town (a mesh ordered for this GPU's
vertex cache would be nearer 0.3), and vertices are 6 to 8 ms of the battle frame. Reordering a draw's triangles
keeps a depth-only image (the nearest fragment wins in any order: prepass and shadow maps, 386,000 of the
battle's 1.31 M vertices) but not provably a colour draw's, and the work is on the CPU, at load.

Battle needs 1.85 ms off the GPU to hold 30 FPS (35.17 ms to 33.3). Candidates 1, 3, 4 and 6 add up to about 3.1
ms there, and 5b and 8 to about another 1.

## Still to measure

- The colours of the replayed clear rectangles (candidate 8).
