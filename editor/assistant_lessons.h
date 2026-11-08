#pragma once
// LSD Assistant > Lessons: worked examples that teach how a feature is coded, step by step, with the
// finished files. Apply sets the lesson up on the selected object (files, input, socket, component),
// so it works at once and can be read and changed afterwards. Console: assistant.Lesson <n>.
#include "editor_ui.h"

#include <string>

namespace lsd::editor::assistant {

struct Lesson {
    const char* title;
    const char* summary;
    // The teaching text; its fenced code blocks headed with a path ("```lua Content/Weapon/Weapon.lua")
    // are the files Apply writes.
    const char* text;
};

const Lesson* lessons(u32& count);
// Sets lesson `index` up on the selected object; false when it cannot, `report` says what was done or why not.
bool apply_lesson(EditorState& s, u32 index, std::string& report);

} // namespace lsd::editor::assistant
