#include "lsd/script/graph.h"

#include "lsd/foundation/log.h"
#include "lsd/reflect/json.h"

#include <cctype>
#include <deque>
#include <iterator>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace lsd::script {

namespace {

constexpr PinDef kExecIn{"", PinType::Exec, ""};
constexpr PinDef kExecOut{"", PinType::Exec, ""};
constexpr PinDef kTarget{"Target", PinType::Entity, "self"};

// The node library. Input / output counts are filled in by node_defs().
NodeDef g_defs[] = {
    // ---- Events ----
    {"OnStart", "Event Start", "Events", "Once, when Play starts (Event BeginPlay)", {}, 0, {kExecOut}, 0, "function on_start(self)", {}},
    {"OnUpdate", "Event Update", "Events", "Every frame (Event Tick)", {}, 0, {kExecOut, {"Delta Seconds", PinType::Float, ""}}, 0,
     "function on_update(self, dt)", {nullptr, "dt"}},
    {"OnHit", "Event Hit", "Events", "A physics contact with another object began", {}, 0, {kExecOut, {"Other", PinType::Entity, ""}}, 0,
     "function on_hit(self, other)", {nullptr, "other"}},
    {"OnOverlapBegin", "Event Overlap Begin", "Events", "Something entered this Trigger Volume (or this entered one)", {}, 0,
     {kExecOut, {"Other", PinType::Entity, ""}}, 0, "function on_overlap_begin(self, other)", {nullptr, "other"}},
    {"OnOverlapEnd", "Event Overlap End", "Events", "Something left this Trigger Volume (or this left one)", {}, 0,
     {kExecOut, {"Other", PinType::Entity, ""}}, 0, "function on_overlap_end(self, other)", {nullptr, "other"}},
    {"CustomEvent", "Custom Event", "Events", "Runs when Call Event names it (an event dispatcher); Value is what was sent",
     {{"Name", PinType::String, "my_event"}}, 0, {kExecOut, {"Value", PinType::Float, ""}}, 0, "function $n0(self, value)", {nullptr, "value"}},
    {"CallEvent", "Call Event", "Events", "Runs that Custom Event on every object that has it (Call Event Dispatcher)",
     {kExecIn, {"Name", PinType::String, "my_event"}, {"Value", PinType::Float, "0"}}, 0, {kExecOut}, 0, "lsd.broadcast($1, $2)", {}},
    {"CallEventOn", "Call Event on Object", "Events", "Runs that Custom Event on one object only (an interface message)",
     {kExecIn, kTarget, {"Name", PinType::String, "my_event"}, {"Value", PinType::Float, "0"}}, 0, {kExecOut}, 0, "lsd.call($1, $2, $3)", {}},
    {"HasEvent", "Has Event", "Events", "True if the object's scripts define that event (Does Implement Interface)",
     {kTarget, {"Name", PinType::String, "my_event"}}, 0, {{"Has", PinType::Bool, ""}}, 0, nullptr, {"lsd.implements($0, $1)"}},
    // ---- Debug ----
    {"DrawDebugLine", "Draw Debug Line", "Debug", "A line over the view for some seconds (0: this frame)",
     {kExecIn, {"Start", PinType::Vector, "0,0,0"}, {"End", PinType::Vector, "0,0,2"}, {"Color", PinType::Vector, "1,0,0"}, {"Duration", PinType::Float, "0"}}, 0,
     {kExecOut}, 0, "do local a, b, c = $1, $2, $3 lsd.draw_line(a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z, $4) end", {}},
    {"DrawDebugSphere", "Draw Debug Sphere", "Debug", "A wire sphere over the view for some seconds (0: this frame)",
     {kExecIn, {"Center", PinType::Vector, "0,0,1"}, {"Radius", PinType::Float, "1"}, {"Color", PinType::Vector, "0,1,0"}, {"Duration", PinType::Float, "0"}}, 0,
     {kExecOut}, 0, "do local p, c = $1, $3 lsd.draw_sphere(p.x, p.y, p.z, $2, c.x, c.y, c.z, $4) end", {}},
    {"DrawDebugString", "Draw Debug String", "Debug", "Text at a place in the world for some seconds (0: this frame)",
     {kExecIn, {"Location", PinType::Vector, "0,0,2"}, {"Text", PinType::String, "Hello"}, {"Color", PinType::Vector, "1,1,1"}, {"Duration", PinType::Float, "0"}}, 0,
     {kExecOut}, 0, "do local p, c = $1, $3 lsd.draw_text(p.x, p.y, p.z, $2, c.x, c.y, c.z, $4) end", {}},
    // ---- Flow ----
    {"Branch", "Branch", "Flow", "True or False path by a condition", {kExecIn, {"Condition", PinType::Bool, "true"}}, 0,
     {{"True", PinType::Exec, ""}, {"False", PinType::Exec, ""}}, 0, "if $1 then\n#0\nelse\n#1\nend", {}},
    {"Sequence", "Sequence", "Flow", "Runs each output in order", {kExecIn}, 0,
     {{"Then 0", PinType::Exec, ""}, {"Then 1", PinType::Exec, ""}, {"Then 2", PinType::Exec, ""}}, 0, "#0\n#1\n#2", {}},
    {"Delay", "Delay", "Flow", "Continues after some seconds", {kExecIn, {"Seconds", PinType::Float, "1"}}, 0,
     {{"Completed", PinType::Exec, ""}}, 0, "lsd.after($1, function()\n#0\nend)", {}},
    {"DoOnce", "Do Once", "Flow", "Lets execution through the first time only", {kExecIn}, 0, {kExecOut}, 0,
     "if not self.__once$id then\nself.__once$id = true\n#0\nend", {}},
    {"SetTimer", "Set Timer", "Flow", "Runs Tick every so many seconds (Set Timer by Event, looping)", {kExecIn, {"Seconds", PinType::Float, "1"}}, 0,
     {{"Then", PinType::Exec, ""}, {"Tick", PinType::Exec, ""}}, 0, "lsd.every($1, function()\n#1\nend)\n#0", {}},
    // ---- Actions ----
    {"PrintString", "Print String", "Utilities", "Writes to the Output Log", {kExecIn, {"Text", PinType::String, "Hello"}}, 0, {kExecOut}, 0,
     "lsd.log($1)", {}},
    {"SetPosition", "Set Location", "Transform", "Moves an object (local Transform, metres)", {kExecIn, kTarget, {"Location", PinType::Vector, "0,0,0"}}, 0,
     {kExecOut}, 0, "__setpos($1, $2)", {}},
    {"AddOffset", "Add Offset", "Transform", "Moves an object by an offset", {kExecIn, kTarget, {"Offset", PinType::Vector, "0,0,1"}}, 0,
     {kExecOut}, 0, "__addpos($1, $2)", {}},
    {"SetRotation", "Set Rotation", "Transform", "Turns an object (degrees, X then Y then Z)", {kExecIn, kTarget, {"Rotation", PinType::Vector, "0,0,0"}}, 0,
     {kExecOut}, 0, "__setrot($1, $2)", {}},
    {"AddRotation", "Add Rotation", "Transform", "Turns an object by some degrees", {kExecIn, kTarget, {"Delta", PinType::Vector, "0,0,90"}}, 0,
     {kExecOut}, 0, "__addrot($1, $2)", {}},
    {"SetColor", "Set Color", "Rendering", "Recolours an object's Static Mesh (r, g, b from 0 to 1)", {kExecIn, kTarget, {"Color", PinType::Vector, "1,0,0"}}, 0,
     {kExecOut}, 0, "__color($1, $2)", {}},
    {"Spawn", "Spawn", "Objects", "Creates a prefab, mesh, particle system or shape (Cube, Sphere) by name", {kExecIn, {"Name", PinType::String, "Cube"}, {"Location", PinType::Vector, "0,0,2"}}, 0,
     {kExecOut, {"Spawned", PinType::Entity, ""}}, 0, "local $o1 = __spawn($1, $2)", {}},
    // ---- Camera ----
    {"SetViewTarget", "Set View Target with Blend", "Camera", "Looks through an object's Camera (unconnected: this object's), blending over Blend Time seconds; Set View Target to Player goes back",
     {kExecIn, {"Camera", PinType::Entity, ""}, {"Blend Time", PinType::Float, "1"}}, 0, {kExecOut}, 0, "lsd.set_view($1, $2)", {}},
    {"SetViewToPlayer", "Set View Target to Player", "Camera", "Back to the player's own view, blending over Blend Time seconds",
     {kExecIn, {"Blend Time", PinType::Float, "1"}}, 0, {kExecOut}, 0, "lsd.set_view(nil, $1)", {}},
    {"CameraShake", "Camera Shake", "Camera", "Shakes the view: Strength 1 is a hard hit, fading out over Duration seconds",
     {kExecIn, {"Strength", PinType::Float, "0.5"}, {"Duration", PinType::Float, "0.5"}, {"Frequency", PinType::Float, "12"}}, 0, {kExecOut}, 0,
     "lsd.camera_shake($1, $2, $3)", {}},
    // ---- Collision (Unreal's Line / Sphere / Box Trace and Overlap Actors) ----
    {"LineTrace", "Line Trace", "Collision", "The first object along a line from Start to End",
     {kExecIn, {"Start", PinType::Vector, "0,0,1"}, {"End", PinType::Vector, "10,0,1"}}, 0,
     {kExecOut, {"Hit", PinType::Bool, ""}, {"Hit Object", PinType::Entity, ""}, {"Location", PinType::Vector, ""}, {"Normal", PinType::Vector, ""}}, 0,
     "local $o1, $o2, $o3, $o4 = __trace(0, $1, $2, 0, 0)", {}},
    {"SphereTrace", "Sphere Trace", "Collision", "A sphere swept from Start to End: the first object it touches",
     {kExecIn, {"Start", PinType::Vector, "0,0,1"}, {"End", PinType::Vector, "10,0,1"}, {"Radius", PinType::Float, "0.5"}}, 0,
     {kExecOut, {"Hit", PinType::Bool, ""}, {"Hit Object", PinType::Entity, ""}, {"Location", PinType::Vector, ""}, {"Normal", PinType::Vector, ""}}, 0,
     "local $o1, $o2, $o3, $o4 = __trace(1, $1, $2, $3, 0)", {}},
    {"CapsuleTrace", "Capsule Trace", "Collision", "An upright capsule (a body's size) swept from Start to End",
     {kExecIn, {"Start", PinType::Vector, "0,0,1"}, {"End", PinType::Vector, "10,0,1"}, {"Radius", PinType::Float, "0.4"}, {"Half Height", PinType::Float, "0.5"}}, 0,
     {kExecOut, {"Hit", PinType::Bool, ""}, {"Hit Object", PinType::Entity, ""}, {"Location", PinType::Vector, ""}, {"Normal", PinType::Vector, ""}}, 0,
     "local $o1, $o2, $o3, $o4 = __trace(2, $1, $2, $3, $4)", {}},
    {"SphereOverlap", "Sphere Overlap", "Collision", "Runs Loop Body for every object within Radius of Center, then Completed",
     {kExecIn, {"Center", PinType::Vector, "0,0,1"}, {"Radius", PinType::Float, "2"}}, 0,
     {{"Completed", PinType::Exec, ""}, {"Loop Body", PinType::Exec, ""}, {"Object", PinType::Entity, ""}, {"Count", PinType::Float, ""}}, 0,
     "do local __c = $1 local __list = lsd.overlap_sphere(__c.x, __c.y, __c.z, $2)\nfor _, $o2 in ipairs(__list) do\n#1\nend\nlocal $o3 = #__list\n#0\nend", {}},
    {"PlayTimeline", "Play Timeline", "Cinematics", "Plays a Timeline (cutscene) by name; Custom Event on_timeline_finished runs when it ends",
     {kExecIn, {"Timeline", PinType::String, "TL_Intro"}, {"Loop", PinType::Bool, "false"}}, 0, {kExecOut}, 0, "lsd.play_timeline($1, $2)", {}},
    {"StopTimeline", "Stop Timeline", "Cinematics", "Stops a playing Timeline where it is", {kExecIn, {"Timeline", PinType::String, "TL_Intro"}}, 0, {kExecOut}, 0,
     "lsd.stop_timeline($1)", {}},
    {"PlayMusic", "Play Music", "Audio", "Crossfades to a music track (sound asset by name) over Fade seconds; the same track again does nothing",
     {kExecIn, {"Music", PinType::String, "Music"}, {"Fade", PinType::Float, "1"}}, 0, {kExecOut}, 0, "lsd.play_music($1, $2)", {}},
    {"StopMusic", "Stop Music", "Audio", "Fades the music and its layers out", {kExecIn, {"Fade", PinType::Float, "1"}}, 0, {kExecOut}, 0, "lsd.stop_music($1)", {}},
    {"MusicLayer", "Set Music Layer", "Audio", "Fades a stem (playing in step with the track) to a volume: more drums in combat, strings when calm",
     {kExecIn, {"Layer", PinType::String, "Drums"}, {"Volume", PinType::Float, "1"}, {"Fade", PinType::Float, "1"}}, 0, {kExecOut}, 0, "lsd.music_layer($1, $2, $3)", {}},
    {"PlaySound2D", "Play Sound 2D", "Audio", "Plays a sound asset (by name) everywhere: UI, music, stingers",
     {kExecIn, {"Sound", PinType::String, "Click"}}, 0, {kExecOut}, 0, "lsd.play_sound($1)", {}},
    {"PlaySoundAt", "Play Sound at Location", "Audio", "Plays a sound asset (by name) in the world, faded by distance",
     {kExecIn, {"Sound", PinType::String, "Click"}, {"Location", PinType::Vector, "0,0,0"}}, 0, {kExecOut}, 0, "__sound($1, $2)", {}},
    {"PushSoundMix", "Push Sound Mix", "Audio", "Applies a Sound Mix (by name) until it is popped: duck music, muffle the game",
     {kExecIn, {"Mix", PinType::String, "MIX_Duck"}}, 0, {kExecOut}, 0, "lsd.push_mix($1)", {}},
    {"PopSoundMix", "Pop Sound Mix", "Audio", "Removes the latest push of a Sound Mix (fading out)", {kExecIn, {"Mix", PinType::String, "MIX_Duck"}}, 0,
     {kExecOut}, 0, "lsd.pop_mix($1)", {}},
    {"SetClassVolume", "Set Sound Class Volume", "Audio", "A Sound Class's volume by name (Master, Music, SFX...): settings sliders",
     {kExecIn, {"Class", PinType::String, "Master"}, {"Volume", PinType::Float, "1"}}, 0, {kExecOut}, 0, "lsd.set_class_volume($1, $2)", {}},
    {"SetAudioParam", "Set Audio Parameter", "Audio", "A value Sound Cue Switch nodes read (surface type, intensity...)",
     {kExecIn, {"Name", PinType::String, "Surface"}, {"Value", PinType::Float, "0"}}, 0, {kExecOut}, 0, "lsd.set_audio_param($1, $2)", {}},
    {"SetAnimParam", "Set Anim Parameter", "Animation", "A parameter of the target's Animation Graph (Speed, InAir, or your own)",
     {kExecIn, kTarget, {"Name", PinType::String, "Speed"}, {"Value", PinType::Float, "0"}}, 0, {kExecOut}, 0, "lsd.set_anim_param($1, $2, $3)", {}},
    {"SetMorph", "Set Morph Target", "Animation", "A morph target (blend shape) weight on the target's skeletal mesh: 0 rest, 1 full",
     {kExecIn, kTarget, {"Name", PinType::String, "Smile"}, {"Weight", PinType::Float, "1"}}, 0, {kExecOut}, 0, "lsd.set_morph($1, $2, $3)", {}},
    {"AIMoveTo", "AI Move To", "AI", "Walks the target (it needs a Nav Agent) to a location on the navmesh, round obstacles and other agents",
     {kExecIn, kTarget, {"Location", PinType::Vector, "0,0,0"}}, 0, {kExecOut}, 0, "lsd.move_to($1, $2.x, $2.y, $2.z)", {}},
    {"BbSet", "Set Blackboard Value", "AI", "A number on the target's AI Controller blackboard (its behaviour tree reads it)",
     {kExecIn, kTarget, {"Key", PinType::String, "Alert"}, {"Value", PinType::Float, "1"}}, 0, {kExecOut}, 0, "lsd.bb_set($1, $2, $3)", {}},
    {"ReportNoise", "Report Noise", "AI", "AI Controllers within hearing range (times Loudness) hear it: NoiseLocation",
     {kExecIn, {"Location", PinType::Vector, "0,0,0"}, {"Loudness", PinType::Float, "1"}}, 0, {kExecOut}, 0, "lsd.report_noise($1.x, $1.y, $1.z, $2)", {}},
    {"UiOpen", "Open UI", "UI", "Shows a UI document (.rml) by asset name over the game; its handle is for Set UI Text and Close UI",
     {kExecIn, {"Document", PinType::String, "UI_HUD"}}, 0, {kExecOut, {"Handle", PinType::Float, ""}}, 0, "local $o1 = lsd.ui_open($1) or -1", {}},
    {"UiClose", "Close UI", "UI", "Removes a UI document", {kExecIn, {"Handle", PinType::Float, "0"}}, 0, {kExecOut}, 0, "lsd.ui_close($1)", {}},
    {"UiSetText", "Set UI Text", "UI", "Changes the text of the element with that id (score, health, prompts)",
     {kExecIn, {"Handle", PinType::Float, "0"}, {"Element Id", PinType::String, "score"}, {"Text", PinType::String, ""}}, 0, {kExecOut}, 0,
     "lsd.ui_set_text($1, $2, $3)", {}},
    {"LocText", "Localized Text", "UI", "A string table key's text in the player's language (Unreal's FText from a String Table)",
     {{"Key", PinType::String, "MENU_PLAY"}}, 0, {{"Text", PinType::String, ""}}, 0, nullptr, {"lsd.text($0)"}},
    {"FormatText", "Format Text", "UI", "A string table key's text with {0} and {1} filled in, plurals picked by number (Unreal's Format Text)",
     {{"Key", PinType::String, "SCORE"}, {"0", PinType::String, ""}, {"1", PinType::String, ""}}, 0, {{"Text", PinType::String, ""}}, 0, nullptr,
     {"lsd.text($0, $1, $2)"}},
    {"UiCursor", "Show Mouse Cursor", "UI", "On: the mouse moves a cursor for menus; off: it controls the player again",
     {kExecIn, {"Show", PinType::Bool, "true"}}, 0, {kExecOut}, 0, "lsd.ui_show_cursor($1)", {}},
    {"AIStop", "AI Stop Moving", "AI", "Stops a Nav Agent where it is", {kExecIn, kTarget}, 0, {kExecOut}, 0, "lsd.stop_move($1)", {}},
    {"AIIsMoving", "Is Moving", "AI", "True while a Nav Agent is still on its way", {kTarget}, 0, {{"Moving", PinType::Bool, ""}}, 0, nullptr,
     {"lsd.is_moving($0)"}},
    {"Destroy", "Destroy", "Objects", "Removes an object and its children", {kExecIn, kTarget}, 0, {kExecOut}, 0, "lsd.destroy($1)", {}},
    {"SetVariable", "Set Variable", "Variables", "Stores a value (any type) on this object", {kExecIn, {"Name", PinType::String, "health"}, {"Value", PinType::Any, "0"}}, 0,
     {kExecOut}, 0, "self[$1] = $2", {}},
    {"SetLocal", "Set Local Variable", "Variables", "Inside a Function: a value that lasts for this call only (elsewhere: on the object)",
     {kExecIn, {"Name", PinType::String, "temp"}, {"Value", PinType::Any, "0"}}, 0, {kExecOut}, 0, "(__L or self)[$1] = $2", {}},
    // ---- Functions (Unreal's Blueprint functions): an entry with up to 3 arguments, Return, and calls ----
    {"FunctionEntry", "Function", "Functions", "A function this script can call by name (Call Function); A, B, C are its arguments",
     {{"Name", PinType::String, "my_function"}}, 0, {kExecOut, {"A", PinType::Any, ""}, {"B", PinType::Any, ""}, {"C", PinType::Any, ""}}, 0,
     "function $n0(self, a, b, c)\nlocal __L = {}", {nullptr, "a", "b", "c"}},
    {"Return", "Return", "Functions", "Ends the Function, handing Value back to Call Function", {kExecIn, {"Value", PinType::Any, "0"}}, 0, {}, 0,
     "do return $1 end", {}},
    {"CallFunction", "Call Function", "Functions", "Runs a Function of this script by name; Result is what it returned",
     {kExecIn, {"Name", PinType::String, "my_function"}, {"A", PinType::Any, "0"}, {"B", PinType::Any, "0"}, {"C", PinType::Any, "0"}}, 0,
     {kExecOut, {"Result", PinType::Any, ""}}, 0, "local $o1 = $n1(self, $2, $3, $4)", {}},
    // ---- Standard macros (Unreal's ForLoop, WhileLoop, FlipFlop) ----
    {"ForLoop", "For Loop", "Flow", "Runs Loop Body for each Index from First to Last, then Completed",
     {kExecIn, {"First", PinType::Float, "0"}, {"Last", PinType::Float, "9"}}, 0,
     {{"Loop Body", PinType::Exec, ""}, {"Index", PinType::Float, ""}, {"Completed", PinType::Exec, ""}}, 0, "for $o1 = $1, $2 do\n#0\nend\n#2", {}},
    {"WhileLoop", "While Loop", "Flow", "Runs Loop Body while Condition is true (10000 rounds at most), then Completed",
     {kExecIn, {"Condition", PinType::Bool, "false"}}, 0, {{"Loop Body", PinType::Exec, ""}, {"Completed", PinType::Exec, ""}}, 0,
     "do local __n = 0\nwhile $1 and __n < 10000 do\n__n = __n + 1\n#0\nend end\n#1", {}},
    {"FlipFlop", "Flip Flop", "Flow", "Goes out A, then B, then A again... each time it runs",
     {kExecIn}, 0, {{"A", PinType::Exec, ""}, {"B", PinType::Exec, ""}, {"Is A", PinType::Bool, ""}}, 0,
     "self.__flip$id = not self.__flip$id\nlocal $o2 = self.__flip$id\nif $o2 then\n#0\nelse\n#1\nend", {}},
    // ---- Organising ----
    {"Reroute", "Reroute", "Utilities", "A bend in a data wire (passes the value through)", {{"", PinType::Any, ""}}, 0, {{"", PinType::Any, ""}}, 0, nullptr, {"$0"}},
    {"RerouteExec", "Reroute (Exec)", "Utilities", "A bend in a white wire", {kExecIn}, 0, {kExecOut}, 0, "#0", {}},
    {"Comment", "Comment", "Utilities", "A labelled box behind nodes; drag its corner to size it", {}, 0, {}, 0, nullptr, {}},
    // ---- Pure ----
    {"Self", "Self", "Objects", "The object this script runs on", {}, 0, {{"Self", PinType::Entity, ""}}, 0, nullptr, {"self.entity"}},
    {"FindObject", "Find Object", "Objects", "An object by name (none if missing)", {{"Name", PinType::String, "Floor"}}, 0,
     {{"Object", PinType::Entity, ""}}, 0, nullptr, {"lsd.find($0)"}},
    {"GetName", "Get Name", "Objects", "", {kTarget}, 0, {{"Name", PinType::String, ""}}, 0, nullptr, {"lsd.name($0)"}},
    {"HasTag", "Has Tag", "Objects", "Gameplay tags: \"Damage\" matches \"Damage.Fire\"", {kTarget, {"Tag", PinType::String, "Enemy"}}, 0,
     {{"Result", PinType::Bool, ""}}, 0, nullptr, {"lsd.has_tag($0, $1)"}},
    {"HasAnyTag", "Has Any Tag", "Objects", "True if the object has at least one of the tags (comma-separated)", {kTarget, {"Tags", PinType::String, "Enemy, Boss"}}, 0,
     {{"Result", PinType::Bool, ""}}, 0, nullptr, {"lsd.has_any_tag($0, $1)"}},
    {"HasAllTags", "Has All Tags", "Objects", "True if the object has every one of the tags (comma-separated)", {kTarget, {"Tags", PinType::String, "Enemy, Flying"}}, 0,
     {{"Result", PinType::Bool, ""}}, 0, nullptr, {"lsd.has_all_tags($0, $1)"}},
    {"GetPosition", "Get Location", "Transform", "", {kTarget}, 0, {{"Location", PinType::Vector, ""}}, 0, nullptr, {"__vec(lsd.position($0))"}},
    {"GetRotation", "Get Rotation", "Transform", "", {kTarget}, 0, {{"Rotation", PinType::Vector, ""}}, 0, nullptr, {"__vec(lsd.rotation($0))"}},
    {"SplineLocation", "Spline Location", "Transform", "World point at a distance along an object's Spline",
     {kTarget, {"Distance", PinType::Float, "0"}}, 0, {{"Location", PinType::Vector, ""}, {"Direction", PinType::Vector, ""}}, 0, nullptr,
     {"__vec(lsd.spline_point($0, $1))", "__vec(select(4, lsd.spline_point($0, $1)))"}},
    {"SplineLength", "Spline Length", "Transform", "", {kTarget}, 0, {{"Length", PinType::Float, ""}}, 0, nullptr, {"lsd.spline_length($0)"}},
    {"GetVariable", "Get Variable", "Variables", "A value stored on this object (0 if never set)", {{"Name", PinType::String, "health"}}, 0,
     {{"Value", PinType::Any, ""}}, 0, nullptr, {"(self[$0] or 0)"}},
    {"GetLocal", "Get Local Variable", "Variables", "A Set Local Variable value of this Function call (0 if never set)", {{"Name", PinType::String, "temp"}}, 0,
     {{"Value", PinType::Any, ""}}, 0, nullptr, {"((__L or self)[$0] or 0)"}},
    {"InputAxis", "Input Axis", "Input", "Value of an Input Action by asset name", {{"Action", PinType::String, "IA_Move"}}, 0,
     {{"X", PinType::Float, ""}, {"Y", PinType::Float, ""}}, 0, nullptr, {"(lsd.input($0))", "select(2, lsd.input($0))"}},
    {"InputPressed", "Input Pressed", "Input", "True on the frame the action fires (its Trigger: pressed, hold, tap...)", {{"Action", PinType::String, "IA_Jump"}}, 0,
     {{"Pressed", PinType::Bool, ""}}, 0, nullptr, {"lsd.pressed($0)"}},
    {"SaveValue", "Save Value", "Save Game", "Stores a number in a save slot (Saved/SaveGames/<Slot>.json); it is still there next time the game runs",
     {kExecIn, {"Slot", PinType::String, "Slot1"}, {"Key", PinType::String, "score"}, {"Value", PinType::Float, "0"}}, 0, {kExecOut}, 0,
     "__save_value($1, $2, $3)", {}},
    {"LoadValue", "Load Value", "Save Game", "A number from a save slot, or Default if it was never saved",
     {{"Slot", PinType::String, "Slot1"}, {"Key", PinType::String, "score"}, {"Default", PinType::Float, "0"}}, 0,
     {{"Value", PinType::Float, ""}}, 0, nullptr, {"(tonumber(__load_value($0, $1, $2)) or $2)"}},
    {"TableValue", "Get Table Value", "Data", "A cell of a Data Table (.csv) by asset name, row name and column name",
     {{"Table", PinType::String, "DT_Items"}, {"Row", PinType::String, "Row1"}, {"Column", PinType::String, "Value"}}, 0,
     {{"Number", PinType::Float, ""}, {"Text", PinType::String, ""}}, 0, nullptr,
     {"(tonumber(__cell($0, $1, $2)) or 0)", "tostring(__cell($0, $1, $2) or \"\")"}},
    {"EvaluateCurve", "Evaluate Curve", "Data", "Value of a Curve data asset (by name) at a time",
     {{"Curve", PinType::String, "C_Fade"}, {"Time", PinType::Float, "0"}}, 0, {{"Value", PinType::Float, ""}}, 0, nullptr, {"(lsd.curve($0, $1))"}},
    {"EvaluateVectorCurve", "Evaluate Vector Curve", "Data", "A Curve Vector (x, y, z) or Curve Color (r, g, b) data asset by name at a time",
     {{"Curve", PinType::String, "CV_Path"}, {"Time", PinType::Float, "0"}}, 0, {{"Value", PinType::Vector, ""}}, 0, nullptr, {"__vec(lsd.curve($0, $1))"}},
    {"EvaluateCurveTable", "Evaluate Curve Table", "Data", "A row of a curve table (a Data Table whose column headers are times) at a time",
     {{"Table", PinType::String, "CT_Levels"}, {"Row", PinType::String, "Damage"}, {"Time", PinType::Float, "0"}}, 0, {{"Value", PinType::Float, ""}}, 0, nullptr,
     {"lsd.curve_table($0, $1, $2)"}},
    {"Time", "Game Time", "Utilities", "Seconds since Play started", {}, 0, {{"Seconds", PinType::Float, ""}}, 0, nullptr, {"lsd.time()"}},
    {"MakeVector", "Make Vector", "Math", "", {{"X", PinType::Float, "0"}, {"Y", PinType::Float, "0"}, {"Z", PinType::Float, "0"}}, 0,
     {{"Vector", PinType::Vector, ""}}, 0, nullptr, {"__vec($0, $1, $2)"}},
    {"BreakVector", "Break Vector", "Math", "", {{"Vector", PinType::Vector, "0,0,0"}}, 0,
     {{"X", PinType::Float, ""}, {"Y", PinType::Float, ""}, {"Z", PinType::Float, ""}}, 0, nullptr, {"$0.x", "$0.y", "$0.z"}},
    {"VectorAdd", "Vector + Vector", "Math", "", {{"A", PinType::Vector, "0,0,0"}, {"B", PinType::Vector, "0,0,0"}}, 0,
     {{"Result", PinType::Vector, ""}}, 0, nullptr, {"__vadd($0, $1)"}},
    {"VectorScale", "Vector * Float", "Math", "", {{"Vector", PinType::Vector, "0,0,1"}, {"Scale", PinType::Float, "1"}}, 0,
     {{"Result", PinType::Vector, ""}}, 0, nullptr, {"__scale($0, $1)"}},
    {"Add", "+", "Math", "", {{"A", PinType::Float, "0"}, {"B", PinType::Float, "0"}}, 0, {{"Result", PinType::Float, ""}}, 0, nullptr, {"$0 + $1"}},
    {"Subtract", "-", "Math", "", {{"A", PinType::Float, "0"}, {"B", PinType::Float, "0"}}, 0, {{"Result", PinType::Float, ""}}, 0, nullptr, {"$0 - $1"}},
    {"Multiply", "*", "Math", "", {{"A", PinType::Float, "0"}, {"B", PinType::Float, "1"}}, 0, {{"Result", PinType::Float, ""}}, 0, nullptr, {"$0 * $1"}},
    {"Divide", "/", "Math", "Division by zero gives 0", {{"A", PinType::Float, "0"}, {"B", PinType::Float, "1"}}, 0,
     {{"Result", PinType::Float, ""}}, 0, nullptr, {"__div($0, $1)"}},
    {"Sin", "Sin", "Math", "Sine of radians", {{"Radians", PinType::Float, "0"}}, 0, {{"Result", PinType::Float, ""}}, 0, nullptr, {"math.sin($0)"}},
    {"Greater", ">", "Math", "", {{"A", PinType::Float, "0"}, {"B", PinType::Float, "0"}}, 0, {{"Result", PinType::Bool, ""}}, 0, nullptr, {"$0 > $1"}},
    {"Less", "<", "Math", "", {{"A", PinType::Float, "0"}, {"B", PinType::Float, "0"}}, 0, {{"Result", PinType::Bool, ""}}, 0, nullptr, {"$0 < $1"}},
    {"Equal", "==", "Math", "", {{"A", PinType::Float, "0"}, {"B", PinType::Float, "0"}}, 0, {{"Result", PinType::Bool, ""}}, 0, nullptr, {"$0 == $1"}},
    {"Not", "NOT", "Math", "", {{"A", PinType::Bool, "false"}}, 0, {{"Result", PinType::Bool, ""}}, 0, nullptr, {"not $0"}},
    {"And", "AND", "Math", "", {{"A", PinType::Bool, "false"}, {"B", PinType::Bool, "false"}}, 0, {{"Result", PinType::Bool, ""}}, 0, nullptr, {"$0 and $1"}},
    {"Or", "OR", "Math", "", {{"A", PinType::Bool, "false"}, {"B", PinType::Bool, "false"}}, 0, {{"Result", PinType::Bool, ""}}, 0, nullptr, {"$0 or $1"}},
};

// Sound Cue nodes (editor/sound_cue.cpp plays them). Audio pins carry "what to play".
constexpr PinDef kAudioIn{"Audio", PinType::Audio, ""};
constexpr PinDef kAudioOut{"Audio", PinType::Audio, ""};
NodeDef g_cue_defs[] = {
    {"CueOutput", "Output", "Sound Cue", "What the cue plays, with an overall volume and pitch",
     {kAudioIn, {"Volume", PinType::Float, "1"}, {"Pitch", PinType::Float, "1"}}, 0, {}, 0, nullptr, {}},
    {"CueWave", "Wave Player", "Sound Cue", "Plays a sound asset (by name)", {{"Sound", PinType::String, ""}, {"Looping", PinType::Bool, "false"}}, 0,
     {kAudioOut}, 0, nullptr, {}},
    {"CueRandom", "Random", "Sound Cue", "Plays one of its inputs, picked at random each time",
     {{"A", PinType::Audio, ""}, {"B", PinType::Audio, ""}, {"C", PinType::Audio, ""}, {"D", PinType::Audio, ""}, {"No Repeat", PinType::Bool, "true"}}, 0,
     {kAudioOut}, 0, nullptr, {}},
    {"CueModulator", "Modulator", "Sound Cue", "Random volume and pitch in a range, new each play",
     {kAudioIn, {"Volume Min", PinType::Float, "0.9"}, {"Volume Max", PinType::Float, "1.1"}, {"Pitch Min", PinType::Float, "0.95"}, {"Pitch Max", PinType::Float, "1.05"}}, 0,
     {kAudioOut}, 0, nullptr, {}},
    {"CueMixer", "Mixer", "Sound Cue", "Plays all its inputs together",
     {{"A", PinType::Audio, ""}, {"B", PinType::Audio, ""}, {"C", PinType::Audio, ""}, {"D", PinType::Audio, ""}}, 0, {kAudioOut}, 0, nullptr, {}},
    {"CueConcat", "Concatenator", "Sound Cue", "Plays its inputs one after another",
     {{"A", PinType::Audio, ""}, {"B", PinType::Audio, ""}, {"C", PinType::Audio, ""}, {"D", PinType::Audio, ""}}, 0, {kAudioOut}, 0, nullptr, {}},
    {"CueDelay", "Delay", "Sound Cue", "Waits a random time in a range before playing",
     {kAudioIn, {"Min", PinType::Float, "0"}, {"Max", PinType::Float, "0.2"}}, 0, {kAudioOut}, 0, nullptr, {}},
    {"CueLooping", "Looping", "Sound Cue", "Repeats what comes in until stopped", {kAudioIn}, 0, {kAudioOut}, 0, nullptr, {}},
    {"CueSwitch", "Switch", "Sound Cue", "Picks input 0-3 by an audio parameter (lsd.set_audio_param / Set Audio Parameter)",
     {{"0", PinType::Audio, ""}, {"1", PinType::Audio, ""}, {"2", PinType::Audio, ""}, {"3", PinType::Audio, ""}, {"Parameter", PinType::String, "Surface"}}, 0,
     {kAudioOut}, 0, nullptr, {}},
};

// Material Graph nodes. Every value is a float3 in the shader (scalars are (x, x, x), like Unreal's
// promotion), so Float and Vector pins connect freely; the pin type only picks the widget for typed
// values. Expressions: $0.. the inputs, mi the material inputs (uv, wpos, normal, view, time); a
// Texture Sample's @ is its texture slot (filled in by the compiler).
NodeDef g_material_defs[] = {
    {"MatOutput", "Material Output", "Output", "The surface: what the mesh looks like (Unreal's main material node)",
     {{"Base Color", PinType::Vector, "0.8,0.8,0.8"}, {"Metallic", PinType::Float, "0"}, {"Roughness", PinType::Float, "0.5"},
      {"Emissive", PinType::Vector, "0,0,0"}, {"Normal", PinType::Vector, "0,0,1"}, {"Ambient Occlusion", PinType::Float, "1"},
      {"Opacity", PinType::Float, "1"}, {"Blend Mode", PinType::String, "Opaque"}}, 0, {}, 0, nullptr, {}},
    {"MatConstant", "Constant", "Constants", "A number (1 key + click in Unreal)", {{"Value", PinType::Float, "1"}}, 0, {{"", PinType::Float, ""}}, 0, nullptr, {"$0"}},
    {"MatColor", "Color", "Constants", "A colour or vector (3 key + click)", {{"Value", PinType::Vector, "1,1,1"}}, 0, {{"", PinType::Vector, ""}}, 0, nullptr, {"$0"}},
    {"MatScalarParam", "Scalar Parameter", "Parameters", "A number Material Instances can change (by its Name) without recompiling",
     {{"Name", PinType::String, "Brightness"}, {"Default", PinType::Float, "1"}}, 0, {{"", PinType::Float, ""}}, 0, nullptr, {"lsd_v3(lsd_params[@].x)"}},
    {"MatVectorParam", "Vector Parameter", "Parameters", "A colour or vector Material Instances can change (by its Name)",
     {{"Name", PinType::String, "Tint"}, {"Default", PinType::Vector, "1,1,1"}}, 0, {{"", PinType::Vector, ""}}, 0, nullptr, {"lsd_params[@].xyz"}},
    {"MatTime", "Time", "Constants", "Seconds since the editor started: animate with Sine, Panner...", {}, 0, {{"", PinType::Float, ""}}, 0, nullptr, {"lsd_v3(mi.time)"}},
    {"MatTexture", "Texture Sample", "Texture", "A texture asset by name. Type: Color (sRGB), Normal (normal map, unpacked to -1..1) or Data (masks, roughness)",
     {{"UV", PinType::Vector, "uv"}, {"Texture", PinType::String, ""}, {"Type", PinType::String, "Color"}}, 0,
     {{"RGB", PinType::Vector, ""}, {"A", PinType::Float, ""}}, 0, nullptr, {"lsd_tex@($0).rgb", "lsd_v3(lsd_tex@($0).a)"}},
    {"MatTexCoord", "Texture Coordinate", "Coordinates", "The mesh's UVs times a tiling", {{"Tiling", PinType::Vector, "1,1,1"}}, 0,
     {{"UV", PinType::Vector, ""}}, 0, nullptr, {"(mi.uv * $0)"}},
    {"MatPanner", "Panner", "Coordinates", "UVs sliding over time (flowing water, scrolling lava)", {{"UV", PinType::Vector, "uv"}, {"Speed", PinType::Vector, "0.1,0,0"}}, 0,
     {{"UV", PinType::Vector, ""}}, 0, nullptr, {"($0 + $1 * mi.time)"}},
    {"MatWorldPosition", "World Position", "Coordinates", "Where the pixel is in the world (metres)", {}, 0, {{"", PinType::Vector, ""}}, 0, nullptr, {"mi.wpos"}},
    {"MatVertexNormal", "Vertex Normal", "Coordinates", "The surface direction in the world", {}, 0, {{"", PinType::Vector, ""}}, 0, nullptr, {"mi.normal"}},
    {"MatCameraVector", "Camera Vector", "Coordinates", "From the pixel towards the camera", {}, 0, {{"", PinType::Vector, ""}}, 0, nullptr, {"mi.view"}},
    {"MatFresnel", "Fresnel", "Coordinates", "1 at grazing angles, 0 facing the camera (rim light)", {{"Exponent", PinType::Float, "5"}}, 0,
     {{"", PinType::Float, ""}}, 0, nullptr, {"lsd_v3(pow(1.0 - saturate(dot(mi.normal, mi.view)), $0.x))"}},
    {"MatAdd", "Add", "Math", "A + B", {{"A", PinType::Vector, "0,0,0"}, {"B", PinType::Vector, "0,0,0"}}, 0, {{"", PinType::Vector, ""}}, 0, nullptr, {"($0 + $1)"}},
    {"MatSubtract", "Subtract", "Math", "A - B", {{"A", PinType::Vector, "0,0,0"}, {"B", PinType::Vector, "0,0,0"}}, 0, {{"", PinType::Vector, ""}}, 0, nullptr, {"($0 - $1)"}},
    {"MatMultiply", "Multiply", "Math", "A x B (tint a texture, scale a value)", {{"A", PinType::Vector, "1,1,1"}, {"B", PinType::Vector, "1,1,1"}}, 0,
     {{"", PinType::Vector, ""}}, 0, nullptr, {"($0 * $1)"}},
    {"MatDivide", "Divide", "Math", "A / B", {{"A", PinType::Vector, "1,1,1"}, {"B", PinType::Vector, "1,1,1"}}, 0, {{"", PinType::Vector, ""}}, 0, nullptr, {"($0 / $1)"}},
    {"MatLerp", "Lerp", "Math", "Blends A to B by Alpha (0 = A, 1 = B)", {{"A", PinType::Vector, "0,0,0"}, {"B", PinType::Vector, "1,1,1"}, {"Alpha", PinType::Float, "0.5"}}, 0,
     {{"", PinType::Vector, ""}}, 0, nullptr, {"lerp($0, $1, $2.x)"}},
    {"MatOneMinus", "One Minus", "Math", "1 - x (invert a mask)", {{"", PinType::Vector, "0,0,0"}}, 0, {{"", PinType::Vector, ""}}, 0, nullptr, {"(lsd_v3(1.0) - $0)"}},
    {"MatPower", "Power", "Math", "Base to the power of Exponent (sharpen a mask)", {{"Base", PinType::Vector, "1,1,1"}, {"Exponent", PinType::Float, "2"}}, 0,
     {{"", PinType::Vector, ""}}, 0, nullptr, {"pow(max($0, lsd_v3(0.0)), lsd_v3($1.x))"}},
    {"MatClamp", "Clamp", "Math", "Keeps the value between Min and Max", {{"", PinType::Vector, "0,0,0"}, {"Min", PinType::Float, "0"}, {"Max", PinType::Float, "1"}}, 0,
     {{"", PinType::Vector, ""}}, 0, nullptr, {"clamp($0, lsd_v3($1.x), lsd_v3($2.x))"}},
    {"MatDot", "Dot", "Math", "Dot product of two directions", {{"A", PinType::Vector, "0,0,1"}, {"B", PinType::Vector, "0,0,1"}}, 0,
     {{"", PinType::Float, ""}}, 0, nullptr, {"lsd_v3(dot($0, $1))"}},
    {"MatNormalize", "Normalize", "Math", "Length 1", {{"", PinType::Vector, "0,0,1"}}, 0, {{"", PinType::Vector, ""}}, 0, nullptr, {"normalize($0)"}},
    {"MatSine", "Sine", "Math", "sin(x), radians: Time into Sine pulses", {{"", PinType::Float, "0"}}, 0, {{"", PinType::Vector, ""}}, 0, nullptr, {"sin($0)"}},
    {"MatAbs", "Abs", "Math", "|x|", {{"", PinType::Vector, "0,0,0"}}, 0, {{"", PinType::Vector, ""}}, 0, nullptr, {"abs($0)"}},
    {"MatFrac", "Frac", "Math", "The part after the point (repeating 0..1)", {{"", PinType::Vector, "0,0,0"}}, 0, {{"", PinType::Vector, ""}}, 0, nullptr, {"frac($0)"}},
    {"MatMask", "Break Vector", "Utility", "Its X, Y and Z (R, G, B) apart (Component Mask)", {{"", PinType::Vector, "0,0,0"}}, 0,
     {{"X", PinType::Float, ""}, {"Y", PinType::Float, ""}, {"Z", PinType::Float, ""}}, 0, nullptr, {"lsd_v3($0.x)", "lsd_v3($0.y)", "lsd_v3($0.z)"}},
    {"MatAppend", "Make Vector", "Utility", "A vector from three numbers (Append)", {{"X", PinType::Float, "0"}, {"Y", PinType::Float, "0"}, {"Z", PinType::Float, "0"}}, 0,
     {{"", PinType::Vector, ""}}, 0, nullptr, {"float3($0.x, $1.x, $2.x)"}},
};

// Behaviour Tree nodes (.lsdbt): white wires go from a parent's Child pins to its children, left
// to right in pin order. The editor turns the graph into an ai::BehaviorTree (editor/behavior.cpp).
constexpr PinDef kParent{"", PinType::Exec, ""};
NodeDef g_bt_defs[] = {
    {"BtRoot", "Root", "Tree", "Where the tree starts (one per tree)", {}, 0, {{"", PinType::Exec, ""}}, 0, nullptr, {}},
    {"BtSelector", "Selector", "Composites", "Tries its children top to bottom until one does not fail. Higher ones interrupt lower ones as soon as they can run (chase beats patrol)",
     {kParent}, 0, {{"1", PinType::Exec, ""}, {"2", PinType::Exec, ""}, {"3", PinType::Exec, ""}, {"4", PinType::Exec, ""}}, 0, nullptr, {}},
    {"BtSequence", "Sequence", "Composites", "Runs its children in order; stops at the first that fails",
     {kParent}, 0, {{"1", PinType::Exec, ""}, {"2", PinType::Exec, ""}, {"3", PinType::Exec, ""}, {"4", PinType::Exec, ""}}, 0, nullptr, {}},
    {"BtMoveTo", "Move To", "Tasks", "Walks (Nav Agent) to the location in a blackboard key; follows it if it moves",
     {kParent, {"Key", PinType::String, "PlayerLocation"}, {"Acceptance", PinType::Float, "1"}}, 0, {}, 0, nullptr, {}},
    {"BtWait", "Wait", "Tasks", "Does nothing for a while", {kParent, {"Seconds", PinType::Float, "2"}}, 0, {}, 0, nullptr, {}},
    {"BtRandomLocation", "Random Location", "Tasks", "Puts a reachable point near the agent into a key (patrols, wandering)",
     {kParent, {"Key", PinType::String, "PatrolLocation"}, {"Radius", PinType::Float, "8"}}, 0, {}, 0, nullptr, {}},
    {"BtSetKey", "Set Key", "Tasks", "Sets a blackboard key to a number or a location (x,y,z)",
     {kParent, {"Key", PinType::String, "Alert"}, {"Value", PinType::String, "1"}}, 0, {}, 0, nullptr, {}},
    {"BtClearKey", "Clear Key", "Tasks", "Removes a blackboard key", {kParent, {"Key", PinType::String, "Alert"}}, 0, {}, 0, nullptr, {}},
    {"BtRunScript", "Run Script", "Tasks", "Calls a function of the agent's Lua script: return true (success), false (failure) or nothing (still running)",
     {kParent, {"Function", PinType::String, "bt_attack"}}, 0, {}, 0, nullptr, {}},
    {"BtIsSet", "Is Set", "Conditions", "Succeeds if the key is set and not 0 (CanSeePlayer, HeardNoise...)",
     {kParent, {"Key", PinType::String, "CanSeePlayer"}}, 0, {}, 0, nullptr, {}},
    {"BtCompare", "Compare", "Conditions", "Succeeds if Key <Op> Value (Op: > < == !=)",
     {kParent, {"Key", PinType::String, "Health"}, {"Op", PinType::String, "<"}, {"Value", PinType::Float, "30"}}, 0, {}, 0, nullptr, {}},
};

constexpr char kLuaPrelude[] = R"lua(-- Generated from a visual script (.lsdgraph). Edit the graph, not this file.
local function __vec(x, y, z) return {x = x or 0, y = y or 0, z = z or 0} end
local function __vadd(a, b) return __vec(a.x + b.x, a.y + b.y, a.z + b.z) end
local function __scale(a, s) return __vec(a.x * s, a.y * s, a.z * s) end
local function __div(a, b) if b == 0 then return 0 end return a / b end
local function __setpos(e, v) lsd.set_position(e, v.x, v.y, v.z) end
local function __addpos(e, v) local x, y, z = lsd.position(e) lsd.set_position(e, x + v.x, y + v.y, z + v.z) end
local function __setrot(e, v) lsd.set_rotation(e, v.x, v.y, v.z) end
local function __addrot(e, v) local x, y, z = lsd.rotation(e) lsd.set_rotation(e, x + v.x, y + v.y, z + v.z) end
local function __spawn(name, v) return lsd.spawn(name, v.x, v.y, v.z) end
local function __color(e, v) lsd.set(e, "StaticMesh", {color = {r = v.x, g = v.y, b = v.z}}) end
local function __sound(name, v) return lsd.play_sound(name, v.x, v.y, v.z) end
local function __trace(kind, a, b, r, h)
  local e, px, py, pz, nx, ny, nz
  if kind == 0 then
    local dx, dy, dz = b.x - a.x, b.y - a.y, b.z - a.z
    e, px, py, pz, nx, ny, nz = lsd.raycast(a.x, a.y, a.z, dx, dy, dz, math.sqrt(dx * dx + dy * dy + dz * dz))
  elseif kind == 1 then e, px, py, pz, nx, ny, nz = lsd.sphere_trace(a.x, a.y, a.z, b.x, b.y, b.z, r)
  else e, px, py, pz, nx, ny, nz = lsd.capsule_trace(a.x, a.y, a.z, b.x, b.y, b.z, r, h) end
  if not e then return false, nil, __vec(b.x, b.y, b.z), __vec(0, 0, 0) end
  return true, e, __vec(px, py, pz), __vec(nx, ny, nz)
end
local function __save_value(slot, key, value) local t = lsd.load_game(slot) if type(t) ~= "table" then t = {} end t[key] = value lsd.save_game(slot, t) end
local function __load_value(slot, key, default) local t = lsd.load_game(slot) local v = type(t) == "table" and t[key] if v == nil then return default end return v end
local function __cell(t, r, c) local d = lsd.table(t) local row = d and d[r] return row and row[c] end
local function __str(v)
  if type(v) == "table" then return string.format("(%g, %g, %g)", v.x, v.y, v.z) end
  return tostring(v)
end
)lua";

