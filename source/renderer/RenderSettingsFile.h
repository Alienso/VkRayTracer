//
// RenderSettings as JSON: every field by its name in Renderer.h, choices (render mode, debug view, ...) by the names
// of their enum values ("renderMode": "DlssDlaa"; numbers are read too). Used by benches (app/Bench.h) to set the
// renderer up from a file.
//

#ifndef VULKANDEMO_RENDERSETTINGSFILE_H
#define VULKANDEMO_RENDERSETTINGSFILE_H

#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>

struct RenderSettings;

// All fields, in the struct's order
nlohmann::ordered_json renderSettingsToJson(const RenderSettings& settings);
// Sets the fields the object has, the others stay as they are. Returns what it couldn't read (unknown keys, wrong
// types, unknown names): the fields named there are unchanged.
std::vector<std::string> applyRenderSettingsJson(RenderSettings& settings, const nlohmann::json& json);

#endif //VULKANDEMO_RENDERSETTINGSFILE_H
