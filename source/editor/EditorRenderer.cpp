//
// The renderer's settings panels: the Rendering panel (render mode, light paths, radiance cache, ReSTIR, image), the
// Debug panel's debug views, noise tool and shader statistics, and the renderer's parts of the Lighting panel. They
// edit the program's RenderSettings; what they show of the renderer comes from Renderer::stats (this frame's, fetched
// at the start of build), what they ask of it goes through its methods (restart, clear the cache, the noise tool...).
//

#include <algorithm>
#include <cmath>
#include <imgui/imgui.h>
#include <nlohmann/json.hpp>
#include "editor/Editor.h"
#include "renderer/RenderSettingsFile.h"

void Editor::buildRenderingPanel() {
    // What is the scene's here (saved with it) and what the program's (kept between runs)
    size_t changed = renderSettingsToJson(settings, SettingsScope::Scene, true).size();
    if (changed == 0) {
        ImGui::TextDisabled("Scene settings: the defaults");
    } else {
        ImGui::TextDisabled("Scene settings: %zu changed from the defaults", changed);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##sceneSettings")) {
            resetRenderSettings(settings, SettingsScope::Scene);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Saved with the scene: everything on this panel and the renderer's parts of the Lighting\n"
                          "panel, except the renderer, image size, DLSS model, Accumulate and samples per pixel (and\n"
                          "the debug view): those are the program's, kept between runs. Opening a scene sets its\n"
                          "settings, a new scene starts from the defaults.");
    }

    ImGui::SeparatorText("Renderer");
    buildRenderModeUi();
    ImGui::BeginDisabled(stats.dlssActive);  // DLSS averages over time itself
    ImGui::Checkbox("Accumulate", &settings.accumulate);
    ImGui::EndDisabled();
    if (settings.accumulate && !stats.dlssActive) {
        ImGui::SameLine();
        ImGui::Text("%u frames (%u samples/pixel)", stats.accumulatedFrames,
                    stats.accumulatedFrames * settings.samplesPerPixel);
        ImGui::SameLine();
        if (ImGui::SmallButton("Restart")) {
            renderer.restartAccumulation();
        }
    }

    ImGui::SeparatorText("Light paths");
    ImGui::SliderInt("Samples per pixel", &settings.samplesPerPixel, 1, 64);
    ImGui::Combo("Random numbers", &settings.pathSampling, "White noise\0Sobol\0Sobol + blue noise\0");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("For the main decisions of a path (camera offset, sun, sky, glowing triangles, bounces).\n"
                          "White noise: independent random numbers.\n"
                          "Sobol: Owen-scrambled Sobol points, a pixel's samples spread evenly: it settles faster.\n"
                          "+ blue noise: the points shifted per pixel by a blue-noise texture, so neighboring pixels'\n"
                          "errors differ too: fine-grained noise instead of blotches, easier for DLSS to filter.");
    }
    ImGui::SliderInt("Max bounces", &settings.maxBounces, 1, 64);
    ImGui::TextDisabled("1 = direct light only (sun, sky, lamps, glowing surfaces), more = light bouncing between surfaces");
    ImGui::SliderInt("Max portal crossings", &settings.maxPortalCrossings, 1, 64);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How many portals one ray may go through. Two ends facing each other pass a ray back and\n"
                          "forth forever: beyond this many it is lost, and the deep end of such a tunnel is black.\n"
                          "Higher shows the tunnel deeper; it costs only where rays go that deep.");
    }
    ImGui::Combo("Emissive light", &settings.emissiveSampling,
                 "Bounces only (old)\0Light sampling only\0Both, weighted (MIS)\0");
    ImGui::TextDisabled("how light from glowing surfaces is found; compare with Accumulate off");
    ImGui::Checkbox("Glowing triangles by solid angle", &settings.emissiveSolidAngle);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Where on a glowing triangle shadow rays (and ReSTIR's candidates) go.\n"
                          "On: spread evenly over the triangle as the surface sees it (its solid angle, Arvo 1995).\n"
                          "Off: evenly over its area. For a large panel close to a surface, its near part fills most of\n"
                          "the surface's view of it but gets few samples: noisier there.");
    }
    ImGui::Combo("Light picking", &settings.lightPicking, "By power (whole scene)\0Light tree\0");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Which glowing triangle a shadow ray goes to. By power: the brighter and larger, the more\n"
                          "often, wherever it is. Light tree: also by how near it is and whether the surface faces it,\n"
                          "so in scenes with many small lights (Bistro at night) the nearby ones get the rays.");
    }
    if (settings.lightPicking == static_cast<int>(LightPicking::Power)) {
        ImGui::SliderInt("Power candidates", &settings.lightCandidates, 1, 16);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Glowing triangles picked by power to keep one of, by how near it is and how it and the\n"
                              "surface face each other: no extra rays, only picks. 1: the plain pick. Bistro at night\n"
                              "with DLAA + ReSTIR: 8 left about a third of the noise, for ~3-5 ms more (Debug).\n"
                              "(The light tree weighs distance and angle itself.)");
        }
    }
    if (stats.lightTreeNodes == 0) {
        ImGui::TextDisabled("%zu glowing triangles", stats.sampledEmissiveTriangles);
    } else {
        ImGui::TextDisabled("%zu glowing triangles, tree: %zu nodes, %u levels", stats.sampledEmissiveTriangles,
                            stats.lightTreeNodes, stats.lightTreeDepth);
    }
    ImGui::Checkbox("Texture mipmaps", &settings.textureMipmaps);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("On: each hit reads textures at the size that fits how much of the surface one pixel sees\n"
                          "(smaller copies for distant or blurry hits): faster, no shimmering in the distance.\n"
                          "Off: always the full-size texture.");
    }
    ImGui::BeginDisabled(!settings.textureMipmaps);
    ImGui::Checkbox("Normal map filtering", &settings.normalMapFiltering);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Toksvig: where a smaller mip level of a normal map averages its bumps away (distant or\n"
                          "blurry hits), the reflection gets as much rougher as the bumps would have spread it.\n"
                          "Off: distant bumpy surfaces reflect as sharply as polished ones (and sparkle).\n"
                          "Needs Texture mipmaps.");
    }
    ImGui::Checkbox("Blur caustics", &settings.blurCaustics);
    ImGui::TextDisabled("light reflected by shiny surfaces onto matte ones becomes a soft glow\n"
                        "instead of slowly converging sparkles (fireflies)");
    ImGui::Checkbox("Basic shader", &settings.basicShader);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The trace shader without the code for portals, lamps (punctual lights), glass, cut-outs\n"
                          "and diffuse transmission: less code, fewer registers, about 14%% faster. The image stays\n"
                          "the same only in scenes without them: there portals and lamps vanish, glass turns opaque\n"
                          "and cut-outs solid. Switching rebuilds the shader (the window stands still a few seconds).");
    }
    buildRadianceCacheUi();
    buildRestirUi();

    ImGui::SeparatorText("Image");
    buildExposureUi();
    ImGui::Checkbox("Tone mapping (ACES)", &settings.toneMapping);
    ImGui::Checkbox("Fog grain", &settings.fogGrain);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Film-like grain where the camera sees fog, added to the finished image (after DLSS, which\n"
                          "removes the path tracer's own noise). A look only: the light is unchanged. Also in\n"
                          "Accumulation and in captures; turn it off for a clean image.");
    }
    ImGui::BeginDisabled(!settings.fogGrain);
    ImGui::SliderFloat("Grain strength", &settings.fogGrainStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("Grain size", &settings.fogGrainSize, 1.0f, 4.0f, "%.1f px");
    ImGui::EndDisabled();
}

