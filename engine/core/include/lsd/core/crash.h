#pragma once
// Crash reporting: when the program crashes, a minidump (Windows; open it in Visual Studio to see
// the call stack) and a copy of the log go into a folder of their own, and a marker remembers it so
// the next start can say so (Unreal's crash reporter, kept on this PC).
#include "lsd/foundation/types.h"

namespace lsd {

// Installs the handler. `crash_folder` gets <date-time>/ with crash.dmp and the log. If the last
// session crashed, `previous` receives that folder (and the marker is cleared); else "".
void install_crash_reporting(const char* crash_folder, char* previous, usize capacity);

} // namespace lsd
