//
// RenderSettings as JSON. See RenderSettingsFile.h.
//

#include <charconv>
#include <cstdlib>
#include <set>
#include <nlohmann/json.hpp>
#include "renderer/RenderSettingsFile.h"
#include "renderer/Renderer.h"

using Names = std::vector<const char*>;

// The names of the choices, in the order of their values (the enums in Renderer.h)
static const Names emissiveSamplingNames = {"BouncesOnly", "LightSampling", "Mis"};
static const Names lightPickingNames = {"Power", "Tree"};
static const Names skySamplingNames = {"Auto", "On", "Off"};
static const Names pathSamplingNames = {"WhiteNoise", "Sobol", "SobolBlueNoise"};
static const Names debugViewNames = {
    "None", "Normals", "TexCoords", "MeshIndex", "MaterialIndex", "BaseColor", "Metallic", "Roughness", "Transmission",
    "Emissive", "Tangents", "RadianceCache", "RestirM", "RestirLight", "GuideDepth", "GuideMotionVectors",
    "GuideDiffuseAlbedo", "GuideSpecularAlbedo", "GuideNormals", "GuideRoughness", "GuideSpecularHitDistance",
    "GuideReprojectionError"};
static const Names renderModeNames = {"Accumulation", "DlssDlaa", "DlssQuality", "DlssBalanced", "DlssPerformance",
                                      "DlssUltraPerformance"};
static const Names outputSizeNames = {"Window", "1080p", "1440p", "2160p"};  // Renderer::outputHeights
static const Names dlssModelNames = {"TransformerF", "TransformerE", "TransformerD", "CnnA", "CnnB", "CnnC"};

// A new field in RenderSettings changes its size: add it to the list below (and update the size here)
static_assert(sizeof(RenderSettings) == 252, "RenderSettings changed: add the new field to forEachSetting");

// Calls visit(name, field) for every field, visit(name, field, names) for the choices
template <typename Settings, typename Visitor>
static void forEachSetting(Settings& s, Visitor& visit) {
    visit("shadowsEnabled", s.shadowsEnabled);
    visit("emissiveSampling", s.emissiveSampling, emissiveSamplingNames);
    visit("lightPicking", s.lightPicking, lightPickingNames);
    visit("lightCandidates", s.lightCandidates);
    visit("skySampling", s.skySampling, skySamplingNames);
    visit("punctualLights", s.punctualLights);
    visit("punctualLightScale", s.punctualLightScale);
    visit("samplesPerPixel", s.samplesPerPixel);
    visit("pathSampling", s.pathSampling, pathSamplingNames);
    visit("emissiveSolidAngle", s.emissiveSolidAngle);
    visit("maxBounces", s.maxBounces);
    visit("maxPortalCrossings", s.maxPortalCrossings);
    visit("blurCaustics", s.blurCaustics);
    visit("textureMipmaps", s.textureMipmaps);
    visit("normalMapFiltering", s.normalMapFiltering);
    visit("basicShader", s.basicShader);
    visit("radianceCache", s.radianceCache);
    visit("cacheCellPixels", s.cacheCellPixels);
    visit("cacheUpdateTile", s.cacheUpdateTile);
    visit("cacheHistoryFrames", s.cacheHistoryFrames);
    visit("cacheFog", s.cacheFog);
    visit("cacheMinSamples", s.cacheMinSamples);
    visit("restirDiAccumulation", s.restirDiAccumulation);
    visit("restirDiDlss", s.restirDiDlss);
    visit("restirLightCandidates", s.restirLightCandidates);
    visit("restirSkyCandidates", s.restirSkyCandidates);
    visit("restirBrdfCandidates", s.restirBrdfCandidates);
    visit("restirMinRoughness", s.restirMinRoughness);
    visit("restirTemporal", s.restirTemporal);
    visit("restirTemporalVisibility", s.restirTemporalVisibility);
    visit("restirPermutation", s.restirPermutation);
    visit("restirMaxHistory", s.restirMaxHistory);
    visit("restirSpatial", s.restirSpatial);
    visit("restirSpatialNeighbors", s.restirSpatialNeighbors);
    visit("restirSpatialRadius", s.restirSpatialRadius);
    visit("restirDisocclusionNeighbors", s.restirDisocclusionNeighbors);
    visit("restirSpatialVisibility", s.restirSpatialVisibility);
    visit("restirGiAccumulation", s.restirGiAccumulation);
    visit("restirGiDlss", s.restirGiDlss);
    visit("restirGiMinRoughness", s.restirGiMinRoughness);
    visit("restirGiTemporal", s.restirGiTemporal);
    visit("restirGiTemporalVisibility", s.restirGiTemporalVisibility);
    visit("restirGiPermutation", s.restirGiPermutation);
    visit("restirGiMaxHistory", s.restirGiMaxHistory);
    visit("restirGiMaxAge", s.restirGiMaxAge);
    visit("restirGiSpatial", s.restirGiSpatial);
    visit("restirGiSpatialNeighbors", s.restirGiSpatialNeighbors);
    visit("restirGiSpatialRadius", s.restirGiSpatialRadius);
    visit("restirGiDisocclusionNeighbors", s.restirGiDisocclusionNeighbors);
    visit("restirGiSpatialVisibility", s.restirGiSpatialVisibility);
    visit("restirGiBoilingFilter", s.restirGiBoilingFilter);
    visit("restirGiBoilingStrength", s.restirGiBoilingStrength);
    visit("restirGiClampRadiance", s.restirGiClampRadiance);
    visit("restirGiRadianceClampLevel", s.restirGiRadianceClampLevel);
    visit("clampBounceLight", s.clampBounceLight);
    visit("bounceLightClampLevel", s.bounceLightClampLevel);
    visit("accumulate", s.accumulate);
    visit("exposure", s.exposure);
    visit("autoExposure", s.autoExposure);
    visit("exposureCompensation", s.exposureCompensation);
    visit("exposureKey", s.exposureKey);
    visit("meterLowPercent", s.meterLowPercent);
    visit("meterHighPercent", s.meterHighPercent);
    visit("centerWeighted", s.centerWeighted);
    visit("minExposureStops", s.minExposureStops);
    visit("maxExposureStops", s.maxExposureStops);
    visit("brightenSpeed", s.brightenSpeed);
    visit("darkenSpeed", s.darkenSpeed);
    visit("toneMapping", s.toneMapping);
    visit("fogGrain", s.fogGrain);
    visit("fogGrainStrength", s.fogGrainStrength);
    visit("fogGrainSize", s.fogGrainSize);
    visit("debugView", s.debugView, debugViewNames);
    visit("renderMode", s.renderMode, renderModeNames);
    visit("outputSize", s.outputSize, outputSizeNames);
    visit("dlssModel", s.dlssModel, dlssModelNames);
}

