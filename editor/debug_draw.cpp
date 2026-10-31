#include "debug_draw.h"

#include <imgui.h>

#include <cmath>
#include <string>
#include <vector>

namespace lsd::editor::debug_draw {

namespace {

struct Line {
    Vec3 a, b, color;
    f32 seconds;
};
struct Text {
    Vec3 at, color;
    std::string text;
    f32 seconds;
};
// shortcut: editor-time vectors with a cap; a ring buffer if scripts draw thousands per frame
std::vector<Line> g_lines;
std::vector<Text> g_texts;
constexpr size_t kMaxLines = 20000, kMaxTexts = 512;

ImU32 to_color(Vec3 c) {
    auto byte = [](f32 v) { return static_cast<int>((v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v) * 255.0f + 0.5f); };
    return IM_COL32(byte(c.x), byte(c.y), byte(c.z), 255);
}

bool project(const Mat4& m, Vec3 p, const ImVec2& pos, const ImVec2& size, ImVec2& out) {
    const f32 x = m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3);
    const f32 y = m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3);
    const f32 w = m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3);
    if (w < 0.01f) return false;
    out = ImVec2(pos.x + (x / w * 0.5f + 0.5f) * size.x, pos.y + (0.5f - y / w * 0.5f) * size.y);
    return true;
}

} // namespace

void line(Vec3 a, Vec3 b, Vec3 color, f32 seconds) {
    if (g_lines.size() < kMaxLines) g_lines.push_back({a, b, color, seconds});
}

void sphere(Vec3 c, f32 r, Vec3 color, f32 seconds) {
    constexpr int kSegments = 24;
    for (int axis = 0; axis < 3; ++axis) {  // three circles
        for (int i = 0; i < kSegments; ++i) {
            const f32 a0 = 6.2831853f * static_cast<f32>(i) / kSegments, a1 = 6.2831853f * static_cast<f32>(i + 1) / kSegments;
            auto at = [&](f32 a) {
                const f32 u = std::cos(a) * r, v = std::sin(a) * r;
                return axis == 0 ? c + Vec3{u, v, 0} : axis == 1 ? c + Vec3{u, 0, v} : c + Vec3{0, u, v};
            };
            line(at(a0), at(a1), color, seconds);
        }
    }
}

void box(Vec3 c, Vec3 h, Vec3 color, f32 seconds) {
    Vec3 corner[8];
    for (int i = 0; i < 8; ++i) corner[i] = c + Vec3{(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z};
    const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) line(corner[e[0]], corner[e[1]], color, seconds);
}

void text(Vec3 at, const char* t, Vec3 color, f32 seconds) {
    if (g_texts.size() < kMaxTexts) g_texts.push_back({at, color, t ? t : "", seconds});
}

void tick(f32 dt) {
    auto age = [dt](auto& list) {
        size_t kept = 0;
        for (auto& item : list) {
            item.seconds -= dt;
            if (item.seconds > 0.0f) list[kept++] = std::move(item);
        }
        list.resize(kept);
    };
    age(g_lines);
    age(g_texts);
}

void clear() {
    g_lines.clear();
    g_texts.clear();
}

u32 count() { return static_cast<u32>(g_lines.size() + g_texts.size()); }

void draw(ImDrawList* dl, const Mat4& view_proj, const ImVec2& pos, const ImVec2& size) {
    for (const Line& l : g_lines) {
        ImVec2 a, b;
        if (project(view_proj, l.a, pos, size, a) && project(view_proj, l.b, pos, size, b)) dl->AddLine(a, b, to_color(l.color), 2.0f);
    }
    for (const Text& t : g_texts) {
        ImVec2 p;
        if (!project(view_proj, t.at, pos, size, p)) continue;
        const ImVec2 extent = ImGui::CalcTextSize(t.text.c_str());
        dl->AddText(ImVec2(p.x - extent.x * 0.5f + 1, p.y - extent.y * 0.5f + 1), IM_COL32(0, 0, 0, 200), t.text.c_str());  // readable on anything
        dl->AddText(ImVec2(p.x - extent.x * 0.5f, p.y - extent.y * 0.5f), to_color(t.color), t.text.c_str());
    }
}

} // namespace lsd::editor::debug_draw