u32 count_pins(const PinDef* pins, u32 max) {
    u32 n = 0;
    while (n < max && pins[n].name) ++n;
    return n;
}

void append(Array<char>& out, const char* text, usize n) {
    const usize at = out.size();
    out.resize(at + n);
    std::memcpy(out.data() + at, text, n);
}
void append(Array<char>& out, const char* text) { append(out, text, std::strlen(text)); }

void append_quoted(Array<char>& out, const char* text) {
    append(out, "\"");
    for (const char* c = text; *c; ++c) {
        if (*c == '"' || *c == '\\') append(out, "\\", 1);
        if (static_cast<unsigned char>(*c) >= 0x20) append(out, c, 1);
    }
    append(out, "\"");
}

std::string lua_string(const char* text) {
    std::string out = "\"";
    for (const char* c = text; *c; ++c) {
        if (*c == '"' || *c == '\\') out += '\\';
        if (*c == '\n') out += "\\n";
        else if (static_cast<unsigned char>(*c) >= 0x20) out += *c;
    }
    return out + "\"";
}

std::string lua_number(const char* text) {
    char* stop = nullptr;
    const double value = std::strtod(text, &stop);
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.9g", stop == text ? 0.0 : value);
    return buffer;
}

// The typed value of an unconnected input as Lua.
std::string literal(PinType type, const char* value) {
    switch (type) {
        case PinType::Float: return lua_number(value);
        case PinType::Bool: return std::strcmp(value, "true") == 0 ? "true" : "false";
        case PinType::String: return lua_string(value);
        case PinType::Vector: {
            float v[3] = {0, 0, 0};
            std::sscanf(value, "%f , %f , %f", &v[0], &v[1], &v[2]);
            char buffer[128];
            std::snprintf(buffer, sizeof buffer, "__vec(%.9g, %.9g, %.9g)", v[0], v[1], v[2]);
            return buffer;
        }
        case PinType::Entity: return "self.entity";
        case PinType::Any: {  // as typed: a number, true / false, or text
            char* end = nullptr;
            std::strtod(value, &end);
            if (*value && end && *end == '\0') return lua_number(value);
            if (std::strcmp(value, "true") == 0 || std::strcmp(value, "false") == 0) return value;
            return lua_string(value);
        }
        case PinType::Exec: break;
    }
    return "nil";
}

