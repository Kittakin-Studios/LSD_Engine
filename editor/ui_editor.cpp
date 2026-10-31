#include "ui_editor.h"

#include "rml_edit.h"

#include "lsd/foundation/log.h"
#include "lsd/rhi/rhi.h"
#include "lsd/ui/ui.h"

#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace lsd::editor {

namespace {

constexpr u32 kPreviewWidth = 1280, kPreviewHeight = 720;

constexpr char kStarter[] = R"(<rml>
<head>
<title>New UI</title>
<style>
body { font-family: sans; width: 100%; height: 100%; }
div { display: block; }
#panel { position: absolute; left: 40px; top: 40px; width: 380px; padding: 18px 24px;
         background-color: rgba(13, 4, 4, 200); border: 2px #F3C312; border-radius: 10px;
         color: #FFE51A; font-size: 26px; }
#title { font-size: 38px; font-weight: bold; color: #ffffff; margin-bottom: 8px; }
button { display: block; margin-top: 14px; padding: 8px 20px; background-color: #025291;
         color: #ffffff; font-size: 22px; border-radius: 6px; text-align: center;
         nav: auto; tab-index: auto; focus: auto; }   /* arrows / D-pad move between buttons */
button:hover, button:focus { background-color: #EEA70E; }
</style>
</head>
<body>
<div id="panel">
  <div id="title">Title</div>
  Score: <span id="score">0</span>
  <!-- onclick calls a Lua function of that name on every script that has one -->
  <button id="play" onclick="start_game" autofocus>Play</button>
  <button id="quit" onclick="quit_game">Quit</button>
</div>
</body>
</rml>
)";

struct UiWindow {
    bool open = false;
    bool dirty = false;
    bool style_sheet = false;  // .rcss: text only (it previews through the documents that link it)
    char name[64] = "";
    char path[300] = "";
    std::string text;
    std::string previewed;     // the text the preview shows
    u64 changed_at = 0;        // ms; 0: preview up to date
    ui::UiContext* context = nullptr;
    rhi::TextureHandle target;
    rhi::Device* device = nullptr;
    char message[200] = "";
    // Designer canvas
    ui::DocumentId document = -1;
    std::string selected;          // element id
    bool dragging = false, moved = false;
    f32 drag_from[2] = {};         // preview pixels
    f32 start_left = 0, start_top = 0;
    bool make_relative = false;    // the element was in the normal flow: dragging offsets it (position: relative)
    bool show_details = false;     // switch to the Details tab once
};
UiWindow g_ui;

int resize_callback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<usize>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

void save(EditorState& s) {
    UiWindow& w = g_ui;
    std::ofstream file(w.path, std::ios::binary);
    if (!file) {
        std::snprintf(w.message, sizeof w.message, "Could not save %s", w.path);
        return;
    }
    file << w.text;
    w.dirty = false;
    std::snprintf(w.message, sizeof w.message, "Saved: the game uses it from the next Play");
    std::snprintf(s.status, sizeof s.status, "%s: saved", w.name);
}

void refresh_preview() {
    UiWindow& w = g_ui;
    if (!w.context || w.style_sheet) return;
    w.context->close_all();
    const std::string base = std::filesystem::path(w.path).generic_string();  // links resolve next to the file
    w.document = w.context->open_text(w.text.c_str(), base.c_str());
    if (w.document < 0) std::snprintf(w.message, sizeof w.message, "The document did not load: see the Output Log");
    else if (std::strncmp(w.message, "The document", 12) == 0) w.message[0] = '\0';
    w.previewed = w.text;
}

} // namespace

std::string create_ui_document(const char* folder_path, const char* base) {
    const std::filesystem::path folder(folder_path);
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::filesystem::path path = folder / (std::string(base) + ".rml");
    for (int n = 2; std::filesystem::exists(path, error); ++n) path = folder / (std::string(base) + std::to_string(n) + ".rml");
    std::ofstream file(path, std::ios::binary);
    if (!file) return {};
    file << kStarter;
    return path.generic_string();
}

