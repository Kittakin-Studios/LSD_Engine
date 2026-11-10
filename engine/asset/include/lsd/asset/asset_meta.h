#pragma once
// Contents of the <source file>.meta sidecar written next to every asset in Content/.
// Reflected, so it saves as JSON and shows in the Details panel when the asset is selected.
#include "lsd/foundation/math.h"
#include "lsd/reflect/reflect.h"

#include <cstring>

namespace lsd::asset {

// Something that happens at a moment of an animation (Unreal's AnimNotify): for now, a sound.
LSD_STRUCT()
struct AnimNotify {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Which animation in the file (0 = the first)") u32 clip = 0;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "When in the clip the notify fires") f32 time = 0.0f;
    LSD_PROPERTY(Edit, Widget = "Asset:Sound", Tooltip = "Played at the object (a footstep on the frame the foot lands)") char sound[33] = "";
};

// The part of a take that is kept (Unreal's Animation Sequence crop / import frame range): seconds of the take as the file has it.
LSD_STRUCT()
struct ClipRange {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Which animation in the file (0 = the first)") u32 clip = 0;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "Where the kept part starts in the file's take") f32 start = 0.0f;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "Where it ends (0 = the take's end)") f32 end = 0.0f;
};

// One key of an animation curve (Unreal's Anim Curve): a named value along a clip that scripts read while it plays
// (lsd.anim_curve): footstep loudness, a weapon trail's strength, how much IK to use.
LSD_STRUCT()
struct AnimCurveKey {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The curve's name (scripts ask for it)") char curve[32] = "";
    LSD_PROPERTY(Edit, Tooltip = "Which animation in the file (0 = the first)") u32 clip = 0;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "When in the clip") f32 time = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "The curve's value there (straight lines between keys, held past the ends)") f32 value = 0.0f;
};

// A named point on a bone of a skinned mesh (Unreal's Skeletal Mesh Socket): weapons in hands, hats
// on heads. Edited in the Mesh Editor; Attach To Socket puts an object there.
LSD_STRUCT()
struct MeshSocket {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The socket's name: Attach To Socket and lsd.socket use it") char name[32] = "";
    LSD_PROPERTY(Edit, Tooltip = "The bone it rides on") char bone[48] = "";
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "Offset from the bone") Vec3 position;
    LSD_PROPERTY(Edit, Units = "deg", Tooltip = "Turn from the bone") Vec3 rotation;
};

// A bone of the retarget pose (Unreal's IK Retargeter Retarget Pose): turned from the rest pose for matching rigs.
LSD_STRUCT()
struct RetargetBoneTurn {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The bone (its children turn with it)") char bone[48] = "";
    LSD_PROPERTY(Edit, Units = "deg", Tooltip = "Turn from the rest pose: degrees about the model's X, Y, Z, at the bone's joint") Vec3 rotation;
};

// A mesh's material slot (Unreal's Material Slots, Element 0..N): the material its triangles that use
// the file's material `name` are drawn with, unless the object overrides it.
LSD_STRUCT()
struct MaterialSlot {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The material's name in the mesh file") char name[64] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:Material", Tooltip = "Drawn with this (Material, Material Instance or Material Graph)") char material[33] = "";
};
// A control-rig control (Unreal's Control Rig control): a named, persistent, weight-blended procedural
// adjustment driven by script. Kind 0 = Turn the bone; 1 = Aim (two-bone reach); 2 = Two-bone IK.
// Stored per mesh asset so the same controls exist on every object using that mesh.
LSD_STRUCT()
struct Control {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The control's name; scripts target it with lsd.set_control / lsd.control_weight") char name[32] = "";
    LSD_PROPERTY(Edit, Widget = "Choice:Rig,Turn,Aim,Two-Bone IK", Tooltip = "What it does: Turn rotates a bone, Aim points a two-bone chain at a target, Two-Bone IK reaches a tip to a point") u32 kind = 0;
    LSD_PROPERTY(Edit, Tooltip = "The bone the control acts on (and, for Aim/Two-Bone IK, the upper bone of the chain)") char bone[48] = "";
    LSD_PROPERTY(Edit, Tooltip = "For Aim/Two-Bone IK: the middle bone (elbow / knee). Unused by Turn") char elbow[48] = "";
    LSD_PROPERTY(Edit, Tooltip = "For Aim/Two-Bone IK: the tip bone (hand / foot / muzzle). Unused by Turn") char tip[48] = "";
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "The target the control reaches toward (in the mesh's local space for Aim/Two-Bone IK)") Vec3 target;
    LSD_PROPERTY(Edit, Units = "deg", Tooltip = "Turn: rotation degrees about the model's X, Y, Z at the joint") Vec3 rotation;
    LSD_PROPERTY(Edit, Tooltip = "For Aim/Two-Bone IK: which way is forward (the chain bends to keep the tip pointing this way)") Vec3 forward{1, 0, 0};
    LSD_PROPERTY(Edit, Min = 0, Max = 360, Tooltip = "For Aim/Two-Bone IK: the chain bends at most this far (0 = no limit)") f32 limit = 0.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "How strongly it blends over the animation (0 = off, 1 = full)") f32 weight = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "On by default when this object starts") bool enabled = true;
};
constexpr u32 kMaxMaterialSlots = 48;

