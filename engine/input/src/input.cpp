#include "lsd/input/input.h"

#include "lsd/foundation/log.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace lsd::input {

namespace {

// Sources: keyboard scancodes (0..SDL_SCANCODE_COUNT-1), then these.
constexpr u32 kMouseX = 1000, kMouseY = 1001, kMouseXY = 1002, kMouseLeft = 1003, kMouseMiddle = 1004, kMouseRight = 1005,
              kWheelUp = 1006, kWheelDown = 1007;  // the wheel: "pressed" for the frame it turned (Unreal's Mouse Wheel Up / Down)
constexpr u32 kLeftStickX = 1010, kLeftStickY = 1011, kRightStickX = 1012, kRightStickY = 1013, kLeftStick = 1014, kRightStick = 1015,
              kLeftTrigger = 1016, kRightTrigger = 1017;
// Touch gestures (bit = source - kTouchTap, as TouchBit), then pinch / drag, the gyro and the tilt.
constexpr u32 kSrcTap = 1020, kSrcSwipeRight = 1025, kTouchPinch = 1026, kTouchDrag = 1027;
constexpr u32 kGyroYaw = 1030, kGyroPitch = 1031, kGyroRoll = 1032, kGyroXY = 1033, kTiltX = 1034, kTiltY = 1035, kTiltXY = 1036;
constexpr u32 kGamepadButton = 1100;  // + SDL_GamepadButton
constexpr u32 kNoSource = 0xFFFFFFFFu;

struct NamedSource {
    const char* name;
    u32 source;
};

const NamedSource kSpecial[] = {
    {"Mouse X", kMouseX}, {"Mouse Y", kMouseY}, {"Mouse XY", kMouseXY},
    {"Left Mouse Button", kMouseLeft}, {"Middle Mouse Button", kMouseMiddle}, {"Right Mouse Button", kMouseRight},
    {"Mouse Wheel Up", kWheelUp}, {"Mouse Wheel Down", kWheelDown},
    {"Touch Tap", 1020}, {"Touch Hold", 1021}, {"Touch Swipe Up", 1022}, {"Touch Swipe Down", 1023}, {"Touch Swipe Left", 1024},
    {"Touch Swipe Right", kSrcSwipeRight}, {"Touch Pinch", kTouchPinch}, {"Touch Drag XY", kTouchDrag},
    {"Gyro Yaw", kGyroYaw}, {"Gyro Pitch", kGyroPitch}, {"Gyro Roll", kGyroRoll}, {"Gyro XY", kGyroXY},
    {"Tilt X", kTiltX}, {"Tilt Y", kTiltY}, {"Tilt XY", kTiltXY},
    {"Gamepad Left Stick", kLeftStick}, {"Gamepad Right Stick", kRightStick},
    {"Gamepad Left Stick X", kLeftStickX}, {"Gamepad Left Stick Y", kLeftStickY},
    {"Gamepad Right Stick X", kRightStickX}, {"Gamepad Right Stick Y", kRightStickY},
    {"Gamepad Left Trigger", kLeftTrigger}, {"Gamepad Right Trigger", kRightTrigger},
    {"Gamepad Face Bottom", kGamepadButton + static_cast<u32>(SDL_GAMEPAD_BUTTON_SOUTH)}, {"Gamepad Face Right", kGamepadButton + static_cast<u32>(SDL_GAMEPAD_BUTTON_EAST)},
    {"Gamepad Face Left", kGamepadButton + static_cast<u32>(SDL_GAMEPAD_BUTTON_WEST)}, {"Gamepad Face Top", kGamepadButton + static_cast<u32>(SDL_GAMEPAD_BUTTON_NORTH)},
    {"Gamepad Left Shoulder", kGamepadButton + static_cast<u32>(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)},
    {"Gamepad Right Shoulder", kGamepadButton + static_cast<u32>(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)},
    {"Gamepad Left Thumb", kGamepadButton + static_cast<u32>(SDL_GAMEPAD_BUTTON_LEFT_STICK)}, {"Gamepad Right Thumb", kGamepadButton + static_cast<u32>(SDL_GAMEPAD_BUTTON_RIGHT_STICK)},
    {"Gamepad DPad Up", kGamepadButton + static_cast<u32>(SDL_GAMEPAD_BUTTON_DPAD_UP)}, {"Gamepad DPad Down", kGamepadButton + static_cast<u32>(SDL_GAMEPAD_BUTTON_DPAD_DOWN)},
    {"Gamepad DPad Left", kGamepadButton + static_cast<u32>(SDL_GAMEPAD_BUTTON_DPAD_LEFT)}, {"Gamepad DPad Right", kGamepadButton + static_cast<u32>(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)},
    {"Gamepad Start", kGamepadButton + static_cast<u32>(SDL_GAMEPAD_BUTTON_START)}, {"Gamepad Back", kGamepadButton + static_cast<u32>(SDL_GAMEPAD_BUTTON_BACK)},
};

// Offered in the key picker; any other SDL key name typed by hand works too.
const char* const kKeyNames[] = {
    "W", "A", "S", "D", "Q", "E", "R", "F", "C", "V", "Z", "X", "Space", "Left Shift", "Left Ctrl", "Left Alt", "Tab",
    "Return", "Escape", "Up", "Down", "Left", "Right", "1", "2", "3", "4", "5", "F1", "F2", "F3", "F4",
    "Mouse X", "Mouse Y", "Mouse XY", "Left Mouse Button", "Middle Mouse Button", "Right Mouse Button", "Mouse Wheel Up", "Mouse Wheel Down",
    "Touch Tap", "Touch Hold", "Touch Swipe Up", "Touch Swipe Down", "Touch Swipe Left", "Touch Swipe Right", "Touch Pinch", "Touch Drag XY",
    "Gyro Yaw", "Gyro Pitch", "Gyro Roll", "Gyro XY", "Tilt X", "Tilt Y", "Tilt XY",
    "Gamepad Left Stick", "Gamepad Right Stick", "Gamepad Left Stick X", "Gamepad Left Stick Y", "Gamepad Right Stick X",
    "Gamepad Right Stick Y", "Gamepad Left Trigger", "Gamepad Right Trigger", "Gamepad Face Bottom", "Gamepad Face Right",
    "Gamepad Face Left", "Gamepad Face Top", "Gamepad Left Shoulder", "Gamepad Right Shoulder", "Gamepad Left Thumb",
    "Gamepad Right Thumb", "Gamepad DPad Up", "Gamepad DPad Down", "Gamepad DPad Left", "Gamepad DPad Right", "Gamepad Start", "Gamepad Back",
};

u32 resolve(const char* key) {
    for (const NamedSource& s : kSpecial) {
        if (SDL_strcasecmp(s.name, key) == 0) return s.source;
    }
    const SDL_Scancode code = SDL_GetScancodeFromName(key);
    return code == SDL_SCANCODE_UNKNOWN ? kNoSource : static_cast<u32>(code);
}

// Radial dead zone for sticks: small drift reads as zero, then the range is rescaled to 0..1.
Value dead_zone(f32 x, f32 y, f32 dead = 0.15f) {
    const f32 len = std::sqrt(x * x + y * y);
    if (len < dead) return {};
    const f32 clamped = len > 1.0f ? 1.0f : len;
    const f32 scaled = (clamped - dead) / (1.0f - dead) / len;
    return {x * scaled, y * scaled};
}

bool is_2d_source(u32 s) { return s == kMouseXY || s == kLeftStick || s == kRightStick || s == kTouchDrag || s == kGyroXY || s == kTiltXY; }
// Pixels, degrees or finger distance rather than -1..1: a binding's dead zone does not apply.
bool is_raw_source(u32 s) { return s == kMouseX || s == kMouseY || s == kMouseXY || s == kTouchPinch || s == kTouchDrag || (s >= kGyroYaw && s <= kGyroXY); }

Value read(u32 source, const InputFrame& f) {
    if (source < static_cast<u32>(f.key_count)) return {f.keys[source] ? 1.0f : 0.0f, 0};
    switch (source) {
        case kMouseX: return {f.mouse_dx, 0};
        case kMouseY: return {-f.mouse_dy, 0};  // up is positive, like the sticks
        case kMouseXY: return {f.mouse_dx, -f.mouse_dy};
        case kMouseLeft: return {(f.mouse_buttons & 1u) ? 1.0f : 0.0f, 0};
        case kMouseMiddle: return {(f.mouse_buttons & 2u) ? 1.0f : 0.0f, 0};
        case kMouseRight: return {(f.mouse_buttons & 4u) ? 1.0f : 0.0f, 0};
        case kWheelUp: return {f.mouse_wheel > 0.0f ? 1.0f : 0.0f, 0};
        case kWheelDown: return {f.mouse_wheel < 0.0f ? 1.0f : 0.0f, 0};
        case kLeftStick: return dead_zone(f.gamepad_axes[0], f.gamepad_axes[1]);
        case kRightStick: return dead_zone(f.gamepad_axes[2], f.gamepad_axes[3]);
        case kLeftStickX: return {dead_zone(f.gamepad_axes[0], 0).x, 0};
        case kLeftStickY: return {dead_zone(0, f.gamepad_axes[1]).y, 0};
        case kRightStickX: return {dead_zone(f.gamepad_axes[2], 0).x, 0};
        case kRightStickY: return {dead_zone(0, f.gamepad_axes[3]).y, 0};
        case kLeftTrigger: return {f.gamepad_axes[4], 0};
        case kRightTrigger: return {f.gamepad_axes[5], 0};
        case kTouchPinch: return {f.touch_pinch, 0};
        case kTouchDrag: return {f.touch_drag[0], f.touch_drag[1]};
        case kGyroYaw: return {f.gyro_deg[1], 0};
        case kGyroPitch: return {f.gyro_deg[0], 0};
        case kGyroRoll: return {f.gyro_deg[2], 0};
        case kGyroXY: return {f.gyro_deg[1], f.gyro_deg[0]};
        case kTiltX: return {f.tilt[0], 0};
        case kTiltY: return {f.tilt[1], 0};
        case kTiltXY: return {f.tilt[0], f.tilt[1]};
        default: break;
    }
    if (source >= kSrcTap && source <= kSrcSwipeRight) return {(f.touch_bits & (1u << (source - kSrcTap))) ? 1.0f : 0.0f, 0};
    if (source >= kGamepadButton && source < kGamepadButton + 32) return {(f.gamepad_buttons & (1u << (source - kGamepadButton))) ? 1.0f : 0.0f, 0};
    return {};
}

} // namespace

