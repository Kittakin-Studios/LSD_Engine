#pragma once
// Multi-user editing (Unreal's Multi-User Editing / Concert): editors of the same project edit one
// level together over ENet (engine/net). One editor hosts, the others join by address or join code
// and get the host's level as it is. Every finished edit (each undo step: move, Details change,
// create, delete, rename, undo / redo itself) travels as multi_user_core ops; the host applies a
// joiner's ops and passes them on. Each user's camera and selection show to the others as a
// coloured marker with their name.
// shortcut: no locking and no conflict resolution (the last op wins per object), assets are not
// sent (every editor needs the same Content/, e.g. from source control), Play pauses syncing (edits
// that arrive are applied on Stop); add object locks and asset transfer when teams need them.
#include "lsd/foundation/types.h"

namespace lsd::editor {

struct EditorState;

namespace multi_user {

constexpr u16 kDefaultPort = 7790;  // Play's game sessions default to 7777, the remote console to 7780

void register_commands(EditorState& s);  // multiuser.Host / Join / Leave / Users / Name
void update(EditorState& s);             // once a frame, before the UI: network, edits both ways, markers
void draw_window(EditorState& s);        // Window > Multi-User Editing
bool host(EditorState& s, u16 port);
bool join(EditorState& s, const char* code_or_address);
void leave();

} // namespace multi_user
} // namespace lsd::editor