struct Compiler {
    const Graph& graph;
    char* error;
    usize error_capacity;
    bool ok = true;

    void fail(const GraphNode& node, const char* what) {
        if (ok) {
            const NodeDef* def = find_node_def(node.type);
            std::snprintf(error, error_capacity, "%s (node %u): %s", def ? def->title : node.type, node.id, what);
        }
        ok = false;
    }

    // Lua for input `pin` of a node: the wire's source, or the typed value.
    std::string input(const GraphNode& node, const NodeDef& def, u32 pin, int depth) {
        const PinType type = def.inputs[pin].type;
        const GraphLink* link = graph.link_into(node.id, pin);
        if (!link) return "(" + literal(type, node.values[pin]) + ")";
        const GraphNode* source = graph.find(link->from_node);
        const NodeDef* source_def = source ? find_node_def(source->type) : nullptr;
        if (!source_def || link->from_pin >= source_def->output_count) {
            fail(node, "wired to a missing node");
            return "nil";
        }
        std::string value = output(*source, *source_def, link->from_pin, depth + 1);
        if (type == PinType::String && source_def->outputs[link->from_pin].type != PinType::String) value = "__str(" + value + ")";
        return "(" + value + ")";
    }

    // Lua for output `pin` of a node: a pure expression, an event parameter or an exec node's variable.
    std::string output(const GraphNode& node, const NodeDef& def, u32 pin, int depth) {
        if (depth > 64) {
            fail(node, "its inputs loop back into themselves");
            return "nil";
        }
        if (is_pure(def) || is_event(def)) return substitute(node, def, def.output_code[pin], depth, false);
        char name[32];
        std::snprintf(name, sizeof name, "n%u_%u", node.id, pin);
        return name;
    }

