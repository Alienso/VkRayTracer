# vulkanDemo

A real-time path tracer written from scratch in C++ and Vulkan. It uses hardware ray tracing through ray queries
in a compute shader (no ray tracing pipeline), loads glTF scenes, and either accumulates a reference image or
denoises and upscales each frame with NVIDIA DLSS Ray Reconstruction.

## Screenshots

<!-- TODO: add screenshots (screenshots/ folder) -->

## Rendering features

### Path tracing
- Unidirectional path tracer in a single compute shader (`shaders/raytrace.comp`, its parts in `shaders/raytrace/`:
  tracing, BRDF, lights, fog, portals, ReSTIR, ...), one path per pixel per sample, up to 64 bounces, with Russian
  roulette.
- Light sampling with multiple importance sampling (MIS) for the sun, the sky and glowing surfaces. Points on
  glowing triangles are spread over the triangle's solid angle.
- A **light tree** picks which glowing triangle to sample, by power, distance and orientation, so scenes with
  thousands of small lights (Bistro at night) stay usable.
- Owen-scrambled **Sobol** points for a path's main decisions, optionally shifted per pixel by blue noise.
- **Ray cones** pick the texture mip level at every hit, and **Toksvig** filtering keeps distant normal-mapped
  surfaces from sparkling.

### Materials (glTF metallic-roughness)
- GGX microfacet reflection with visible-normal sampling and energy compensation for rough surfaces.
- Normal maps (including BC5), emissive surfaces with `KHR_materials_emissive_strength`.
- **Glass** (`KHR_materials_transmission`, `KHR_materials_ior`): thin panes and solid volumes that refract, with
  absorption (`KHR_materials_volume`). Shadow rays pass through glass, tinted.
- **Diffuse transmission** (`KHR_materials_diffuse_transmission`): leaves, paper, fabric.
- **Alpha**: mask (cut-outs) and blend (stochastic, no sorting needed).
- `KHR_texture_transform` on every texture slot; block-compressed DDS textures (`MSFT_texture_dds`).

### Lights and environment
- A sun with a configurable size and a gradient sky, or an HDR environment map with importance sampling.
- Punctual lights from glTF files (`KHR_lights_punctual`): point, spot and directional.
- Background: sky, map, black or white (a white furnace for energy tests).

### Fog and participating media
- Any closed mesh can be filled with fog: density, color and forward scattering (Henyey-Greenstein).
- Even fog, or **varying fog** from procedural 3D noise (patchy, with soft edges and drift), unbiased: delta
  tracking finds where paths scatter, ratio tracking dims the shadow rays.
- Invisible surfaces, so only the fog inside shows (the built-in "Fog box"). Fog works inside glass, inside other
  fog, through portals and in mirrors.
- Equi-angular sampling for lamps seen through fog, and an optional film-like grain where the camera sees fog.

### Portals
- Pairs of flat openings (rectangles or ellipses) placed in the editor: a ray that enters one leaves through the
  other. Light sampling reaches the sun and the punctual lights through them. Each end is an entity of its own, so
  either can be moved, or attached to something that moves.
- The camera can fly through: it comes out of the other end, seeing what it saw through the opening, with no seam
  (also in the DLSS modes: the motion vectors follow it through).
- Objects part way through are in both places: the part that went in shows at the other end (a ghost of the model,
  cut to that side), the rest stays, cut at the opening (`source/portals/PortalPieces.h`, shared with the
  physics). Through several pairs, or between ends facing each other, an object shows once per crossing, up to "Max
  portal crossings". Clicking either part selects the object; both are outlined.
- Optional rims. A pair with one end hidden is off; its visible end's rim still shows where it is.

### Noise reduction and real-time modes
- **ReSTIR DI**: reservoir resampling of direct light at the first surface, with temporal and spatial reuse and
  unbiased visibility.
- **ReSTIR GI**: resampling of the first bounce's indirect light (off by default).
- **Radiance cache**: a hash grid of surface cells holding indirect light; paths can stop after a diffuse bounce
  and read it. Fog has cells too: paths stop at their second scatter point (thick patchy fog 3x faster).
- **DLSS Ray Reconstruction** (through NVIDIA Streamline): DLAA, Quality, Balanced, Performance and Ultra
  Performance; transformer or CNN models. The shader writes the guide images DLSS needs: depth, motion vectors,
  albedos, normals, roughness and specular hit distance, including through mirrors.
