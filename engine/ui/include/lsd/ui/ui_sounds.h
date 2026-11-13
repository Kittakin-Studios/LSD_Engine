#pragma once
// Button sounds (UMG's Hovered / Pressed sounds), the rules without RmlUi so tests can check them.
// A clickable element plays the hover sound once when the mouse or the gamepad / keyboard focus
// enters it and the click sound when it is pressed (ui.h: set_sounds).
#include "lsd/foundation/types.h"

#include <cstring>
#include <initializer_list>

namespace lsd::ui {

// Clickable: a <button> or <select>, an <input> of type button / submit / checkbox / radio, anything
// with an onclick attribute or a data-hover-sound / data-click-sound of its own; never a disabled one.
inline bool is_clickable(const char* tag, const char* input_type, bool has_onclick, bool has_sound_attribute, bool disabled) {
    if (disabled) return false;
    if (has_onclick || has_sound_attribute) return true;
    if (std::strcmp(tag, "button") == 0 || std::strcmp(tag, "select") == 0) return true;
    if (std::strcmp(tag, "input") != 0 || !input_type) return false;
    for (const char* t : {"button", "submit", "checkbox", "radio"}) {
        if (std::strcmp(input_type, t) == 0) return true;
    }
    return false;
}

// One hover sound per element entered: `element` is the clickable element under the pointer (or
// focused) this frame, nullptr if none; `moved` is whether the pointer moved (or the focus was moved
// by the arrows / D-pad) since the last frame. Staying on an element, or a document opening under a
// still cursor, plays nothing. True: play the hover sound now.
// shortcut: elements are compared by address (a freed element's address reused by a new one under the
// pointer skips one hover sound); keep RmlUi ObserverPtrs if that ever shows.
struct HoverTracker {
    const void* last = nullptr;
    bool enter(const void* element, bool moved) {
        if (element == last) return false;
        last = element;
        return element && moved;
    }
};

} // namespace lsd::ui
