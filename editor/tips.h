#pragma once

#include <imgui.h>

namespace lsd::editor {

// Hover help for the widget just drawn. Returns what the widget returned, so it wraps the call:
// if (with_tip(ImGui::Button("Save"), "Writes it to disk")) ...
inline bool with_tip(bool result, const char* text) {
    ImGui::SetItemTooltip("%s", text);
    return result;
}

} // namespace lsd::editor
