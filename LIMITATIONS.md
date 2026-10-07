# Known limitations

Shortcuts and approximations taken on purpose, to revisit later. Each entry: what happens, why it was
accepted, where it lives in the code, and a possible fix.

## Glass and transmission (Phase 6c)

### No caustics
Light focused by glass (the bright spot under a glass ball) is not rendered. Shadow rays pass glass in a
straight line, tinted, instead of following the bent path; solid glass therefore casts a plain tinted shadow.
A bounce ray that reaches a light through glass after a reflection is ignored, because the tinted shadow ray
already counted that light.
- Why: agreed design ("pass through & tint"); real caustics need photon mapping or similar.
- Where: `shadowTransmittance`, the `throughGlass` / `reflectedOnce` rules in `tracePath` (shaders/raytrace.comp).
- Fix: photon mapping, manifold next-event estimation, or path guiding.

### Rough glass lit by the sun sparkles
Looking through frosted glass at a light (sun or lamp) is only found when a scattered ray happens to hit the
light, so it shows fireflies that fade slowly (yellow rough sphere in TransmissionTest).
- Why: the transmission lobe is only sampled, never evaluated, so there is no light sampling through it.
- Where: `sampleBrdf` (transmission branch), `evaluateBrdf` covers reflection only.
- Fix: evaluate the rough BTDF (Walter et al. 2007) and include it in light sampling + MIS.

### Transmission energy split is approximate
The light that goes through is `(1 - reflection albedo) * transmission * base color`, averaged over the
surface's reflection, not split per microfacet; its direction follows the visible microfacets without the
shadowing/masking loss. Entering uses Schlick's Fresnel (via the reflection table), leaving uses the exact
Fresnel equations.
- Where: `sampleBrdf` (transmission weight), `sampleGlassExit`.
- Fix: a full rough dielectric BSDF with consistent Fresnel on both sides.

### Solid glass is tinted once, on the way in
The glTF reference applies the base color at every surface event; we apply it when entering (thin glass: at
every crossing). Colored solid glass is therefore lighter than in the reference.
- Where: `sampleBrdf` (weight includes albedo), `sampleGlassExit` (weight 1), `shadowTransmittance` (`entering`).

### Volumes assume closed meshes and a camera outside
Absorption is applied when a ray hits the back face of a solid material, over the distance since the previous
hit. Breaks for open meshes, a camera inside glass, nested or overlapping volumes (glass in water), and objects
inside glass. The radiance scaling by (n1/n2)^2 at interfaces is ignored (cancels when entering and leaving
the same object).
- Where: `tracePath` (absorption, `leavingGlass`).
- Fix: a medium stack along the path.

### Light trapped inside glass is cut off
Paths that bounce around inside glass (total internal reflection) end at `maxBounces`. Clear solid glass in
the white furnace reaches 93% of the background at 8 bounces, 96.6% at 16.
- Fix: no cap for transmission events (with Russian roulette), or a separate bounce budget for glass.

### Absorption units
glTF gives the attenuation distance in the file's world units; the shader converts mesh units using the
glTF node's scale (`MeshInfo.unitScale`). A mesh used by several nodes with different scales takes the first
node's scale. The volume's thickness texture is not used (not needed: we trace the real geometry).
- Where: `uploadSceneGeometry` (source/Scene.cpp).

### Shadow-ray tint uses the flat triangle normal
The Fresnel factor of glass along a shadow ray uses the triangle's flat normal and ignores roughness and
normal maps.
- Where: `shadowTransmittance`.

## Reflection model (Phase 6a)

### Energy compensation is an approximation (Turquin 2019)
The light lost by single-bounce GGX is added back by scaling the reflection lobe. The restored light keeps the
glossy lobe's shape (real multiple scattering is a bit wider) and the result is not exactly reciprocal.
- Where: `reflectionEnergy` (shader), `createSpecularAlbedoTable` (source/Materials.cpp).
- Fix: Kulla-Conty 2017 (separate multiple-scattering lobe).

### Diffuse weighting differs from the glTF reference
The diffuse layer gets `1 - (total light the reflection returns)` for the view direction, instead of the
reference's `1 - F(v.h)`, which creates energy at grazing angles (bright rims in the furnace).
- Where: `evaluateBrdf`.

