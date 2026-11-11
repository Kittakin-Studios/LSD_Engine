// Input tests: mapping contexts turn keys, mouse and sticks into action values; data asset round trip.
#include "lsd/asset/data_asset.h"
#include "lsd/input/input.h"
#include "lsd/input/touch.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

using namespace lsd;
using namespace lsd::input;

constexpr char kMove[] = "00000000000000000000000000000001";
constexpr char kJump[] = "00000000000000000000000000000002";
constexpr char kLook[] = "00000000000000000000000000000003";
constexpr char kTalk[] = "00000000000000000000000000000004";
constexpr char kHold[] = "00000000000000000000000000000005";
constexpr char kTap[] = "00000000000000000000000000000006";
constexpr char kUp[] = "00000000000000000000000000000007";

static KeyMapping map(const char* action, const char* key, u32 axis = 0, f32 scale = 1.0f) {
    KeyMapping m;
    std::snprintf(m.action, sizeof m.action, "%s", action);
    std::snprintf(m.key, sizeof m.key, "%s", key);
    m.axis = axis;
    m.scale = scale;
    return m;
}

static void test_mappings() {
    InputMappingContext walk;
    walk.mappings.push_back(map(kMove, "W", 1));
    walk.mappings.push_back(map(kMove, "S", 1, -1.0f));
    walk.mappings.push_back(map(kMove, "D"));
    walk.mappings.push_back(map(kMove, "A", 0, -1.0f));
    walk.mappings.push_back(map(kMove, "Gamepad Left Stick"));
    walk.mappings.push_back(map(kJump, "Space"));
    walk.mappings.push_back(map(kLook, "Mouse XY"));
    walk.mappings.push_back(map(kJump, "Not A Key"));  // logged and skipped

    InputSystem input;
    input.add_context(walk);
    static bool keys[SDL_SCANCODE_COUNT];
    InputFrame frame;
    frame.keys = keys;
    frame.key_count = SDL_SCANCODE_COUNT;

    keys[SDL_SCANCODE_W] = keys[SDL_SCANCODE_A] = true;
    frame.mouse_dx = 4.0f;
    frame.mouse_dy = 2.0f;
    input.update(frame);
    CHECK(input.value(kMove).x == -1.0f && input.value(kMove).y == 1.0f);
    CHECK(input.value(kLook).x == 4.0f && input.value(kLook).y == -2.0f);  // mouse up is positive
    CHECK(!input.down(kJump) && !input.pressed(kJump));

    keys[SDL_SCANCODE_SPACE] = true;
    input.update(frame);
    CHECK(input.down(kJump) && input.pressed(kJump));
    input.update(frame);
    CHECK(input.down(kJump) && !input.pressed(kJump));  // pressed only on the first frame

    // Stick dead zone: drift reads as zero; a full push reads as about one.
    keys[SDL_SCANCODE_W] = keys[SDL_SCANCODE_A] = false;
    frame.gamepad_axes[0] = 0.1f;
    input.update(frame);
    CHECK(input.value(kMove).x == 0.0f);
    frame.gamepad_axes[0] = 1.0f;
    input.update(frame);
    CHECK(std::fabs(input.value(kMove).x - 1.0f) < 1e-4f);

    // A higher-priority context that uses Space takes it: Jump no longer sees it.
    InputMappingContext dialogue;
    dialogue.priority = 10;
    dialogue.mappings.push_back(map(kTalk, "Space"));
    input.add_context(dialogue);
    input.update(frame);
    CHECK(input.down(kTalk) && !input.down(kJump));
    CHECK(input.value("ffffffffffffffffffffffffffffffff").x == 0.0f);  // unknown action reads as zero

    // Triggers: Hold fires once after 0.5 s held; Tap fires on a quick release only; Released on any release.
    InputMappingContext buttons;
    buttons.priority = 20;
    buttons.mappings.push_back(map(kHold, "E"));
    buttons.mappings.push_back(map(kTap, "F"));
    buttons.mappings.push_back(map(kUp, "G"));
    input.add_context(buttons);
    input.set_trigger(kHold, Trigger::Hold, 0.5f);
    input.set_trigger(kTap, Trigger::Tap, 0.3f);
    input.set_trigger(kUp, Trigger::Released, 0.0f);
    keys[SDL_SCANCODE_E] = keys[SDL_SCANCODE_F] = keys[SDL_SCANCODE_G] = true;
    int hold_fires = 0;
    for (int i = 0; i < 10; ++i) {  // 1 s held
        input.update(frame, 0.1f);
        hold_fires += input.fired(kHold);
        CHECK(!input.fired(kTap) && !input.fired(kUp));
    }
    CHECK(hold_fires == 1);
    keys[SDL_SCANCODE_E] = keys[SDL_SCANCODE_F] = keys[SDL_SCANCODE_G] = false;
    input.update(frame, 0.1f);
    CHECK(!input.fired(kTap) && input.fired(kUp));  // held 1 s: too long for a tap
    keys[SDL_SCANCODE_F] = true;
    input.update(frame, 0.1f);
    input.update(frame, 0.1f);
    keys[SDL_SCANCODE_F] = false;
    input.update(frame, 0.1f);
    CHECK(input.fired(kTap));
    input.update(frame, 0.1f);
    CHECK(!input.fired(kTap));  // once
}