const char* const* key_names(u32& count) {
    count = static_cast<u32>(sizeof kKeyNames / sizeof kKeyNames[0]);
    return kKeyNames;
}

bool is_2d_key(const char* key) {
    const u32 s = resolve(key);
    return is_2d_source(s);
}

bool is_key(const char* key) { return key && key[0] && resolve(key) != kNoSource; }

bool key_down(const InputFrame& frame, const char* key) {
    const u32 source = key && key[0] ? resolve(key) : kNoSource;
    return source != kNoSource && !is_2d_source(source) && !is_raw_source(source) && read(source, frame).x > 0.5f;
}

bool held_key(const InputFrame& frame, char* out, usize capacity, const InputFrame* before) {
    const auto was_down = [before](u32 source) { return before && read(source, *before).x > 0.5f; };
    for (i32 k = 0; frame.keys && k < frame.key_count; ++k) {
        const char* name = frame.keys[k] && !was_down(static_cast<u32>(k)) ? SDL_GetScancodeName(static_cast<SDL_Scancode>(k)) : nullptr;
        if (name && name[0]) {
            SDL_snprintf(out, capacity, "%s", name);
            return true;
        }
    }
    for (const NamedSource& s : kSpecial) {  // buttons only: sticks and the mouse drift
        const bool button = (s.source >= kMouseLeft && s.source <= kWheelDown) || s.source >= kGamepadButton;
        if (button && read(s.source, frame).x > 0.5f && !was_down(s.source)) {
            SDL_snprintf(out, capacity, "%s", s.name);
            return true;
        }
    }
    return false;
}

