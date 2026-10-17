# vulkanDemo

Vulkan path tracer (ray queries in compute shaders) with an ImGui editor, a scene format, and a small game layer.
README.md covers features and controls. LIMITATIONS.md lists known gaps and open items; update it when you add or
fix one.

## Rules
- **Never commit.** The user makes all commits by hand.
- Ask before deleting files, especially tracked ones.
- Scratch files, test scenes and one-off benches go in the scratchpad, not the repo.
- Keep test runs short. Say how long a run will take before anything over a few minutes. Drop experiments that
  don't help.

## Layout
- `source/core`: shared types (Input.h, ...).
- `source/scene`, `source/assets`: Scene, SceneFile (JSON), AssetStore, model loading. These go into
  `vulkanDemoCore` (no Vulkan).
- `source/game`: `vulkanDemoGame` (Game, GameLoop, StepInterpolation). Links only vulkanDemoCore and runs at a
  fixed 60 Hz step. New behaviours: one file in `source/game/behaviours/` (see Behaviour.h), listed in
  Behaviours.cpp.
- `source/renderer`: Renderer, Frame, passes (Display, Selection, ...). `shaders/raytrace/*.glsl` are included
  by `raytrace.comp`.
- `source/editor`: ImGui editor, ImGuizmo gizmo, EditHistory (undo as whole-scene text snapshots).
- `source/app`: Application, CameraController.
- `include/`: vendored libraries (Catch2, ImGuizmo, ...). `resource/`: models and environments. `scenes/`, `benches/`.

## Build
- CMake with MinGW from CLion. Ninja is recommended. Release and Debug both exist; Debug has `_GLIBCXX_DEBUG` and
  validation on.
- PATH must put `C:/Program Files/JetBrains/CLion 2025.1.1/bin/mingw/bin` first. With Git's mingw64 first, the exe
  loads the wrong libstdc++ and segfaults silently.
- Builds outside the project need `--resources <project>/resource`.
- C++17. Code style: match the surrounding code, which uses plain descriptive comments.

## Checks
- Unit tests: `tests/` (Catch2) on `vulkanDemoCore` and `vulkanDemoGame`. Run `tests.exe` in **both Release and
  Debug** (the Debug build uses the checked STL). File-local helpers go in an anonymous namespace.
- For refactors that must not change output, use the capture check: deterministic benches (Accumulation, no
  radiance cache, fixed exposure, capture at frame 64) in Sponza and fog_orb. Compare to a baseline built from
  `git archive HEAD`. Captures must be byte-identical. Bistro is not deterministic. Run only this check for
  refactors (~3 min), not timing sweeps.
- Timing and image A/B: `vulkanDemo.exe --bench benches/X.json` and `python tools/bench.py run --a BUILD --b BUILD
  --runs N`. For shader variants, use `--shader-dir`. Full timing runs are only for shader or GPU-work changes.
- `--sync-validation` runs synchronization validation. The 4 messages from `nv.ngx.dlssd` at startup are
  expected.

## Shader performance (raytrace.comp, RTX 3070)
- Code size and ray-query count matter more than arithmetic. Fixed-count loops get fully unrolled at every inlined
  call site, even with `[[dont_unroll]]`. Prefer loops that stop early, and call big functions from one place only.
- Per-path state kept live slows every path, even at the same register count. Watch the driver's "Shared Memory Size"
  stat. Put new per-path state behind a specialization constant.
- Don't put a memory read in front of reads most candidates need: it turns parallel loads into a serial chain.
- Timing noise is about ±0.4 ms. Compare shaders with 3 or more interleaved runs per side, and check that the `.spv`
  hashes differ.
