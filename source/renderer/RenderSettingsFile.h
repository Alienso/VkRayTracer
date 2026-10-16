//
// RenderSettings as JSON: every field by its name in RenderSettings.h, choices (render mode, debug view, ...) by the
// names of their enum values ("renderMode": "DlssDlaa"; numbers are read too).
//
// The fields are the scene's or the program's (scope). The program's are about the machine and what is being done
// right now: render mode, DLSS model, image size, debug view, accumulating, samples per pixel; they are kept between
// runs (app/UserSettings.h). All others are the scene's: saved in scene files (only the ones that differ from the
// defaults), reset by a new scene. Benches (app/Bench.h) set any of them.
//

#ifndef VULKANDEMO_RENDERSETTINGSFILE_H
#define VULKANDEMO_RENDERSETTINGSFILE_H

#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>

struct RenderSettings;

enum class SettingsScope { All, Scene, Program };

// Which scope the field of that name is in (a name that isn't a field: the scene's)
SettingsScope settingScope(const std::string& name);

// The fields of the scope, in the struct's order; onlyChanged: just the ones that differ from the defaults
nlohmann::ordered_json renderSettingsToJson(const RenderSettings& settings, SettingsScope scope = SettingsScope::All,
                                            bool onlyChanged = false);
// Sets the fields the object has, the others stay as they are. Returns what it couldn't read (unknown keys, keys of
// the other scope, wrong types, unknown names): the fields named there are unchanged.
std::vector<std::string> applyRenderSettingsJson(RenderSettings& settings, const nlohmann::json& json,
                                                 SettingsScope scope = SettingsScope::All);
// The scope's fields back to their defaults / taken from another settings object; the others stay
void resetRenderSettings(RenderSettings& settings, SettingsScope scope);
void copyRenderSettings(RenderSettings& to, const RenderSettings& from, SettingsScope scope);

#endif //VULKANDEMO_RENDERSETTINGSFILE_H