    // Fills a template: $id, $oN (output variables), $N (inputs), #N (exec chains, if `chains`).
    std::string substitute(const GraphNode& node, const NodeDef& def, const char* code, int depth, bool chains) {
        std::string out;
        if (!code) return "nil";
        for (const char* c = code; *c && ok;) {
            if (c[0] == '$' && c[1] == 'i' && c[2] == 'd') {
                out += std::to_string(node.id);
                c += 3;
            } else if (c[0] == '$' && c[1] == 'o' && c[2] >= '0' && c[2] <= '9') {
                out += "n" + std::to_string(node.id) + "_" + std::string(1, c[2]);
                c += 3;
            } else if (c[0] == '$' && c[1] == 'n' && c[2] >= '0' && c[2] <= '9') {  // an input's text as a Lua name
                std::string name;
                for (const char* v = node.values[c[2] - '0']; *v; ++v) {
                    const char ch = *v;
                    name += (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') ? ch : '_';
                }
                if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) name = "_" + name;
                out += name;
                c += 3;
            } else if (c[0] == '$' && c[1] >= '0' && c[1] <= '9') {
                out += input(node, def, static_cast<u32>(c[1] - '0'), depth);
                c += 2;
            } else if (chains && c[0] == '#' && c[1] >= '0' && c[1] <= '9') {
                out += chain(node, static_cast<u32>(c[1] - '0'), depth);
                c += 2;
            } else {
                out += *c++;
            }
        }
        return out;
    }