// The "Renderer" combo at the top of the Rendering section
void Editor::buildRenderModeUi() {
    const char* modes[] = {"Accumulation (reference)", "DLSS RR: DLAA (full size)", "DLSS RR: Quality",
                           "DLSS RR: Balanced", "DLSS RR: Performance", "DLSS RR: Ultra Performance"};
    const char* models[] = {"Transformer F (newest)", "Transformer E", "Transformer D", "CNN A", "CNN B", "CNN C"};
    ImGui::BeginDisabled(!stats.dlssSupported);
    ImGui::Combo("Renderer", &settings.renderMode, modes, static_cast<int>(std::size(modes)));
    ImGui::EndDisabled();
    const char* sizes[] = {"Window", "1080p", "1440p", "2160p (4K)"};
    static_assert(std::size(sizes) == std::size(Renderer::outputHeights));
    ImGui::Combo("Image size", &settings.outputSize, sizes, static_cast<int>(std::size(sizes)));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The size of the image, and of what Capture saves. Bigger than the window: shown shrunk\n"
                          "to fit, and slower in proportion (4K has 5.8x the pixels of 1600 x 900). Keeps the\n"
                          "window's shape. For a sharp 4K capture: Accumulation, let it settle, then Capture.");
    }
    ImGui::TextDisabled("traces %u x %u, image %u x %u", stats.renderExtent.width, stats.renderExtent.height,
                        stats.outputExtent.width, stats.outputExtent.height);
    ImGui::BeginDisabled(!stats.dlssSupported);
    // Models the loaded DLSS version doesn't have are shown but can't be picked
    if (ImGui::BeginCombo("DLSS model", models[settings.dlssModel])) {
        for (int i = 0; i < static_cast<int>(std::size(models)); i++) {
            ImGuiSelectableFlags flags = renderer.dlssModelAvailable(i) ? 0 : ImGuiSelectableFlags_Disabled;
            if (ImGui::Selectable(models[i], settings.dlssModel == i, flags)) {
                settings.dlssModel = i;
            }
            if (!renderer.dlssModelAvailable(i) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(stats.dlssCnnVersion ? "only in the current DLSS version: start without --dlss-cnn"
                                                  : "only in the older DLSS version: start with --dlss-cnn");
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The network DLSS Ray Reconstruction runs. Transformer: the newer models, sharper and\n"
                          "steadier. CNN: the older models, about half the cost on a 20/30 series GPU.\n"
                          "DLSS %s is loaded (%s).", stats.dlssVersion.c_str(),
                          stats.dlssCnnVersion ? "--dlss-cnn: CNN A-C, transformer D-E"
                                          : "transformer D-F; start with --dlss-cnn for the CNN models");
    }
    ImGui::Checkbox("Clamp bounce light", &settings.clampBounceLight);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::SliderFloat("##bounceClamp", &settings.bounceLightClampLevel, 0.25f, 64.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("DLSS modes: one sample of light that reached the first surface via another surface adds at\n"
                          "most this much on screen (after exposure, 1 = white before tone mapping). Rare very bright\n"
                          "samples (a bounce ray that happens to find a lamp) otherwise stand out, and DLSS smears\n"
                          "them over many frames: ghosting, and an image that settles slowly. Biased: a bit darker.\n"
                          "Measured in Bistro at night: no less noise or drift, so off by default.");
    }
    ImGui::EndDisabled();
    if (!stats.dlssSupported) {
        ImGui::TextDisabled("DLSS unavailable: %s", stats.dlssStatus.c_str());
    } else if (stats.dlssActive) {
        double megabytes = renderer.dlssMemoryMegabytes();
        ImGui::TextDisabled("render %ux%u -> %ux%u (%.0f%% per axis), DLSS memory %.0f MB",
                            stats.renderExtent.width, stats.renderExtent.height, stats.outputExtent.width,
                            stats.outputExtent.height,
                            100.0 * stats.renderExtent.width / stats.outputExtent.width, megabytes);
    }
}