### "Blur caustics" is biased
With it on (default), shiny surfaces reached after a rough bounce are treated as at least half as rough as
that bounce. Removes fireflies, but sharp caustic patterns (sun reflected by metal onto a floor) are lost.
The checkbox switches it off for comparison.
- Where: `tracePath` (path regularization).

### Directions below the real triangle end the path
A sampled direction that is above the smooth / normal-mapped normal but below the actual triangle ends the
path (that light is lost), mostly near silhouettes of normal-mapped surfaces.
- Where: `tracePath` (`side` check).

### Diffuse transmission is thin only
`KHR_materials_diffuse_transmission` is a Lambert lobe on the other side of the surface, for thin things (leaves,
paper, fabric). On a solid (`KHR_materials_volume`) material it still acts at each surface: nothing scatters
inside the volume (wax, skin, marble would need subsurface scattering). Shadow rays stop at translucent surfaces;
light gets through them only by scattering at the surface a path hits, which is right for one leaf, and a
canopy of many leaves is found by the paths themselves (slowly, so it stays noisy).
- Where: `evaluateBrdf` (l.z < 0), `sampleBrdf`, the sun and `sampleEmissiveLight` (light behind the surface).

## Lights

### Emissive texture weighting is an estimate (Phase 6d)
Each glowing triangle is weighted by its emissive texture's average, measured at load time from a grid of
1 to 256 points inside the triangle (nearest texel, first UV set). Small bright details can be missed; such a
triangle gets probability 0 and its light is then found only by bounce rays (correct, but noisier there).
Alpha (cut-out holes) is not part of the weight, and within a picked triangle the point is still chosen
uniformly, not by texture brightness. Built-in models have no emissive textures.
- Where: `computeEmissiveTextureAverages` (source/ModelLoader.cpp), `buildLightList` (source/Lights.cpp).
- Fix: a small minimum weight for every triangle, or importance-sample texels within the triangle.

### Light list and light tree are rebuilt on the CPU
Whenever an instance moves or a material changes, every emissive triangle is transformed on the CPU, the light
tree is built again over all of them, and both are uploaded. Fine for thousands of triangles (Bistro Exterior
has 20,600), slow for scenes with many glowing meshes or one moving every frame.
- Fix: build it on the GPU, or keep it per mesh and transform in the shader; refit the tree's boxes instead of
  rebuilding it when only positions change.

