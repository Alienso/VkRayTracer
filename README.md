# vulkanDemo

A real-time path tracer written from scratch in C++ and Vulkan. It uses hardware ray tracing through ray queries
in a compute shader (no ray tracing pipeline), loads glTF scenes, and either accumulates a reference image or
denoises and upscales each frame with NVIDIA DLSS Ray Reconstruction.

## Screenshots

<!-- TODO: add screenshots (screenshots/ folder) -->

## Rendering features

### Path tracing
- Unidirectional path tracer in a single compute shader (`shaders/raytrace.comp`), one path per pixel per sample,
  up to 64 bounces, with Russian roulette.
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
- Optional rims.

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
differs from the model file). `scenes/` in the project has Sponza, Bistro at night and Jungle Ruins; CMake copies
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
3. Build with CMake. The shaders are compiled to SPIR-V with `glslc` as part of the build.
4. Run `vulkanDemo.exe`, or `vulkanDemo.exe scenes/sponza.json` to open a scene. Options: `--no-dlss` (don't load
   Streamline), `--dlss-cnn` (the DLSS version that still has the CNN models).

Controls: right mouse button to look around, W/A/S/D to move, Space/Shift up and down, Ctrl to move faster, F11
for fullscreen, F1 to hide the UI, Ctrl+S to save the scene.

The editor's panels (Outliner, Details, Rendering, Lighting, ...) dock around the image and can be dragged into other
places, tabbed together or closed; View > Reset layout puts them back. The layout is kept in `imgui.ini` next to the
program. The UI is Dear ImGui (docking branch, include/imgui, MIT license).