void InputSystem::clear() {
    action_count_ = binding_count_ = context_count_ = 0;
}

i32 InputSystem::find_action(const char* guid) const {
    for (u32 i = 0; i < action_count_; ++i) {
        if (std::strcmp(actions_[i].guid, guid) == 0) return static_cast<i32>(i);
    }
    return -1;
}

void InputSystem::add_context(const InputMappingContext& context) {
    const u32 context_id = context_count_++;
    for (const KeyMapping& m : context.mappings) {
        if (!m.action[0] || !m.key[0]) continue;
        const u32 source = resolve(m.key);
        if (source == kNoSource) {
            LSD_LOG_WARN("input", "Unknown key '%s' in a mapping context", m.key);
            continue;
        }
        i32 action = find_action(m.action);
        if (action < 0) {
            if (action_count_ == kMaxActions || binding_count_ == kMaxBindings) {
                LSD_LOG_WARN("input", "Too many input actions or mappings; some are ignored");
                return;
            }
            Action& a = actions_[action_count_];
            std::memcpy(a.guid, m.action, sizeof a.guid);
            a.value = {};
            a.down = a.was_down = false;
            a.trigger = Trigger::Down;
            a.trigger_time = 0.5f;
            a.held = 0.0f;
            a.fired = a.hold_done = false;
            a.chord[0] = '\0';
            a.smoothing = 0.0f;
            a.smoothed = {};
            action = static_cast<i32>(action_count_++);
        }
        if (binding_count_ == kMaxBindings) return;
        // Keep bindings sorted by priority (highest first) so higher contexts claim keys first.
        u32 at = binding_count_++;
        while (at > 0 && bindings_[at - 1].priority < context.priority) {
            bindings_[at] = bindings_[at - 1];
            --at;
        }
        bindings_[at] = {static_cast<u32>(action), source, m.axis, m.scale, m.dead_zone, context.priority, context_id};
    }
}