### The light tree doesn't know what blocks the light
The tree picks glowing triangles by a guess of how much light they could send to the point: power, distance, and
whether the surface faces them. A lamp behind a wall looks just as good as one in plain view, so a room next to a
bright street still sends many shadow rays at the street. Large lamps close to a surface still give some
speckles, as a random point on them can be very close (that's the area sampling, not the tree). Emitters are
treated as shining both ways, so there is no "which way does the light face" test in the tree. The median split
is simple; a split by cost (SAH-like, as in BVHs) would group lights better. Measured at night in Bistro Exterior
(2048-frame reference, 64 frames): street views 2.3x to 4.4x less noise, and up to 8x per unit of GPU time. Scenes
with one or a few lights are unchanged (Cornell box), and so is Bistro Interior, lit mostly by its spots. Picking
"By power" in Rendering brings back the old way, for comparison.
- Where: `buildLightTree` (source/Lights.cpp), `lightTreeImportance` / `pickFromLightTree` /
  `lightPickProbability` (shaders/raytrace.comp).
- Partly fixed by ReSTIR DI (below) at the first surface the camera sees, in the DLSS modes: its reuse keeps the
  samples that turned out visible. Deeper bounces, and Accumulation by default, still pick by the tree alone.

### Sun of size 0
A sun with angular radius 0 can only be found by shadow rays: mirrors don't show it, and the camera sees a
disc clamped to a minimum size.

### Punctual lights: all of them, or one at random
Lights from the files (KHR_lights_punctual) get a shadow ray each at every surface, up to 8 lights. With more,
one is picked at random per surface (uniformly, not by how much it matters there), so scenes with many lamps get
noisy (ReSTIR DI, below, picks among them better at the first surface). They are not in the light tree that glowing triangles have (a tree over both would fix this). They have no size: sharp shadows,
and mirrors don't show them (no ray can hit a point). When an object is scaled (or fitted to 2 units), its lights'
intensity is scaled by the square of that, so the model stays lit the same. A point light touching geometry (a bulb
inside its lamp) makes those surfaces extremely bright a few centimeters away: speckles everywhere, as paths find
them only rarely. Such files need the light moved out, or a spot that keeps the fixture outside its cone.
- Where: `samplePunctualLights` (shaders/raytrace.comp), `buildPunctualLightList` (source/Lights.cpp).

### Sky sampling doesn't know what blocks the sky
Shadow rays pick sky directions by brightness alone. For an HDR map that is what matters (its sun gets the
samples): one-frame error vs bounce rays only, Bistro Exterior ~150x lower per ms, Bistro Interior ~3x. The
gradient sky is brightest at the horizon and on the "ground" below it, which walls and floors usually block, so
many of its shadow rays are wasted. Still, "Sample the sky" defaults to On (Auto: HDR maps only; Off: never). With
Auto as the default (from "Optimized shader" on), a Sponza side aisle lit only by sky through the arches was found
by rare bounce rays alone: one frame's error 4000x higher than with sampling, and under DLSS a dim image whose
patches never settled. Measured with the gradient sky and sun, DLAA, On vs Auto: one-frame error -74% in that aisle
(+0.5 ms), -44% in Sponza's hall (+3.1 ms), -51% in Bistro Exterior (no measurable cost: ReSTIR DI takes the sky
into its reservoir at the first surface).
- Where: source/Environment.cpp (the table), `sampleEnvironment` and the sky slot of the light loop in `tracePath`.
- Fix: also weight by the surface's cosine (a few tables per normal direction), or learn which directions are
  open per region of the scene (path guiding).

### Sky behind glass: thin panes only
Through thin glass (windows) the sky's shadow rays and bounce rays share it by MIS. Behind solid (refracting)
glass only the shadow rays count it, in a straight line (as for the sun and lamps, see `shadowTransmittance`),
so a glossy surface reflecting the sky through solid glass gets fireflies. Rough thin glass bends bounce rays a
little, so MIS there is slightly approximate.

### HDR maps: one size, no filtering, two suns
The map is kept at full size as 32-bit floats (128 MB for a 4k map) without mip levels: fine for paths, which
average over many directions. The analytic Sun is independent of the map: a map with its own sun lights the scene
twice unless the Sun is switched off. Only equirectangular .hdr (Radiance) files are read (stb_image).
- Fix: estimate the map's sun (brightest cells) and offer to replace it by the analytic one; EXR via tinyexr.

### ReSTIR DI: first surface only, with shortcuts
ReSTIR DI (RESTIR_DI_PLAN.md) picks the direct light at the first surface the camera sees from many candidates,
reused from last frame and from nearby pixels. On by default in the DLSS modes, off in Accumulation (which stays
the plain path tracer, the reference; it can be switched on there). Measured at 1600x900 under DLAA, noise x GPU
time: Bistro at night −47%, by day −44%, Sponza with an HDR sky −46%; it costs 5-7 ms (Sponza) to 12 ms (Bistro)
of extra GPU time. It is unbiased (brightness within 0.02% of ReSTIR off). Its shortcuts:
- Only the first surface, and only rough ones (roughness >= 0.25, no glass): mirrors and deeper bounces keep the
  usual light sampling. Only the first sample per pixel uses it.
- The sun isn't in it: it keeps its own shadow ray everywhere (it would crowd out every other light).
- History is dropped for a frame whenever the light lists are rebuilt (any object moves, a material changes): the
  samples name lights by index. A moving glowing object keeps ReSTIR without history.
