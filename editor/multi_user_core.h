#pragma once
// Multi-user editing's level edits (Unreal's Multi-User Editing / Concert transactions), no network
// here: each editor keeps a Baseline, the level as every user last agreed on it, one text per object
// (the level file's own format). After a local edit diff() gives the change as ops and moves the
// baseline; another editor's apply() runs them. Ops are JSON:
//   {"settings": {...WorldSettings}, "remove": [ids], "set": [{"id": 3, "name": "Cube", ...}]}
// each part only when something in it changed. Ids are the entity handles, the same on every editor
// because joiners load the host's level text.
// shortcut: two users creating an object in the same instant get the same free id and the later op
// replaces the other's object; let the host hand out ids when teams edit one level all day.
#include "lsd/world/world.h"

#include <string>
#include <unordered_map>

namespace lsd::editor::multi_user {

struct Baseline {
    std::unordered_map<u32, std::string> objects;  // by entity handle value (editor-time STL)
    std::string settings;                          // World Settings
};

void capture(const world::World& world, Baseline& base);  // the world as it is now
// The ops from `base` to the world as it is now ("" when nothing changed); `base` then matches it.
std::string diff(const world::World& world, Baseline& base);
// Runs another editor's ops on `world` and moves `base` to match (so they are not sent back).
// False on malformed text (nothing after the bad part is applied).
bool apply(world::World& world, const char* ops, usize length, Baseline& base);

} // namespace lsd::editor::multi_user