// Part of the "Rendering" section
void Editor::buildRadianceCacheUi() {
    ImGui::Checkbox("Radiance cache", &settings.radianceCache);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Remembers how much indirect light leaves small patches of the surfaces. Paths stop\n"
                          "after their first diffuse bounce: they light that point directly and take the\n"
                          "indirect light from the cache. Much less noise in indirectly lit places, and faster.\n"
                          "Slightly blurs indirect light; reacts to changes in the lighting within a few dozen frames.");
    }
    if (settings.radianceCache) {
        ImGui::SameLine();
        ImGui::TextDisabled("%u cells (%.0f%% full)%s", stats.cacheUsedEntries,
                            100.0f * stats.cacheUsedEntries / stats.cacheCapacity,
                            stats.cacheQueriesSuspended ? ", not used for the noise reference" : "");
    }
    if (!ImGui::TreeNode("Radiance cache settings")) {
        return;
    }
    ImGui::SliderFloat("Cell size", &settings.cacheCellPixels, 1.0f, 32.0f, "%.1f pixels", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Cells are about this wide on screen, wherever they are. Smaller: sharper indirect\n"
                          "light, but fewer samples per cell (noisier cache).");
    }
    ImGui::SliderInt("Update paths", &settings.cacheUpdateTile, 1, 16, "1 per %d x %d pixels");
    ImGui::SliderFloat("History", &settings.cacheHistoryFrames, 4.0f, 256.0f, "%.0f frames", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How long old samples keep counting. Longer: less noise in the cache, slower to follow\n"
                          "changes in the lighting.");
    }
    ImGui::SliderFloat("Minimum samples", &settings.cacheMinSamples, 1.0f, 64.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A cell needs this many samples before paths use it; until then they trace on.");
    }
    ImGui::Checkbox("In fog too", &settings.cacheFog);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Cells in fog as well: paths stop at their second scatter point and take the light that\n"
                          "comes there via further scattering from the cache. Much faster in thick fog; light inside\n"
                          "the fog comes a little blurred and as if scattered evenly in all directions.");
    }
    if (ImGui::Button("Clear cache")) {
        renderer.clearRadianceCache();
    }
    ImGui::TreePop();
}