void InputSystem::set_trigger(const char* action, Trigger trigger, f32 seconds) {
    const i32 i = find_action(action);
    if (i < 0) return;
    actions_[i].trigger = trigger;
    actions_[i].trigger_time = seconds;
}

void InputSystem::configure(const char* action, const InputAction& settings) {
    set_trigger(action, static_cast<Trigger>(settings.trigger), settings.trigger_time);
    const i32 i = find_action(action);
    if (i < 0) return;
    std::memcpy(actions_[i].chord, settings.chord_action, sizeof actions_[i].chord);
    actions_[i].smoothing = settings.smoothing;
}

void InputSystem::update(const InputFrame& frame, f32 dt) {
    for (u32 i = 0; i < action_count_; ++i) {
        actions_[i].was_down = actions_[i].down;
        actions_[i].value = {};
    }
    // A source used by one context is not seen by lower-priority contexts (Unreal's consume input).
    struct Claim { u32 source, context; };
    Claim claims[kMaxBindings];
    u32 claim_count = 0;
    for (u32 b = 0; b < binding_count_; ++b) {
        const Binding& binding = bindings_[b];
        Value v = read(binding.source, frame);
        if (binding.dead_zone > 0.0f && !is_raw_source(binding.source)) v = dead_zone(v.x, v.y, binding.dead_zone);  // the mapping's own, on top
        if (v.x == 0.0f && v.y == 0.0f) continue;
        bool blocked = false, claimed = false;
        for (u32 c = 0; c < claim_count; ++c) {
            if (claims[c].source != binding.source) continue;
            claimed = true;
            blocked = claims[c].context != binding.context;
        }
        if (blocked) continue;
        if (!claimed) claims[claim_count++] = {binding.source, binding.context};
        Value& out = actions_[binding.action].value;
        if (is_2d_source(binding.source)) {
            out.x += v.x * binding.scale;
            out.y += v.y * binding.scale;
        } else if (binding.axis == 1) {
            out.y += v.x * binding.scale;
        } else {
            out.x += v.x * binding.scale;
        }
    }
    for (u32 i = 0; i < action_count_; ++i) {  // smoothing: ease toward the raw value
        Action& a = actions_[i];
        if (a.smoothing > 0.0f && dt > 0.0f) {
            const f32 k = 1.0f - std::exp(-dt / a.smoothing);
            a.smoothed.x += (a.value.x - a.smoothed.x) * k;
            a.smoothed.y += (a.value.y - a.smoothed.y) * k;
            a.value = a.smoothed;
        }
        a.down = a.value.x * a.value.x + a.value.y * a.value.y > 0.25f;
    }
    for (u32 i = 0; i < action_count_; ++i) {  // chords: nothing unless the other action is down too
        Action& a = actions_[i];
        if (!a.chord[0]) continue;
        const i32 c = find_action(a.chord);
        if (c >= 0 && actions_[c].down) continue;
        a.value = {};
        a.down = false;
    }
    fire(dt);
}

void InputSystem::fire(f32 dt) {
    for (u32 i = 0; i < action_count_; ++i) {
        Action& a = actions_[i];
        const f32 held_before = a.held;
        a.held = a.down ? a.held + dt : 0.0f;
        switch (a.trigger) {
            case Trigger::Down:
            case Trigger::Pressed: a.fired = a.down && !a.was_down; break;
            case Trigger::Released: a.fired = !a.down && a.was_down; break;
            case Trigger::Hold:
                a.fired = a.down && !a.hold_done && a.held >= a.trigger_time;
                a.hold_done = a.down && (a.hold_done || a.fired);
                break;
            case Trigger::Tap: a.fired = !a.down && a.was_down && held_before <= a.trigger_time; break;
        }
    }
}

