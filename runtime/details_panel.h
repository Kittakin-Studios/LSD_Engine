#pragma once
// Debug Details panel: draws any reflected object with Dear ImGui, grouped by Category.
// The editor's real Details panel (milestone 3) replaces this but reads the same TypeInfo.
#include "lsd/reflect/reflect.h"

namespace lsd {

// Draws the object's Edit and Visible properties into the current ImGui window.
// Returns true if the user changed a value this frame.
bool draw_details(const reflect::TypeInfo& type, void* object);

} // namespace lsd
