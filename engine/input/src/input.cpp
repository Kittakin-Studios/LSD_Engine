#include "lsd/input/input.h"

#include "lsd/foundation/log.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstring>

namespace lsd::input {

namespace {

// Sources: keyboard scancodes (0..SDL_SCANCODE_COUNT-1), then these.
constexpr u32 kMouseX = 1000, kMouseY = 1001, kMouseXY = 1002, kMouseLeft = 1003, kMouseMiddle = 1004, kMouseRight = 1005;
constexpr u32 kLeftStickX = 1010, kLeftStickY = 1011, kRightStickX = 1012, kRightStickY = 1013, kLeftStick = 1014, kRightStick = 1015,
              kLeftTrigger = 1016, kRightTrigger = 1017;
constexpr u32 kGamepadButton = 1100;  // + SDL_GamepadButton
constexpr u32 kNoSource = 0xFFFFFFFFu;

struct NamedSource {
    const char* name;
    u32 source;
};

const NamedSource kSpecial[] = {
    {"Mouse X", kMouseX}, {"Mouse Y", kMouseY}, {"Mouse XY", kMouseXY},
    {"Left Mouse Button", kMouseLeft}, {"Middle Mouse Button", kMouseMiddle}, {"Right Mouse Button", kMouseRight},
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
    "Mouse X", "Mouse Y", "Mouse XY", "Left Mouse Button", "Middle Mouse Button", "Right Mouse Button",
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
Value dead_zone(f32 x, f32 y) {
    constexpr f32 kDead = 0.15f;
    const f32 len = std::sqrt(x * x + y * y);
    if (len < kDead) return {};
    const f32 clamped = len > 1.0f ? 1.0f : len;
    const f32 scaled = (clamped - kDead) / (1.0f - kDead) / len;
    return {x * scaled, y * scaled};
}

Value read(u32 source, const InputFrame& f) {
    if (source < static_cast<u32>(f.key_count)) return {f.keys[source] ? 1.0f : 0.0f, 0};
    switch (source) {
        case kMouseX: return {f.mouse_dx, 0};
        case kMouseY: return {-f.mouse_dy, 0};  // up is positive, like the sticks
        case kMouseXY: return {f.mouse_dx, -f.mouse_dy};
        case kMouseLeft: return {(f.mouse_buttons & 1u) ? 1.0f : 0.0f, 0};
        case kMouseMiddle: return {(f.mouse_buttons & 2u) ? 1.0f : 0.0f, 0};
        case kMouseRight: return {(f.mouse_buttons & 4u) ? 1.0f : 0.0f, 0};
        case kLeftStick: return dead_zone(f.gamepad_axes[0], f.gamepad_axes[1]);
        case kRightStick: return dead_zone(f.gamepad_axes[2], f.gamepad_axes[3]);
        case kLeftStickX: return {dead_zone(f.gamepad_axes[0], 0).x, 0};
        case kLeftStickY: return {dead_zone(0, f.gamepad_axes[1]).y, 0};
        case kRightStickX: return {dead_zone(f.gamepad_axes[2], 0).x, 0};
        case kRightStickY: return {dead_zone(0, f.gamepad_axes[3]).y, 0};
        case kLeftTrigger: return {f.gamepad_axes[4], 0};
        case kRightTrigger: return {f.gamepad_axes[5], 0};
        default: break;
    }
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
    return s == kMouseXY || s == kLeftStick || s == kRightStick;
}

bool is_key(const char* key) { return key && key[0] && resolve(key) != kNoSource; }

bool held_key(const InputFrame& frame, char* out, usize capacity) {
    for (i32 k = 0; frame.keys && k < frame.key_count; ++k) {
        const char* name = frame.keys[k] ? SDL_GetScancodeName(static_cast<SDL_Scancode>(k)) : nullptr;
        if (name && name[0]) {
            SDL_snprintf(out, capacity, "%s", name);
            return true;
        }
    }
    for (const NamedSource& s : kSpecial) {  // buttons only: sticks and the mouse drift
        const bool button = (s.source >= kMouseLeft && s.source <= kMouseRight) || s.source >= kGamepadButton;
        if (button && read(s.source, frame).x > 0.5f) {
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
            action = static_cast<i32>(action_count_++);
        }
        if (binding_count_ == kMaxBindings) return;
        // Keep bindings sorted by priority (highest first) so higher contexts claim keys first.
        u32 at = binding_count_++;
        while (at > 0 && bindings_[at - 1].priority < context.priority) {
            bindings_[at] = bindings_[at - 1];
            --at;
        }
        bindings_[at] = {static_cast<u32>(action), source, m.axis, m.scale, context.priority, context_id};
    }
}

void InputSystem::set_trigger(const char* action, Trigger trigger, f32 seconds) {
    const i32 i = find_action(action);
    if (i < 0) return;
    actions_[i].trigger = trigger;
    actions_[i].trigger_time = seconds;
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
        const Value v = read(binding.source, frame);
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
        const bool two_d = binding.source == kMouseXY || binding.source == kLeftStick || binding.source == kRightStick;
        if (two_d) {
            out.x += v.x * binding.scale;
            out.y += v.y * binding.scale;
        } else if (binding.axis == 1) {
            out.y += v.x * binding.scale;
        } else {
            out.x += v.x * binding.scale;
        }
    }
    for (u32 i = 0; i < action_count_; ++i) {
        Action& a = actions_[i];
        a.down = a.value.x * a.value.x + a.value.y * a.value.y > 0.25f;
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
