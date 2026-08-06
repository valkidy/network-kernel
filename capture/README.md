# capture/

Output directory for capture harnesses. Everything under `locomotion_tests/` is
regenerated on every run and is git-ignored; nothing there is hand-edited.

`target_4feet.mp4` is the external reference clip (LocomotionTest) the procedural
gait is compared against by eye. It is not produced by any target here.

## Running the locomotion capture

```bash
bazel run --config=macos -c opt //engine/src/tests/kernel_tests:locomotion_capture -- --sampling=300 --path="+X"
```

Full argument documentation lives in the BUILD comment above the target in
[engine/src/tests/kernel_tests/BUILD.bazel](../engine/src/tests/kernel_tests/BUILD.bazel).
The short version:

| Argument | Default | Meaning |
|----------|---------|---------|
| `--sampling=N` | `300` | Samples to record. 300 @ 30 Hz = 10 s. |
| `--tick-rate=HZ` | `30` | Simulation rate; duration = sampling / tick-rate. |
| `--path=SCRIPT` | `+X` | Movement description, see below. |
| `--spawn=x,y,z` | `0,10,0` | Subject spawn position. |
| `--skip-plugin` | off | Native run only: no plugin staging, no parity gate. |
| `--venv=DIR` | `capture/.venv` | Virtualenv the Python steps run in. |
| `--recreate-venv` | off | Rebuild that virtualenv first. |
| `--no-venv` | off | Use the host `python3` as-is and install nothing. |

Path scripts are `;`-separated steps: an axis (`+X` `-X` `+Z` `-Z`),
`forward <n>m`, `turn <n>` degrees (`+X` toward `+Z` is positive) and
`wait <n>s`. Chinese keywords work too, with or without spaces:

```bash
bazel run --config=macos -c opt //engine/src/tests/kernel_tests:locomotion_capture -- --sampling=600 --path="前進10m; 旋轉45度; 前進5m"
```

`turn` moves the *input* direction instantly; the body yaws toward it at the
rig's `max_yaw_degrees_per_second` (45°/s for `monster_sim_actor`), so a turn
takes a couple of seconds of walking to complete. When the script runs out the
subject stands still for the remaining samples — a bare axis never runs out.

## Outputs — `capture/locomotion_tests/`

| File | Contents |
|------|----------|
| `native_raw_root.csv` | Root presentation transform per sample, built kernel |
| `native_raw_bones.csv` | 41 bone local transforms + FK world positions |
| `native_locomotion.mp4` | Skeleton animation, follow camera |
| `plugin_raw_root.csv` / `plugin_raw_bones.csv` / `plugin_locomotion.mp4` | Same, driven by the dylib + bundle shipped in `plugins/` |
| `report.txt` | Path summary, sanity checks, native ↔ plugin parity verdict |

The run exits non-zero when the Python environment is incomplete, when a capture
ends early, or when native and plugin disagree beyond tolerance.

## What the two runs mean

Both runs drive the identical public Kernel/GameServer C ABI sequence the Unity
plugin's `NetworkHost.Start` / `NetworkHost.Update` wrappers P/Invoke into. The
only difference is which `(dylib, bundle)` pair is loaded, which makes the diff
the layer-1 "native raw ↔ plugin raw" comparison the test guideline prescribes.

> The plugin's managed `Kernel`/`NetworkHost` API is a thin, Unity-independent
> P/Invoke wrapper, and no .NET/Mono runtime is available here, so the plugin
> path is exercised through the identical C ABI those wrappers call 1:1. A live
> Unity-Editor presentation test (layer 2/3: prefab hierarchy + bind pose) is
> out of scope for this target — it needs the Editor.

Two failure modes are worth knowing about, both learned the hard way:

- **Stage the bundle, not just the dylib.** Gameplay config (foothold raycast
  coverage, patrol extents, controller type) lives in the gameplay bundle. A run
  that restaged only the dylib once produced a 104° "parity failure" that was
  really a stale `bundle.bytes`. The orchestrator now stages both, and only when
  their md5 differs from the freshly built pair.
- **Compare like-for-like build flavors.** `-c opt` and fastbuild codegen differ
  by floating-point association; at a threshold-sensitive decision point (a
  patrol reversal) that split the gait phase and looked like an ABI bug. It is
  not: the same dylib run twice is byte-identical. Always capture with the same
  `-c` flag the plugin was built with (`-c opt`).

## Python prerequisites

Rendering needs `numpy` and `matplotlib`, and macOS/homebrew interpreters are
externally managed (PEP 668), so **nothing is ever installed into the host
python**. The preflight step provisions a project-local virtualenv instead:

- first run creates `capture/.venv` and installs
  [tools/capture/requirements.txt](../tools/capture/requirements.txt) into it
  (this needs network once, and takes a minute);
- later runs reuse it and only re-check that the imports resolve;
- `--recreate-venv` rebuilds it, `--venv=DIR` puts it somewhere else, and
  `--no-venv` runs against the host `python3` untouched (which then has to
  already provide both packages).

`ffmpeg` is the one thing the venv cannot supply — it comes from the host:

```bash
brew install ffmpeg
```

The preflight runs before any simulation work, so a missing piece costs a
second, not two full capture runs. On failure it writes
`locomotion_tests/preflight_error.log` naming exactly what is missing and what
to run.

## Reusing this for other captures

The recording, CSV schema, renderer and analyzer are subject-agnostic; see
[engine/src/tests/capture/transform_capture.h](../engine/src/tests/capture/transform_capture.h).
A capture of entity movement paths is the same `EntityTransformWriter` with more
than one `net_id` per sample, and `render_transforms.py` switches itself to a
top-down XZ path plot when the capture has no parented nodes.
