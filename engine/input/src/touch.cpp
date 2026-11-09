#include "lsd/input/touch.h"

#include <SDL3/SDL.h>

#include <cmath>

namespace lsd::input {

namespace {
// Pad layout (0..1 of the window): buttons on the right, sticks float where the thumb lands in each lower half.
constexpr f32 kButtonX[2] = {0.92f, 0.82f}, kButtonY[2] = {0.72f, 0.86f}, kButtonRadius = 0.07f;
constexpr f32 kStickZoneY = 0.35f;
} // namespace

TouchState::Finger* TouchState::find(u64 id) {
    for (u32 i = 0; i < count_; ++i) {
        if (fingers_[i].id == id) return &fingers_[i];
    }
    return nullptr;
}

f32 TouchState::dist(f32 dx, f32 dy) const {
    const f32 sx = dx * aspect();
    return std::sqrt(sx * sx + dy * dy);
}

u32 TouchState::claim(f32 x, f32 y) const {
    if (!pad_on_) return 0;
    for (u32 b = 0; b < 2; ++b) {
        if (dist(x - kButtonX[b], y - kButtonY[b]) <= kButtonRadius) return 3 + b;
    }
    if (y < kStickZoneY) return 0;
    const u32 owner = x < 0.4f ? 1 : (x > 0.55f ? 2 : 0);
    for (u32 i = 0; i < count_; ++i) {
        if (fingers_[i].owner == owner) return 0;  // one thumb per stick
    }
    return owner;
}

void TouchState::down(u64 id, f32 x, f32 y) {
    if (find(id) || count_ == kMaxFingers) return;
    fingers_[count_++] = {id, x, y, x, y, x, y, 0.0f, claim(x, y)};
}

void TouchState::move(u64 id, f32 x, f32 y) {
    if (Finger* f = find(id)) {
        f->x = x;
        f->y = y;
    }
}

void TouchState::up(u64 id, f32 x, f32 y) {
    Finger* f = find(id);
    if (!f) return;
    f->x = x;
    f->y = y;
    const f32 dx = x - f->start_x, dy = y - f->start_y, d = dist(dx, dy);
    if (f->owner == 0) {
        if (d <= kMoveSlop && f->time <= kTapSeconds) {
            pending_ |= kTouchTap;
        } else if (d >= kSwipeDistance && f->time <= kSwipeSeconds) {
            const f32 ax = std::fabs(dx) * aspect(), ay = std::fabs(dy);
            pending_ |= ax > ay ? (dx > 0 ? kTouchSwipeRight : kTouchSwipeLeft) : (dy > 0 ? kTouchSwipeDown : kTouchSwipeUp);
        }
    }
    *f = fingers_[--count_];  // order is not kept; gestures never depend on it
}

void TouchState::update(f32 dt) {
    drag_x_ = drag_y_ = 0.0f;
    const Finger* gesture[2] = {};
    u32 n = 0;
    for (u32 i = 0; i < count_; ++i) {
        Finger& f = fingers_[i];
        f.time += dt;
        if (f.owner != 0) continue;
        if (n == 0) {  // the first free finger drags, in pixels with y up like the mouse
            drag_x_ = (f.x - f.prev_x) * width_;
            drag_y_ = -(f.y - f.prev_y) * height_;
        }
        if (n < 2) gesture[n] = &f;
        ++n;
        f.prev_x = f.x;
        f.prev_y = f.y;
    }
    hold_ = n == 1 && gesture[0]->time >= kHoldSeconds && dist(gesture[0]->x - gesture[0]->start_x, gesture[0]->y - gesture[0]->start_y) <= kMoveSlop;
    const f32 d = n >= 2 ? dist(gesture[0]->x - gesture[1]->x, gesture[0]->y - gesture[1]->y) : -1.0f;
    pinch_ = (d >= 0 && pinch_prev_ >= 0) ? (d - pinch_prev_) * kPinchUnit : 0.0f;
    pinch_prev_ = d;
}

void TouchState::fill(InputFrame& frame) const {
    frame.touch_bits |= pending_ | (hold_ ? kTouchHold : 0u);
    frame.touch_pinch = pinch_;
    frame.touch_drag[0] = drag_x_;
    frame.touch_drag[1] = drag_y_;
    frame.touch_count = count_;
    for (u32 i = 0; i < count_; ++i) {
        frame.touch_xy[i][0] = fingers_[i].x;
        frame.touch_xy[i][1] = fingers_[i].y;
    }
    for (u32 i = 0; i < count_; ++i) {  // pad: the same slots a gamepad fills, the larger value wins
        const Finger& f = fingers_[i];
        if (f.owner == 0) continue;
        if (f.owner >= 3) {
            frame.gamepad_buttons |= 1u << (f.owner == 3 ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST);
            continue;
        }
        f32 x = (f.x - f.start_x) * aspect() / kStickRadius, y = -(f.y - f.start_y) / kStickRadius;
        const f32 len = std::sqrt(x * x + y * y);
        if (len > 1.0f) {
            x /= len;
            y /= len;
        }
        const u32 axis = f.owner == 1 ? 0 : 2;
        if (std::fabs(x) > std::fabs(frame.gamepad_axes[axis])) frame.gamepad_axes[axis] = x;
        if (std::fabs(y) > std::fabs(frame.gamepad_axes[axis + 1])) frame.gamepad_axes[axis + 1] = y;
    }
}

u32 TouchState::pad_widgets(PadWidget* out, u32 capacity) const {
    if (!pad_on_ || capacity < 4) return 0;
    for (u32 s = 0; s < 2; ++s) {  // resting sticks sit at a default spot
        out[s] = {s == 0 ? 0.15f : 0.85f, 0.75f, kStickRadius, 0, 0, true, false};
        out[s].knob_x = out[s].x;
        out[s].knob_y = out[s].y;
        for (u32 i = 0; i < count_; ++i) {
            const Finger& f = fingers_[i];
            if (f.owner != s + 1) continue;
            out[s].x = out[s].knob_x = f.start_x;
            out[s].y = out[s].knob_y = f.start_y;
            out[s].active = true;
            f32 dx = (f.x - f.start_x) * aspect(), dy = f.y - f.start_y;
            const f32 len = std::sqrt(dx * dx + dy * dy);
            if (len > kStickRadius) {
                dx *= kStickRadius / len;
                dy *= kStickRadius / len;
            }
            out[s].knob_x += dx / aspect();
            out[s].knob_y += dy;
        }
    }
    for (u32 b = 0; b < 2; ++b) {
        out[2 + b] = {kButtonX[b], kButtonY[b], kButtonRadius, 0, 0, false, false};
        for (u32 i = 0; i < count_; ++i) out[2 + b].active = out[2 + b].active || fingers_[i].owner == 3 + b;
    }
    return 4;
}

} // namespace lsd::input
