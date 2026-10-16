//
// RenderSettings as JSON (renderer/RenderSettingsFile.h): every field written and read back, choices by name or
// number, and what can't be read reported without touching the field.
//

#include <catch_amalgamated.hpp>
#include <nlohmann/json.hpp>
#include "renderer/RenderSettingsFile.h"
#include "renderer/RendererTypes.h"

static std::string text(const RenderSettings& settings) { return renderSettingsToJson(settings).dump(); }

TEST_CASE("RenderSettings JSON: written and read back gives the same settings", "[RenderSettingsFile]") {
    RenderSettings defaults;
    nlohmann::json json = renderSettingsToJson(defaults);
    // Read onto settings that differ in every field the JSON has: each one must come back
    RenderSettings changed;
    nlohmann::json other = json;
    for (auto& [key, value] : other.items()) {
        if (value.is_boolean()) {
            value = !value.get<bool>();
        } else if (value.is_number_integer()) {
            value = value.get<int>() + 1;
        } else if (value.is_number()) {
            value = value.get<double>() + 0.5;
        }
        // (choices are names: left as they are)
    }
    REQUIRE(applyRenderSettingsJson(changed, other).empty());
    CHECK(text(changed) != text(defaults));
    REQUIRE(applyRenderSettingsJson(changed, json).empty());
    CHECK(text(changed) == text(defaults));
}

TEST_CASE("RenderSettings JSON: choices are written by name and read by name or number", "[RenderSettingsFile]") {
    RenderSettings settings;
    settings.renderMode = static_cast<int>(RenderMode::DlssDlaa);
    CHECK(renderSettingsToJson(settings)["renderMode"] == "DlssDlaa");

    RenderSettings read;
    CHECK(applyRenderSettingsJson(read, nlohmann::json{{"renderMode", "DlssQuality"}}).empty());
    CHECK(read.renderMode == static_cast<int>(RenderMode::DlssQuality));
    CHECK(applyRenderSettingsJson(read, nlohmann::json{{"renderMode", 0}}).empty());
    CHECK(read.renderMode == static_cast<int>(RenderMode::Accumulation));
}

TEST_CASE("RenderSettings JSON: every name of every choice reads back as its value", "[RenderSettingsFile]") {
    // Each choice field written for every value until the writer runs out of names (then it writes the number)
    for (const char* field : {"emissiveSampling", "lightPicking", "skySampling", "pathSampling", "debugView",
                              "renderMode", "outputSize", "dlssModel"}) {
        INFO(field);
        int count = 0;
        for (int value = 0; value < 64; value++) {
            RenderSettings settings;
            nlohmann::json number = {{field, value}};
            if (!applyRenderSettingsJson(settings, number).empty()) {
                break;  // past the last name
            }
            nlohmann::json written = renderSettingsToJson(settings);
            REQUIRE(written[field].is_string());
            RenderSettings again;
            REQUIRE(applyRenderSettingsJson(again, nlohmann::json{{field, written[field]}}).empty());
            CHECK(text(again) == text(settings));
            count++;
        }
        CHECK(count >= 2);
    }
}

TEST_CASE("RenderSettings JSON: the debug view names match the debug panel's list", "[RenderSettingsFile]") {
    // EditorRenderer.cpp's "Debug view" combo has 22 entries, Off first
    RenderSettings settings;
    CHECK(applyRenderSettingsJson(settings, nlohmann::json{{"debugView", 21}}).empty());
    CHECK_FALSE(applyRenderSettingsJson(settings, nlohmann::json{{"debugView", 22}}).empty());
}

TEST_CASE("RenderSettings JSON: what can't be read is reported and left unchanged", "[RenderSettingsFile]") {
    RenderSettings settings;
    std::string before = text(settings);
    CHECK(applyRenderSettingsJson(settings, nlohmann::json{{"renderMode", "Fastest"}}).size() == 1);
    CHECK(applyRenderSettingsJson(settings, nlohmann::json{{"maxBounces", "four"}}).size() == 1);
    CHECK(applyRenderSettingsJson(settings, nlohmann::json{{"maxBounces", 4.5}}).size() == 1);
    CHECK(applyRenderSettingsJson(settings, nlohmann::json{{"accumulate", 1}}).size() == 1);
    CHECK(applyRenderSettingsJson(settings, nlohmann::json{{"noSuchSetting", 1}}).size() == 1);
    CHECK(applyRenderSettingsJson(settings, nlohmann::json::array()).size() == 1);
    CHECK(text(settings) == before);
    // A float field takes a whole number
    CHECK(applyRenderSettingsJson(settings, nlohmann::json{{"exposure", 2}}).empty());
    CHECK(settings.exposure == 2.0f);
}

TEST_CASE("RenderSettings JSON: floats are written as the shortest number that reads back", "[RenderSettingsFile]") {
    RenderSettings settings;
    settings.exposure = 0.3f;
    CHECK(renderSettingsToJson(settings)["exposure"].dump() == "0.3");
}