// The shortest decimal that reads back as the same float (0.6, not 0.6000000238418579)
static double shortest(float value) {
    char text[32];
    std::to_chars_result result = std::to_chars(text, text + sizeof(text) - 1, value);
    *result.ptr = '\0';
    return std::strtod(text, nullptr);
}

namespace {

struct Writer {
    nlohmann::ordered_json& json;
    void operator()(const char* name, bool value) { json[name] = value; }
    void operator()(const char* name, int value) { json[name] = value; }
    void operator()(const char* name, float value) { json[name] = shortest(value); }
    void operator()(const char* name, int value, const Names& names) {
        if (value >= 0 && static_cast<size_t>(value) < names.size()) {
            json[name] = names[value];
        } else {
            json[name] = value;
        }
    }
};

struct Reader {
    const nlohmann::json& json;
    std::vector<std::string>& problems;
    std::set<std::string> known;

    // The field's value, or null if the object doesn't have it
    const nlohmann::json* find(const char* name) {
        known.insert(name);
        auto it = json.find(name);
        return it == json.end() ? nullptr : &*it;
    }
    void wrongType(const char* name, const char* expected) {
        problems.push_back(std::string(name) + ": expected " + expected);
    }

    void operator()(const char* name, bool& value) {
        if (const nlohmann::json* v = find(name)) {
            if (v->is_boolean()) value = v->get<bool>(); else wrongType(name, "true or false");
        }
    }
    void operator()(const char* name, int& value) {
        if (const nlohmann::json* v = find(name)) {
            if (v->is_number_integer()) value = v->get<int>(); else wrongType(name, "a whole number");
        }
    }
    void operator()(const char* name, float& value) {
        if (const nlohmann::json* v = find(name)) {
            if (v->is_number()) value = v->get<float>(); else wrongType(name, "a number");
        }
    }
    void operator()(const char* name, int& value, const Names& names) {
        const nlohmann::json* v = find(name);
        if (v == nullptr) {
            return;
        }
        if (v->is_number_integer() && v->get<int>() >= 0 && static_cast<size_t>(v->get<int>()) < names.size()) {
            value = v->get<int>();
            return;
        }
        if (v->is_string()) {
            for (size_t i = 0; i < names.size(); i++) {
                if (v->get<std::string>() == names[i]) {
                    value = static_cast<int>(i);
                    return;
                }
            }
        }
        std::string list;
        for (const char* option : names) {
            list += (list.empty() ? "" : ", ") + std::string(option);
        }
        problems.push_back(std::string(name) + ": expected one of " + list);
    }
};

}  // namespace

nlohmann::ordered_json renderSettingsToJson(const RenderSettings& settings) {
    nlohmann::ordered_json json = nlohmann::ordered_json::object();
    Writer writer{json};
    forEachSetting(settings, writer);
    return json;
}

std::vector<std::string> applyRenderSettingsJson(RenderSettings& settings, const nlohmann::json& json) {
    std::vector<std::string> problems;
    if (!json.is_object()) {
        problems.emplace_back("settings: expected an object");
        return problems;
    }
    Reader reader{json, problems, {}};
    forEachSetting(settings, reader);
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (reader.known.count(it.key()) == 0) {
            problems.push_back(it.key() + ": no such setting");
        }
    }
    return problems;
}