- Reuse costs shadow rays to stay unbiased (2 for last frame's samples, up to 1 per neighbor); without them, areas
  near shadows came out up to 10% too dark (Sponza). The UI's "Unbiased" boxes turn them off for comparison.
- Neighbors share samples: their errors are alike over a few pixels. Under DLSS this looks calmer (less flicker
  and ghosting than without ReSTIR), but in Bistro at night the error after 8 still frames is 10-25% higher than
  without the neighbors. A lucky bright sample can be held for up to 5 frames ("Max. history").
- Memory: about 190 bytes per render pixel (270 MB at 1600x900: reservoirs, surfaces, camera and bounce hits).
- Where: "ReSTIR DI" in `shaders/raytrace.comp` (`restirPass`, `restirTemporal`, `restirSpatial`), Rendering →
  ReSTIR DI in the UI; debug views "ReSTIR M" and "ReSTIR light".
- Fix: a shared reservoir for the sun when it is large or partly blocked. (The bounces: ReSTIR GI, below.)

### ReSTIR GI: first bounce only, slightly darker
ReSTIR GI (RESTIR_GI_PLAN.md) reuses the light that reaches the first surface via a second one: where the first
bounce went (x2) and the light found from there on (Lo), from last frame and nearby pixels. Off by default (an
option for both kinds of mode; independent of ReSTIR DI): see the last point. Measured at 1600x900 (temporal + spatial): noise after 64
frames under DLAA -21% (Bistro night), -60% (Sponza), -17% (Bistro day); in Accumulation -46..-82%. It costs
~2.5-5 ms of GPU time. Its shortcuts:
- Only the first surface ReSTIR DI would use (roughness >= 0.25, no glass); glossy surfaces keep their plain
  bounce. x2 can be any surface but glass ("Min. roughness (bounce)"): Lo of a glossy x2 depends on where it is seen
  from, and is reused as if it didn't.
- Measured before the fixes below: 0.35% (Bistro night), 0.85% (Sponza), 0.7% (Bistro day) too dark against GI
  off. Sponza's side aisle (lit only through gaps by the sky and sun) came out 14.5% too dark and flickered: 9% from
  the boiling filter (RTXDI's: it drops samples far brighter than their neighbors', which there are the ones that
  carry the light), 6% from the temporal reuse ignoring visibility (RTXDI's "basic"). Now the filter is off by
  default and the temporal reuse has DI's ray ("Unbiased"): within 0.6% there. Left: the Jacobian clamp (as RTXDI:
  a sample next to the surface that reuses it would get a huge weight). Without the filter, noise in evenly lit
  scenes is 15-20% higher.
- A GI sample's path doesn't end in the radiance cache at its second surface (it may one surface later): its Lo
  would hold the cell's value of that frame, kept and spread by the reuse; where cells are noisy, patches came and
  went.
- Lo is the light of the frame the sample was found in: a light that moves or changes brightness is followed only
  as samples age out (30 frames) or are replaced. A changed sun or sky, or rebuilt light lists, drop the history.
  An object that moves takes its x2 points with it only when they are found again.
- x2's own glow and what the bounce ray sees without a surface (sky, sun) aren't part of a sample: they stay with
  the pixel's own path (they depend on x1's light sampling).
- Rays: a sample reused from elsewhere needs a visibility ray when shaded; "Unbiased" adds one per pixel in the
  temporal reuse and up to one per neighbor in the spatial reuse.
- Memory: 120 bytes per render pixel (170 MB at 1600x900).
- Where: "ReSTIR GI" in `shaders/raytrace.comp` (the split in `tracePath`, `restirGiTemporalPass`,
  `restirGiSpatial`, `restirGiShade`), Rendering → ReSTIR GI in the UI.