    // The statements wired after exec output `pin`.
    std::string chain(const GraphNode& node, u32 pin, int depth) {
        for (const GraphLink& link : graph.links) {
            if (link.from_node != node.id || link.from_pin != pin) continue;
            const GraphNode* next = graph.find(link.to_node);
            if (!next) return "";
            return statement(*next, depth + 1);
        }
        return "";
    }

    std::string statement(const GraphNode& node, int depth) {
        const NodeDef* def = find_node_def(node.type);
        if (!def) {
            fail(node, "unknown node type");
            return "";
        }
        if (depth > 200) {
            fail(node, "the white wires loop back (use Set Timer or Delay to repeat)");
            return "";
        }
        std::string code = def->code;
        if (code.find('#') == std::string::npos) code += "\n#0";
        // A marker before each node's Lua: the debugger maps lines back to nodes (breakpoints, live flow).
        return "--@node " + std::to_string(node.id) + "\n" + substitute(node, *def, code.c_str(), depth, true);
    }
};

} // namespace

// Built-in Script nodes plus registered ones, with the registered nodes' strings.
std::vector<NodeDef> g_script_nodes;
std::deque<std::string> g_custom_strings;
u32 g_custom_count = 0;

const char* keep(const char* text) {
    if (!text) return nullptr;
    g_custom_strings.emplace_back(text);
    return g_custom_strings.back().c_str();
}

