#pragma once
// Source control in the Content Browser (Unreal's Source Control, Git provider): status badges on
// assets, and per asset History, Diff and Submit; with Git LFS, Lock / Unlock (Check Out / Check In)
// and a lock badge (yours or a teammate's). Runs the `git` command line in the project folder;
// does nothing when the project is not a Git repository or git is not installed.
// shortcut: Git only, through the command line; add Perforce / a library (libgit2) if a team needs it.

#include <string>

namespace lsd::editor::source_control {

void start();  // a background thread reads `git status` every few seconds
void stop();
void refresh();  // read it again now (after saving, importing...)

// The asset's state for its badge: 'M' modified, 'A' new (added or untracked), 'R' renamed, 0 unchanged.
// `content_path` is relative to Content/ ("Levels/Main.lsdlevel"); its .meta counts too.
char status(const char* content_path);
// Git LFS file locking (Unreal's Check Out): who holds the asset's lock, "" if nobody; `mine` when
// it is this user. Read with `git lfs locks --verify` every 20 seconds (a server round trip).
std::string locked_by(const char* content_path, bool& mine);

void asset_menu(const char* content_path);  // a "Source Control" submenu for the asset's right-click menu
void open_changes();  // the Changes window (Unreal's View Changelists): every changed asset, submit or revert
void draw();  // its History / Diff window and the Submit dialog

} // namespace lsd::editor::source_control
