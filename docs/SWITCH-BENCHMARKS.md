# Switch benchmarks

How to measure the Switch build the same way every time, so a change can be
judged by numbers: the runs, the conditions they need, and the reference
results each optimisation is compared with.

## What a run is

A benchmark run is one game start with a benchmark `env.txt` (`LO_BENCH`). The
game:

1. presses the scenario's buttons (title screen, Continue, the save slot);
2. waits for the scene: the first frame outside a loading screen with at least
   `LO_BENCH_START_DRAWS` draws;
3. holds the scenario's stick from there, settles for `LO_BENCH_SETTLE` frames
   (150), then measures `LO_BENCH_FRAMES` frames;
4. logs one `bench result:` line, takes a screenshot
   (`bench-<name>.ppm` in the app folder) and quits.

Nothing is driven from the PC: the network log only carries the result back.
The scenario files are the PC ones (`tools/scenarios/*.toml`). Console-only
values go in their `[switch]` table (see `tools/switch/bench.py --help`).

The result line has the frame time distribution (mean, p50, p90, p95, p99,
max, standard deviation, 1% low FPS as 1000 / p99, the share of frames over
1.25× the target), hitches, where the frame time went (pacing sleep, present,
render thread idle), GPU busy time, draws and deko3d work per frame (pipeline
and state binds, descriptor writes, barriers, uploads, copies, shader compiles),
every thread's CPU share, the scene size, docked or handheld, the CPU, GPU and
memory clocks, the map, and the build. A `bench env:` line lists every `LO_*`
setting of the run.

## Conditions

Runs are only comparable when these are the same; `bench.py compare` warns when
mode, scene size, target, clocks or any `LO_*` setting differ.

- **The same console mode.** Handheld and docked run at different clocks.
  Handheld runs use the 460 MHz GPU configuration (`[apm]` line in
  `stderr.log`). Do all runs of a comparison in one mode; the reference below
  is handheld.
- **The release `env.txt`** as the base (`bench.py env` starts from
  `tools/switch/release-env.txt`). Dynamic resolution stays off: it changes the
  workload from frame to frame.
- **Warm caches.** The first start after a new build compiles and translates
  shaders and builds pipelines in the background. `bench.py watch` treats the
  first run as a warm-up (`--warmup 1`) and leaves it out.
- **The same settings.ini** (Settings menu): frame rate 30, the same
  antialiasing. The target is in every result (`target_fps`).
- **A committed build.** Results name the build's revision; a `-dirty` one
  cannot be rebuilt later to check a number.
- **The measurement's own cost**, checked once on hardware: the `perf:` line
  keeps GPU timestamps on (two per command buffer submission). One scenario
  with `LO_PERF_GPU=0` in `env.txt` against the same without shows whether
  they cost anything (its GPU figures then read `-`).
- **Three counted runs** per scenario. The report shows their median, minimum
  and maximum. A change smaller than the spread between runs is noise.

## Running

The console needs the network log, so it must be on the same network as the
PC. Copy each scenario's save slot once: `out/drive-city/save/<slot>` from the
PC goes to `sdmc:/switch/LostOdysseyRecomp/bench/save/<slot>`. Runs read their
own save folder (`LO_SAVE_DIR`) and never touch the player's saves.

```bash
python3 tools/switch/bench.py env disc1-ship --out env.txt
```

Copy `env.txt` to `sdmc:/switch/LostOdysseyRecomp/env.txt`, or upload it with
`--ftp <console IP>` when an FTP server runs on the console (ftpd, sys-ftpd;
`--ftp-user`/`--ftp-password` if it asks), which also uploads the save slot.

```bash
python3 tools/switch/bench.py watch <console IP> --runs 3
```

Start the game four times (one warm-up, three counted). Each run quits by
itself after its result. The results, the raw log and a Markdown report go to
`out/bench/<date>/`. Restore the normal `env.txt` when done.

### With the debug server

When the game runs its debug server (`LO_DEBUG_SERVER=1`, see
[SWITCH.md](SWITCH.md), "Debug server"), one command does all of the above:

```bash
python3 tools/switch/bench.py env disc1-ship --debug-server <console IP> --runs 3
```

It keeps a copy of the console's `env.txt`, uploads the benchmark one (with
`LO_DEBUG_SERVER=1`, the console's own `LO_DEBUG_RESTART`/`LO_DEBUG_PORT`, and
without `LO_BENCH_EXIT`: the game stays up for the next restart) and the save
slot (`--saves`), each checked by CRC32. Then, for each run (`--warmup`, default
1, plus `--runs`), it restarts the game, reads the session's log from the server
until `bench done`, and moves on. The raw log (`log.txt`), `results.json` and
`report.md` go to `out/bench/<date>/` (`--results-out`). Last, it puts the
console's `env.txt` back (or removes the benchmark one if there was none) and
restarts the game once more without waiting for it; `--keep-env` leaves the
benchmark `env.txt` in place. A Ctrl-C or a failed run still restores it.

