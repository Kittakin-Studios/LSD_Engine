#pragma once
// Live Link (Unreal's Live Link, file based): the editor watches a folder a DCC exports to and copies
// new or changed meshes, textures and sounds into Content/LiveLink (sub-folders kept). They then
// reload like any changed asset, in the editor and in Play.
// shortcut: polling once a second, copies only (no deletes); a Blender / Maya add-on that exports on
// save would remove the export step (written by us, it must not take GPL code from the DCC).

namespace lsd::editor {

// Copies what changed since the last call; returns how many files (0 when the folder is empty or off).
unsigned live_link_sync(const char* watched_folder, const char* content_dir, char* status, unsigned long long status_capacity);

} // namespace lsd::editor
