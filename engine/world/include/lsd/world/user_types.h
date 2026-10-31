#pragma once
// Enums and structs made in the editor for scripts (Unreal's User Defined Enum / Struct): data
// assets ("UserEnum", "UserStruct"). Lua reads them with lsd.enum(name) and lsd.struct(name).
#include "lsd/reflect/reflect.h"

namespace lsd::world {

LSD_STRUCT()
struct EnumEntry {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) char name[48] = "";
};

// A list of names; lsd.enum("E_Weapon") -> {Sword = 0, Bow = 1, [0] = "Sword", [1] = "Bow"}.
LSD_STRUCT(Category = "Data")
struct UserEnum {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) List<EnumEntry, 64> values;
};

LSD_STRUCT()
struct StructField {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) char name[48] = "";
    LSD_PROPERTY(Edit, Widget = "Choice:Number,Boolean,Text,Vector") u32 type = 0;
    LSD_PROPERTY(Edit, Tooltip = "Number: 1.5, Boolean: true, Text: anything, Vector: 1, 2, 3") char default_value[64] = "";
};

// Named fields with defaults; lsd.struct("S_Item") -> a new table holding them (vectors as {x, y, z}).
LSD_STRUCT(Category = "Data")
struct UserStruct {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) List<StructField, 32> fields;
};

} // namespace lsd::world
