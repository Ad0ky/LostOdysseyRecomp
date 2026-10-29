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

### Baseline: `67e92592` plus this tooling (to be measured)

Not measured yet: this section is filled by the first console run of the
procedure above (handheld, release `env.txt`, 3 counted runs per row).

| Scenario | Target | FPS | p50 ms | p99 ms | 1% low | Late % | GPU ms | GPU % | Main % | Render % | Draws |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| disc1-ship | 30 | | | | | | | | | | |
| disc1-ship | 60 | | | | | | | | | | |
| disc2-forest | 30 | | | | | | | | | | |
| disc2-forest | 60 | | | | | | | | | | |
| numara | 30 | | | | | | | | | | |
| numara | 60 | | | | | | | | | | |
