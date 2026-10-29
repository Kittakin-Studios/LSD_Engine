#pragma once
// Contents of the <source file>.meta sidecar written next to every asset in Content/.
// Reflected, so it saves as JSON and shows in the Details panel when the asset is selected.
#include "lsd/reflect/reflect.h"

namespace lsd::asset {

LSD_STRUCT(Category = "Asset")
struct AssetMeta {
    LSD_GENERATED()

    LSD_PROPERTY(Visible, Category = "Asset", Tooltip = "Permanent ID; references use this, not the file name")
    char guid[33] = "";

    LSD_PROPERTY(Visible, Category = "Asset")
    char type[16] = "";

    LSD_PROPERTY(Edit, Category = "Import", Min = 0.001, Max = 1000, Tooltip = "Uniform scale applied on import (after unit conversion to metres)")
    f32 import_scale = 1.0f;
};

} // namespace lsd::asset