void register_node(const NodeDef& def) {
    u32 count = 0;
    node_defs(count);  // fills the built-ins first
    NodeDef copy = def;
    copy.id = keep(def.id);
    copy.title = keep(def.title);
    copy.category = keep(def.category);
    copy.tooltip = keep(def.tooltip);
    copy.code = keep(def.code);
    for (auto& pin : copy.inputs) {
        pin.name = keep(pin.name);
        pin.default_value = keep(pin.default_value);
    }
    for (auto& pin : copy.outputs) {
        pin.name = keep(pin.name);
        pin.default_value = keep(pin.default_value);
    }
    for (auto& code : copy.output_code) code = keep(code);
    copy.input_count = count_pins(copy.inputs, kMaxNodeInputs);
    copy.output_count = count_pins(copy.outputs, kMaxNodeOutputs);
    for (NodeDef& existing : g_script_nodes) {
        if (std::strcmp(existing.id, copy.id) == 0) {
            existing = copy;
            return;
        }
    }
    g_script_nodes.push_back(copy);
    ++g_custom_count;
}

void clear_custom_nodes() {
    g_script_nodes.resize(g_script_nodes.size() - g_custom_count);
    g_custom_count = 0;
    g_custom_strings.clear();  // shortcut: replaced nodes' old strings live until the next clear
}