- **Accumulation** mode for a converged reference image.

### Image
- Auto exposure (metered on the GPU) or manual exposure, ACES tone mapping.
- Output resolutions of 1080p, 1440p and 2160p, independent of the window.
- PNG capture of the image without the UI.

### Performance and debugging
- Specialized pipelines: each pass is compiled with only the code it needs (radiance cache updates, the ReSTIR
  passes, the image pass), and again with and without the fog code.
- **Basic shader**: an optional build of the trace shader without portals, lamps, glass, cut-outs and diffuse
  transmission (Renderer panel). About 14% faster in scenes that don't use them.
- Debug views: normals, UVs, mesh and material IDs, material channels, the radiance cache, ReSTIR statistics and
  every DLSS guide image.
- Per-pass GPU timings, and the driver's statistics for the trace shader (registers, binary size, local memory).
- GPU memory use against the driver's budget. Buffers for ReSTIR and the DLSS guides exist only while those are on,
  and vertices (28 bytes) and instances (56 bytes) are packed: Jungle Ruins fits in 8 GB at 4K. Memory comes from
  AMD's Vulkan Memory Allocator (include/vma, MIT license) in 32 MB blocks: Bistro Exterior and Interior together
  (4445 buffers and images) take 112 memory objects, well under the 4096 Vulkan promises.
- A noise measurement panel that compares frames against an accumulated reference.

## Built-in scenes and objects

The scene is a tree of entities: models, portal ends and groups. Each has a position, rotation and scale relative to
its parent, so moving a group moves everything under it; in the editor's Outliner, drag an entity onto another to attach
it there (it stays where it is).

Scenes are saved as JSON files (one line per entity, so changes show in a diff) in the `scenes` folder next to
the program: the entities, the camera, the sun and sky, and edits to the models' materials and lights (only what
differs from the model file). `scenes/` in the project has Sponza, Bistro at night, Jungle Ruins and two game scenes (below); CMake copies
them next to the program, but never over a scene already there. The JSON reader is nlohmann/json (include/nlohmann,
MIT license).

Model files and environment maps load in the background, one at a time, while the window keeps going: an entity
shows "(loading)" in the Outliner until its model arrives, and the menu bar names what is loading. When it arrives,
its upload to the GPU still holds up one frame (about 4.6 s for Jungle Ruins). A file that can't be loaded leaves its
entity without a model; the Log says why, and saving keeps the entity as the scene file had it.

The editor can add a cube, Cornell boxes (empty, with blocks, or with a point light), a mirror maze, a fog box,
a light orb and a flashlight (a spot light), besides any glTF file.

