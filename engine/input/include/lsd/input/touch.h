#pragma once
// Touch input (Unreal's touch events + Touch Interface): fingers become tap / hold / swipe / pinch / drag
// gestures and an on-screen pad (two floating thumbsticks + two buttons) that feeds the same values as a gamepad.
// Pure logic: the platform layer sends finger events and calls update() once a frame.
#include "lsd/input/input.h"

namespace lsd::input {

// Where a pad control sits, for drawing it. Coordinates are 0..1 of the window; radius is in window heights.
struct PadWidget {
    f32 x = 0, y = 0, radius = 0;
    f32 knob_x = 0, knob_y = 0;  // the thumb (sticks only)
    bool stick = false, active = false;
};

class TouchState {
public:
    static constexpr u32 kMaxFingers = 10;
    static constexpr f32 kTapSeconds = 0.3f, kHoldSeconds = 0.4f, kSwipeSeconds = 0.6f;
    static constexpr f32 kMoveSlop = 0.03f, kSwipeDistance = 0.1f;  // window heights
    static constexpr f32 kStickRadius = 0.15f, kPinchUnit = 10.0f;  // pinch value 1 = a tenth of the height per frame

    void set_view(f32 width, f32 height) { width_ = width; height_ = height; }  // pixels
    void set_pad(bool on) { pad_on_ = on; }
    void down(u64 id, f32 x, f32 y);  // x, y: 0..1 of the window, y down
    void move(u64 id, f32 x, f32 y);
    void up(u64 id, f32 x, f32 y);
    void update(f32 dt);                 // hold timers, pinch and drag this frame
    void fill(InputFrame& frame) const;  // gestures, finger positions, pad merged into the gamepad values
    void end_frame() { pending_ = 0; }   // after the game has read fill()'s one-frame gestures
    u32 pad_widgets(PadWidget* out, u32 capacity) const;

private:
    struct Finger {
        u64 id;
        f32 x, y, start_x, start_y, prev_x, prev_y, time;
        u32 owner;  // 0 gestures, 1 left stick, 2 right stick, 3 / 4 buttons
    };
    Finger* find(u64 id);
    f32 dist(f32 dx, f32 dy) const;  // in window heights
    f32 aspect() const { return height_ > 0 ? width_ / height_ : 1.0f; }
    u32 claim(f32 x, f32 y) const;
    Finger fingers_[kMaxFingers] = {};
    u32 count_ = 0, pending_ = 0;
    bool pad_on_ = false, hold_ = false;
    f32 width_ = 1280, height_ = 720, pinch_ = 0, pinch_prev_ = -1, drag_x_ = 0, drag_y_ = 0;
};

} // namespace lsd::input