- Fix: final MIS for glossy first surfaces (RTXDI); re-tracing a few samples per frame to follow changing light.
- Why off: once unbiased (boiling filter off, temporal visibility ray, no cache at x2) it lowered the noise under
  DLSS only 0-8% (Sponza aisle, Bistro night, Sponza hall) for 3-10 ms, and a rare bright sample kept for several
  frames shows as a flash (6x more than without GI in Sponza's aisle; a history of 1-2 removes them and the gain).
  The noise numbers above were measured with the boiling filter and the cache at x2. Next try: the cache at x2
  again, its value read fresh whenever a sample is used.

## Radiance cache

### The cache is biased
Paths stop at the first cell they reach after a diffuse or rough bounce: they light that point directly
(shadow rays, exact) and take the cell's averaged indirect light. A cell averages its whole area (about 32 pixels
wide on screen) and all directions, so indirect light is slightly blurred. Direct light is kept out of the cells
on purpose: with it, cells across a sun shadow's edge made Sponza 0.2..0.3% too bright. Skylight is split by its
MIS weights: the shadow rays' share stays out, the bounce rays' share goes in (a smooth sky is mostly that share,
and taking it from the cell is much less noisy than one shadow ray; a map's sun is almost all the shadow rays'
share, so its shadows stay sharp). Cells are only used by
rays longer than 2 cells (inside a corner the paths trace on). Measured against the plain path tracer (HDR):
Sponza within 0.1%, the dark corner by the basin within the measurement's ±2-3%, Cornell box -0.2%, helmet
-0.1%. Cached values come from update paths that hit the cell at any depth, so they don't carry exactly
`maxBounces` minus the depth of the path that reads them, and the light rules of glass and "Blur caustics"
follow the update path's own history.
- Where: shaders/raytrace.comp ("Radiance cache"), source/RadianceCache.cpp.
- Workaround: the "Radiance cache" checkbox gives the plain path tracer (the noise reference always uses it).

### Dark corners look brighter with the cache than without (after the same time)
Not a leak: without the cache, a dark corner of Sponza is still far from converged after 10 000 frames (each
pixel ±90%), and most of its light sits in rare bright pixels that tone mapping compresses, so on screen the
corner looks darker than it is. Its HDR average matches the cache's (see above). DLSS darkens such corners even
more without the cache (see "DLSS RR darkens Sponza's shade a little").

### The cache follows changes with a delay
Old samples fade out over ~32 frames ("History"). The cache is cleared when the sun, sky (or HDR map), bounce count, light
sampling settings or cell size change, when a model is loaded and when a material is edited; moving objects are
only followed by the fading (their old light lingers for a moment, in DLSS modes as a short glow or shadow trail).
- Where: `buildSceneUniforms` (`lightingChanged`), `recordMaterialUpdate`, `uploadSceneGeometry`.

### Fixed size, fixed cost
The table has 2^20 entries (40 MB); when the 8 slots a cell may use are taken, the cell isn't cached (its paths
just trace on). The update paths and the resolve over all entries cost ~0.3 ms even where the cache helps
little (the helmet scene: 1.27 ms instead of 1.03 ms per frame).
- Fix: resolve only the entries that were used (a list of them), size the table from the scene.

### One sample's light is clamped
Update paths clamp what one sample brings into a cell to 256 (a glint of the sun seen in a mirror would make the
cell flicker for its whole history). Biased only for very bright indirect light.

### It doesn't help the first bounce
The noise left in Sponza's shade comes mostly from whether a pixel's first bounce happens to reach the sunlit
part of the courtyard; the cache only replaces what comes after that bounce.
- Fix: ReSTIR GI (reuse first-bounce samples across pixels and frames) or path guiding; or several cheap
  first-bounce rays per pixel that end in the cache.

## Alpha (Phase 6b)

### Blend mode is noisy
See-through (blend) surfaces stop each ray with chance alpha. Correct on average, but noisy until accumulated.
- Where: `candidateIsSolid`, `shadowTransmittance`.

## Textures and models

### Mipmaps: isotropic cones, flat bounces, cut-outs at full size
Textures have mip levels (from the .dds file, or made on the CPU for PNG/JPEG) and each hit picks its level with a
ray cone (`uvFootprint`), filtered anisotropically. Simplifications:
- The cone starts at half an output pixel (sharp enough for accumulation and DLSS; a whole render pixel looked
  soft) and widens by 2 x GGX alpha per bounce (the diffuse layer as roughness 1). Curved surfaces don't widen or
  narrow it, so a curved mirror or a glass ball reads textures behind it at the wrong level.
- Cut-out tests (traversal, shadow rays), glass tints in shadow rays and points on glowing triangles still read
  the full-size level. Tried and measured for the cut-outs (Bistro Exterior street view), not worth it:
  - With the ray cone of colors, which widens at every rough bounce: -4..-7 % trace time, but bounce rays then see
    a bush as a uniform fog of leaves at its average coverage, so light leaks in between them (the bush +3 % too
    bright).
  - With one camera pixel's footprint along the path (visibility stays as sharp as at full size; at small levels
    a mask passes with chance = averaged alpha, which keeps coverage): the bush matches, but no faster (0..+6 %,
    Sponza +2 %). The full-size reads that cost are those near the camera, inside bushes, which need full size.
  Forcing every cut-out test to level 3 gave -8 %, so there is little to win there anyway.