void open_ui_editor(EditorState& s, const asset::AssetRecord& record) {
    UiWindow& w = g_ui;
    if (w.open && w.dirty) save(s);
    s.registry->source_path(record, w.path, sizeof w.path);
    std::ifstream file(w.path, std::ios::binary);
    if (!file) {
        std::snprintf(s.status, sizeof s.status, "Could not open %s", record.path);
        return;
    }
    std::stringstream text;
    text << file.rdbuf();
    w.text = text.str();
    std::snprintf(w.name, sizeof w.name, "%s", record.name);
    w.style_sheet = record.type == asset::AssetType::UiStyle;
    w.open = true;
    w.dirty = false;
    w.previewed.clear();
    w.changed_at = 1;  // preview at once
    w.message[0] = '\0';
    if (!w.context) w.context = new ui::UiContext("preview");
    ImGui::SetWindowFocus("###UiEditor");
}

namespace {

f32 pixels_of(const char* value) { return static_cast<f32>(std::atof(value)); }  // "40px" -> 40; "auto" -> 0

void edited(UiWindow& w) {
    w.dirty = true;
    w.changed_at = SDL_GetTicks() + 300;
}

void select_element(UiWindow& w, const std::string& id) {
    w.selected = id;
    w.show_details = !id.empty();
}

// The palette: a new element at the end of <body>, positioned where it can be dragged from.
void add_element(UiWindow& w, const char* kind) {
    const std::string kind_name = kind;
    std::string id, markup;
    if (kind_name == "Text") {
        id = rml_unique_id(w.text, "text");
        markup = "<div id=\"" + id + "\" style=\"position: absolute; left: 80px; top: 80px; font-size: 32px; color: #ffffff;\">Text</div>";
    } else if (kind_name == "Button") {
        id = rml_unique_id(w.text, "button");
        markup = "<button id=\"" + id + "\" onclick=\"" + id + "_clicked\" style=\"position: absolute; left: 80px; top: 140px; width: 200px;\">Button</button>";
    } else if (kind_name == "Image") {
        id = rml_unique_id(w.text, "image");
        markup = "<div id=\"" + id + "\" style=\"position: absolute; left: 80px; top: 200px; width: 128px; height: 128px; background-color: #596CC1;\"></div>"
                 "<!-- an image: decorator: image(../Textures/MyImage.png); in its style -->";
    } else {
        id = rml_unique_id(w.text, "panel");
        markup = "<div id=\"" + id + "\" style=\"position: absolute; left: 80px; top: 80px; width: 360px; height: 220px; background-color: rgba(13, 4, 4, 200); "
                 "border: 2px #F3C312; border-radius: 10px;\"></div>";
    }
    if (!rml_append_to_body(w.text, markup)) {
        std::snprintf(w.message, sizeof w.message, "The document has no </body> to add to");
        return;
    }
    edited(w);
    select_element(w, id);
}

// The preview with its selection: click to select, drag to move (writes left / top into the text).
void draw_canvas(UiWindow& w, ImVec2 image_min, f32 image_width) {
    if (!w.context || w.document < 0) return;
    const f32 scale = static_cast<f32>(kPreviewWidth) / image_width;  // preview pixels per screen point
    const ImVec2 mouse = ImGui::GetMousePos();
    const f32 px = (mouse.x - image_min.x) * scale, py = (mouse.y - image_min.y) * scale;
    const bool hovered = ImGui::IsItemHovered();
    char hover_id[64] = "";
    if (hovered && !w.dragging) w.context->element_at(px, py, hover_id, sizeof hover_id);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        select_element(w, hover_id);
        if (hover_id[0]) {
            char value[64] = "";
            w.context->get_property(w.document, hover_id, "position", value, sizeof value);
            w.make_relative = std::strcmp(value, "static") == 0;
            w.start_left = w.context->get_property(w.document, hover_id, "left", value, sizeof value) ? pixels_of(value) : 0.0f;
            w.start_top = w.context->get_property(w.document, hover_id, "top", value, sizeof value) ? pixels_of(value) : 0.0f;
            w.drag_from[0] = px;
            w.drag_from[1] = py;
            w.dragging = true;
            w.moved = false;
        }
    }
    if (w.dragging) {
        const f32 step = ImGui::GetIO().KeyCtrl ? 10.0f : 1.0f;  // Ctrl: snap to 10 px
        const f32 left = std::round((w.start_left + px - w.drag_from[0]) / step) * step;
        const f32 top = std::round((w.start_top + py - w.drag_from[1]) / step) * step;
        char left_text[32], top_text[32];
        std::snprintf(left_text, sizeof left_text, "%gpx", static_cast<double>(left));
        std::snprintf(top_text, sizeof top_text, "%gpx", static_cast<double>(top));
        if (std::fabs(px - w.drag_from[0]) + std::fabs(py - w.drag_from[1]) > 3.0f) w.moved = true;
        if (w.moved && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {  // live, on the preview only
            if (w.make_relative) w.context->set_property(w.document, w.selected.c_str(), "position", "relative");
            w.context->set_property(w.document, w.selected.c_str(), "left", left_text);
            w.context->set_property(w.document, w.selected.c_str(), "top", top_text);
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {  // dropped: into the text
            w.dragging = false;
            if (w.moved) {
                if (w.make_relative) rml_set_style(w.text, w.selected.c_str(), "position", "relative");
                rml_set_style(w.text, w.selected.c_str(), "left", left_text);
                rml_set_style(w.text, w.selected.c_str(), "top", top_text);
                w.dirty = true;
                w.previewed = w.text;  // the preview already shows it
            }
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto outline = [&](const char* id, ImU32 color, f32 thickness) {
        f32 box[4];
        if (!id[0] || !w.context->element_box(w.document, id, box)) return;
        const ImVec2 a(image_min.x + box[0] / scale, image_min.y + box[1] / scale);
        dl->AddRect(a, ImVec2(a.x + box[2] / scale, a.y + box[3] / scale), color, 0.0f, 0, thickness);
    };
    if (hover_id[0] && w.selected != hover_id) outline(hover_id, IM_COL32(155, 225, 31, 200), 1.0f);
    outline(w.selected.c_str(), IM_COL32(243, 195, 18, 255), 2.0f);
}

// Details for the selected element: its text, click action and common style properties, written
// straight into the document text.
void draw_element_details(UiWindow& w) {
    if (w.selected.empty()) {
        ImGui::TextDisabled("Click an element in the preview (it needs an id), or add one with the buttons above it.");
        return;
    }
    size_t begin, end;
    if (!rml_find_tag(w.text, w.selected.c_str(), begin, end)) {
        ImGui::TextDisabled("'%s' is no longer in the document.", w.selected.c_str());
        return;
    }
    ImGui::Text("Element: %s", w.selected.c_str());
    char buffer[256];
    std::string text;
    if (rml_text(w.text, w.selected.c_str(), text)) {
        std::snprintf(buffer, sizeof buffer, "%s", text.c_str());
        if (ImGui::InputText("Text", buffer, sizeof buffer)) {
            rml_set_text(w.text, w.selected.c_str(), buffer);
            edited(w);
        }
    }
    std::snprintf(buffer, sizeof buffer, "%s", rml_attribute(w.text, w.selected.c_str(), "onclick").c_str());
    if (ImGui::InputText("On Click", buffer, sizeof buffer)) {
        rml_set_attribute(w.text, w.selected.c_str(), "onclick", buffer);
        edited(w);
    }
    ImGui::SetItemTooltip("The Lua function called when it is clicked: function name(self, element_id) in any script");
    ImGui::SeparatorText("Style (inline; empty = from the style sheet)");
    const char* properties[] = {"position", "left", "top", "width", "height", "color", "background-color", "font-size", "font-weight",
                                "text-align", "padding", "margin", "border", "border-radius", "opacity", "display"};
    for (const char* property : properties) {
        std::snprintf(buffer, sizeof buffer, "%s", rml_style(w.text, w.selected.c_str(), property).c_str());
        if (ImGui::InputText(property, buffer, sizeof buffer)) {
            rml_set_style(w.text, w.selected.c_str(), property, buffer);
            edited(w);
        }
    }
    if (ImGui::Button("Delete Element")) {
        rml_remove(w.text, w.selected.c_str());
        w.selected.clear();
        edited(w);
    }
}

} // namespace

void draw_ui_editor(EditorState& s) {
    UiWindow& w = g_ui;
    if (!w.open) return;
    w.device = s.device;
    char title[128];
    std::snprintf(title, sizeof title, "%s%s - %s###UiEditor", w.name, w.dirty ? "*" : "", w.style_sheet ? "UI Style" : "UI Document");
    ImGui::SetNextWindowSize(ImVec2(1200, 640), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin(title, &open)) {
        ImGui::End();
        return;
    }
    if (!open) {
        if (w.dirty) save(s);
        w.open = false;
    }
    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))) save(s);
    ImGui::SameLine();
    if (w.message[0]) ImGui::TextUnformatted(w.message);
    else ImGui::TextDisabled("HTML / CSS (RmlUi). onclick=\"name\" calls the Lua function name(self, element_id). Fonts: family sans, or a .ttf in Content/Fonts");