Larger scenes are prepared with the scripts in `tools/` (run through Blender's Python):
- `tools/sponza`: Intel's Sponza 2022, with textures converted to compressed DDS.
- `tools/bistro`: Amazon Lumberyard Bistro (exterior and interior) from FBX.
- `tools/jungle`: Intel's Jungle Ruins from USD, with the plants thinned out around the camera.

## Game logic

A small game layer sits beside the engine (`source/game/`, its own library linking only the Vulkan-free core): the
scene, the renderer and the editor don't know about it, and it uses the scene the way the editor does. It isn't a
general engine: one game, in C++, with one file per behaviour in `source/game/behaviours/` (how a type is made:
`source/game/Behaviour.h`).

- **Game components**: an entity can have a list of them, each a `type` and its `properties` as free-form JSON
  (Details panel: "+ Game", or the Outliner's right-click menu; the Type menu fills in the type's defaults and shows
  what it does). The scene stores and saves them without reading
  the properties; the game reads them when Play starts. A typo or a wrong kind of value is a warning in the Log,
  and the default is used. Other entities are named by their id (shown next to the name in the Details panel).
- **Play / Stop** (F5, or the menu bar): Play keeps the scene, Stop puts it back as it was, so what changes while
  playing (by the game or by hand) is undone. While playing, New, Open and Save are off. While the game runs it has
  the mouse (captured, as in a game: the mouse turns the view, W/A/S/D move, Space / Ctrl up and down, Shift faster;
  unless a camera path drives the camera) and the editor's controls are off.
- **Pause / Resume** (F6; Esc and switching to another window pause too): the game stops where it is, the mouse is
  free and the editor works again (select, move, add, delete, edit components, undo the pause's changes); Resume
  goes on from there with the changes: a bobbing or spinning thing moved by hand carries on where it was put. `--play` plays as soon as
  the scene is open.
- **Fixed steps**: the game advances 60 times a second whatever the frame rate (`source/game/GameLoop.h`), so it
  behaves the same on any machine. What moves is shown blended between the steps, so it moves smoothly at any
  frame rate (View > "Blend game steps" to compare).
- **Behaviours**: `spin`, `bob` and `slide` (turning, floating, moving out and back along an axis), `door` (E near it opens and closes it), `pickup` (taken by
  coming near, counted over the image), `trigger` (a box that shows or hides another entity while the camera is
  inside), `spawner` (copies of an entity that come and go), `cameraPath` (the game flies the camera along the
  entity's children), `collider` (a solid box: around the entity's model, or of a given size; it moves with the
  entity and isn't solid while hidden; `"solid": true` also stops it at other colliders when the game moves it,
  pushing the player but not squeezing it) and `player` (the camera as a body: W/A/S/D walk, Shift runs, Space jumps,
  the mouse turns the view; it is stopped by colliders, slides along walls, walks up low steps and gentle slopes,
  and falls under gravity), and `portalGun` (left click shoots the first end of its portal pair onto the collider at
  the view's center, right click the second; shooting again moves that end; the pair works once both ends are
  placed; a crosshair shows where it aims; on a wall an end hangs down from where it hit, or stands on the floor
  when there is too little room, so a shot at eye height can be walked into; an end goes under the entity it was
  put on and moves with it; colliders with `"portalable": false` stop the shot but take no end).
  Without a player the camera flies (game-style) and stands in for the player.
- Physics (`source/physics/`, its own library: only glm and the portal pieces) sits behind an interface
  (`physics/PhysicsWorld.h`): our own SimplePhysics (turned boxes, a capsule character) now, collision with the
  models' own triangles or a library later, without changing the game. The selected entity's collider box is
  outlined in green over the image (View > "Collider outlines": all of them), so a size can be checked by eye; one
  part way through a portal is outlined as the physics has it: cut at the opening, its part out of the other end in
  orange.
- `scenes/game_demo.json` has each of the first ones except the camera path (walk up to the door and press E,
  collect the coins and the helmet, go into the room behind the door); `scenes/game_flythrough.json` has a camera
  path around the helmet; `scenes/game_walk.json` has a player among colliders: a crate to jump on, stairs, a ramp,
  the door, a moving lift, a solid box that slides into the wall (and pushes you), and a portal pair that leads
  behind the wall; `scenes/game_portals.json` has portals: one set in a wall (walk in), a solid box sliding through
  one in a wall and back, a stick between two ends facing each other, a stick through a chain of two pairs, and
  the portal gun on the player.
  What it can't do yet: LIMITATIONS.md, "Game".
- **Portals while playing**: anything the game moves through a portal's opening (the player, a bobbing thing, a
  spawned copy, with what is under it) comes out of the other end and carries on there; the player's view and
  speed are turned with it. The player collides through portals: a wall with a portal in it has a hole there by
  itself (no colliders built around the opening), and half through, the player bumps into what is beyond the
  other end (or a collider half through it). Solid colliders too: one sliding into a portal in a wall goes in and
  comes out of the other end, stopped by what is there; a solid thing coming out into its own way stops.

## Requirements

- Windows, with a GPU that supports Vulkan ray queries (`VK_KHR_ray_query`, `VK_KHR_acceleration_structure`) and
  float atomics in buffers (`VK_EXT_shader_atomic_float`). Developed on an NVIDIA RTX 3070.
- DLSS Ray Reconstruction needs an NVIDIA RTX GPU; without one, the app runs in Accumulation mode.
- The Vulkan SDK (CMake finds it through the `VULKAN_SDK` environment variable its installer sets), CMake 3.16+, and
  a MinGW-w64 GCC toolchain.

## Building and running

1. Download the glTF sample models into `resource/models` (not in git):
   ```bash
   powershell -ExecutionPolicy Bypass -File download_models.ps1
   ```
2. Download the Streamline / DLSS DLLs into `lib/streamline` (optional, needed for DLSS):
   ```bash
   powershell -ExecutionPolicy Bypass -File download_streamline.ps1
   ```
3. Build with CMake, preferably with the Ninja generator (`-G Ninja`; CLion: Settings > Build, Execution, Deployment >
   CMake > Generator). The shaders are compiled to SPIR-V with `glslc` as part of the build. Builds have no debug
   info (faster links, a 6 MB exe instead of 97 MB); `-DVULKANDEMO_DEBUG_INFO=ON` adds it for a debugger.
4. Run `vulkanDemo.exe`, or `vulkanDemo.exe scenes/sponza.json` to open a scene. Models and HDR environments are read
   from the project's `resource/` folder (not copied next to the executable). Options:
   - `--no-dlss`: don't load Streamline (starts ~3 s faster; Accumulation only)
   - `--dlss-cnn`: the DLSS version that still has the CNN models
   - `--validation` / `--no-validation`: the Vulkan validation layer (default: on in Debug builds, off in Release)
   - `--sync-validation`: the validation layer with synchronization validation (missing or wrong barriers; slow). Four
     messages from inside DLSS (`nv.ngx.dlssd`) each time it starts are NVIDIA's, not ours.
   - `--resources <dir>`: read models/ and environments/ from another folder
   - `--shader-dir <dir>`: read the compiled shaders (.spv) from another folder (it keeps its own pipeline cache)
   - `--play`: start playing the game once the scene is open (see Game logic)
   - `--bench <file.json>` (and `--bench-out <dir>`): run a bench and exit, see below

Controls (as in Unity and Unreal):
- Flying: hold the right mouse button to look around; while it is held, W/A/S/D move, Space/Shift up and down, Ctrl
  faster.
- Selecting: click an object in the image (outlined in orange); click the same spot again to select its parent
  (a door's panel, the door, the wall). Esc selects nothing.
- Handles on the selection: Q none, W move, E rotate, R scale, X world / local axes; hold Ctrl while dragging to
  snap. F frames the selection, Ctrl+D duplicates, Delete deletes (with the mouse over the image), Ctrl+Z / Ctrl+Y
  undo / redo any change to the scene: moves, deletes, re-parenting, components, materials, lights, the environment
  (the Tools menu lists them).
- F11 fullscreen, F1 hides the UI, Ctrl+S saves the scene, F5 plays / stops, F6 (or Esc) pauses / resumes, E opens
  a door while playing.

## Benches

A bench (`benches/*.json`, format in `source/app/Bench.h`) is a scene, renderer settings, a camera (or a camera path),
warm-up and measured frame counts, frames to capture and optionally a noise measurement. `vulkanDemo.exe --bench
benches/sponza.json` runs it and writes `results.json` (GPU time per pass with mean/median/min/max, wall time per frame,
process and GPU memory, load times, the trace shader's statistics from the driver, noise results) and the captures into
`bench_results/<name>/`, then exits (exit code 1 if it failed: a typo in a setting, a scene that doesn't open, a DLSS
mode without DLSS). Frames count from the first one after the scene has loaded, and the renderer is told a fixed time
step, so captures are taken at the same point in every run.

`tools/bench.py` runs benches against one or two builds, taking turns, and prints a table of medians and spreads:
```bash
python tools/bench.py run benches/sponza.json benches/bistro_night.json --a cmake-build-release --b ../other/cmake-build-release
```
For a shader change, the same build with `--b-args "--shader-dir <folder with the new .spv files>"`. It also compares
the captures of A and B (and against stored references: `--save-reference DIR`, `--reference DIR`), with a tolerance
that passes the run-to-run differences of DLSS frames. GPU times move by about 0.4 ms between runs: use `--runs 3` (the
default) and trust differences larger than the spread. The standard benches: Sponza (Accumulation, DLAA, a noise
measurement), Bistro at night, Jungle Ruins, the fog orb and two rooms joined by portals (`scenes/test/`).

The editor's panels (Outliner, Details, Rendering, Lighting, ...) dock around the image and can be dragged into other
places, tabbed together or closed; View > Reset layout puts them back. The layout is kept in `imgui.ini` next to the
program. The UI is Dear ImGui (docking branch, include/imgui, MIT license); the move / rotate / scale handles
are ImGuizmo (include/imguizmo, MIT license).
