#include "lsd/debug_ui/details_panel.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>

namespace lsd {

namespace {

constexpr f32 kLabelWidth = 160.0f;

const DetailsStyle* g_style = nullptr;  // set for the duration of draw_details
u32 g_row = 0;

// Copy / Paste between properties of the same kind.
// shortcut: an in-editor clipboard of raw bytes; switch to text on the system clipboard when values
// need to go between editors or into documents.
u8 g_clip[512];
u32 g_clip_size = 0;
reflect::FieldType g_clip_type = reflect::FieldType::Bool;
const reflect::TypeInfo* g_clip_struct = nullptr;

bool contains_ignore_case(const char* text, const char* needle) {
    for (; *text; ++text) {
        const char* a = text;
        const char* b = needle;
        while (*a && *b && (*a | 32) == (*b | 32)) { ++a; ++b; }
        if (!*b) return true;
    }
    return !*needle;
}

// Right-click menu on the value just drawn. Returns true if it changed the value.
bool property_menu(const reflect::Property& p, u8* field, const u8* default_field) {
    if (!(p.flags & reflect::kEdit) || !ImGui::BeginPopupContextItem("property_menu")) return false;
    bool changed = false;
    const bool differs = default_field && std::memcmp(field, default_field, p.size) != 0;
    if (ImGui::MenuItem("Reset to Default", nullptr, false, differs)) {
        std::memcpy(field, default_field, p.size);
        changed = true;
    }
    if (ImGui::MenuItem("Copy", nullptr, false, p.size <= sizeof g_clip)) {
        std::memcpy(g_clip, field, p.size);
        g_clip_size = p.size;
        g_clip_type = p.type;
        g_clip_struct = p.struct_type;
    }
    const bool fits = g_clip_size == p.size && g_clip_type == p.type && g_clip_struct == p.struct_type;
    if (ImGui::MenuItem("Paste", nullptr, false, fits)) {
        std::memcpy(field, g_clip, p.size);
        if (p.type == reflect::FieldType::String) field[p.size - 1] = '\0';
        changed = true;
    }
    ImGui::EndPopup();
    return changed;
}

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

bool draw_fields(const reflect::TypeInfo& type, u8* object, const u8* defaults, const char* category);

// `defaults` is the default object the property belongs to (nullptr if unknown).
bool draw_property(const reflect::Property& p, u8* object, const u8* defaults) {
    u8* field = object + p.offset;
    const u8* default_field = defaults ? defaults + p.offset : nullptr;
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
        changed |= property_menu(p, field, default_field);
        ImGui::PopID();
        return changed;
    }

    // Structs flagged Vector (x, y, z floats) edit on one row.
    if (p.type == reflect::FieldType::Struct && (p.struct_type->flags & reflect::kVector)) {
        row_background();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (p.tooltip[0]) ImGui::SetItemTooltip("%s", p.tooltip);
        ImGui::SameLine(kLabelWidth);
        ImGui::SetNextItemWidth(-1.0f);
        char format[32];
        std::snprintf(format, sizeof format, p.units[0] ? "%%.3f %s" : "%%.3f", p.units);
        const bool read_only = !(p.flags & reflect::kEdit);
        if (read_only) ImGui::BeginDisabled();
        changed = ImGui::DragFloat3("##value", reinterpret_cast<f32*>(field), 0.05f, 0.0f, 0.0f, format);
        if (read_only) ImGui::EndDisabled();
        changed |= property_menu(p, field, default_field);
        ImGui::PopID();
        return changed;
    }

    if (p.type == reflect::FieldType::Struct) {
        row_background();
        const bool open = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
        if (p.tooltip[0]) ImGui::SetItemTooltip("%s", p.tooltip);
        if (open) {
            changed = draw_fields(*p.struct_type, field, default_field, nullptr);
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
    changed |= property_menu(p, field, default_field);
    ImGui::PopID();
    return changed;
}

// Draws properties in `category` (nullptr = all) that the Details panel should show.
bool draw_fields(const reflect::TypeInfo& type, u8* object, const u8* defaults, const char* category) {
    bool changed = false;
    for (u32 i = 0; i < type.property_count; ++i) {
        const reflect::Property& p = type.properties[i];
        if (!(p.flags & (reflect::kEdit | reflect::kVisible))) continue;
        if (category && std::strcmp(p.category, category) != 0) continue;
        if (g_style && g_style->filter && g_style->filter[0] &&
            (p.type != reflect::FieldType::Struct || (p.struct_type->flags & (reflect::kVector | reflect::kColor)))) {
            char label[128];
            display_name(p.name, label, sizeof label);
            if (!contains_ignore_case(label, g_style->filter)) continue;
        }
        changed |= draw_property(p, object, defaults);
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
            changed |= draw_fields(type, static_cast<u8*>(object), static_cast<const u8*>(type.defaults), category);
        }
    }
    g_style = nullptr;
    return changed;
}

bool draw_properties(const reflect::TypeInfo& type, void* object, const DetailsStyle* style) {
    g_style = style;
    g_row = 0;
    const bool changed = draw_fields(type, static_cast<u8*>(object), static_cast<const u8*>(type.defaults), nullptr);
    g_style = nullptr;
    return changed;
}

} // namespace lsd