// ReSTIR DI (see LIMITATIONS.md)
void Editor::buildRestirUi() {
    ImGui::SeparatorText("ReSTIR DI");
    bool& restirDi = settings.restirDi(settings.renderMode);
    bool accumulation = settings.renderMode == static_cast<int>(RenderMode::Accumulation);
    ImGui::Checkbox(accumulation ? "ReSTIR DI (Accumulation)" : "ReSTIR DI (DLSS modes)", &restirDi);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Direct light at the first surface the camera sees (if rough enough): several candidate light\n"
                          "samples per pixel, the most promising one kept and reused by the next frame and nearby\n"
                          "pixels. Less noise with many lights (Bistro at night). The sun keeps its own shadow ray.\n"
                          "Set separately for Accumulation (off by default: the plain path tracer, the reference)\n"
                          "and the DLSS modes (on by default).");
    }
    ImGui::BeginDisabled(!restirDi);
    ImGui::SliderInt("Light candidates", &settings.restirLightCandidates, 1, 16);
    ImGui::SliderInt("Sky candidates", &settings.restirSkyCandidates, 1, 8);
    ImGui::SliderInt("BRDF candidates", &settings.restirBrdfCandidates, 0, 4);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Light candidates: points on glowing triangles (light tree), each per pixel and frame.\n"
                          "Sky candidates: directions picked by the sky's brightness (whenever the sky is sampled: see Sample the sky).\n"
                          "BRDF candidates: directions the surface reflects into, a ray each (the light a glossy\n"
                          "surface mirrors).");
    }
    ImGui::SliderFloat("Min. roughness", &settings.restirMinRoughness, 0.0f, 1.0f, "%.2f");
    ImGui::Checkbox("Reuse last frame", &settings.restirTemporal);
    ImGui::BeginDisabled(!settings.restirTemporal);
    ImGui::SameLine();
    ImGui::Checkbox("Unbiased", &settings.restirTemporalVisibility);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Two more shadow rays per pixel, from last frame's surface. Without them, areas near shadow\n"
                          "edges come out too dark (Sponza with the sky: 3.7%%, Bistro at night: 0.6%%).");
    }
    ImGui::Checkbox("Shuffle pixels", &settings.restirPermutation);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Last frame's samples come from pixels up to 3 away, shuffled differently every frame.\n"
                          "Off: each pixel keeps its samples, and DLSS paints their error into the image.");
    }
    ImGui::SliderFloat("Max. history", &settings.restirMaxHistory, 1.0f, 50.0f, "%.0f x");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Last frame's samples count for at most this many times this frame's candidates. Higher:\n"
                          "less noise on still images, slower reaction to changes.");
    }
    ImGui::EndDisabled();
    ImGui::Checkbox("Reuse neighbors", &settings.restirSpatial);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Each pixel also resamples a few nearby pixels' samples (a pass of its own, ~3 ms in Bistro).\n"
                          "Less noise, flicker and ghosting, above all where the camera uncovers something (no\n"
                          "history there).");
    }
    ImGui::BeginDisabled(!settings.restirSpatial);
    ImGui::SameLine();
    ImGui::Checkbox("Unbiased##spatial", &settings.restirSpatialVisibility);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Shadow rays from the neighbors' surfaces when a pixel keeps its own sample. Without them,\n"
                          "areas near shadows come out too dark (Sponza with the sky: 9.7%% at a radius of 30).");
    }
    ImGui::SliderInt("Neighbors", &settings.restirSpatialNeighbors, 0, 8);
    ImGui::SliderFloat("Radius", &settings.restirSpatialRadius, 2.0f, 60.0f, "%.0f px");
    ImGui::SliderInt("Neighbors (uncovered)", &settings.restirDisocclusionNeighbors, 0, 16);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Neighbors for pixels that found no history (just uncovered by the camera or an object).");
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // ReSTIR GI (see LIMITATIONS.md)
    ImGui::SeparatorText("ReSTIR GI");
    bool& restirGi = settings.restirGi(settings.renderMode);
    ImGui::Checkbox(accumulation ? "ReSTIR GI (Accumulation)" : "ReSTIR GI (DLSS modes)", &restirGi);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Light reaching the first surface via another one (bounce light), at the surfaces ReSTIR DI\n"
                          "would use (Min. roughness above): where the first bounce went and the light found there\n"
                          "become a sample, reused by the next frame and nearby pixels. Independent of ReSTIR DI.\n"
                          "Set separately for Accumulation and the DLSS modes (off by default in both: see LIMITATIONS.md).");
    }
    ImGui::BeginDisabled(!restirGi);
    ImGui::SliderFloat("Min. roughness (bounce)", &settings.restirGiMinRoughness, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Second surfaces at least this rough give a sample (0: all but glass). The light a smooth\n"
                          "surface sends depends on where it is seen from: reused by another pixel it is a little off.");
    }
    ImGui::Checkbox("Reuse last frame##gi", &settings.restirGiTemporal);
    ImGui::BeginDisabled(!settings.restirGiTemporal);
    ImGui::SameLine();
    ImGui::Checkbox("Unbiased##gitemporal", &settings.restirGiTemporalVisibility);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("A ray from last frame's surface per pixel. Without it, areas that see light through gaps\n"
                          "come out too dark (Sponza's side aisle: 6%%).");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Shuffle pixels##gi", &settings.restirGiPermutation);
    ImGui::SliderFloat("Max. history##gi", &settings.restirGiMaxHistory, 1.0f, 50.0f, "%.0f x");
    ImGui::SliderInt("Max. age##gi", &settings.restirGiMaxAge, 1, 100, "%d frames");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Max. history: last frame's samples count for at most this many times this frame's.\n"
                          "Max. age: a sample is dropped after this many frames (its light may be out of date).");
    }
    ImGui::EndDisabled();
    ImGui::Checkbox("Reuse neighbors##gi", &settings.restirGiSpatial);
    ImGui::BeginDisabled(!settings.restirGiSpatial);
    ImGui::SameLine();
    ImGui::Checkbox("Unbiased##gispatial", &settings.restirGiSpatialVisibility);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Rays from the neighbors' surfaces for the kept sample, as ReSTIR DI's. Off (RTXDI's\n"
                          "default): areas near occluders can come out a little too dark.");
    }
    ImGui::SliderInt("Neighbors##gi", &settings.restirGiSpatialNeighbors, 0, 8);
    ImGui::SliderFloat("Radius##gi", &settings.restirGiSpatialRadius, 2.0f, 60.0f, "%.0f px");
    ImGui::SliderInt("Neighbors (uncovered)##gi", &settings.restirGiDisocclusionNeighbors, 0, 16);
    ImGui::EndDisabled();
    ImGui::Checkbox("Boiling filter", &settings.restirGiBoilingFilter);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Drops a sample that brings far more light than the others nearby, so one lucky sample\n"
                          "doesn't spread into a blotch that lasts for frames. Stronger: drops more (takes light away).");
    }
    ImGui::BeginDisabled(!settings.restirGiBoilingFilter);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("Strength##boiling", &settings.restirGiBoilingStrength, 0.01f, 1.0f, "%.2f");
    ImGui::EndDisabled();
    ImGui::Checkbox("Clamp sample light", &settings.restirGiClampRadiance);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Limits the light of one GI sample to this brightness on screen (after exposure). Fewer\n"
                          "bright blotches, a little darker.");
    }
    ImGui::BeginDisabled(!settings.restirGiClampRadiance);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("##giClamp", &settings.restirGiRadianceClampLevel, 0.25f, 64.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

// Part of the "Rendering" section
void Editor::buildExposureUi() {
    ImGui::Checkbox("Auto exposure", &settings.autoExposure);
    if (!settings.autoExposure) {
        ImGui::SliderFloat("Exposure", &settings.exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        return;
    }

    ImGui::SameLine();
    if (stats.exposureValid) {
        ImGui::Text("x%.2f (%+.1f stops)", stats.exposure.exposure * std::exp2(settings.exposureCompensation),
                    std::log2(stats.exposure.exposure) + settings.exposureCompensation);
    }
    ImGui::SliderFloat("Compensation", &settings.exposureCompensation, -4.0f, 4.0f, "%+.1f stops");

    if (!ImGui::TreeNode("Auto exposure settings")) {
        return;
    }

    // Histogram of the last metered frame: brightness from dark (left) to bright (right), log scale.
    // The highlighted band is the metered part, the line is the measured average.
    if (stats.exposureValid) {
        float bins[exposureBinCount - 1];
        float largest = 1.0f;
        for (uint32_t i = 1; i < exposureBinCount; i++) {
            bins[i - 1] = static_cast<float>(stats.exposure.lastHistogram[i]);
            largest = std::max(largest, bins[i - 1]);
        }
        ImGui::PlotHistogram("##histogram", bins, exposureBinCount - 1, 0, nullptr, 0.0f, largest,
                             ImVec2(ImGui::GetContentRegionAvail().x, 70.0f));
        ImVec2 min = ImGui::GetItemRectMin();
        ImVec2 max = ImGui::GetItemRectMax();
        auto xOf = [&](float luminance) {
            float t = (std::log2(std::max(luminance, 1e-30f)) - histogramMinLog2) / histogramLog2Range;
            return min.x + std::clamp(t, 0.0f, 1.0f) * (max.x - min.x);
        };
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(ImVec2(xOf(stats.exposure.lowLuminance), min.y),
                            ImVec2(xOf(stats.exposure.highLuminance), max.y),
                            IM_COL32(80, 200, 120, 50));
        float average = xOf(stats.exposure.averageLuminance);
        draw->AddLine(ImVec2(average, min.y), ImVec2(average, max.y), IM_COL32(255, 220, 80, 255), 2.0f);
        ImGui::TextDisabled("metered: luminance %.3g .. %.3g, average %.3g", stats.exposure.lowLuminance,
                            stats.exposure.highLuminance, stats.exposure.averageLuminance);
    }

    ImGui::SliderFloat("Ignore darkest", &settings.meterLowPercent, 0.0f, 98.0f, "%.0f%%");
    ImGui::SliderFloat("Ignore above", &settings.meterHighPercent, 2.0f, 100.0f, "%.0f%%");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pixels are ranked from dark to bright. Only the ones between the two\n"
                          "percentages are averaged: deep shadows and a black background don't\n"
                          "make the image brighter, the sun, the sky and fireflies don't make it darker.");
    }
    ImGui::Checkbox("Center-weighted", &settings.centerWeighted);
    ImGui::SameLine();
    ImGui::TextDisabled("(the middle of the image counts up to 4x)");
    ImGui::SliderFloat("Key", &settings.exposureKey, 0.05f, 0.5f, "%.2f");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The measured average is made this bright before tone mapping.\n"
                          "0.18 = middle grey; Compensation does the same in stops.");
    }
    ImGui::DragFloatRange2("Limits", &settings.minExposureStops, &settings.maxExposureStops, 0.1f, -10.0f, 12.0f,
                           "%+.1f stops", "%+.1f stops");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How far auto exposure may darken (x%.2f) or brighten (x%.1f) the image.\n"
                          "Keeps very dark scenes from turning into grey noise.",
                          std::exp2(settings.minExposureStops), std::exp2(settings.maxExposureStops));
    }
    ImGui::SliderFloat("Brighten speed", &settings.brightenSpeed, 0.1f, 10.0f, "%.1f /s", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Darken speed", &settings.darkenSpeed, 0.1f, 10.0f, "%.1f /s", ImGuiSliderFlags_Logarithmic);
    if (ImGui::Button("Snap now")) {
        renderer.snapExposure();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(skip the adaptation once)");
    ImGui::TreePop();
}

void Editor::buildRendererDebugUi() {
    ImGui::Combo("Debug view", &settings.debugView,
                 "Off (path traced)\0Normals\0Texture coordinates\0Mesh index\0Material index\0"
                 "Base color\0Metallic\0Roughness\0Transmission\0Emissive\0Tangents\0"
                 "Radiance cache\0ReSTIR M\0ReSTIR light\0"
                 "DLSS input: depth\0DLSS input: motion vectors\0DLSS input: diffuse albedo\0"
                 "DLSS input: specular albedo\0DLSS input: normals\0DLSS input: roughness\0"
                 "DLSS input: specular hit distance\0DLSS input: reprojection error\0");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Radiance cache: the indirect light the cache holds for what the camera sees, cell by cell\n"
                          "(magenta: no cell yet, dark red: too few samples).\n"
                          "ReSTIR M: how many candidates each pixel's light sample stands for, from this frame's only\n"
                          "(red) through yellow and green to 64x (blue). Grey: no ReSTIR there.\n"
                          "ReSTIR light: the kind of light each pixel's sample is on: glowing triangle (orange),\n"
                          "punctual light (purple), sky (blue), none found (dark red), blocked (black, W = 0).\n"
                          "DLSS inputs: the images DLSS will get, written during normal path tracing.\n"
                          "Depth / hit distance: grey = near to far, light blue = sky, black = unknown.\n"
                          "Motion vectors: grey = still, red/green = moving left/up.\n"
                          "Reprojection error: last frame's albedo fetched through the motion vectors,\n"
                          "minus this frame's. Should stay black while the camera or objects move,\n"
                          "except at edges that just came into view (blue: off-screen last frame).");
    }
    if (settings.debugView != 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Off")) {
            settings.debugView = 0;
        }
    }

    buildNoiseUi();

    if (!stats.tracePipelineStatistics.empty() && ImGui::CollapsingHeader("Trace shader (from the driver)")) {
        ImGui::TextUnformatted(stats.tracePipelineStatistics.c_str());
        ImGui::TextDisabled("More registers per thread = fewer threads per SM at once, so waiting for rays\n"
                            "and memory is hidden worse. Counts for the whole shader, even unused branches.");
    }
}