    // Designer (left: palette and the preview canvas) and the code / details (right).
    const f32 preview_width = w.style_sheet ? 0.0f : ImGui::GetContentRegionAvail().x * 0.5f;
    if (!w.style_sheet) {
        ImGui::BeginChild("Preview", ImVec2(preview_width, 0), ImGuiChildFlags_Borders);
        for (const char* kind : {"Text", "Button", "Image", "Panel"}) {
            char label[32];
            std::snprintf(label, sizeof label, "+ %s", kind);
            if (ImGui::Button(label)) add_element(w, kind);
            ImGui::SameLine();
        }
        ImGui::TextDisabled("Click to select, drag to move (Ctrl: 10 px steps), Delete removes");
        if (!w.target.valid() && s.device) w.target = s.device->create_render_target(kPreviewWidth, kPreviewHeight);
        const f32 width = ImGui::GetContentRegionAvail().x;
        if (w.target.valid()) {
            const bool flipped = s.device->render_targets_flipped();
            const ImVec2 image_min = ImGui::GetCursorScreenPos();
            ImGui::Image(static_cast<ImTextureID>(s.device->debug_ui_texture(w.target)), ImVec2(width, width * kPreviewHeight / kPreviewWidth), ImVec2(0, flipped ? 1.0f : 0.0f),
                         ImVec2(1, flipped ? 0.0f : 1.0f));
            draw_canvas(w, image_min, width);
        }
        if (!w.selected.empty() && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            rml_remove(w.text, w.selected.c_str());
            w.selected.clear();
            edited(w);
        }
        ImGui::TextDisabled("Preview at %u x %u, live as you type", kPreviewWidth, kPreviewHeight);
        ImGui::EndChild();
        ImGui::SameLine();
    }
    ImGui::BeginChild("Code", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::BeginTabBar("CodeTabs");
    if (ImGui::BeginTabItem(w.style_sheet ? "RCSS" : "Code (RML + style)")) {
        if (w.text.capacity() < w.text.size() + 1) w.text.reserve(w.text.size() + 1);
        if (ImGui::InputTextMultiline("##code", w.text.data(), w.text.capacity() + 1, ImVec2(-1, -1), ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput,
                                      resize_callback, &w.text)) {
            edited(w);
        }
        ImGui::EndTabItem();
    }
    if (!w.style_sheet && ImGui::BeginTabItem("Details", nullptr, w.show_details ? ImGuiTabItemFlags_SetSelected : 0)) {
        draw_element_details(w);
        ImGui::EndTabItem();
    }
    w.show_details = false;
    ImGui::EndTabBar();
    ImGui::EndChild();
    ImGui::End();
    if (w.changed_at && SDL_GetTicks() >= w.changed_at && w.text != w.previewed) {
        w.changed_at = 0;
        refresh_preview();
    }
}

void render_ui_editor(rhi::Device& device, const f32 background[4]) {
    UiWindow& w = g_ui;
    if (!w.open || w.style_sheet || !w.context || !w.target.valid()) return;
    const f32 dark[4] = {background[0] * 0.6f, background[1] * 0.6f, background[2] * 0.6f, 1.0f};
    device.set_render_target(w.target, dark);
    const f32 viewport[4] = {0, 0, static_cast<f32>(kPreviewWidth), static_cast<f32>(kPreviewHeight)};
    w.context->update(viewport);
    w.context->render(device);
    device.set_render_target({}, nullptr);
}

void shutdown_ui_editor() {
    UiWindow& w = g_ui;
    delete w.context;
    w.context = nullptr;
    if (w.device && w.target.valid()) w.device->destroy_texture(w.target);
    w.target = {};
    w.open = false;
}

} // namespace lsd::editor
