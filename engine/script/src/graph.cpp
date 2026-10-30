#include "lsd/script/graph.h"

#include "lsd/foundation/log.h"
#include "lsd/reflect/json.h"

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
    {"Spawn", "Spawn", "Objects", "Creates a prefab, mesh or shape (Cube, Sphere) by name", {kExecIn, {"Name", PinType::String, "Cube"}, {"Location", PinType::Vector, "0,0,2"}}, 0,
     {kExecOut, {"Spawned", PinType::Entity, ""}}, 0, "local $o1 = __spawn($1, $2)", {}},
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
    {"Destroy", "Destroy", "Objects", "Removes an object and its children", {kExecIn, kTarget}, 0, {kExecOut}, 0, "lsd.destroy($1)", {}},
    {"SetVariable", "Set Variable", "Variables", "Stores a number on this object", {kExecIn, {"Name", PinType::String, "health"}, {"Value", PinType::Float, "0"}}, 0,
     {kExecOut}, 0, "self[$1] = $2", {}},
    // ---- Pure ----
    {"Self", "Self", "Objects", "The object this script runs on", {}, 0, {{"Self", PinType::Entity, ""}}, 0, nullptr, {"self.entity"}},
    {"FindObject", "Find Object", "Objects", "An object by name (none if missing)", {{"Name", PinType::String, "Floor"}}, 0,
     {{"Object", PinType::Entity, ""}}, 0, nullptr, {"lsd.find($0)"}},
    {"GetName", "Get Name", "Objects", "", {kTarget}, 0, {{"Name", PinType::String, ""}}, 0, nullptr, {"lsd.name($0)"}},
    {"HasTag", "Has Tag", "Objects", "Gameplay tags: \"Damage\" matches \"Damage.Fire\"", {kTarget, {"Tag", PinType::String, "Enemy"}}, 0,
     {{"Result", PinType::Bool, ""}}, 0, nullptr, {"lsd.has_tag($0, $1)"}},
    {"GetPosition", "Get Location", "Transform", "", {kTarget}, 0, {{"Location", PinType::Vector, ""}}, 0, nullptr, {"__vec(lsd.position($0))"}},
    {"GetRotation", "Get Rotation", "Transform", "", {kTarget}, 0, {{"Rotation", PinType::Vector, ""}}, 0, nullptr, {"__vec(lsd.rotation($0))"}},
    {"SplineLocation", "Spline Location", "Transform", "World point at a distance along an object's Spline",
     {kTarget, {"Distance", PinType::Float, "0"}}, 0, {{"Location", PinType::Vector, ""}, {"Direction", PinType::Vector, ""}}, 0, nullptr,
     {"__vec(lsd.spline_point($0, $1))", "__vec(select(4, lsd.spline_point($0, $1)))"}},
    {"SplineLength", "Spline Length", "Transform", "", {kTarget}, 0, {{"Length", PinType::Float, ""}}, 0, nullptr, {"lsd.spline_length($0)"}},
    {"GetVariable", "Get Variable", "Variables", "A number stored on this object (0 if never set)", {{"Name", PinType::String, "health"}}, 0,
     {{"Value", PinType::Float, ""}}, 0, nullptr, {"(self[$0] or 0)"}},
    {"InputAxis", "Input Axis", "Input", "Value of an Input Action by asset name", {{"Action", PinType::String, "IA_Move"}}, 0,
     {{"X", PinType::Float, ""}, {"Y", PinType::Float, ""}}, 0, nullptr, {"(lsd.input($0))", "select(2, lsd.input($0))"}},
    {"InputPressed", "Input Pressed", "Input", "True on the frame the action fires (its Trigger: pressed, hold, tap...)", {{"Action", PinType::String, "IA_Jump"}}, 0,
     {{"Pressed", PinType::Bool, ""}}, 0, nullptr, {"lsd.pressed($0)"}},
    {"TableValue", "Get Table Value", "Data", "A cell of a Data Table (.csv) by asset name, row name and column name",
     {{"Table", PinType::String, "DT_Items"}, {"Row", PinType::String, "Row1"}, {"Column", PinType::String, "Value"}}, 0,
     {{"Number", PinType::Float, ""}, {"Text", PinType::String, ""}}, 0, nullptr,
     {"(tonumber(__cell($0, $1, $2)) or 0)", "tostring(__cell($0, $1, $2) or \"\")"}},
    {"EvaluateCurve", "Evaluate Curve", "Data", "Value of a Curve data asset (by name) at a time",
     {{"Curve", PinType::String, "C_Fade"}, {"Time", PinType::Float, "0"}}, 0, {{"Value", PinType::Float, ""}}, 0, nullptr, {"lsd.curve($0, $1)"}},
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
        return substitute(node, *def, code.c_str(), depth, true);
    }
};

} // namespace

const NodeDef* node_defs(u32& count, GraphKind kind) {
    static const bool counted = [] {
        for (NodeDef& d : g_defs) {
            d.input_count = count_pins(d.inputs, 6);
            d.output_count = count_pins(d.outputs, 4);
        }
        for (NodeDef& d : g_cue_defs) {
            d.input_count = count_pins(d.inputs, 6);
            d.output_count = count_pins(d.outputs, 4);
        }
        return true;
    }();
    (void)counted;
    if (kind == GraphKind::SoundCue) {
        count = static_cast<u32>(sizeof g_cue_defs / sizeof g_cue_defs[0]);
        return g_cue_defs;
    }
    count = static_cast<u32>(sizeof g_defs / sizeof g_defs[0]);
    return g_defs;
}

const NodeDef* find_node_def(const char* id) {
    for (const GraphKind kind : {GraphKind::Script, GraphKind::SoundCue}) {
        u32 count = 0;
        const NodeDef* defs = node_defs(count, kind);
        for (u32 i = 0; i < count; ++i) {
            if (std::strcmp(defs[i].id, id) == 0) return &defs[i];
        }
    }
    return nullptr;
}

bool is_event(const NodeDef& def) { return def.input_count == 0 && def.output_count > 0 && def.outputs[0].type == PinType::Exec; }

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
    // Same type, or anything (but exec) into a String (it is converted to text).
    if (exec != (in == PinType::Exec) || (out != in && (in != PinType::String || out == PinType::Audio))) return false;
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
        const u32 count = def ? (def->input_count < kMaxPinValues ? def->input_count : kMaxPinValues) : 0;
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
            if (other.id < node.id && std::strcmp(other.type, node.type) == 0) c.fail(node, "this event is in the graph twice");
        }
        const std::string body = c.chain(node, 0, 0);
        append(lua, "\n");
        append(lua, def->code);
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
