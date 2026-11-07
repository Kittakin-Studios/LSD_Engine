#pragma once
// Gather Text (Unreal's Localization Dashboard > Gather Text): the keys a project shows, found in
// its scripts (lsd.text("KEY") in .lua / .js), graphs (Localized Text / Format Text nodes) and UI
// documents (each element's text, which translates when it is a key), added to the string table.
#include "editor_settings.h"

#include <string>
#include <vector>

namespace lsd::asset {
class AssetRegistry;
}

namespace lsd::editor {

struct GatheredText {
    std::string key;
    std::string text;  // what the first language column starts with ("" for script keys: to be written)
};

// From one file's contents, appended to `out` (duplicates kept; merge drops them).
void gather_script_text(const char* source, std::vector<GatheredText>& out);  // lsd.text("KEY" ...)
void gather_rml_text(const char* rml, std::vector<GatheredText>& out);        // element texts outside head / style / script
// The string table CSV (Key, then a column per language) with the keys it lacks added at the end;
// an empty `csv` starts "Key,en". Returns how many were added.
u32 merge_string_table(const std::string& csv, const std::vector<GatheredText>& found, std::string& out);

// The whole project: every script, graph and UI document in Content/, merged into the project's
// string table (made as Content/Localization/ST_Game.csv and set in the project when there is none:
// `project_changed`, save the project then). Writes a summary into `message`; false if the table
// cannot be written.
bool gather_project_text(ProjectSettings& project, asset::AssetRegistry& registry, char* message, usize capacity, bool& project_changed);

} // namespace lsd::editor
