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

    // Textures. Colour vs data (sRGB vs linear) is decided by the material slot that uses them.
    LSD_PROPERTY(Edit, Category = "Texture", Min = 1, Max = 8192, Tooltip = "Larger images are halved until they fit (Unreal's Maximum Texture Size)")
    u32 max_size = 4096;

    // Sound files (Unreal's Sound Wave settings).
    LSD_PROPERTY(Edit, Category = "Sound", Min = 0, Max = 4) f32 volume = 1.0f;
    LSD_PROPERTY(Edit, Category = "Sound", Min = 0.1, Max = 4) f32 pitch = 1.0f;
    LSD_PROPERTY(Edit, Category = "Sound") bool looping = false;
    LSD_PROPERTY(Edit, Category = "Sound", Tooltip = "Read from disk while playing (long music); off = decoded into memory (short effects)") bool stream = false;
    LSD_PROPERTY(Edit, Category = "Sound", Widget = "Asset:SoundClass", Tooltip = "Its volume group (Master > SFX > Footsteps...)") char sound_class[33] = "";
    LSD_PROPERTY(Edit, Category = "Sound", Widget = "Asset:SoundAttenuation", Tooltip = "How it fades with distance; none = defaults (2 m to 40 m)") char attenuation[33] = "";
    LSD_PROPERTY(Edit, Category = "Sound", Widget = "Asset:SoundConcurrency", Tooltip = "How many of its group may play at once") char concurrency[33] = "";
    LSD_PROPERTY(Edit, Category = "Sound") char subtitle[128] = "";
};

} // namespace lsd::asset