void Editor::buildNoiseUi() {
    ImGui::SeparatorText("Noise measurement");
    ImGui::TextDisabled("Keep the camera still. The reference is the current view, accumulated\n"
                        "for a long time; the measurement then starts over and compares every frame.");
    bool idle = stats.noiseState == NoiseState::Idle;
    ImGui::BeginDisabled(!idle);
    ImGui::SliderInt("Reference frames", &noiseReferenceFrames, 256, 65536, "%d", ImGuiSliderFlags_Logarithmic);
    if (ImGui::Button("Make reference")) {
        renderer.startNoiseReference(noiseReferenceFrames);
    }
    ImGui::SameLine();
    if (stats.noiseState == NoiseState::BuildingReference) {
        ImGui::Text("%u / %d frames", stats.dlssActive ? 0u : stats.accumulatedFrames, stats.noiseReferenceFrames);
    } else if (stats.noiseReferenceValid) {
        ImGui::TextDisabled("reference ready (average luminance %.3g)", stats.noiseReferenceLuminance);
    } else {
        ImGui::TextDisabled("no reference");
    }

    ImGui::SliderInt("Measured frames", &noiseMeasureFrames, 1, 1024, "%d", ImGuiSliderFlags_Logarithmic);
    ImGui::DragInt4("Region", &noiseRegion.x, 1.0f, 0,
                    static_cast<int>(std::max(stats.outputExtent.width, stats.outputExtent.height)));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("x0, y0, x1, y1 in pixels: a part of the image whose brightness is also measured\n"
                          "on its own (\"region\" column). All 0 = the whole image.");
    }
    ImGui::BeginDisabled(!stats.noiseReferenceValid);
    if (ImGui::Button("Measure")) {
        renderer.startNoiseMeasurement(settings.renderMode, noiseMeasureFrames, noiseRegion);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(in the current renderer)");
    ImGui::EndDisabled();
    if (stats.noiseState == NoiseState::Measuring) {
        ImGui::Text("frame %u / %d", stats.noiseFrame, stats.noiseMeasureFrames);
    } else if (!stats.noiseStatus.empty()) {
        ImGui::TextDisabled("%s", stats.noiseStatus.c_str());
    }

    if (!stats.noiseResults.empty() &&
        ImGui::BeginTable("noise", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("frame");
        ImGui::TableSetupColumn("rel. MSE");
        ImGui::TableSetupColumn("rel. RMSE");
        ImGui::TableSetupColumn("brightness");
        ImGui::TableSetupColumn("region");
        ImGui::TableHeadersRow();
        for (const NoiseResult& result : stats.noiseResults) {
            if ((result.frame & (result.frame - 1)) != 0 && &result != &stats.noiseResults.back()) {
                continue;  // powers of two (and the last one)
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", result.frame);
            ImGui::TableNextColumn();
            ImGui::Text("%.3e", result.relativeMse);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f%%", 100.0f * std::sqrt(result.relativeMse));
            ImGui::TableNextColumn();
            ImGui::Text("%.2f%%", 100.0f * result.brightness);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f%%", 100.0f * result.regionBrightness);
        }
        ImGui::EndTable();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("rel. MSE: mean of ((frame - reference) / reference)^2 over the image (luminance).\n"
                          "Unbiased: halves when the frame count doubles. Stops shrinking: biased.\n"
                          "brightness: the frame's total light compared to the reference's.\n"
                          "region: the same inside the Region rectangle above (light leaking into a corner shows up there).");
    }
}

// How the renderer samples the sky (part of the editor's Lighting panel, shown unless the background is black)
void Editor::buildSkySamplingUi() {
    ImGui::Combo("Sample the sky", &settings.skySampling, "Auto (HDR maps only)\0On\0Off\0");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("On: every surface aims a shadow ray at the sky (bright parts more often), combined with\n"
                          "the bounce rays (MIS). Off: the sky is only found by bounce rays that happen to leave\n"
                          "the scene. Both give the same image. Sampling costs a ray per bounce: it pays off for an\n"
                          "HDR map with a sun (much less noise), hardly for the smooth gradient sky. Auto: maps only.");
    }
}

// The "Lights from models" part of the Lighting panel
void Editor::buildPunctualLightSettingsUi() {
    ImGui::Checkbox("Enabled##punctual", &settings.punctualLights);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu lights", stats.punctualLights);
    ImGui::SliderFloat("Scale##punctual", &settings.punctualLightScale, 0.001f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Multiplies every light from the files (KHR_lights_punctual). They come in candela (point,\n"
                          "spot) and lux (directional). For comparison: the Sun at intensity 1 gives pi lux, a 100 cd\n"
                          "lamp 2 m away gives 25 lux. Edit single lights under Scene > object > Lights.");
    }
}