- Normal maps get flatter at small levels (averaged bumps). Toksvig puts the lost bumpiness back as roughness
  (Rendering tab "Normal map filtering"): at load time each normal map (8-bit or BC5) gets a half-size BC4 texture
  with the spread its levels average away (`makeNormalVarianceTexture`, all of a model's on all cores: Intel Sponza
  0.9 s, Bistro 0.8 s; memory 1/8 of a BC5 map), which the shader adds to the material's alpha^2. Approximations:
  the spread is one number (round, not stretched along grooves), alpha^2 = 1 / kappa of a von Mises-Fisher fit
  was chosen by measurement (average brightness against mipmaps off: Intel Sponza +2.7 % -> -0.2 %, Sponza
  +0.2 % -> -0.2 %, Zero-Day +0.45 % -> +0.34 %), the normal map's strength scales it linearly, and a roughness
  texture of another size or UV transform is combined per hit, not per texel. Normal maps in other formats
  (BC1/BC3/BC7) get none. No measurable cost per frame. Zero-Day's white panels no longer glitter at a distance.
- Odd-sized 8-bit images drop their last row / column when halved (a tiny shift).
- Measured: Sponza trace -15 to -30 %, Bistro Exterior ~-2 % (its cost is the geometry and the cut-out foliage),
  the helmet unchanged. Texture memory +33 % (Bistro Exterior 774 -> 1043 MB).
- Where: `uvFootprint`, `traceClosest`, `sampleTexture` (shader), `generateMipLevels`, `getSampler`
  (source/Materials.cpp), `pixelSpreadAngle` (source/RayTracer.cpp), `makeNormalVarianceTexture`
  (source/ModelLoader.cpp), Rendering tab "Texture mipmaps".

### DDS textures: no CPU copy
DDS files (`MSFT_texture_dds`) in BC1/BC2/BC3/BC5/BC7 go to the GPU still compressed, with the mip levels the file
has (a DDS without them gets none: compressed blocks can't be averaged without decoding). BC5 normal maps have X
and Y only; the shader rebuilds Z.
Compressed emissive textures are not decoded on the CPU, so light sampling weights their triangles by the
emissive constant alone (noisier when only small parts glow); the Bistro converter writes PNG emissives for this reason.
- Where: `readDds` (source/ModelLoader.cpp), `toVulkan` (source/Materials.cpp), `normalTwoChannel` (shader).
- Fix: a small CPU BC1/BC3 decoder for the emissive averages.

### Bistro conversion is approximate
`tools/bistro/convert_bistro.py` turns the FBX + DDS release into glTF. Falcor sets parts of the materials in its
own scene files, so the converter guesses: every material named "glass" becomes clear glass, roughness 0 except
frosted (the files' dark base colors are made for raster blending and would turn glass black). The exception is
the Exterior's windows and doors (MASTER_Glass*): its buildings are hollow shells, so they stay opaque with their
dark base color and roughness 0, a reflection over a dark room (as in Falcor). Normal maps are
mostly DirectX (Y down); each one is tested (a real normal map has no curl) and flipped only if it isn't Y up
already. Leaves and flowers get diffuse transmission 0.5, curtains and awnings 0.3, tinted by their base color
(by name, a guess like the glass). The Interior's 4 point lights become spots pointing down
3 cm below their pendant lamps (as points inside the lamps they made speckles), at intensity 35 chosen by eye
against the app's daylight (Blender's export gave 21740 cd); the Exterior's directional light is dropped (the app
has its own sun). Emissive strength is 1 unless given.
In the Wine scene, liquids touch or sit inside their glasses (Falcor uses nested priorities), see "Volumes assume closed meshes".

