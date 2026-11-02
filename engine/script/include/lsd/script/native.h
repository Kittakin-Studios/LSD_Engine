#pragma once
// Behaviours from game code (C / C++ in the project's Game.dll, written against lsd_game.h). A module
// registers them into a NativeModule; ScriptSystem runs them on Native Script components next to the
// Lua scripts, with the same events. Unreal: the classes of your project's C++ module.
#include "lsd/foundation/types.h"
#include "lsd/lsd_game.h"

namespace lsd::script {

struct NativeModule {
    static constexpr u32 kMaxBehaviours = 128;
    lsd_behaviour behaviours[kMaxBehaviours];
    char names[kMaxBehaviours][64];  // copies, for listing; the module's own strings live in its DLL
    u32 count = 0;
    // Visual-script nodes the module registered (C / C++ nodes); valid while the DLL is loaded.
    static constexpr u32 kMaxFunctions = 128;
    lsd_function functions[kMaxFunctions];
    char function_names[kMaxFunctions][64];
    u32 function_count = 0;

    void clear() {
        count = 0;
        function_count = 0;
    }
    const lsd_behaviour* find(const char* name) const;
    const lsd_function* find_function(const char* name) const;
    lsd_registry* registry() { return reinterpret_cast<lsd_registry*>(this); }
};

// The engine's function table handed to modules (lsd_game_register). Its calls act on the
// ScriptSystem that is playing; outside Play they do nothing and return 0.
const lsd_api* native_api();

// The module's functions as visual-script nodes ("Cpp_<name>", calling lsd.native_call). Returns how many.
u32 register_native_nodes(const NativeModule& module);

// A property's default as text ("90", "true", "Door"), read from a fresh state of the behaviour.
void native_property_default(const lsd_behaviour& behaviour, const lsd_property& property, char* out, usize capacity);

} // namespace lsd::script
