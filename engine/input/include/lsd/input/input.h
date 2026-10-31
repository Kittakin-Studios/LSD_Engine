#pragma once
// Input assets and their runtime, like Unreal's Enhanced Input: an Input Action is something the
// game reacts to (Move, Look, Jump); an Input Mapping Context maps keys, mouse and gamepad to actions.
// Both are data assets edited in the Details panel, so input is wired up without code.
#include "lsd/reflect/reflect.h"

namespace lsd::input {

// What an action's value means. Read it with InputSystem::value / down / pressed.
LSD_STRUCT(Category = "Input")
struct InputAction {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Choice:Button (bool),Axis 1D (float),Axis 2D (Vector2D)") u32 value_type = 0;
    LSD_PROPERTY(Edit) char description[96] = "";
    LSD_PROPERTY(Edit, Widget = "Choice:Down,Pressed,Released,Hold,Tap",
                 Tooltip = "When the action fires (Unreal's Triggers): Down and Pressed when pushed, Released when let go, Hold after Trigger Time held, Tap when let go within Trigger Time") u32 trigger = 0;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "Hold: how long to hold. Tap: the longest press that still counts") f32 trigger_time = 0.5f;
    LSD_PROPERTY(Edit, Widget = "Asset:InputAction", Tooltip = "Counts only while this other action is held too (Unreal's Chorded Action: Shift + E)")
    char chord_action[33] = "";
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "The value eases toward the input over this time (Unreal's Smooth modifier): steadier mouse or stick look") f32 smoothing = 0.0f;
};

enum class Trigger : u32 { Down, Pressed, Released, Hold, Tap };

// One key (or stick, or mouse axis) feeding one action.
LSD_STRUCT()
struct KeyMapping {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:InputAction") char action[33] = "";
    LSD_PROPERTY(Edit, Widget = "Key") char key[32] = "";
    LSD_PROPERTY(Edit, Widget = "Choice:X,Y", Tooltip = "Which part of a 2D action a 1D key drives (Unreal's Swizzle YXZ = Y)") u32 axis = 0;
    LSD_PROPERTY(Edit, Tooltip = "Multiplier; -1 is Unreal's Negate (S and A for Move)") f32 scale = 1.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 0.95, Tooltip = "Values under this read as 0, the rest rescaled to 0..1 (Unreal's Dead Zone modifier); sticks already have 0.15")
    f32 dead_zone = 0.0f;
};

LSD_STRUCT(Category = "Input")
struct InputMappingContext {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "When two contexts use the same key, the higher priority one gets it") i32 priority = 0;
    LSD_PROPERTY(Edit) List<KeyMapping, 64> mappings;
};

// Device state for one frame, filled by the platform layer (SDL in the editor and runtime).
struct InputFrame {
    const bool* keys = nullptr;  // indexed by SDL scancode (SDL_GetKeyboardState)
    i32 key_count = 0;
    f32 mouse_dx = 0, mouse_dy = 0;  // pixels moved this frame
    u32 mouse_buttons = 0;           // bit 0 left, 1 middle, 2 right
    f32 gamepad_axes[6] = {};        // left x, left y, right x, right y (-1..1, +y = up), left / right trigger (0..1)
    u32 gamepad_buttons = 0;         // bit per SDL_GamepadButton
};

// Key names offered by the editor's key picker. Keyboard keys use SDL's names ("W", "Space", "Left Shift").
const char* const* key_names(u32& count);
bool is_2d_key(const char* key);  // "Mouse XY", "Gamepad Left Stick"...
bool is_key(const char* key);     // a name the input system understands
// The name of a key, mouse button or gamepad button held this frame (the first found); false if none.
// "Press a key" menus use it to rebind.
bool held_key(const InputFrame& frame, char* out, usize capacity);

struct Value {
    f32 x = 0, y = 0;
};

class InputSystem {
public:
    void clear();
    // Adds a context's mappings. Actions are asset GUIDs (32 hex characters). Unknown keys are logged and skipped.
    void add_context(const InputMappingContext& context);
    // How an action fires (from its InputAction asset); actions not yet in a context are ignored.
    void set_trigger(const char* action, Trigger trigger, f32 seconds);
    // Everything from the action's asset: its trigger, chord action and smoothing.
    void configure(const char* action, const InputAction& settings);
    void update(const InputFrame& frame, f32 dt = 0.0f);

    Value value(const char* action) const;  // Axis 2D: x, y; Axis 1D and Button: x
    bool down(const char* action) const;    // value length above 0.5
    bool pressed(const char* action) const; // went down in the last update
    bool fired(const char* action) const;   // its trigger fired in the last update (Pressed when not set)

    static constexpr u32 kMaxActions = 64;
    static constexpr u32 kMaxBindings = 256;

private:
    struct Action {
        char guid[33];
        Value value;
        bool down, was_down;
        Trigger trigger;
        f32 trigger_time;
        f32 held;       // seconds down so far
        bool fired;
        bool hold_done; // Hold fires once per press
        char chord[33]; // another action that must be down too ("" = none)
        f32 smoothing;  // seconds
        Value smoothed; // the eased value, kept between frames
    };
    struct Binding {
        u32 action;
        u32 source;  // see input.cpp: keyboard scancode, or a mouse / gamepad id above the scancodes
        u32 axis;
        f32 scale;
        f32 dead_zone;
        i32 priority;
        u32 context;
    };
    i32 find_action(const char* guid) const;

    Action actions_[kMaxActions];
    u32 action_count_ = 0;
    Binding bindings_[kMaxBindings];
    u32 binding_count_ = 0;
    u32 context_count_ = 0;
};

} // namespace lsd::input