const NodeDef* node_defs(u32& count, GraphKind kind) {
    static const bool counted = [] {
        for (NodeDef& d : g_defs) {
            d.input_count = count_pins(d.inputs, kMaxNodeInputs);
            d.output_count = count_pins(d.outputs, kMaxNodeOutputs);
        }
        for (NodeDef& d : g_cue_defs) {
            d.input_count = count_pins(d.inputs, kMaxNodeInputs);
            d.output_count = count_pins(d.outputs, kMaxNodeOutputs);
        }
        for (NodeDef& d : g_bt_defs) {
            d.input_count = count_pins(d.inputs, kMaxNodeInputs);
            d.output_count = count_pins(d.outputs, kMaxNodeOutputs);
        }
        for (NodeDef& d : g_material_defs) {
            d.input_count = count_pins(d.inputs, kMaxNodeInputs);
            d.output_count = count_pins(d.outputs, kMaxNodeOutputs);
        }
        g_script_nodes.assign(std::begin(g_defs), std::end(g_defs));
        return true;
    }();
    (void)counted;
    if (kind == GraphKind::SoundCue) {
        count = static_cast<u32>(sizeof g_cue_defs / sizeof g_cue_defs[0]);
        return g_cue_defs;
    }
    if (kind == GraphKind::Material) {
        count = static_cast<u32>(sizeof g_material_defs / sizeof g_material_defs[0]);
        return g_material_defs;
    }
    if (kind == GraphKind::BehaviorTree) {
        count = static_cast<u32>(sizeof g_bt_defs / sizeof g_bt_defs[0]);
        return g_bt_defs;
    }
    count = static_cast<u32>(g_script_nodes.size());
    return g_script_nodes.data();
}

const NodeDef* find_node_def(const char* id) {
    for (const GraphKind kind : {GraphKind::Script, GraphKind::SoundCue, GraphKind::Material, GraphKind::BehaviorTree}) {
        u32 count = 0;
        const NodeDef* defs = node_defs(count, kind);
        for (u32 i = 0; i < count; ++i) {
            if (std::strcmp(defs[i].id, id) == 0) return &defs[i];
        }
    }
    return nullptr;
}

bool is_material_node(const NodeDef& def) { return std::strncmp(def.id, "Mat", 3) == 0; }

bool is_event(const NodeDef& def) {  // no exec in, exec out (Custom Event has a Name field)
    for (u32 i = 0; i < def.input_count; ++i) {
        if (def.inputs[i].type == PinType::Exec) return false;
    }
    return def.output_count > 0 && def.outputs[0].type == PinType::Exec;
}

bool is_pure(const NodeDef& def) {
    for (u32 i = 0; i < def.input_count; ++i) {
        if (def.inputs[i].type == PinType::Exec) return false;
    }
    for (u32 i = 0; i < def.output_count; ++i) {
        if (def.outputs[i].type == PinType::Exec) return false;
    }
    return true;
}

// ---- Graph editing ----

GraphNode* Graph::add(const char* type, f32 x, f32 y) {
    const NodeDef* def = find_node_def(type);
    if (!def) return nullptr;
    GraphNode node;
    node.id = next_id++;
    std::snprintf(node.type, sizeof node.type, "%s", type);
    node.x = x;
    node.y = y;
    for (u32 i = 0; i < def->input_count && i < kMaxPinValues; ++i) std::snprintf(node.values[i], sizeof node.values[i], "%s", def->inputs[i].default_value);
    nodes.push_back(node);
    return &nodes[nodes.size() - 1];
}

void Graph::remove(u32 node_id) {
    for (usize i = nodes.size(); i-- > 0;) {
        if (nodes[i].id == node_id) nodes.remove_swap(i);
    }
    for (usize i = links.size(); i-- > 0;) {
        if (links[i].from_node == node_id || links[i].to_node == node_id) links.remove_swap(i);
    }
}

GraphNode* Graph::find(u32 node_id) {
    for (GraphNode& n : nodes) {
        if (n.id == node_id) return &n;
    }
    return nullptr;
}

const GraphNode* Graph::find(u32 node_id) const { return const_cast<Graph*>(this)->find(node_id); }

const GraphLink* Graph::link_into(u32 node_id, u32 pin) const {
    for (const GraphLink& l : links) {
        if (l.to_node == node_id && l.to_pin == pin) return &l;
    }
    return nullptr;
}

