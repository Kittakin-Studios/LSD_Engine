#pragma once
// Contents of the <source file>.meta sidecar written next to every asset in Content/.
// Reflected, so it saves as JSON and shows in the Details panel when the asset is selected.
#include "lsd/foundation/math.h"
#include "lsd/reflect/reflect.h"

namespace lsd::asset {

// Something that happens at a moment of an animation (Unreal's AnimNotify): for now, a sound.
LSD_STRUCT()
struct AnimNotify {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Which animation in the file (0 = the first)") u32 clip = 0;
    LSD_PROPERTY(Edit, Units = "s", Min = 0) f32 time = 0.0f;
    LSD_PROPERTY(Edit, Widget = "Asset:Sound", Tooltip = "Played at the object (a footstep on the frame the foot lands)") char sound[33] = "";
};

// A named point on a bone of a skinned mesh (Unreal's Skeletal Mesh Socket): weapons in hands, hats
// on heads. Edited in the Mesh Editor; Attach To Socket puts an object there.
LSD_STRUCT()
struct MeshSocket {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) char name[32] = "";
    LSD_PROPERTY(Edit) char bone[48] = "";
    LSD_PROPERTY(Edit, Units = "m") Vec3 position;
    LSD_PROPERTY(Edit, Units = "deg") Vec3 rotation;
};

LSD_STRUCT(Category = "Asset")
struct AssetMeta {
    LSD_GENERATED()

    LSD_PROPERTY(Visible, Category = "Asset", Tooltip = "Permanent ID; references use this, not the file name")
    char guid[33] = "";

    LSD_PROPERTY(Visible, Category = "Asset")
    char type[16] = "";

    LSD_PROPERTY(Edit, Category = "Asset", Tooltip = "Comma-separated words the Content Browser search also matches (Unreal's asset metadata tags)")
    char tags[96] = "";

    LSD_PROPERTY(Edit, Category = "Import", Min = 0.001, Max = 1000, Tooltip = "Uniform scale applied on import (after unit conversion to metres)")
    f32 import_scale = 1.0f;
    LSD_PROPERTY(Edit, Category = "Import", Widget = "Choice:Import,Smooth,Solid (Flat),Auto Smooth",
                 Tooltip = "Shading (Blender's Shade Smooth / Flat / Auto Smooth; Unreal's Normal Import Method): the file's normals, smooth (the default), solid faceted faces, or smooth with edges sharper than the angle kept hard")
    u32 normals = 1;
    LSD_PROPERTY(Edit, Category = "Import", Units = "deg", Min = 0, Max = 180, Tooltip = "Auto Smooth: edges sharper than this stay hard")
    f32 smoothing_angle = 30.0f;

    // Auto LODs (Unreal's LOD Screen Size): how big the mesh looks (bounding radius over distance)
    // below which each simpler LOD takes over. Larger = switches sooner.
    LSD_PROPERTY(Edit, Category = "LOD", Min = 0, Max = 2, Tooltip = "LOD 1 (half the triangles) below this size; about half the screen at 0.12")
    f32 lod1_screen_size = 0.12f;
    LSD_PROPERTY(Edit, Category = "LOD", Min = 0, Max = 2, Tooltip = "LOD 2 (a quarter) below this size") f32 lod2_screen_size = 0.06f;
    LSD_PROPERTY(Edit, Category = "LOD", Min = 0, Max = 2, Tooltip = "LOD 3 (an eighth) below this size") f32 lod3_screen_size = 0.03f;

    // Animation files (FBX with takes): notifies, edited in Details, played in Play.
    LSD_PROPERTY(Edit, Category = "Animation") List<AnimNotify, 16> notifies;
    LSD_PROPERTY(Edit, Category = "Skeleton") List<MeshSocket, 16> sockets;

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