LSD_STRUCT(Category = "Asset")
struct AssetMeta {
    LSD_GENERATED()

    LSD_PROPERTY(Visible, Category = "Asset", Tooltip = "Permanent ID; references use this, not the file name")
    char guid[33] = "";

    LSD_PROPERTY(Visible, Category = "Asset", Tooltip = "What kind of asset this is")
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
    LSD_PROPERTY(Edit, Category = "Animation", Tooltip = "Events at times in this clip (footsteps, sounds, a script call), like Unreal's Anim Notifies") List<AnimNotify, 16> notifies;
    LSD_PROPERTY(Edit, Category = "Animation", Tooltip = "Trims takes to a part of them (Unreal's Crop): start and end seconds per clip")
    List<ClipRange, 16> clip_ranges;
    LSD_PROPERTY(Edit, Category = "Animation", Tooltip = "Animation curves (Unreal's Anim Curves): keys of named values along the clips; lsd.anim_curve reads them")
    List<AnimCurveKey, 64> curve_keys;
    LSD_PROPERTY(Edit, Category = "Skeleton", Tooltip = "Named points on bones to attach props to (Unreal's Sockets)") List<MeshSocket, 16> sockets;
    LSD_PROPERTY(Edit, Category = "Skeleton", Tooltip = "Retarget pose (Unreal's Retarget Pose): bones turned from the rest pose (an A-pose made a T-pose) so animations retarget between rigs that rest differently")
    List<RetargetBoneTurn, 64> retarget_pose;
    LSD_PROPERTY(Edit, Category = "Rig", Tooltip = "Control rig (Unreal's Control Rig): named, weight-blended procedural controls (turns, aims, two-bone IK) that script drives on top of the animation")
    List<Control, 32> control_rig;
    LSD_PROPERTY(Edit, Category = "Materials", Tooltip = "The material for each of the file's materials (Unreal's Material Slots); Import Materials fills them")
    List<MaterialSlot, 48> material_slots;  // kMaxMaterialSlots (the reflection generator reads the number)

    // Textures. Colour vs data (sRGB vs linear) is decided by the material slot that uses them.
    LSD_PROPERTY(Edit, Category = "Texture", Min = 1, Max = 8192, Tooltip = "Larger images are halved until they fit (Unreal's Maximum Texture Size)")
    u32 max_size = 4096;
    LSD_PROPERTY(Edit, Category = "Texture", Tooltip = "Only the mips the view needs are on the GPU, within the texture pool (r.TexturePoolSize), like Unreal's texture streaming; off = always all of it (Unreal's Never Stream)")
    bool mip_streaming = true;

    // Sound files (Unreal's Sound Wave settings).
    LSD_PROPERTY(Edit, Category = "Sound", Min = 0, Max = 4, Tooltip = "Loudness of this sound (1 = as recorded)") f32 volume = 1.0f;
    LSD_PROPERTY(Edit, Category = "Sound", Min = 0.1, Max = 4, Tooltip = "Playback speed and pitch (2 = an octave up, twice as fast)") f32 pitch = 1.0f;
    LSD_PROPERTY(Edit, Category = "Sound", Tooltip = "Plays again from the start when it ends (music, ambience)") bool looping = false;
    LSD_PROPERTY(Edit, Category = "Sound", Tooltip = "Read from disk while playing (long music); off = decoded into memory (short effects)") bool stream = false;
    LSD_PROPERTY(Edit, Category = "Sound", Widget = "Asset:SoundClass", Tooltip = "Its volume group (Master > SFX > Footsteps...)") char sound_class[33] = "";
    LSD_PROPERTY(Edit, Category = "Sound", Widget = "Asset:SoundAttenuation", Tooltip = "How it fades with distance; none = defaults (2 m to 40 m)") char attenuation[33] = "";
    LSD_PROPERTY(Edit, Category = "Sound", Widget = "Asset:SoundConcurrency", Tooltip = "How many of its group may play at once") char concurrency[33] = "";
    LSD_PROPERTY(Edit, Category = "Sound", Tooltip = "Text shown at the bottom of the screen while it plays (when subtitles are on)") char subtitle[128] = "";
};

// Curve `name` of clip `clip` at `time` seconds: straight between its keys (in any order), held past the ends.
// False when that clip has no such curve.
inline bool curve_value(const AssetMeta& meta, u32 clip, const char* name, f32 time, f32& out) {
    const AnimCurveKey *before = nullptr, *after = nullptr;
    for (const AnimCurveKey& k : meta.curve_keys) {
        if (k.clip != clip || std::strcmp(k.curve, name) != 0) continue;
        if (k.time <= time && (!before || k.time >= before->time)) before = &k;
        if (k.time >= time && (!after || k.time < after->time)) after = &k;
    }
    if (!before && !after) return false;
    if (!before || !after || after->time <= before->time) {
        out = before ? before->value : after->value;
        return true;
    }
    const f32 t = (time - before->time) / (after->time - before->time);
    out = before->value + (after->value - before->value) * t;
    return true;
}

} // namespace lsd::asset