void Graph::disconnect_input(u32 node_id, u32 pin) {
    for (usize i = links.size(); i-- > 0;) {
        if (links[i].to_node == node_id && links[i].to_pin == pin) links.remove_swap(i);
    }
}

bool Graph::connect(u32 from, u32 from_pin, u32 to, u32 to_pin) {
    const GraphNode* a = find(from);
    const GraphNode* b = find(to);
    const NodeDef* da = a ? find_node_def(a->type) : nullptr;
    const NodeDef* db = b ? find_node_def(b->type) : nullptr;
    if (!da || !db || from == to || from_pin >= da->output_count || to_pin >= db->input_count) return false;
    const PinType out = da->outputs[from_pin].type, in = db->inputs[to_pin].type;
    const bool exec = out == PinType::Exec;
    // Same type, or anything (but exec) into a String (it is converted to text). Material nodes:
    // numbers and vectors mix (all float3 in the shader).
    const bool numeric = is_material_node(*da) && is_material_node(*db) && (out == PinType::Float || out == PinType::Vector) &&
                         (in == PinType::Float || in == PinType::Vector);
    const bool wildcard = (out == PinType::Any || in == PinType::Any) && out != PinType::Audio && in != PinType::Audio;
    if (exec != (in == PinType::Exec) || (out != in && !numeric && !wildcard && (in != PinType::String || out == PinType::Audio))) return false;
    for (usize i = links.size(); i-- > 0;) {
        const GraphLink& l = links[i];
        // An exec output leads to one place; a data input reads from one place.
        if ((exec && l.from_node == from && l.from_pin == from_pin) || (!exec && l.to_node == to && l.to_pin == to_pin)) links.remove_swap(i);
    }
    links.push_back({from, from_pin, to, to_pin});
    return true;
}

// ---- Files ----

void save_graph(const Graph& graph, Array<char>& out) {
    out.clear();
    char line[160];
    std::snprintf(line, sizeof line, "{\n  \"lsd_graph\": 1,\n  \"next_id\": %u,\n  \"nodes\": [", graph.next_id);
    append(out, line);
    for (usize i = 0; i < graph.nodes.size(); ++i) {
        const GraphNode& n = graph.nodes[i];
        std::snprintf(line, sizeof line, "%s\n    {\"id\": %u, \"type\": ", i ? "," : "", n.id);
        append(out, line);
        append_quoted(out, n.type);
        std::snprintf(line, sizeof line, ", \"x\": %.1f, \"y\": %.1f, \"values\": [", static_cast<double>(n.x), static_cast<double>(n.y));
        append(out, line);
        const NodeDef* def = find_node_def(n.type);
        u32 count = def ? (def->input_count < kMaxPinValues ? def->input_count : kMaxPinValues) : 0;
        for (u32 v = count; v < kMaxPinValues; ++v) {  // values past the pins (a Comment's text and size) when set
            if (n.values[v][0]) count = v + 1;
        }
        for (u32 v = 0; v < count; ++v) {
            if (v) append(out, ", ");
            append_quoted(out, n.values[v]);
        }
        append(out, "]}");
    }
    append(out, "\n  ],\n  \"links\": [");
    for (usize i = 0; i < graph.links.size(); ++i) {
        const GraphLink& l = graph.links[i];
        std::snprintf(line, sizeof line, "%s\n    [%u, %u, %u, %u]", i ? "," : "", l.from_node, l.from_pin, l.to_node, l.to_pin);
        append(out, line);
    }
    append(out, "\n  ]\n}\n");
}

namespace {
u32 read_u32(reflect::JsonReader& r) {
    char token[32];
    r.read_token(token, sizeof token);
    return static_cast<u32>(std::strtoul(token, nullptr, 10));
}
f32 read_f32(reflect::JsonReader& r) {
    char token[32];
    r.read_token(token, sizeof token);
    return static_cast<f32>(std::strtod(token, nullptr));
}
} // namespace

bool load_graph(Graph& graph, const char* text, usize length) {
    graph.nodes.clear();
    graph.links.clear();
    graph.next_id = 1;
    reflect::JsonReader r{text, text + length};
    r.expect('{');
    while (r.ok && !r.peek('}')) {
        char key[32];
        r.read_string(key, sizeof key);
        r.expect(':');
        if (std::strcmp(key, "next_id") == 0) {
            graph.next_id = read_u32(r);
        } else if (std::strcmp(key, "nodes") == 0) {
            r.expect('[');
            while (r.ok && !r.peek(']')) {
                GraphNode n;
                r.expect('{');
                while (r.ok && !r.peek('}')) {
                    char field[16];
                    r.read_string(field, sizeof field);
                    r.expect(':');
                    if (std::strcmp(field, "id") == 0) n.id = read_u32(r);
                    else if (std::strcmp(field, "type") == 0) r.read_string(n.type, sizeof n.type);
                    else if (std::strcmp(field, "x") == 0) n.x = read_f32(r);
                    else if (std::strcmp(field, "y") == 0) n.y = read_f32(r);
                    else if (std::strcmp(field, "values") == 0) {
                        r.expect('[');
                        for (u32 v = 0; r.ok && !r.peek(']'); ++v) {
                            char value[64];
                            r.read_string(value, sizeof value);
                            if (v < kMaxPinValues) std::snprintf(n.values[v], sizeof n.values[v], "%s", value);
                            if (!r.accept(',')) break;
                        }
                        r.expect(']');
                    } else {
                        r.skip_value();
                    }
                    if (!r.accept(',')) break;
                }
                r.expect('}');
                graph.nodes.push_back(n);
                if (n.id >= graph.next_id) graph.next_id = n.id + 1;
                if (!r.accept(',')) break;
            }
            r.expect(']');
        } else if (std::strcmp(key, "links") == 0) {
            r.expect('[');
            while (r.ok && !r.peek(']')) {
                GraphLink l;
                r.expect('[');
                l.from_node = read_u32(r);
                r.expect(',');
                l.from_pin = read_u32(r);
                r.expect(',');
                l.to_node = read_u32(r);
                r.expect(',');
                l.to_pin = read_u32(r);
                r.expect(']');
                graph.links.push_back(l);
                if (!r.accept(',')) break;
            }
            r.expect(']');
        } else {
            r.skip_value();
        }
        if (!r.accept(',')) break;
    }
    r.expect('}');
    if (!r.ok) {
        graph.nodes.clear();
        graph.links.clear();
    }
    return r.ok;
}

bool save_graph_file(const Graph& graph, const char* path) {
    Array<char> text(default_allocator());
    save_graph(graph, text);
    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    return std::fclose(file) == 0 && ok;
}

bool load_graph_file(Graph& graph, const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    Array<char> text(default_allocator());
    char chunk[8192];
    for (usize n; (n = std::fread(chunk, 1, sizeof chunk, file)) > 0;) append(text, chunk, n);
    std::fclose(file);
    return load_graph(graph, text.data(), text.size());
}

// ---- Compiler ----

bool compile_graph(const Graph& graph, Array<char>& lua, char* error, usize error_capacity) {
    if (error_capacity) error[0] = '\0';
    lua.clear();
    append(lua, kLuaPrelude);
    Compiler c{graph, error, error_capacity};
    for (const GraphNode& node : graph.nodes) {
        const NodeDef* def = find_node_def(node.type);
        if (!def) {
            c.fail(node, "unknown node type (made by a newer editor?)");
            break;
        }
        if (!is_event(*def)) continue;
        for (const GraphNode& other : graph.nodes) {
            const bool named = std::strcmp(node.type, "CustomEvent") == 0 || std::strcmp(node.type, "FunctionEntry") == 0;
            const bool same_name = !named || std::strcmp(other.values[0], node.values[0]) == 0;  // Custom Events and Functions differ by name
            if (other.id < node.id && std::strcmp(other.type, node.type) == 0 && same_name) c.fail(node, "this event is in the graph twice");
        }
        const std::string body = c.chain(node, 0, 0);
        append(lua, "\n");
        append(lua, ("--@node " + std::to_string(node.id) + "\n").c_str());
        append(lua, c.substitute(node, *def, def->code, 0, false).c_str());  // a Custom Event's header carries its name
        append(lua, "\n");
        append(lua, body.c_str());
        append(lua, "\nend\n");
    }
    return c.ok;
}

bool compile_graph_file(const char* graph_path, const char* lua_path, char* error, usize error_capacity) {
    Graph graph(default_allocator());
    if (!load_graph_file(graph, graph_path)) {
        std::snprintf(error, error_capacity, "Could not read %s", graph_path);
        return false;
    }
    Array<char> lua(default_allocator());
    if (!compile_graph(graph, lua, error, error_capacity)) return false;
    FILE* file = std::fopen(lua_path, "wb");
    if (!file) {
        std::snprintf(error, error_capacity, "Could not write %s", lua_path);
        return false;
    }
    const bool ok = std::fwrite(lua.data(), 1, lua.size(), file) == lua.size();
    return std::fclose(file) == 0 && ok;
}

} // namespace lsd::script