### Jungle Ruins is a cut-out of the jungle
Intel's Jungle Ruins has about 8.7 million plants (USD point instancers) on 8 x 8 km of terrain: one TLAS entry
each would not fit. `tools/jungle/convert_jungle.py` keeps each kind of plant within its own distance of the
default camera: trees 400 m, seedlings 100 m, the pyramid's moss and grass all of it (90 m), grass and shrubs
60 m, terrain tiles 1 km. That leaves about 2.6 million copies of 34 plant meshes (EXT_mesh_gpu_instancing), 2.9
million TLAS instances in the app. Beyond those distances the hills are bare. The converter's options change the
distances, or keep only a share of a plant type (`--keep Moss=0.5`).
Leaves and grass get diffuse transmission 0.5 (as the Bistro's, by name); the USD materials have none and don't
use the translucency textures that come with some plants. The river is clear thin water (transmission 1, IOR
1.333). The swaying trees keep their first pose. Textures larger than 4K (the terrain's 8K) are scaled down.
- Fix: per-instance level of detail, or a TLAS with only the instances near the camera, rebuilt as it moves.

### Instances are remade only when something changes
An object's instances (one per part of its model) and their shader data are rebuilt on the CPU and uploaded
only when an object, a part's visibility or a material changed, or something moved the frame before: with
millions of instances doing it every frame would cost tens of milliseconds. Moving an object in a scene that
large is slow (every instance is remade per frame while it moves).
- Where: `updateScene` (source/Scene.cpp).

### Unsupported glTF features
- Second UV set (`TEXCOORD_1`): the first set is used instead (a warning is printed).
- KTX2 / WebP images: the texture is skipped, the constant is used.
- Specular/glossiness workflow: only the diffuse color is kept.
- Ambient occlusion and height maps are ignored on purpose (AO comes from ray tracing).
- Other material extensions (clearcoat, sheen, specular, iridescence, anisotropy, ...) are ignored.
- Cameras in the file are ignored (lights are read: KHR_lights_punctual).
- Where: source/ModelLoader.cpp.

### No OBJ or FBX loader
Only glTF / GLB is loaded; FBX scenes go through Blender first (see "Bistro conversion is approximate").

## Display

### Auto exposure meters like a camera
The exposure is measured from the finished HDR image and applied by the display pass in the same frame.
Like a real light meter, it maps the average to middle grey, so a mostly white scene
(the sunlit default floor) comes out greyer than with a hand-picked exposure; use Compensation. The
histogram covers luminance 2^-16 .. 2^12; darker pixels count as black and are not metered. While
accumulating, the measurement comes from the averaged image, so early noisy frames may meter slightly
differently from the converged result.
- Where: shaders/exposure.comp, source/Exposure.cpp (applied in shaders/display.comp).
- Fix (if it matters): a smarter metering mode.

### The noise measurement's reference has noise too
The reference is the same renderer accumulated for N frames (default 4096), so its own error is about 1/N of
one frame's. Measurements far beyond N/16 frames start to show the reference's noise (with 2048 frames, a
plain 64-frame measurement in Sponza already looked like a bias that wasn't there). The error is the
luminance's only (colour noise doesn't count), relative to the reference plus 1% of its average brightness.
In very noisy views (Sponza's dark corners) even a 8192-frame reference is ±2-3% off over an 80x180 pixel
region; compare brightness with a control run (the same renderer against its own reference). Judge brightness
by the HDR numbers, not by screenshots: tone mapping compresses unconverged bright pixels.
- Where: source/Noise.cpp, shaders/noise.comp.

## DLSS / Streamline (Phase 7)

### The NVIDIA driver applies its vsync setting once Streamline is loaded
As soon as `slInit` runs, the NVIDIA driver treats the program as a DLSS game and applies its own "Vertical
sync" setting to our window, even with the MAILBOX present mode. With the default setting, the frame rate is
capped at the monitor's refresh rate (120 FPS here, 819 without Streamline). The NVIDIA overlay ("Alt+Z")
appears at the same moment. Not the cause: the present mode (IMMEDIATE is affected the same way once vsync is
forced), DXGI or a D3D12 device in the process (tested without Streamline: no cap), the DLSS plugin or model
(also capped with no features requested), and the "Vulkan/OpenGL present method" setting.
- Workaround: NVIDIA Control Panel > Manage 3D settings > Program Settings > add `vulkanDemo.exe` >
  Vertical sync = Off. Or start with `--no-dlss` when DLSS isn't needed.
- Where: `initStreamline` (source/Dlss.cpp).

### Specular hit distance is often unknown
DLSS gets the distance from the first surface to what its reflection (or refraction) shows. It comes for free
from the path's first bounce, but only when that bounce picked the reflection; when it picked the diffuse
layer the value is 0 (unknown). Shiny surfaces get it almost everywhere, rough ones in roughly 1 of 10 pixels.
- Where: tracePath in shaders/raytrace.comp (`firstBounceSpecular`).
- Fix (if DLSS reflections smear): trace one extra reflection ray from the first hit just for the distance,
  or provide specular motion vectors instead.

### Streamline costs about 0.28 ms per frame even when unused
With Streamline loaded (vsync off), the helmet scene takes 1.57 ms per frame instead of 1.29 ms, although no
Streamline function is called per frame yet. Likely the driver's handling of "DLSS games" (its overlay and
telemetry DLLs get loaded into the process); not investigated further. Small next to the DLSS evaluation itself.
- Workaround: start with `--no-dlss` (Streamline isn't loaded at all; no DLSS modes then).
- Where: `initStreamline` (source/Dlss.cpp).

### DLSS RR darkens Sponza's shade a little
In Sponza DLSS RR comes out darker than the accumulated reference: ~7% on sunlit walls and ~17% under the
arches, where the light only arrives bounced. ACES' toe makes the shade look darker still (nearly black under
the big arch). Not changed by 4 samples per pixel, by `exposureScale` (146 vs 1), by leaving out the specular
hit distance, or by moving everything into the diffuse albedo. The other test scenes (helmet, Cornell box,
dragon, glass) match the reference within ~1/255. Probably DLSS's firefly suppression costing energy where the
noisy input consists of rare bright samples.
- Where: DLSS itself (source/Dlss.cpp only passes the inputs).
- Workarounds to try: more light paths per pixel for that light (e.g. a firefly-free estimator for the bounce
  from the sunlit floor), or exposure compensation in DLSS modes.

### The CNN models need a restart with --dlss-cnn
The CNN models (presets A-C) only exist in DLSS RR 310.3.0, the transformer F only in 310.9.1, and a process can
load only one `nvngx_dlssd.dll`. The default loads 310.9.1 (transformer D-F); `--dlss-cnn` loads 310.3.0 (CNN A-C,
transformer D-E). The model combo greys out what the loaded version doesn't have.
- Where: `initStreamline` (plugin folder `dlss/transformer` or `dlss/cnn`), `lib/streamline/`.

### NGX reports two validation errors when DLSS starts (310.9.1 only)
Each time DLSS 310.9.1 creates its resources (first frame in a DLSS mode, after a mode, model or size change),
the validation layer reports `VUID-vkCmdDraw-None-09600` for two of NGX's own images ("nv.ngx.dlssd.resource",
used in GENERAL while still UNDEFINED). Inside NVIDIA's code; calling `slAllocateResources` explicitly doesn't
change it, and 310.3.0 (`--dlss-cnn`) doesn't do it. Harmless as far as visible.

### DLSS RR is expensive on an RTX 3070
At 1600x900 output the transformer models take ~8.5 ms (DLAA), ~4.5 ms (Quality), ~3.2 ms (Performance), more
than the path tracing at those render sizes (helmet scene 0.5-1.8 ms, Sponza 1.7-7.2 ms); they are meant for
newer GPUs. The CNN models (DLSS model combo) take about half (Quality: ~2.2-2.4 ms) and look a little softer.
Tables in PHASE7_PLAN.md.

### Streamline warns that it can't hook some Vulkan commands
`Hook sl.common:Vulkan:CmdBindPipeline / CmdBindDescriptorSets / BeginCommandBuffer is NOT supported`: with
manual hooking Streamline can't track command buffer state, which is why we must restore our own bindings
after `slEvaluateFeature` (our exposure and display passes bind theirs anyway). Expected with manual hooking.

### Startup takes about 3 seconds longer
`slInit` queries NGX (the driver's DLSS runtime) for its requirements, which is slow.