// Chords (Shift + E), smoothing (the value eases in) and a mapping's own dead zone.
static void test_modifiers() {
    constexpr char kShift[] = "00000000000000000000000000000011";
    constexpr char kPower[] = "00000000000000000000000000000012";
    constexpr char kTurn[] = "00000000000000000000000000000013";
    constexpr char kThrottle[] = "00000000000000000000000000000014";
    InputMappingContext context;
    context.mappings.push_back(map(kShift, "Left Shift"));
    context.mappings.push_back(map(kPower, "E"));
    context.mappings.push_back(map(kTurn, "D"));
    KeyMapping trigger = map(kThrottle, "Gamepad Right Trigger");
    trigger.dead_zone = 0.5f;
    context.mappings.push_back(trigger);
    InputSystem input;
    input.add_context(context);
    InputAction power;
    std::snprintf(power.chord_action, sizeof power.chord_action, "%s", kShift);
    power.trigger = static_cast<u32>(Trigger::Pressed);
    input.configure(kPower, power);
    InputAction turn;
    turn.smoothing = 0.2f;
    input.configure(kTurn, turn);

    static bool keys[SDL_SCANCODE_COUNT];
    InputFrame frame;
    frame.keys = keys;
    frame.key_count = SDL_SCANCODE_COUNT;
    keys[SDL_SCANCODE_E] = true;
    input.update(frame, 0.1f);
    CHECK(!input.down(kPower) && !input.fired(kPower));  // E alone: nothing
    keys[SDL_SCANCODE_LSHIFT] = true;
    input.update(frame, 0.1f);
    CHECK(input.down(kPower) && input.fired(kPower));    // Shift + E

    keys[SDL_SCANCODE_D] = true;
    input.update(frame, 0.1f);
    const f32 first = input.value(kTurn).x;
    for (int i = 0; i < 20; ++i) input.update(frame, 0.1f);
    CHECK(first > 0.2f && first < 0.6f && input.value(kTurn).x > 0.99f);  // eased toward 1

    frame.gamepad_axes[5] = 0.4f;  // under the mapping's 0.5 dead zone
    input.update(frame, 0.1f);
    CHECK(input.value(kThrottle).x == 0.0f);
    frame.gamepad_axes[5] = 0.75f;
    input.update(frame, 0.1f);
    CHECK(std::fabs(input.value(kThrottle).x - 0.5f) < 1e-4f);  // rescaled: halfway past the dead zone
}

