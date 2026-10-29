#include "lsd/debug_ui/details_panel.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>

namespace lsd {

namespace {

constexpr f32 kLabelWidth = 160.0f;

const DetailsStyle* g_style = nullptr;  // set for the duration of draw_details
u32 g_row = 0;

// Paints the alternating background behind the next property row.
void row_background() {
    if (!g_style) return;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const f32 height = ImGui::GetFrameHeightWithSpacing();
    const f32 right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(ImGui::GetWindowPos().x, pos.y - ImGui::GetStyle().ItemSpacing.y * 0.5f),
                                              ImVec2(right, pos.y + height - ImGui::GetStyle().ItemSpacing.y * 0.5f),
                                              g_style->row_colors[g_row++ & 1]);
}

// "attenuation_radius" -> "Attenuation Radius", as Unreal shows property names.
void display_name(const char* name, char* out, usize capacity) {
    usize n = 0;
    bool word_start = true;
    for (const char* c = name; *c && n + 1 < capacity; ++c) {
        if (*c == '_') { out[n++] = ' '; word_start = true; continue; }
        out[n++] = word_start && *c >= 'a' && *c <= 'z' ? static_cast<char>(*c - 'a' + 'A') : *c;
        word_start = false;
    }
    out[n] = '\0';
}

template <typename T>
bool drag(ImGuiDataType data_type, void* field, const reflect::Property& p, const char* format) {
    const T min = static_cast<T>(p.min), max = static_cast<T>(p.max);
    const bool slider = p.has_min && p.has_max;
    if (slider) return ImGui::SliderScalar("##value", data_type, field, &min, &max, format, ImGuiSliderFlags_AlwaysClamp);
    return ImGui::DragScalar("##value", data_type, field, 0.1f, p.has_min ? &min : nullptr, p.has_max ? &max : nullptr,
                             format, ImGuiSliderFlags_AlwaysClamp);
}

bool draw_fields(const reflect::TypeInfo& type, u8* object, const char* category);

bool draw_property(const reflect::Property& p, u8* object) {
    u8* field = object + p.offset;
    char label[128];
    display_name(p.name, label, sizeof label);
    ImGui::PushID(p.name);
    bool changed = false;

    // Structs flagged Color (r, g, b[, a] floats) get a colour picker.
    if (p.type == reflect::FieldType::Struct && (p.struct_type->flags & reflect::kColor)) {
        row_background();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (p.tooltip[0]) ImGui::SetItemTooltip("%s", p.tooltip);
        ImGui::SameLine(kLabelWidth);
        ImGui::SetNextItemWidth(-1.0f);
        auto* rgba = reinterpret_cast<f32*>(field);
        changed = p.struct_type->property_count >= 4 ? ImGui::ColorEdit4("##value", rgba) : ImGui::ColorEdit3("##value", rgba);
        ImGui::PopID();
        return changed;
    }

    if (p.type == reflect::FieldType::Struct) {
        row_background();
        const bool open = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
        if (p.tooltip[0]) ImGui::SetItemTooltip("%s", p.tooltip);
        if (open) {
            changed = draw_fields(*p.struct_type, field, nullptr);
            ImGui::TreePop();
        }
        ImGui::PopID();
        return changed;
    }

    row_background();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (p.tooltip[0]) ImGui::SetItemTooltip("%s", p.tooltip);
    ImGui::SameLine(kLabelWidth);
    ImGui::SetNextItemWidth(-1.0f);

    const bool read_only = !(p.flags & reflect::kEdit);
    if (read_only) ImGui::BeginDisabled();
    char float_format[32], int_format[32];
    std::snprintf(float_format, sizeof float_format, p.units[0] ? "%%.3f %s" : "%%.3f", p.units);
    std::snprintf(int_format, sizeof int_format, p.units[0] ? "%%d %s" : "%%d", p.units);
    char u64_format[32];
    std::snprintf(u64_format, sizeof u64_format, p.units[0] ? "%%llu %s" : "%%llu", p.units);
    switch (p.type) {
        case reflect::FieldType::Bool: changed = ImGui::Checkbox("##value", reinterpret_cast<bool*>(field)); break;
        case reflect::FieldType::I32:  changed = drag<i32>(ImGuiDataType_S32, field, p, int_format); break;
        case reflect::FieldType::U32:  changed = drag<u32>(ImGuiDataType_U32, field, p, int_format); break;
        case reflect::FieldType::I64:  changed = drag<i64>(ImGuiDataType_S64, field, p, int_format); break;
        case reflect::FieldType::U64:  changed = drag<u64>(ImGuiDataType_U64, field, p, u64_format); break;
        case reflect::FieldType::F32:  changed = drag<f32>(ImGuiDataType_Float, field, p, float_format); break;
        case reflect::FieldType::F64:  changed = drag<f64>(ImGuiDataType_Double, field, p, float_format); break;
        case reflect::FieldType::String:
            changed = ImGui::InputText("##value", reinterpret_cast<char*>(field), p.size, read_only ? ImGuiInputTextFlags_ReadOnly : 0);
            break;
        case reflect::FieldType::Struct: break;
    }
    if (read_only) ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

// Draws properties in `category` (nullptr = all) that the Details panel should show.
bool draw_fields(const reflect::TypeInfo& type, u8* object, const char* category) {
    bool changed = false;
    for (u32 i = 0; i < type.property_count; ++i) {
        const reflect::Property& p = type.properties[i];
        if (!(p.flags & (reflect::kEdit | reflect::kVisible))) continue;
        if (category && std::strcmp(p.category, category) != 0) continue;
        changed |= draw_property(p, object);
    }
    return changed;
}

} // namespace

bool draw_details(const reflect::TypeInfo& type, void* object, const DetailsStyle* style) {
    g_style = style;
    g_row = 0;
    bool changed = false;
    // One collapsible section per category, in the order categories first appear.
    for (u32 i = 0; i < type.property_count; ++i) {
        const char* category = type.properties[i].category;
        bool seen = false;
        for (u32 j = 0; j < i; ++j) seen = seen || std::strcmp(type.properties[j].category, category) == 0;
        if (seen) continue;
        if (ImGui::CollapsingHeader(category[0] ? category : "Default", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= draw_fields(type, static_cast<u8*>(object), category);
        }
    }
    g_style = nullptr;
    return changed;
}

} // namespace lsd