Through title takeover the server refuses `restart` (it would start the game
that was taken over). `bench.py` then asks to close the game and start it again
by hand for each run, and waits for the new session.
The port defaults to 6543 (`--debug-server <IP>:<port>` for another one).

Tested only against the host stand-in, `tools/switch/debug_server_host_test.cpp`
(build line at its top; it listens on 127.0.0.1). With `LO_BENCH` in its
folder's `env.txt` it logs made-up `bench result`/`bench env`/`bench done`
lines once its scene "loads", and `takeover` after its other options makes it
refuse `restart`:

```bash
out/debug_server_host_test out/fake-sd 6543 &
python3 tools/switch/bench.py env disc1-ship --debug-server 127.0.0.1 --runs 3 --frames 120 --saves <a folder with user15/>
```

The same scenario at 60 FPS (`--fps 60`, `LO_FPS=60`) shows headroom that the
30 FPS cap hides: at 30, a faster build only shows as more pacing sleep.
Each scenario is measured at both.

### Scenarios

| Scenario | Scene | Starts at |
|---|---|---|
| `disc1-ship` | Disc 1 ship cells, walking (save slot `user15`) | 700 draws |
| `disc2-forest` | Disc 2 Crimson Forest shrine, walking (`user16`) | 700 draws |
| `numara` | City of Numara from the Disc 4 world map (`user17`) | 3,000 draws (Numara has about 3,300) |

Button stamps come from the PC and may need retuning: console loads take
longer. `watch` prints each scripted press with its frame number and time. If a
run reports `status=timeout` (the scene was never reached), move the stamps
later in the scenario's `[switch]` table. Or use `bench.py env --manual`: load
the save by hand and the run starts by itself once the scene is reached.

### Results without the network

The same lines are in the runtime log
(`state/logs/runtime-*.log`; the last 10 sessions are kept):

```bash
python3 tools/switch/bench.py parse runtime-*.log --json out/bench/manual.json
python3 tools/switch/bench.py report out/bench/manual.json --md
```

### Comparing two builds

```bash
python3 tools/switch/bench.py compare out/bench/<baseline>/results.json out/bench/<candidate>/results.json
```

prints a Markdown table of every metric with its change. Changes over 3%
(`--threshold`) are marked as a win or a loss.

## Reference results

The numbers every optimisation is compared with. Add a section per measured
build; keep the older ones.

### Baseline: `cde98786` (2026-10-29)

Switch OLED, handheld, CPU 1020 / GPU 461 / memory 1331 MHz (the 460 MHz GPU
configuration), release `env.txt` (deko3d, `LO_DEKO3D_GPU_FAST=1`, 960x540,
dynamic resolution off, `LO_PIPELINE_PREBUILD=all`) plus the debug server and
network log, settings frame rate 30. One warm-up run, then 3 counted runs;
median (min to max). Raw results: `out/bench/20261029-175212/` on the machine
that ran them.

`new-game-battle`: the first battle's command menu (Kaim, Wohl Highlands),
waiting for input. Reached from a new game with no save: START, New Game, B
(settings screen), then START and BACK skip the opening movie.

| Metric | Value |
|---|---:|
| FPS | 14.36 (14.25 to 14.36) |
| Frame time p50 / p95 / p99 | 66.5 / 87.4 / 102.7 ms |
| 1% low | 9.7 FPS |
| GPU busy | 52.2 ms per frame (75% of the time) |
| Render thread waiting for the game | 28.6 ms per frame |
| Present | 6.8 ms per frame |
| Draws (submitted) | 1,190 (1,220) per frame |
| Pipeline binds / state binds / descriptor writes | 562 / 372 / 1,380 per frame |
| Full barriers / uploads | 17.1 / 0.25 per frame |
| Threads (% of one core) | Guest Main 93, GPU CmdProc 70, XMA Decoder 7 |

Reading: the GPU alone needs 52 ms of a 33 ms frame, so at 960x540 the scene is
GPU-bound first; at a frame of about 66 ms the GPU still idles a quarter of the
time while the render thread waits 29 ms for the game's main thread (93%), so
the game thread is the next limit. Runs agree within 1%.

**What limits the scene (540 against 360 rows, build `893ac83c`, same scene and draws):**

| Render height | FPS | Frame p50 | GPU busy | Render thread waiting for the game | Guest Main |
|---|---:|---:|---:|---:|---:|
| 540 | 14.39 | 66.5 ms | 52.0 ms (75%) | 28.4 ms | 93% |
| 360 | 14.62 | 66.6 ms | 31.8 ms (46%) | 31.8 ms | 93% |

At 360 rows the GPU fits inside 33 ms and the frame doesn't change, so this scene is
limited by the game's main thread first. At 540 rows the GPU's 52 ms is the next limit:
it caps the scene near 19 FPS however fast the CPU gets. Pinning the three busiest
threads to their own cores gains under 1%.

**Since the baseline:** build `71aa681b` (CR, XER, reservation and non-argument registers
as locals) reads 15.4–15.5 FPS in the same scene, from logs rather than a benchmark run.

The scenarios with saves (`disc1-ship`, `disc2-forest`, `numara`) need their
save slots on the console; not measured yet.