static void test_held_key() {  // rebinding's "press a key"
    static bool keys[SDL_SCANCODE_COUNT];
    InputFrame frame;
    frame.keys = keys;
    frame.key_count = SDL_SCANCODE_COUNT;
    char name[32];
    CHECK(!held_key(frame, name, sizeof name));
    frame.mouse_dx = 30.0f;  // moving the mouse is not a key
    CHECK(!held_key(frame, name, sizeof name));
    frame.mouse_buttons = 4;
    CHECK(held_key(frame, name, sizeof name) && std::strcmp(name, "Right Mouse Button") == 0 && is_key(name));
    keys[SDL_SCANCODE_E] = true;
    CHECK(held_key(frame, name, sizeof name) && std::strcmp(name, "E") == 0 && is_key(name));
    CHECK(!is_key("Not A Key") && !is_key(""));
    // lsd.key_down: any named key, whatever else is held; a mouse axis is not a key.
    keys[SDL_SCANCODE_F] = true;
    CHECK(key_down(frame, "F") && key_down(frame, "E") && key_down(frame, "Right Mouse Button"));
    CHECK(!key_down(frame, "W") && !key_down(frame, "Mouse XY") && !key_down(frame, "Not A Key") && !key_down(frame, nullptr));
    // lsd.key_pressed: only what went down since the last frame (F while E stays held; nothing when F is let go).
    static bool keys_before[SDL_SCANCODE_COUNT];
    InputFrame before = frame;
    before.keys = keys_before;
    keys_before[SDL_SCANCODE_E] = true;
    CHECK(held_key(frame, name, sizeof name, &before) && std::strcmp(name, "F") == 0);
    keys_before[SDL_SCANCODE_F] = true;
    keys[SDL_SCANCODE_F] = false;
    CHECK(!held_key(frame, name, sizeof name, &before));
    before.mouse_buttons = 0;
    CHECK(held_key(frame, name, sizeof name, &before) && std::strcmp(name, "Right Mouse Button") == 0);
}

static void test_data_asset() {
    asset::register_data_type(reflect::type_of<InputMappingContext>());
    asset::register_data_type(reflect::type_of<InputAction>());
    asset::register_data_type(reflect::type_of<InputAction>());  // twice is harmless
    u32 count = 0;
    asset::data_types(count);
    CHECK(count == 2 && asset::find_data_type("InputAction"));

    const std::string path = std::string(LSD_TEST_TEMP_DIR) + "/IMC_Test.lsdasset";
    InputMappingContext imc;
    imc.priority = 3;
    imc.mappings.push_back(map(kMove, "W", 1));
    CHECK(asset::save_data_asset(path.c_str(), reflect::type_of<InputMappingContext>(), &imc));
    char type[32];
    CHECK(asset::read_data_asset_type(path.c_str(), type, sizeof type) && std::strcmp(type, "InputMappingContext") == 0);
    InputMappingContext loaded;
    CHECK(asset::load_data_asset(path.c_str(), loaded));
    CHECK(loaded.priority == 3 && loaded.mappings.count == 1 && std::strcmp(loaded.mappings[0].key, "W") == 0 && loaded.mappings[0].axis == 1);
    InputAction wrong;
    std::snprintf(wrong.description, sizeof wrong.description, "keep me");
    CHECK(!asset::load_data_asset(path.c_str(), wrong) && std::strcmp(wrong.description, "keep me") == 0);  // type mismatch leaves it alone
    std::filesystem::remove(path);
}

