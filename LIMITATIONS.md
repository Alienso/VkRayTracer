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

### Light sampling through rough glass only where nothing reflected before
Rough glass (roughness 0.1 and up) light-samples what is behind it: a thin pane, or solid glass where the path
leaves it into air (BTDF of Walter et al. 2007, MIS with the bounce). Only on the camera's side of the first
reflection, and its shadow rays stop at more glass. So these still sparkle: frosted glass seen in a mirror (the
mirror's straight shadow ray stands in for it, and counts nothing visible), and a light seen through frosted glass
and then more glass behind it (a frosted pane in front of a clear one: only bounce rays find the light).
Measured unbiased (thin and solid slab with the sun and a lamp behind it, 16k frames: within 0.2% of before,
which still sparkled at 16k).
- Why: after a reflection, its straight shadow ray already counted the light behind the glass (see "No
  caustics"); also light-sampling the glass would count it twice.
- Where: `glassLit`, `hit.lightThrough` in `tracePath`; `evaluateGlassTransmission`.
- Fix: comes with caustics (a reflection's shadow rays would have to stop at glass, too).

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

### Glass volumes: no priorities, closed meshes
Paths keep which solid glass they are in, in the fog's list of media (up to 4 volumes; see "Glass volumes" in
the shader). The glass entered last is the one the path is in; overlapping volumes are resolved by that alone,
as glTF has no priorities. For liquids modelled into their glass's wall (Bistro) the glass-to-water boundary is
then a few mm early; ice poking out of water bends at the water's surface inside the ice (1.31 vs 1.33). Meshes
must be closed. The radiance scaling by (n1/n2)^2 at interfaces is ignored (cancels when entering and leaving).
Shadow rays still add up each volume's absorption separately, so where volumes overlap both absorb. Scenes with
solid glass use the fog pipelines (the plain ones without the media were 6% faster on Sponza).
- Where: `tracePath` (`glassHere`, `insideGlass`, `passThrough`), `mediumAt`, `shadowTransmittance`.
- Fix: per-volume priorities (Schmidt & Budge 2002) set in the editor.

### Glass passes have a budget of their own
Passes through glass (in, out, reflected inside) don't count towards Max bounces, but at most 16 of them per path
(`MAX_GLASS_PASSES`). Clear solid glass in the white furnace now matches the background (before: 93% at 8
bounces). Close-ups full of glass cost more: Bistro's glass of water with ice up close took 46-60 ms instead of
33 (the old image was dark and murky there); a budget of 8 was back at 35 ms but lost up to 40% of the light in
the glass.
- Where: `glassPasses` in `tracePath`.

### DLSS guides through clear glass are approximate
Under DLSS, pixels that see smooth glass (roughness 0, transmission 0.5 and up) get the guides of what is seen
through it, as for mirrors (before, the glass's own: no diffuse albedo, the normals of a solid cylinder, so DLSS
turned Bistro's glasses into grey solid shapes, worse in motion). Through thin panes the surface behind is placed at
the whole path length along the camera ray (exact). Solid glass is a lens: there the depth and motion vectors are
the glass surface's, as nothing behind it moves like one point (at the whole length, the motion vectors jumped
wherever something else came into view behind the glass, and the view through it shifted while the camera
moved). What is seen through a lens still moves differently from the glass, so it can lag a little in
motion. The normal behind is not bent, and the tint is
the glass's color and Fresnel only, not the absorption (wine shows the guides of what is behind it). The retrace
bends at each surface as if into air, without the media (water in a glass). The glass's own reflection now shares
those guides, so DLSS may soften reflections on glass a little.
- Where: `guideClear`, `storeMirrorGuides`.

### Absorption units
glTF gives the attenuation distance in the file's world units; the shader converts mesh units using the
glTF node's scale (`MeshInfo.unitScale`). A mesh used by several nodes with different scales takes the first
node's scale. The volume's thickness texture is not used (not needed: we trace the real geometry).
- Where: `uploadSceneGeometry` (source/scene/Scene.cpp).

### Shadow-ray tint uses the flat triangle normal
The Fresnel factor of glass along a shadow ray uses the triangle's flat normal and ignores roughness and
normal maps.
- Where: `shadowTransmittance`.

## Reflection model (Phase 6a)

### Energy compensation is an approximation (Turquin 2019)
The light lost by single-bounce GGX is added back by scaling the reflection lobe. The restored light keeps the
glossy lobe's shape (real multiple scattering is a bit wider) and the result is not exactly reciprocal.
- Where: `reflectionEnergy` (shader), `createSpecularAlbedoTable` (source/renderer/Materials.cpp).
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
- Where: `computeEmissiveTextureAverages` (source/assets/ModelLoader.cpp), `buildLightList` (source/renderer/Lights.cpp).
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
bright street still sends many shadow rays at the street. (Large lamps close to a surface: see "Points on glowing
triangles by solid angle" below; that is about the point on a triangle, not the tree.) Emitters are
treated as shining both ways, so there is no "which way does the light face" test in the tree. The median split
is simple; a split by cost (SAH-like, as in BVHs) would group lights better. Measured at night in Bistro Exterior
(2048-frame reference, 64 frames): street views 2.3x to 4.4x less noise, and up to 8x per unit of GPU time. Scenes
with one or a few lights are unchanged (Cornell box), and so is Bistro Interior, lit mostly by its spots. Picking
"By power" in Rendering brings back the old way, for comparison.
- Where: `buildLightTree` (source/renderer/Lights.cpp), `lightTreeImportance` / `pickFromLightTree` /
  `lightPickProbability` (shaders/raytrace.comp).
- Partly fixed by ReSTIR DI (below) at the first surface the camera sees, in the DLSS modes: its reuse keeps the
  samples that turned out visible. Deeper bounces, and Accumulation by default, still pick by the tree alone.

### Points on glowing triangles by solid angle, without the surface's cosine
Once a glowing triangle is picked, the point on it is chosen so that its directions spread evenly over the
triangle's solid angle seen from the surface (Arvo 1995, as in pbrt-v4), for the path's shadow rays and for ReSTIR
DI's candidates. Evenly by area ("Glowing triangles by solid angle" off) puts most samples on the far part of a
large panel close to the surface, though the near part fills most of the surface's view of it. Triangles under
3e-4 sr (most of them, seen from afar) and above 6.22 sr are still picked by area: the same there, and the spherical
formulas lose precision. Checked against area sampling: the same 1024-frame references (within 0.01%) in Zero-Day
and Bistro at night. In those two scenes it makes little difference. In Zero-Day, after 64 frames, the error is 14%
lower in Accumulation and 4-8% lower under DLSS Quality. Bistro at night is better in single frames and after 8, but
7-10% worse after 64 (that error is mostly a few persistent fireflies), whatever the size limit (also tried 0.01 and
0.05 sr, so it isn't the precision). Neither scene has the case it is for: big glowing panels next to lit surfaces
(Zero-Day's light reaches most surfaces indirectly). In a numerical test of one large triangle close to a point,
the error was 7-30x lower than by area. It ignores the cosine at the surface and the emissive texture.
- Where: `sphericalSolidAngle`, `sampleSphericalTriangle`, `pointOnTriangle`, `lightPdf` (shaders/raytrace.comp).
- Fix: projected solid angle sampling (Peters 2021) adds the surface's cosine.

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
- Where: `samplePunctualLights` (shaders/raytrace.comp), `buildPunctualLightList` (source/renderer/Lights.cpp).

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
- Where: source/renderer/Environment.cpp (the table), `sampleEnvironment` and the sky slot of the light loop in `tracePath`.
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
ReSTIR DI (Bitterli et al. 2020; Wyman & Panteleev 2021 for the unbiased reuse) picks the direct light at the first surface the camera sees from many candidates,
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
- Memory: 244 bytes per render pixel (350 MB at 1600x900: reservoirs, surfaces, camera and bounce hits), only
  while ReSTIR DI or GI is on (`PerPixelBuffers`).
- Where: "ReSTIR DI" in `shaders/raytrace.comp` (`restirPass`, `restirTemporal`, `restirSpatial`), Rendering →
  ReSTIR DI in the UI; debug views "ReSTIR M" and "ReSTIR light".
- Fix: a shared reservoir for the sun when it is large or partly blocked. (The bounces: ReSTIR GI, below.)

### ReSTIR GI: first bounce only, slightly darker
ReSTIR GI (Ouyang et al. 2021; Lin et al. 2022 for the generalized MIS) reuses the light that reaches the first surface via a second one: where the first
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
- Memory: 120 bytes per render pixel (170 MB at 1600x900), only while it is on, plus ReSTIR DI's buffers (above).
- Where: "ReSTIR GI" in `shaders/raytrace.comp` (the split in `tracePath`, `restirGiTemporalPass`,
  `restirGiSpatial`, `restirGiShade`), Rendering → ReSTIR GI in the UI.
- Fix: final MIS for glossy first surfaces (RTXDI); re-tracing a few samples per frame to follow changing light.
- Why off: once unbiased (boiling filter off, temporal visibility ray, no cache at x2) it lowered the noise under
  DLSS only 0-8% (Sponza aisle, Bistro night, Sponza hall) for 3-10 ms, and a rare bright sample kept for several
  frames shows as a flash (6x more than without GI in Sponza's aisle; a history of 1-2 removes them and the gain).
  The noise numbers above were measured with the boiling filter and the cache at x2. Next try: the cache at x2
  again, its value read fresh whenever a sample is used.

## Sampling

### Sobol points for a path's main decisions only
"Sobol random numbers" (on by default) gives the camera offset, the sun's point, the sky direction, the glowing
triangle and each bounce (lobe, direction, Russian roulette) Owen-scrambled Sobol points (Burley 2020): a pixel's
samples spread evenly. Measured after 64 frames in Accumulation, the remaining error (mean squared) is 12-42% lower
with ReSTIR DI (Bistro night -42%, jungle -33%, Bistro day -18%, Sponza -12%) and 28-56% without it, for about
0.3 ms. Under DLSS Quality the gain is smaller: Bistro night -15% after 64 frames (-44% in a single frame), the
others 1-4%. The rest keeps independent random numbers: ReSTIR's candidates and reuse (the
first surface's direct light when ReSTIR DI is on, hence the smaller gain), the punctual lights, cut-outs, the
cache's update paths.
- Across the screen the error is white noise (each pixel scrambles on its own). "Sobol + blue noise" instead
  shares one scrambled sequence between all pixels, each shifted by its texel of a 64 x 64 blue-noise texture
  (Georgiev & Fajardo 2016; made at startup, BlueNoise.cpp). It measured no better: under DLSS Quality within
  +-4% of Sobol after 64 frames in all four scenes, and in Accumulation 22% worse in Bistro at night (one shared
  scramble randomizes less than one per pixel), even elsewhere. Blue noise per decision only becomes blue error
  where a single number decides a pixel's light smoothly (a soft shadow edge); here most of the noise is light
  found through several bounces, which depends on many numbers together, and the first surface's direct light
  comes from ReSTIR DI, whose candidates use white noise. Giving those candidates blue-noise numbers is the place
  it could still pay off.
- Where: `pathSample`, `startPathSamples`, `blueNoiseAt` (shaders/raytrace.comp).

### Rays leave surfaces by their rounding error
New rays start just off the surface by a fixed number of float steps of the hit's coordinates (Wachter & Binder,
Ray Tracing Gems 2019): tiny near the origin, larger far out, so the scene's size doesn't matter. The hit's
position comes from the triangle (barycentrics through the instance's matrix), not origin + t * direction, whose
error grows with the ray's length. Shadow rays towards a glowing triangle, and ReSTIR GI's visibility rays, stop
short of the far point by its own rounding error. Rays have no length limit. This replaced a fixed 1 mm offset plus
1 mm minimum distance, a 1000 m limit, and shadow rays stopping 0.1% short of the light. What changed:
- Jungle Ruins seen from 60 m up: the far hills and the river beyond 1000 m showed the sky's ground color.
- Bistro at night is 0.4% darker (4.4% on the right side of the street). 0.1% short of a lamp 15 m away skipped
  whatever lay within the last 1.5 cm before its glowing triangles, which bounce rays never skip. It is closer
  to a bounce-rays-only render now.
- Sponza: the curtains' tops are a little brighter where they touch the rod; elsewhere only noise changed.
What is left: surfaces lying on top of each other (decals, overlapping cloth) can now shadow each other, where
2 mm used to hide them. Smooth normals on coarse meshes still give the jagged shadow edge of the "shadow
terminator" problem; nothing handles that. Rays towards point lights still stop 0.1% short.
- Where: `offsetRay`, `stopShortOf`, `shadowRayStart`, `hitFromIntersection` (shaders/raytrace.comp); the
  instance's translation in `GpuInstance` (source/app/Application.h).

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
-0.1%. Cached values carry the light of `maxBounces` - 1 bounces after the cell, as for an image path that reads
them at its second surface (one that reads them later gets a few more than it would have traced), and the light
rules of glass and "Blur caustics" follow the update path's own history. Cells in fog: see "Fog".
- Where: shaders/raytrace.comp ("Radiance cache"), source/renderer/RadianceCache.cpp.
- Workaround: the "Radiance cache" checkbox gives the plain path tracer (the noise reference always uses it).

### Dark corners look brighter with the cache than without (after the same time)
Not a leak: without the cache, a dark corner of Sponza is still far from converged after 10 000 frames (each
pixel ±90%), and most of its light sits in rare bright pixels that tone mapping compresses, so on screen the
corner looks darker than it is. Its HDR average matches the cache's (see above). DLSS darkens such corners even
more without the cache (see "DLSS RR darkens light that comes as rare samples").

### The cache follows changes with a delay
Old samples fade out over ~32 frames ("History"). The cache is cleared when the sun, sky (or HDR map), bounce count, light
sampling settings or cell size change, when a model is loaded and when a material is edited; moving objects are
only followed by the fading (their old light lingers for a moment, in DLSS modes as a short glow or shadow trail).
- Where: `buildSceneUniforms` (`lightingChanged`), `recordMaterialUpdate`, `uploadSceneGeometry`.

### Fixed size, fixed cost
The table has 2^20 entries (40 MB); when the 8 slots a cell may use are taken, the cell isn't cached (its paths
just trace on). The update paths and the resolve over all entries cost ~0.3 ms even where the cache helps
little (the helmet scene: 1.27 ms instead of 1.03 ms per frame). Nearly all of it is the update paths: the resolve
alone takes ~0.03 ms (helmet) to ~0.06 ms (Sponza). In Jungle Ruins the cache pass takes ~6 ms at 1600x900, and grows
with the pixel count.
- Fix: see CACHE_PERFORMANCE_PLAN.md (cells and update budget independent of the resolution, update paths that read
  the cache, fewer or adaptive update paths); size the table from the scene.

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
  (source/renderer/Materials.cpp), `pixelSpreadAngle` (source/renderer/RayTracer.cpp), `makeNormalVarianceTexture`
  (source/assets/ModelLoader.cpp), Rendering tab "Texture mipmaps".

### DDS textures: no CPU copy
DDS files (`MSFT_texture_dds`) in BC1/BC2/BC3/BC5/BC7 go to the GPU still compressed, with the mip levels the file
has (a DDS without them gets none: compressed blocks can't be averaged without decoding). BC5 normal maps have X
and Y only; the shader rebuilds Z.
Compressed emissive textures are not decoded on the CPU, so light sampling weights their triangles by the
emissive constant alone (noisier when only small parts glow); the Bistro converter writes PNG emissives for this reason.
- Where: `readDds` (source/assets/ModelLoader.cpp), `toVulkan` (source/renderer/Materials.cpp), `normalTwoChannel` (shader).
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
- Where: `Scene::buildRenderList` (source/scene/Scene.cpp), `updateScene` (source/renderer/SceneGpu.cpp).

### Loading and unloading a model stalls a frame
Deleting the last object of a model keeps the model loaded, unused, so adding it again is instant; unused models are
unloaded (least recently used first) once they take more than 1 GiB together, or when a new model needs their
texture slots. Loading or unloading still waits for the GPU and remakes the material buffer, the mesh infos, the
instances, the light list and the radiance cache. The geometry isn't re-packed any more: each mesh keeps its range
in the shared vertex/index buffers (only new meshes are copied, freed ranges are reused, a full buffer grows on the
GPU). Sponza loaded, a small model removed or added: ~70 ms before, ~45 ms now, ~16 ms for a kept model.
Reading and decoding the file (and making the textures' mip levels) happens on a worker thread, so only the arrival
stalls: Jungle Ruins' upload and BLAS builds take one frame of ~4.6 s (Debug), after 5 s of loading during which the
window keeps going.
- The unused models' budget is 1 GiB (`AssetStore::defaultUnusedBudget`), less while the GPU memory left (the
  driver's budget, VK_EXT_memory_budget) is under an eighth of it (`unusedModelBudget` in Application.cpp); the
  models' sizes are estimates (no BLASes). Freed ranges can fragment the buffers (they only grow, never shrink).
- The wait: `GpuScene::updateScene` (step 1) calls `waitIdle()` whenever `AssetStore::revision()` moved, because
  unloading shifts the store's lists down (`AssetStore::unload` renumbers the later models' meshes, materials,
  textures and parts; tests/AssetStoreTests.cpp pins that). BLASes, textures and geometry ranges survive it (matched
  by mesh / texture id); the material buffer, mesh infos, texture descriptors and the TLAS are remade.
- The texture array has a fixed size, 1024 (`GpuScene::maxTextures`); the shader's array is unsized already, so
  only the descriptor set layout and pool would read the device's limit instead.
- Why it stays: in the editor loads are rare and asked for; one stall then isn't noticed. It matters once models or
  LODs stream in and out while the camera moves, or loads happen during a measured bench.
- Fix (stable slots, ~2-3 days): the store's meshes / materials / textures in fixed slots with a free list (a slot
  is freed, not erased: no renumbering); mesh infos and materials updated slot by slot in the frame's command buffer
  (as instance ranges already are, StreamedBuffer) instead of remade; texture descriptors written per slot (while
  frames are in flight that likely needs UPDATE_AFTER_BIND / PARTIALLY_BOUND); releases through DeferredDeletion as
  now. Then the waitIdle goes; the AssetStore test "unloading moves ... down" becomes "keeps its slots". Do
  maxTextures with it. Beyond that: upload over several frames (a transfer queue, BLAS builds spread out).
- Where: `AssetStore::release` / `trimUnused` / `unload` (source/assets/AssetStore.cpp), `updateScene` /
  `syncSceneGeometry` (source/renderer/SceneGpu.cpp).

### Unsupported glTF features
- Second UV set (`TEXCOORD_1`): the first set is used instead (a warning is printed).
- KTX2 / WebP images: the texture is skipped, the constant is used.
- Specular/glossiness workflow: only the diffuse color is kept.
- Ambient occlusion and height maps are ignored on purpose (AO comes from ray tracing).
- Other material extensions (clearcoat, sheen, specular, iridescence, anisotropy, ...) are ignored.
- Cameras in the file are ignored (lights are read: KHR_lights_punctual).
- Where: source/assets/ModelLoader.cpp.

### No OBJ or FBX loader
Only glTF / GLB is loaded; FBX scenes go through Blender first (see "Bistro conversion is approximate").

## Display

### Image size bigger than the window: a plain shrink, and per-pixel settings
"Image size" (Rendering) makes the image 1080, 1440 or 2160 rows tall, as wide as the window's shape, for Capture
(a 4K PNG of a 1600 x 900 window). Everything per pixel grows with it: 2160 rows cost about 5.8x the time and
memory of 1600 x 900 (a heavy scene's single frame can then near Windows' 2-second GPU timeout; keep samples per
pixel at 1 and let it accumulate). The window shows it shrunk by a linear blit, which only blends 2 x 2 pixels:
fine edges shimmer in the preview, not in the capture. The radiance cache's cells stay sized for the window (else
they would get smaller and many more); ReSTIR's spatial radius stays in render pixels, so it covers less of the
scene at a bigger size.
- Where: `chooseExtents`, `applyOutputSize` (source/renderer/Display.cpp), the blit in `recordRayTracing` and
  `cacheCellScale` in `buildSceneUniforms` (source/renderer/RayTracer.cpp).
- Fix (preview): a proper downscale filter (a compute pass averaging all covered pixels).

### Auto exposure meters like a camera
The exposure is measured from the finished HDR image and applied by the display pass in the same frame.
Like a real light meter, it maps the average to middle grey, so a mostly white scene
(the sunlit default floor) comes out greyer than with a hand-picked exposure; use Compensation. The
histogram covers luminance 2^-16 .. 2^12; darker pixels count as black and are not metered. While
accumulating, the measurement comes from the averaged image, so early noisy frames may meter slightly
differently from the converged result.
- Where: shaders/exposure.comp, source/renderer/Exposure.cpp (applied in shaders/display.comp).
- Fix (if it matters): a smarter metering mode.

### The noise measurement's reference has noise too
The reference is the same renderer accumulated for N frames (default 4096), so its own error is about 1/N of
one frame's. Measurements far beyond N/16 frames start to show the reference's noise (with 2048 frames, a
plain 64-frame measurement in Sponza already looked like a bias that wasn't there). The error is the
luminance's only (colour noise doesn't count), relative to the reference plus 1% of its average brightness.
In very noisy views (Sponza's dark corners) even a 8192-frame reference is ±2-3% off over an 80x180 pixel
region; compare brightness with a control run (the same renderer against its own reference). Judge brightness
by the HDR numbers, not by screenshots: tone mapping compresses unconverged bright pixels.
- Where: source/renderer/Noise.cpp, shaders/noise.comp.

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
- Where: `initStreamline` (source/renderer/Dlss.cpp).

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
- Where: `initStreamline` (source/renderer/Dlss.cpp).

### DLSS RR darkens light that comes as rare samples
Where a pixel's light depends on a shadow ray that only sometimes gets through, DLSS RR loses part of it on its
first frame (no history) and gets it back only slowly as the history grows (a still camera: ~1000 frames). Sponza's
side aisle (scenes/sponza.json, DLSS Quality, whole image vs the accumulated reference): 93.9% on the first frame,
96.7% after 64. It is the noise, not the guides: 4 samples per pixel 99.0%, 16 100.4%. The light there is nearly
all sky through the arches (direct light only: 90.6%); the bounce light and the cache hardly matter. ReSTIR DI made
it worse: one shadow ray towards the reservoir's sample gave a pixel all of the sky or none (ReSTIR DI off: 96.2%,
99.6% after 64, but noisier). So half of the first surface's skylight now comes from the usual light sampling
(`RESTIR_SKY_SHARE`): 96.6%, 98.6% after 64, the same error and cost; unbiased (accumulated: 100.1%).
- Not better: 4 BRDF candidates (94.3%), the sky only through BRDF candidates, spatial or temporal reuse off,
  a bounce ray of its own instead of ReSTIR's BRDF candidate, a 75/25 split. 8 sky candidates: 94.6% (+0.4 ms).
- Earlier, also no change: `exposureScale` (146 vs 1), no specular hit distance, everything in the diffuse albedo.
- Still there: Jungle Ruins 94.3%, 96.6% after 64, from sunlight through the foliage (direct light only: 93.3%;
  4 samples per pixel: 97.4%), which this doesn't touch; scenes/bistro_night.json 96.7%, 97.7% after 64.
- Where: DLSS itself; `RESTIR_SKY_SHARE` in the shader.
- Fix to try: more shadow rays at the first surface for partly blocked light (two towards the sun).

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

### Streamline warns that it can't hook some Vulkan commands
`Hook sl.common:Vulkan:CmdBindPipeline / CmdBindDescriptorSets / BeginCommandBuffer is NOT supported`: with
manual hooking Streamline can't track command buffer state, which is why we must restore our own bindings
after `slEvaluateFeature` (our exposure and display passes bind theirs anyway). Expected with manual hooking.

### Startup takes about 3 seconds longer
`slInit` queries NGX (the driver's DLSS runtime) for its requirements, which is slow. Measured (Sponza, Release, warm
pipeline cache): 4.6 s to the first frame with DLSS, 1.6 s with `--no-dlss`. Streamline init 2.0-2.5 s (~1.5 s of it
NGX checking the DLSS DLL and the GPU), and device creation ~1 s slower with it. Done on every start besides: the fog
noise (128^3, on the CPU, 0.35-0.5 s) and the specular albedo table (0.14 s).
- Fix: the fog noise only for scenes with fog, or both on threads (~0.6 s); Streamline init on a thread meanwhile
  (~0.2-0.7 s); or DLSS loaded only when a DLSS mode is picked (~3 s, but that needs the device made again, or a
  restart). Jungle Ruins' first frame (3.6 s in updateScene: first upload, BLAS builds) isn't broken down yet.
- Where: `Dlss::initStreamline` (source/renderer/Dlss.cpp), `Renderer::init`.

### Leaving a DLSS mode keeps ~326 MB of video memory
After switching from a DLSS mode to Accumulation, the program uses ~326 MB more than when it starts in
Accumulation (1600x900, after DLAA: 647 MB instead of 321 MB), and it stays that way. Our own resources are the
same in both cases (VMA: 55 memory objects, 68 buffers and images), and `slFreeResources` is called on every mode
switch (`freeDlssResources`). So it's Streamline / NGX keeping memory of its own (its model or work buffers, perhaps
to come back quickly). Not investigated further.
- Where: `applyRenderMode` / `freeDlssResources` (source/renderer/Dlss.cpp).
- To try: whether it grows with the output size or the model, `slFreeResources` for every viewport and feature, or
  `slSetFeatureLoaded(false)` for DLSS RR while Accumulation is on.

## Portals

### Only the sun and punctual lights are light-sampled through portals, through one portal
Shadow rays to a light go through a portal only towards its image through that portal (light images: the sun and
the punctual lights, through one portal), in image and cache update paths alike. Glowing surfaces and the sky
behind a portal, and the sun behind two or more, are found by bounce rays: at rough opaque surfaces that see an
opening, half of them aim through one (`PORTAL_BOUNCE_SHARE`; an end picked by about its solid angle, a point on it
evenly), the pdf the mix of both strategies, so unbiased (the references with and without agree within 0.01%).
Test scene, a closed dark room with a window to the sky and one into a lit room (Accumulation, error after one
frame / 16): sky only 42 / 3.0 -> 17 / 1.1 (6.2 -> 7.4 ms), the lit room only 74 / 6.9 -> 40 / 4.7 (4.1 -> 5.7 ms).
- The aim doesn't know what is in the way: an opening behind a wall, or in another room, takes its share of the
  bounce rays for nothing. Not at the surfaces where paths end in the cache (their light comes from the cell),
  nor in fog, glass or leaves (diffuse transmission).
- The sun behind two portals is still found only by chance (rarely: its disc is small).
- Where: `portalLightImage`, `samplePortalDirection`, `portalDirectionPdf`, `shadowTransmittance` (`throughEnd`),
  `portalCrossings` in `tracePath`.
- Fix to try: aim by the solid angle of the whole opening (spherical rectangles, Urena et al. 2013) instead of
  points spread evenly over its area; images of glowing triangles through the light tree from the moved point.

### The radiance cache's update paths had no light images (fixed 2026-10-15)
They found the sun through a portal only when a bounce ray hit its disc, a sample far above the cache's limit of
256 (see "One sample's light is clamped"), and the punctual lights not at all: the test room lit by the sun through
a window came out 41% too dark (59%), lit by a point light in the room next door 18% (82%). With the light images
(the shadow ray's second part in update paths too): 100% and 98%, the update pass still at 128 registers. It costs
~0.1 ms of the cache pass in scenes without portals (Sponza: 0.69 -> 0.82 ms; the image pass unchanged).

### No ReSTIR for pixels seen through a portal
A pixel whose camera ray went through a portal samples its lights as without ReSTIR, and a bounce through a
portal gives no ReSTIR GI sample: both reuse samples along straight lines between surfaces.
- Where: `PRIMARY_THROUGH_PORTAL`, `BOUNCE_RETRACE`, `crossedPortal` (shaders/raytrace.comp).

### Lost after "Max portal crossings"
A ray that goes through portals more often than "Max portal crossings" (8 by default; two ends facing each other)
brings back no light: the deep end of such a tunnel is black. The camera moving through portals takes at most 8 in
one frame's step.
- Where: `scene.maxPortalCrossings` in `intersectClosest`, `Scene::moveCameraThroughPortals`.

### Geometry within 1 mm of an opening's plane is skipped there
So that a portal set into a wall shows its view, not the wall, rays stop 1 mm before an opening and start 1 mm
after its partner.
- Where: `PORTAL_GAP`.

### DLSS through portals: normals, and portals seen in mirrors
Through a portal, depth and motion vectors are for where the surface appears. Moving portals and objects moving behind
one are handled: last frame's position comes back through the openings as they were then (`RenderPortal::previous`,
`seenThroughPortalsLastFrame`; before, the view through a moving end had no motion and DLSS smeared it). The rims move
with their ends too (each bar has its own motion, from last frame's same bar: found by its end and its number, as a
rim turned on or off moves the others in the list). Left:
- The normal guide is the surface's real one, not turned like the view.
- A portal seen in a mirror or through clear glass: its view is reprojected as if everything there stood still.
- A pair turned on or off: DLSS starts from what was there before (the wall), like any sudden change.
- Where: `storeSurfaceGuides`, `seenThroughPortalsLastFrame` (dlss_inputs.glsl).

### Objects half through a portal: what is drawn
A model going into an opening is cut there and shown again at the other end (Scene::findPortalPieces, ghosts at the
render list's end, the cut in the shader: "Portal pieces" in tracing.glsl).
- Only when where it crosses the opening's plane lies all within the opening: a model crossing it partly beside the
  opening (half in the wall around it, or a room the portal is in) isn't cut, as before.
- Ghosts don't light anything: a glowing model's part at the other end isn't in the light list (its emission shows
  where seen), nor are its punctual lights moved there.
- The model's box decides which openings it goes into: a model whose box crosses an opening but whose triangles
  don't still gets an (invisible) cut and ghost.
- At most 64 ghosts in a frame (all models together) and 4 cuts per piece; past them, pieces are left out.
- Cost: cut instances are "not opaque" (the hardware hands their hits to the shader), so an object in a busy opening
  costs: portal_rooms with a 0.3 m cube half through the lamp window traces in 7.05 ms, 6.6 ms with two whole cubes
  in the same places (RTX 3070, 1600x900). Scenes with nothing cut: within noise (Sponza 6.28 -> 6.34 ms), captures
  byte-identical.
- The statistics' triangle count leaves ghosts out; a ghost appearing or going rebuilds the TLAS (not updated).
- A new ghost has no motion of its own (DLSS sees it appear). A click is read back a frame or two later: if the
  ghosts changed meanwhile, a click on one can select another ghost's entity, or nothing.

### The camera comes out upright
The camera has no roll, so going through a pair whose ends lean differently (one in a wall, the other in the floor)
turns the view upright at once as it comes out, instead of continuing it. Only the camera's own movement takes it
through (the fly controls); moving the camera in the editor, or an end onto the camera, doesn't.
- Where: `Scene::moveCameraThroughPortals`.
- Fix: a camera roll, turned back to upright over a moment after coming out.

## Fog

### Thick patchy fog is slow
Patchy fog and fog with soft edges (varying density) are tracked exactly: delta tracking for where paths scatter,
ratio tracking for shadow rays, both with steps of random length sized for the thickest fog anywhere in the volume
(about one per 1 / thickness), each reading the noise and the distance field. Even fog stays cheap: its first step
is where the path scatters. Fog orb scene, 1600x900, Accumulation, 8 bounces (image pass / cache update pass):

| Fog | Thickness 1.9 | 7.5 | 30 |
|---|---|---|---|
| Even | 34 / 2.3 ms | | 31 / 2.1 ms |
| Patchy (the orb's noise) | 34 / 5.8 ms | 55 / 10 ms | 87 / 14.5 ms |

With 32 bounces the patchy fog at 30 takes 304 / 43 ms, even fog 74 / 5.5 ms. Of the 87 ms, about 22 are the first
bounce (one bounce only: 21.9 ms); every further bounce adds ~8 ms. Past the first bounce the time is not in the
steps: what each scatter point costs besides them (its bounce ray's query through the scene, the shadow rays'
queries, light sampling) dominates.

Tried and set aside, both measured against the exact tracking:
- **Majorant grids** (VOLUMES_PLAN.md step 5): local maxima per grid cell, so empty space is crossed in long steps.
  Slower everywhere (Cornell box with thick patchy fog 40 -> 67 ms): this noise leaves few cells empty, and the
  extra code raised the shader's registers (128 -> 168).
- **Ray marching** (2026-10-14): steps of a fixed share of the noise's blob size, the density read at one
  jittered point per step; thickness then costs nothing more. It cut the steps per pixel 2-6x (thickness 30, step
  half a blob: 132 -> 23) and the cache update pass 2-3x (14.5 -> 4.5 ms), but the image pass only ~20% at
  thickness 30 (87 -> 70 ms) and none at 7.5 (55 -> 67-80 ms, slower): from the second bounce on, a bounce cost as
  much as with exact tracking. Biased beyond what it saved: averaging the density over a step thins patchy fog
  (scatter points per path at thickness 7.5: 2.57 exact, 2.30 at half a blob, 1.76 at a whole blob). Exact
  tracking's speed was sensitive to the added code too (a nested loop in the shadow-ray march made it 87 -> 146 ms
  with marching off).
- Where: `deltaTrack`, `ratioTrack`, `varyingTransmittance` (shaders/raytrace.comp).

What helped is fewer scatter points per path: the radiance cache has cells in fog too ("In fog too", on by default),
and image paths stop at their second scatter point (2026-10-14). Same scene, cache update pass + image pass:

| Fog | Without cells in fog | With |
|---|---|---|
| Even, thickness 30 | 2.3 + 31 ms | 2.4 + 8.3 ms |
| Patchy 1.9 | 2.9 + 35 ms | 2.9 + 15 ms |
| Patchy 7.5 | 7.0 + 58 ms | 6.5 + 24 ms |
| Patchy 30 | 9.3 + 92 ms | 10 + 28 ms |

(The update pass was also cut from 14.5 to 9.3 ms in thick fog by filling 4 cells per update path instead of 8:
the shader then fits 128 registers instead of 168.) Thick patchy fog is still the slowest case; the first bounce
(~22 ms at thickness 30) is now most of it. Next, if needed: sunlight through the fog precomputed per frame.

### Light in fog from the cache is slightly biased
A fog cell averages its volume (about 32 pixels wide, like surface cells) and all directions the light leaves it in,
so the light that scatters more than once comes slightly blurred and as if the fog scattered evenly (strongly
forward-scattering fog differs a little more). Direct light at the scatter point is still exact. Measured against
the image without cells in fog (1024 frames): the averages within 0.2-1.1%, regions within 1-3%, which is about how
much two runs of that reference differ from each other (up to 2.4%), so the real bias may be smaller.
Update paths give each cell the light of Max bounces - 1 further bounces, as many as the image path that ends there
would have traced; before that, thick fog came out 10-16% too dark (cells found late in an update path got fewer).
- Where: shaders/raytrace.comp ("Radiance cache", `tracePath`).
- Workaround: untick "In fog too" (Radiance cache settings).

### DLSS lags behind changes seen through fog
A pixel looking into fog gets one path per frame, which either scatters in the fog or reaches what is behind: the
pixel jumps between the two from frame to frame. DLSS can't tell a real change from that noise and keeps old frames:
a flashlight moving through thin fog leaves its glow where the beam was, and a dark trail where its grip was (fog box,
DLAA: the moving frame 21-24% too bright, against 2% standing still). The guides can't help: the fog really stands
still (motion 0), and a moving beam is a change of light, not of geometry.
Tried and dropped (2026-10-14):
- **Guides of the fog** (the camera ray traced again without chance, the fog where it hides half of what is behind
  as a surface): fog kept its shape when the camera moved (error 0.121 -> 0.114, fog orb), but a moving flashlight
  was no better (0.393 -> 0.397), for 4-6 ms.
- **DLSS RR's transparency overlay** (the fog in front as a layer of its own, the surface behind as the input color
  with its own guides): the trail and the lingering glow were gone, but DLSS filters that layer far less than the
  input color: it came out speckled and 12-19% too dark (flashlight scene, standing still: error 0.37-0.47 against
  0.22 without), also with its opacity made exact and its light forced from a scatter point in every pixel. The layer
  composites as premultiplied, and our two parts added up matched the reference (Accumulation), so the encoding was
  right. Tagging the layer also gave validation errors inside DLSS (internal images in UNDEFINED layout, first frame).
- Fix: a fog layer clean enough for that overlay (our own temporal and spatial filter on it), or the fog composited
  after DLSS.
- Where: `tracePath` (the camera ray in fog), `recordDlss` (source/renderer/Dlss.cpp).

## Engine and tools

### Barriers are written by hand
Each pass writes its own `vkCmdPipelineBarrier`s (~40); nothing tracks the images' and buffers' last use. A wrong
stage mask once cost 1.8 ms, and a missing barrier shows only as an occasional wrong frame.
- Checked: `--sync-validation` (the validation layer's synchronization checks) over 15 short benches, every render
  mode, ReSTIR, fog, portals, the noise tool, captures, debug views, model loading (2026-10-15): two hazards in
  one-time setup, fixed; nothing else of ours. Not covered: window resize, render-mode switches by hand, the basic
  shader switched while running, model unloading.
- Fix: a small helper for new passes (named barriers such as computeToCompute / transferToCompute, image layout
  changes; per resource its last access, a pass declaring what it reads and writes, one batched barrier). Write it
  with the first new pass that wants it, run that pass with `--sync-validation`; convert old passes only when they
  are worked on anyway, each checked with sync validation and a timing bench (a wrong conversion costs time silently).
- Where: the passes' record functions (source/renderer/*.cpp).

### The renderer's parts hold references to each other's members
`Renderer` is split into parts (GpuContext, GpuScene, RayTracer, RadianceCache, Dlss, ExposurePass, NoiseTool,
DisplayPass, FrameCapture; Renderer.h). Each takes the renderer state it reads or sets as constructor references,
and their data is public (other parts and `Renderer::stats` read it). So a part's dependencies are listed, but not
enforced, and members must be declared in order: a part that binds to another's members comes after it.
- Why: the split moved code without changing it (byte-identical images); tightening it was left for when a part is
  worked on.
- Fix: per part, what others read becomes accessors; references to other parts' members become arguments of the
  calls that need them.

### Benches: absolute camera paths, and frame times before 2026-10-16
- A bench's camera path sets absolute positions every frame: the camera doesn't follow portals (each frame's
  position undoes the jump).
- `frameMs` (wall time) ended after the next frame's start until 2026-10-16: a capture on the last measured frame
  put its PNG write in (sponza_dlaa read 21.3 ms, really 19.2). Wall times of such benches from older builds aren't
  comparable; GPU times are.
- Bistro at night isn't fully deterministic even in Accumulation without the radiance cache and with fixed exposure
  (about 1 run in 4 differs, 0.05% of pixels, fireflies), in builds before the Renderer split too: byte-identical
  checks use Sponza and the fog orb. Cause unknown (light tree build, load timing?).
- Where: app/Bench.cpp, tools/bench.py.

### The program needs the compiler's libstdc++ first on PATH
Run from Git Bash (Git's mingw64/bin first on PATH), vulkanDemo.exe loads Git's libstdc++-6.dll and crashes at
start without a message. tools/bench.py puts the build's compiler folder (from CMakeCache.txt) first; the tests
executable is linked statically and doesn't care.
- Fix: link vulkanDemo with `-static-libstdc++ -static-libgcc` too.

### No precompiled header
Measured: 0.6-0.7 s saved per file (15-20%: most of the time is optimizing, not parsing). Not worth it yet.

## Game

### Collision: boxes only, and only the player
The player (a capsule) collides with boxes the scene marks (`collider`), not with the models' own triangles: a
Sponza wall without a collider is walked through. Only solid colliders (`"solid": true`) stop at other colliders;
nothing is pushed or falls (no rigid bodies), and a lift or platform doesn't carry the player along sideways (it
only lifts it).
- Fix: collision against the models' triangles (PHYSICS_PLAN.md, option B: a mesh shape behind the same
  PhysicsWorld interface, built from Mesh::vertices / indices, which stay on the CPU); rigid bodies with a library
  (Jolt) behind it.

### What blocked moves don't do
A solid collider's move (PhysicsWorld::moveBodies) stops where it would go into another collider, then slides the
straight part of what is left along it (twice at most).
- A turn only stops: a door swinging into a crate stops at it instead of turning while it slides off.
- Behaviours aren't told: a held door keeps opening by its own count and catches up when freed; a bob or slide goes
  on with its time. They could read "blocked" from GameWorld.
- Groups move rigidly: the colliders under the topmost entity that moved go as one; a child's own move in the same
  step isn't tested (its body is put where it ends up).
- What a solid collider is already in when it moves (it was placed so, or a non-solid one went into it) doesn't
  block it, so they can part; a non-solid collider goes through anything, solid ones too.
- The player blocks a move only when it is squeezed (a trial push can't free it); it is never carried sideways.
- A door made solid in a wall needs its panel's collider a little narrower than the gap: a panel that touches the
  wall at the hinge swings into it and is blocked at once.
- Where: SimplePhysicsWorld::moveBodies (source/physics/SimplePhysics.cpp), Collider::updateAll
  (game/behaviours/Collider.cpp).

### The portal gun
Left and right click put the gun's pair's ends where a ray from the camera hits a collider (PhysicsWorld::rayCast,
portals::placePortalEnd).
- Only colliders take portals: a wall drawn without a collider is shot through (mesh colliders, PHYSICS_PLAN.md
  option B, would fix it).
- An end is fitted to the box face it hit only: another collider covering part of that face (a wall standing on a
  floor) or the player standing there isn't checked, so an end can reach a little into a neighbouring wall. Portal
  ends on the same face are: the gun's own other end is pushed away from; a spot that would cover another pair's end
  takes no end (it isn't slid aside).
- On a wall an end hangs down from the hit, or stands on what is below when there is too little room: the room below
  is found by three rays down the wall (its middle and near both sides), so a thin ledge between them isn't seen.
  On floors and ceilings it is centered on the hit.
- An end becomes a child of the entity it was put on (it moves with it, is hidden with it; its scale is taken back
  out). Under an entity scaled differently along a floor's two axes, an end turned at an angle would be sheared:
  its top then goes along the floor's nearest axis instead of the shot's way. Removing that entity removes the end,
  and so its other end too: the gun makes a new pair.
- A collider offset from its entity (its "offset", or a model whose box isn't centered) still puts the end under
  that entity: what moves the entity moves the end.
- Moving an end while something is part way through it: its part at the other end is gone at once, and collision
  pushes the rest out of the wall.
- One gun per game (it owns its pair); it shoots from the camera, so it also works without a player body.
- Where: game/behaviours/PortalGun.cpp, portals/PortalPlacement.cpp, SimplePhysicsWorld::rayCast.

### Distances are from the eyes
Doors, pickups and triggers measure from the camera's position: with a player that is its eyes, 1.65 m up, so a
pickup on the floor needs a radius over 1.65 (the scenes use 1.5 with coins 0.5 up). They could use the body
instead (PhysicsWorld::overlapsCapsule).
- Where: `GameWorld::player` (source/game/Behaviour.h), set in Game::update.

### Collider outlines are drawn over everything
The green collider boxes (the selection's, or all with View > "Collider outlines") are lines drawn over the image,
not traced: a box behind a wall still shows, and nothing marks which edges are hidden. They show while the game
doesn't run (paused too), from the components' properties, not the game's bodies. A box part way through a portal
is drawn as its pieces (its part out of the other end in orange), each edge cut where its piece ends; the faces made
by a cut have no edges drawn, so a cut box looks open there. (The physics' boxes are grown to 5 cm on thin axes;
the outlines aren't.)
- Where: Editor::drawColliderBoxes (source/editor/EditorGizmo.cpp), colliderBoxes (game/Colliders.h).

### The player is lifted onto a step in one step
Stepping up a ledge (up to stepHeight) moves the body up at once; the camera is blended between the steps but still
rises by the whole step within 1/60 s. Games often smooth the camera's height over a few frames.
- Where: SimplePhysicsWorld::moveCharacter (step up), Player::update.

### Entity ids in properties aren't remapped
`"target": 12` names entity 12. Duplicating a door, trigger or spawner copies the id as it is, so the copy still
points at the original's target; nothing warns. Removing the target leaves the id dangling (a warning at Play).
- Fix: `Scene::duplicate` rewriting ids that point inside the copied subtree needs to know which properties are
  ids: either the game tells the editor (a list of id properties per type) or ids get a marked form
  (`{"entity": 12}`).
- Where: Scene::duplicate (source/scene/Scene.cpp), GameProperties::entity.

### Behaviours that move the same thing fight
Each behaviour writes what it moves every step: `spin` and `door` on one entity both set the rotation, and the later
one in the update order wins (spin, bob, slide, door, pickup, trigger, spawner, camera path:
game/Behaviours.cpp). A trigger and a pickup on
the same target both set its visibility. Nothing warns.
- Fix: put one of them on a child entity (spin the child, swing the parent), or a warning at Play for known pairs.

### Edits made while playing are lost
Stop puts back the scene as it was at Play: moving, adding or editing things by hand while paused is undone too
(as in Unity). Save is off while playing, so the played state can't be saved by mistake.
- Resuming after a pause: changed properties start that entity's behaviours over (a half-open door starts closed);
  a camera path follows its entity moved as a whole, not a point moved on its own; undo can't go back past the
  pause (it restores whole scene texts: the game's progress would be undone too).
- Also: Stop logs a missing model's "failed to load" again (putting the scene back asks for it again).

### What is shown runs up to a step behind
Moving things are shown blended between the last two steps (StepInterpolation): up to 16.7 ms behind the game. And
a moving entity changes the TLAS every frame instead of 60 times a second: above 60 fps that is more refits (cheap
in small scenes; Jungle Ruins runs well below 60 fps, where every frame has a step anyway). Not measured on its own.
View > "Blend game steps" turns it off.

### A camera path ignores portals
A camera path sets the camera's position from its points every step; while the game drives the camera the camera
isn't taken through portals by the program (the player goes through on its own), so a path through an opening just
passes the plane. Paths are in world positions taken at Play (points moved while playing don't change the path).

### What portals carry, and what they don't
While playing, an entity the game moved in a step whose origin's path went through an opening comes out of the
other end (Game::carryThroughPortals): the topmost one that moved (what is under it comes along), never a portal
end itself. Until then a model or collider part way through is in both places (drawn and colliding as pieces:
"Objects half through a portal: what is drawn", "Collision through portals"); which of its pieces is the real one
changes when its origin goes through. A model whose origin is outside the opening (a door hinge) never goes through
(its part beyond shows at the other end for as long as it stays in the opening). Something moving back and forth
across an opening (a bob through a flat portal facing the same way at both ends) goes through each time it crosses.
- An object bigger than the opening doesn't go in at all (it crosses the plane partly beside the opening): it stays
  whole, as without portals, and a wall around the opening stops it. Portal doesn't let such objects in either.
- Not carried: light from a glowing piece at the other end, punctual lights of a model part way through, the
  outline and picks through portals (see their own items).
- The player goes through when its eyes do (the physics carries it: PhysicsWorld::moveCharacter); its body
  collides through portals (see "Collision through portals").
- Solid colliders' moves collide through portals too (see "Collision through portals"); the game still carries
  one through by its entity's origin, after the step.
- Only in scenes with portals: each step then works out every entity's world position twice (a few microseconds per
  hundred entities).

### Collision through portals
The player's capsule and the colliders near an opening are cut into pieces (portals/PortalPieces.h, as drawn): a part
that went in touches what is beyond the other end, and what is behind the entrance within the opening isn't touched
(PORTAL_PHYSICS_PLAN.md phases 3-4). A solid collider's move tests its pieces the same way, and its own pieces placed
differently (its real part and the part out of the other end) block each other.
- Each pair of pieces gives one contact (the capsule against box test's deepest), kept or dropped by the point halfway
  into the overlap. A box all behind the exit a piece came out of is skipped first (`pieceMissesBox`): with a wall
  1 cm behind an end, the ghost's deep overlap with it had its middle in front of the end, and pushed the player back
  out of the entrance. So a capsule rests on what is under its axis: half through, the floor beyond the exit doesn't
  hold it up until its middle is through. With portals in walls the eyes and the axis go through together, so it
  doesn't show; a floor that ends before the opening drops the player as without portals.
- The player goes in when its whole outline fits the opening grown by its `portalSlack` (player property, default
  0.25 m, on every side): a portal a little above the floor, or the eyes at its top edge, still let it through. Within
  the slack the wall around the opening doesn't stop it, and its eyes go through there too, so with a large slack the
  camera can come right up to the wall beside or above the opening before it goes through (a frame or so of wall up
  close). Further out the wall stops it, as it should. Colliders get no slack.
- A collider's real side is its box's center (the game carries its entity by the entity's origin: a collider offset
  far from its entity can be the wrong piece while half through). It keeps the side it had while a move goes on, and
  when a placement goes through an opening (until the game's carry places it at the other end).
- Box against box is judged at the contact's points (the deepest one both pieces keep). A solid collider that would
  go into its own other piece only stops (no slide).
- A solid move is tried at points along it: a box sliding sideways out of an opening without a wall around it stops
  being in the portal between two points (its part at the other end is gone at once), so what that part would have
  hit in between isn't seen. With a wall around the opening the wall stops it first.
- The character stays upright: a pair that isn't level (a floor to a wall) puts its eyes through, its body standing
  under them.
- Where: SimplePhysicsWorld::contact, boxContact, ownContact, carry (source/physics/SimplePhysics.cpp);
  Game::update hands the openings over (Scene::portalOpenings).

### F-keys reach the game along with the editor's
While the game runs the editor's shortcuts are off, but F1, F5, F6, F11 and Esc still work, and the game sees them
too (no behaviour uses them). Benches never play.

### Properties are edited as JSON text
The Details panel's Type menu lists the game's types (Game::types: name, help, default properties) and fills in a
new component's defaults, but the properties stay a JSON text field. Mistakes show as warnings in the Log at Play.
- Fix: the types' fields with their kinds (number, flag, vector, entity id) instead of a defaults text, for proper
  widgets (and an entity picker for ids).

### Behaviours look entities up every step
Each behaviour finds its entity by id (a hash lookup) every step, and world positions walk up the parents. 10,000
behaviours (entities two groups deep) take ~0.7 ms per step in Release, ~4% of a core at 60 steps a second; 1,000
take ~0.07 ms.
- Fix when it matters: the world transforms worked out once per step for all entities (as the render list does),
  and behaviours holding entity indices, refreshed when entities are added or removed.

### An older build drops game components
The scene file's version stayed 1 (`"game"` is a new optional field): a build from before game components opens a
newer scene without complaint, and saving it there drops them.

## Editor

### Clicking selects whole entities
A click selects the model entity whose part was hit: in Sponza or Bistro (one model each) that is the whole scene.
The pick knows the part (the TLAS instance), so selecting a part (to hide it, or see its material) could follow.
Clicking a portal's rim selects nothing (the rims' bars aren't entities').
- Where: Application::updateSelection, RenderList::entityOfInstance.

### Picks and the outline don't go through portals
The selection's rays stop at the first surface in the scene's own space: what is seen through a portal opening is
neither clickable nor outlined there (the click selects what is behind the opening's plane instead).
- Fix: follow the openings in shaders/selection.comp as intersectClosest does.

### The outline's cost grows with what it covers
It traces a first-hit ray per output pixel inside the rectangle the selection covers on the image (0.07 ms for
the display pass with it, the helmet selected). A selection covering the whole image (all of Jungle Ruins, its
alpha-tested plants) traces the whole image once more per frame: not measured.
- Fix if it shows: trace it at a lower resolution, or only when the camera or the selection changed.

### Undo keeps whole scene texts, and restores the whole scene
Each change is the scene's text before and after it (editor/EditHistory.h; a few KB, at most 100 changes or 64 MB).
Undoing loads the text before it back, like opening the scene: the render list and the TLAS are rebuilt once and
the image starts accumulating again; models still loaded aren't reloaded (one that was unloaded since loads again
in the background). The camera stays where it is; the program's settings (render mode, samples per pixel, ...)
aren't part of it. A change counts when an edit ends (a widget let go, a handle drag ended, a key): a model
arriving from the background between two edits goes into the next change (undoing that shows it as loading
again, briefly). The history is cleared when another scene opens; nothing is undone while playing.
- Writing the text walks every part of every model (hidden parts are saved by index): a few ms per edit in Jungle
  Ruins (not measured on its own).

### Handles under a scaled, turned parent
A handle works in the world and the result is turned back into the entity's own transform. Under a parent that is
scaled differently along its axes and turned, the entity's transform can't hold what the handle did exactly
(it would need shear): rotating or scaling there drifts slightly from what the handle showed.
