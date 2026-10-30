#pragma once
// Debug Details panel: draws any reflected object with Dear ImGui, grouped by Category.
// The editor's real Details panel (milestone 3) replaces this but reads the same TypeInfo.
#include "lsd/reflect/reflect.h"

namespace lsd {

// Draws the value widget for properties with a Widget hint the panel does not know ("Asset:...",
// "Key"). The label is already drawn and the item width set. Return false to fall back to the default.
using CustomWidget = bool (*)(void* user, const reflect::Property& property, u8* field, bool& changed);

struct DetailsStyle {
    u32 row_colors[2];             // alternating row backgrounds, packed ImU32 (IM_COL32)
    const char* filter = nullptr;  // show only properties whose name contains this (case-insensitive)
    CustomWidget custom_widget = nullptr;
    void* custom_user = nullptr;
    // Prefab overrides: overridden properties get a highlighted name and "Revert to Prefab" in
    // their right-click menu (called with custom_user; only top-level properties are asked about).
    bool (*is_overridden)(void* user, const reflect::Property& property) = nullptr;
    void (*revert)(void* user, const reflect::Property& property) = nullptr;
};

// Draws the object's Edit and Visible properties into the current ImGui window.
// Right-click a value: Reset to Default (from the type's defaults), Copy, Paste.
// Returns true if the user changed a value this frame.
bool draw_details(const reflect::TypeInfo& type, void* object, const DetailsStyle* style = nullptr);

// Same rows without the category headers, for callers that draw their own header (components).
bool draw_properties(const reflect::TypeInfo& type, void* object, const DetailsStyle* style = nullptr);

} // namespace lsd