// Recording: what update() gave is saved, loaded and replayed: same values and the same triggers firing.
static void test_recording() {
    InputMappingContext walk;
    walk.mappings.push_back(map(kMove, "W", 1));
    walk.mappings.push_back(map(kJump, "Space"));
    walk.mappings.push_back(map(kLook, "Mouse XY"));
    InputSystem live;
    live.add_context(walk);
    InputAction tap;
    tap.trigger = static_cast<u32>(Trigger::Tap);
    tap.trigger_time = 0.2f;
    live.configure(kJump, tap);
    static bool keys[SDL_SCANCODE_COUNT];
    InputFrame frame;
    frame.keys = keys;
    frame.key_count = SDL_SCANCODE_COUNT;
    InputRecording recording;
    Value moves[6];
    bool fired[6];
    for (u32 i = 0; i < 6; ++i) {
        keys[SDL_SCANCODE_W] = i >= 1 && i <= 3;
        keys[SDL_SCANCODE_SPACE] = i == 2;  // a tap: fires on frame 3
        frame.mouse_dx = static_cast<f32>(i) * 0.37f;
        const f32 dt = 1.0f / 60.0f + static_cast<f32>(i) * 0.001f;
        live.update(frame, dt);
        live.record(recording, dt);
        moves[i] = live.value(kMove);
        fired[i] = live.fired(kJump);
    }
    CHECK(fired[3] && !fired[2]);
    const char* path = LSD_TEST_TEMP_DIR "/test.inputrec";
    CHECK(save_recording(path, recording));
    InputRecording loaded;
    CHECK(load_recording(path, loaded));
    CHECK(loaded.frame_count() == 6 && loaded.action_count == recording.action_count);
    CHECK(loaded.data == recording.data);  // exact floats
    InputSystem replay;
    replay.add_context(walk);
    replay.configure(kJump, tap);
    for (u32 i = 0; i < loaded.frame_count(); ++i) {
        replay.replay(loaded, i);
        CHECK(replay.value(kMove).x == moves[i].x && replay.value(kMove).y == moves[i].y);
        CHECK(replay.fired(kJump) == fired[i]);
        CHECK(replay.value(kLook).x == static_cast<f32>(i) * 0.37f);
    }
    std::FILE* bad = std::fopen(path, "w");
    std::fputs("lsd_input_recording 1\nactions 1\nshort\n", bad);
    std::fclose(bad);
    CHECK(!load_recording(path, loaded) && loaded.frame_count() == 0);
    std::remove(path);
}

static InputFrame touch_frame(const TouchState& t) {
    InputFrame f;
    t.fill(f);
    return f;
}

static void test_touch() {
    TouchState t;
    t.set_view(1280, 720);
    t.down(1, 0.5f, 0.2f);  // a quick tap
    t.update(0.1f);
    t.up(1, 0.5f, 0.2f);
    t.update(0.016f);
    CHECK(touch_frame(t).touch_bits == kTouchTap);
    t.end_frame();
    CHECK(touch_frame(t).touch_bits == 0 && touch_frame(t).touch_count == 0);

    t.down(1, 0.2f, 0.2f);  // a swipe right, then up
    t.update(0.1f);
    t.up(1, 0.5f, 0.2f);
    CHECK(touch_frame(t).touch_bits == kTouchSwipeRight);
    t.end_frame();
    t.down(1, 0.5f, 0.6f);
    t.up(1, 0.5f, 0.3f);
    CHECK(touch_frame(t).touch_bits == kTouchSwipeUp);
    t.end_frame();

    t.down(1, 0.5f, 0.5f);  // a long press is a hold while it lasts, not a tap
    t.update(0.2f);
    CHECK((touch_frame(t).touch_bits & kTouchHold) == 0);
    t.update(0.3f);
    CHECK((touch_frame(t).touch_bits & kTouchHold) != 0 && touch_frame(t).touch_count == 1);
    t.up(1, 0.5f, 0.5f);
    CHECK((touch_frame(t).touch_bits & kTouchTap) == 0);
    t.end_frame();
    t.update(0.016f);
    CHECK(touch_frame(t).touch_bits == 0);

    t.down(1, 0.4f, 0.2f);  // pinch: two fingers apart
    t.down(2, 0.6f, 0.2f);
    t.update(0.016f);
    t.move(2, 0.7f, 0.2f);
    t.update(0.016f);
    CHECK(touch_frame(t).touch_pinch > 0.1f);
    t.move(2, 0.65f, 0.2f);
    t.update(0.016f);
    CHECK(touch_frame(t).touch_pinch < 0.0f);
    t.up(1, 0.4f, 0.2f);
    t.up(2, 0.65f, 0.2f);
    t.end_frame();

    t.down(1, 0.5f, 0.5f);  // drag: pixels, y up
    t.update(0.016f);
    t.move(1, 0.5f + 10.0f / 1280.0f, 0.5f - 5.0f / 720.0f);
    t.update(0.016f);
    CHECK(std::fabs(touch_frame(t).touch_drag[0] - 10.0f) < 0.01f && std::fabs(touch_frame(t).touch_drag[1] - 5.0f) < 0.01f);
    t.up(1, 0.5f, 0.5f);
    t.end_frame();

    // The pad: a left thumb pushed a full stick radius right reads 1 on the gamepad's left stick; a button press reads as Face Bottom.
    t.set_pad(true);
    t.down(1, 0.15f, 0.75f);
    t.move(1, 0.15f + TouchState::kStickRadius * 720.0f / 1280.0f, 0.75f);
    t.down(2, 0.92f, 0.72f);
    t.update(0.016f);
    InputFrame pad = touch_frame(t);
    CHECK(std::fabs(pad.gamepad_axes[0] - 1.0f) < 0.001f && std::fabs(pad.gamepad_axes[1]) < 0.001f);
    CHECK((pad.gamepad_buttons & (1u << SDL_GAMEPAD_BUTTON_SOUTH)) != 0 && pad.touch_bits == 0);
    t.move(1, 0.15f, 0.75f - 0.5f * TouchState::kStickRadius);  // half way up: y is up-positive
    pad = touch_frame(t);
    CHECK(std::fabs(pad.gamepad_axes[1] - 0.5f) < 0.001f);
    PadWidget widgets[4];
    CHECK(t.pad_widgets(widgets, 4) == 4 && widgets[0].active && widgets[2].active && !widgets[3].active);
    t.up(1, 0.15f, 0.7f);
    t.up(2, 0.92f, 0.72f);
    CHECK(touch_frame(t).gamepad_buttons == 0 && touch_frame(t).gamepad_axes[0] == 0.0f && touch_frame(t).touch_bits == 0);
    t.set_pad(false);
    t.down(1, 0.15f, 0.75f);  // pad off: the same finger is a plain gesture finger
    CHECK(t.pad_widgets(widgets, 4) == 0 && touch_frame(t).gamepad_axes[0] == 0.0f);
}

