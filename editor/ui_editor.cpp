#include "ui_editor.h"

#include "lsd/foundation/log.h"
#include "lsd/rhi/rhi.h"
#include "lsd/ui/ui.h"

#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include <cstdio>
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
         color: #ffffff; font-size: 22px; border-radius: 6px; text-align: center; }
button:hover { background-color: #EEA70E; }
</style>
</head>
<body>
<div id="panel">
  <div id="title">Title</div>
  Score: <span id="score">0</span>
  <!-- onclick calls a Lua function of that name on every script that has one -->
  <button id="play" onclick="start_game">Play</button>
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
    if (w.context->open_text(w.text.c_str(), base.c_str()) < 0) std::snprintf(w.message, sizeof w.message, "The document did not load: see the Output Log");
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

    // Preview (left) and the code (right).
    const f32 preview_width = w.style_sheet ? 0.0f : ImGui::GetContentRegionAvail().x * 0.5f;
    if (!w.style_sheet) {
        ImGui::BeginChild("Preview", ImVec2(preview_width, 0), ImGuiChildFlags_Borders);
        if (!w.target.valid() && s.device) w.target = s.device->create_render_target(kPreviewWidth, kPreviewHeight);
        const f32 width = ImGui::GetContentRegionAvail().x;
        if (w.target.valid()) {
            const bool flipped = s.device->render_targets_flipped();
            ImGui::Image(static_cast<ImTextureID>(s.device->debug_ui_texture(w.target)), ImVec2(width, width * kPreviewHeight / kPreviewWidth), ImVec2(0, flipped ? 1.0f : 0.0f),
                         ImVec2(1, flipped ? 0.0f : 1.0f));
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
            w.dirty = true;
            w.changed_at = SDL_GetTicks() + 300;
        }
        ImGui::EndTabItem();
    }
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