void InputSystem::record(InputRecording& recording, f32 dt) const {
    if (recording.data.empty() && recording.action_count == 0) {  // the first frame names the actions
        recording.action_count = action_count_;
        for (u32 i = 0; i < action_count_; ++i) std::memcpy(recording.actions[i], actions_[i].guid, sizeof recording.actions[i]);
    }
    recording.data.push_back(dt);
    for (u32 i = 0; i < recording.action_count; ++i) {
        const Value v = value(recording.actions[i]);  // actions added later (a hot-reloaded context) are not recorded
        recording.data.push_back(v.x);
        recording.data.push_back(v.y);
    }
}

void InputSystem::replay(const InputRecording& recording, u32 frame) {
    const f32* row = &recording.data[frame * recording.stride()];
    for (u32 i = 0; i < action_count_; ++i) {
        Action& a = actions_[i];
        a.was_down = a.down;
        a.value = {};
        for (u32 r = 0; r < recording.action_count; ++r) {
            if (std::strcmp(recording.actions[r], a.guid) == 0) a.value = {row[1 + 2 * r], row[2 + 2 * r]};
        }
        a.smoothed = a.value;  // recorded after smoothing and chords: only the triggers are worked out again
        a.down = a.value.x * a.value.x + a.value.y * a.value.y > 0.25f;
    }
    fire(row[0]);
}

bool save_recording(const char* path, const InputRecording& recording) {
    std::FILE* f = std::fopen(path, "w");
    if (!f) {
        LSD_LOG_ERROR("input", "Cannot write the input recording %s", path);
        return false;
    }
    std::fprintf(f, "lsd_input_recording 1\nactions %u\n", recording.action_count);
    for (u32 i = 0; i < recording.action_count; ++i) std::fprintf(f, "%s\n", recording.actions[i]);
    const u32 frames = recording.frame_count(), stride = recording.stride();
    std::fprintf(f, "frames %u\n", frames);
    for (u32 i = 0; i < frames; ++i) {
        for (u32 k = 0; k < stride; ++k) std::fprintf(f, k ? " %.9g" : "%.9g", static_cast<double>(recording.data[i * stride + k]));  // %.9g: floats round-trip exactly
        std::fputc('\n', f);
    }
    return std::fclose(f) == 0;
}

bool load_recording(const char* path, InputRecording& out) {
    out = {};
    std::FILE* f = std::fopen(path, "r");
    if (!f) {
        LSD_LOG_WARN("input", "No input recording %s", path);
        return false;
    }
    unsigned version = 0, actions = 0, frames = 0;
    bool ok = std::fscanf(f, "lsd_input_recording %u actions %u", &version, &actions) == 2 && version == 1 && actions <= InputSystem::kMaxActions;
    out.action_count = ok ? actions : 0;
    for (u32 i = 0; ok && i < out.action_count; ++i) ok = std::fscanf(f, "%32s", out.actions[i]) == 1 && std::strlen(out.actions[i]) == 32;
    ok = ok && std::fscanf(f, " frames %u", &frames) == 1 && frames <= 10000000u;  // ~46 hours at 60 fps
    if (ok) out.data.resize(static_cast<usize>(frames) * out.stride());
    for (usize i = 0; ok && i < out.data.size(); ++i) ok = std::fscanf(f, "%f", &out.data[i]) == 1;
    std::fclose(f);
    if (!ok) {
        LSD_LOG_WARN("input", "%s is not an input recording (or is cut short)", path);
        out = {};
    }
    return ok;
}

bool InputSystem::fired(const char* action) const {
    const i32 i = find_action(action);
    return i >= 0 && actions_[i].fired;
}

Value InputSystem::value(const char* action) const {
    const i32 i = find_action(action);
    return i < 0 ? Value{} : actions_[i].value;
}

bool InputSystem::down(const char* action) const {
    const i32 i = find_action(action);
    return i >= 0 && actions_[i].down;
}

bool InputSystem::pressed(const char* action) const {
    const i32 i = find_action(action);
    return i >= 0 && actions_[i].down && !actions_[i].was_down;
}

} // namespace lsd::input