static void test_touch_mappings() {
    constexpr char kLookGyro[] = "00000000000000000000000000000008";
    constexpr char kSwipe[] = "00000000000000000000000000000009";
    constexpr char kZoom[] = "0000000000000000000000000000000a";
    constexpr char kLean[] = "0000000000000000000000000000000b";
    InputMappingContext ctx;
    ctx.mappings.push_back(map(kLookGyro, "Gyro XY", 0, 2.0f));  // sensitivity is the mapping's scale
    ctx.mappings.push_back(map(kSwipe, "Touch Swipe Up"));
    ctx.mappings.push_back(map(kZoom, "Touch Pinch", 0, 3.0f));
    ctx.mappings.push_back(map(kLean, "Tilt XY"));
    CHECK(is_key("Touch Tap") && is_key("Gyro Yaw") && is_2d_key("Gyro XY") && is_2d_key("Touch Drag XY") && !is_2d_key("Touch Pinch"));
    InputSystem input;
    input.add_context(ctx);
    InputFrame f;
    input.update(f);  // no sensors, no fingers: harmless zeros
    CHECK(input.value(kLookGyro).x == 0.0f && !input.down(kSwipe));
    f.gyro_deg[0] = 1.0f;  // pitch
    f.gyro_deg[1] = 3.0f;  // yaw
    f.touch_bits = kTouchSwipeUp;
    f.touch_pinch = 0.5f;
    f.tilt[0] = 0.25f;
    input.update(f);
    CHECK(input.value(kLookGyro).x == 6.0f && input.value(kLookGyro).y == 2.0f);
    CHECK(input.pressed(kSwipe) && input.fired(kSwipe) && input.value(kZoom).x == 1.5f && input.value(kLean).x == 0.25f);
    f.touch_bits = 0;
    input.update(f);
    CHECK(!input.down(kSwipe));
}

int main() {
    test_mappings();
    test_held_key();
    test_modifiers();
    test_data_asset();
    test_recording();
    test_touch();
    test_touch_mappings();
    if (g_failures == 0) std::printf("All input tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
