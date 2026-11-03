#pragma once
// Enums and structs made in the editor for scripts (Unreal's User Defined Enum / Struct): data
// assets ("UserEnum", "UserStruct"). Lua reads them with lsd.enum(name) and lsd.struct(name).
#include "lsd/reflect/reflect.h"

namespace lsd::world {

LSD_STRUCT()
struct EnumEntry {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "One of the enum's values") char name[48] = "";
};

// A list of names; lsd.enum("E_Weapon") -> {Sword = 0, Bow = 1, [0] = "Sword", [1] = "Bow"}.
LSD_STRUCT(Category = "Data")
struct UserEnum {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The values to pick from (Unreal's Enumeration)") List<EnumEntry, 64> values;
};

LSD_STRUCT()
struct StructField {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The field's name") char name[48] = "";
    LSD_PROPERTY(Edit, Widget = "Choice:Number,Boolean,Text,Vector", Tooltip = "What the field holds") u32 type = 0;
    LSD_PROPERTY(Edit, Tooltip = "Number: 1.5, Boolean: true, Text: anything, Vector: 1, 2, 3") char default_value[64] = "";
};

// Named fields with defaults; lsd.struct("S_Item") -> a new table holding them (vectors as {x, y, z}).
LSD_STRUCT(Category = "Data")
struct UserStruct {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The struct's fields (Unreal's Structure)") List<StructField, 32> fields;
};

LSD_STRUCT()
struct NodePin {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The pin's name on the node") char name[32] = "";
    LSD_PROPERTY(Edit, Widget = "Choice:Exec,Number,Boolean,Text,Vector,Object", Tooltip = "What the pin carries: Exec (white, the order things run), or a value") u32 type = 1;
    LSD_PROPERTY(Edit, Tooltip = "Inputs: the value when nothing is wired (Object: self)") char default_value[48] = "";
};

// A visual-script node made in the editor (Unreal's Blueprint function libraries): it shows in the
// node menu under Category, named after the asset. Code is the Lua it becomes: $1, $2... are the
// inputs (by position, the white Exec pin counts), $o0 is the first output's variable, #0 is where
// the next node's code goes. A node without Exec pins is pure: Expression is its output's Lua.
LSD_STRUCT(Category = "Data")
struct ScriptNode {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Where the node is listed in the node menu") char category[32] = "Custom";
    LSD_PROPERTY(Edit, Tooltip = "The help shown when hovering the node") char tooltip[128] = "";
    LSD_PROPERTY(Edit, Tooltip = "The node's input pins") List<NodePin, 6> inputs;
    LSD_PROPERTY(Edit, Tooltip = "The node's output pins") List<NodePin, 4> outputs;
    LSD_PROPERTY(Edit, Tooltip = "Exec nodes: Lua with $1.. (inputs), $o0.. (outputs), #0 (what comes next)") char code[512] = "";
    LSD_PROPERTY(Edit, Tooltip = "Pure nodes: the first output's Lua expression, e.g. ($1 * $2)") char expression[256] = "";
};

} // namespace lsd::world
