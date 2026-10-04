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

## Lights

### Emissive texture weighting is an estimate (Phase 6d)
Each glowing triangle is weighted by its emissive texture's average, measured at load time from a grid of
1 to 256 points inside the triangle (nearest texel, first UV set). Small bright details can be missed; such a
triangle gets probability 0 and its light is then found only by bounce rays (correct, but noisier there).
Alpha (cut-out holes) is not part of the weight, and within a picked triangle the point is still chosen
uniformly, not by texture brightness. Built-in models have no emissive textures.
- Where: `computeEmissiveTextureAverages` (source/ModelLoader.cpp), `buildLightList` (source/Lights.cpp).
- Fix: a small minimum weight for every triangle, or importance-sample texels within the triangle.

### Light list is rebuilt on the CPU
Whenever an instance moves or a material changes, every emissive triangle is transformed on the CPU and
uploaded. Fine for thousands of triangles, slow for scenes with many glowing meshes.
- Fix: build it on the GPU, or keep it per mesh and transform in the shader.

### Sun of size 0
A sun with angular radius 0 can only be found by shadow rays: mirrors don't show it, and the camera sees a
disc clamped to a minimum size.

## Alpha (Phase 6b)

### Blend mode is noisy
See-through (blend) surfaces stop each ray with chance alpha. Correct on average, but noisy until accumulated.
- Where: `candidateIsSolid`, `shadowTransmittance`.

## Textures and models

### No mipmaps
Textures are always read at full resolution (a compute shader has no screen derivatives). Distant textures
alias; accumulation averages it out, single frames shimmer.
- Where: `sampleTexture` (shader), `uploadNewTextures` (source/Materials.cpp).
- Fix: generate mipmaps + ray cones (or ray differentials) to pick a level.

### Unsupported glTF features
- Second UV set (`TEXCOORD_1`): the first set is used instead (a warning is printed).
- KTX2 / WebP images: the texture is skipped, the constant is used.
- Specular/glossiness workflow: only the diffuse color is kept.
- Ambient occlusion and height maps are ignored on purpose (AO comes from ray tracing).
- Other material extensions (clearcoat, sheen, specular, iridescence, anisotropy, ...) are ignored.
- Where: source/ModelLoader.cpp.

### No OBJ loader
Only glTF / GLB is loaded.

## Display

### Auto exposure is one frame late and meters like a camera
The exposure is measured from the finished image and used for the next frame (invisible: adaptation takes
many frames anyway). Like a real light meter, it maps the average to middle grey, so a mostly white scene
(the sunlit default floor) comes out greyer than with a hand-picked exposure; use Compensation. The
histogram covers luminance 2^-20 .. 2^12; darker pixels count as black and are not metered. While
accumulating, the measurement comes from the averaged image, so early noisy frames may meter slightly
differently from the converged result.
- Where: shaders/exposure.comp, source/Exposure.cpp.
- Fix (if it matters): meter before tracing from a cheap low-resolution pass; a smarter metering mode.
