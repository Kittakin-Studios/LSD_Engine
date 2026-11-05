#include "assistant.h"
#include "assistant_core.h"
#include "tips.h"

#include "lsd/reflect/reflect.h"
#include "lsd/world/components.h"
#include "lsd/world/world.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace lsd::editor::assistant {

namespace fs = std::filesystem;

namespace {

// The studio: who answers. Each adds its focus to the shared engine guide.
struct Role {
    const char* name;
    const char* focus;
};
constexpr Role kRoles[] = {
    {"Engine Expert", "how LSD_Engine works: its editors, assets and import, rendering, physics, animation, audio, networking, packaging; "
                      "you map the developer's Unreal habits onto LSD_Engine's tools and say where each thing is."},
    {"Gameplay Programmer", "gameplay code: you write complete, working Lua, JavaScript or C++ scripts against the lsd API below, explain how "
                            "to attach them, and debug errors from the Output Log."},
    {"Technical Artist", "materials and material graphs, textures, lighting, post-processing, animation setup (rigs, Anim Graphs, Motion "
                         "Matching), particles, and performance budgets."},
    {"Level Designer", "level layout, blockout with basic shapes, pacing, lighting moods, navigation, encounters, and placing content in the editor."},
    {"Game Designer", "mechanics, systems, progression, balance, player experience; you write short, concrete design notes and suggest how to prototype them."},
    {"Producer", "planning: you break goals into milestones and small tasks in order, flag risks and scope, and keep plans realistic for a small team."},
    {"QA Tester", "testing: you write test plans and edge cases, help reproduce bugs step by step, and turn findings into clear bug reports."},
};
constexpr int kRoleCount = static_cast<int>(sizeof kRoles / sizeof kRoles[0]);

// The engine guide and the Lua API (script.h's own comment block), read once.
std::string engine_guide(const Role& role) {
    static std::string api;
    if (api.empty()) {
        std::ifstream header("engine/script/include/lsd/script/script.h");
        std::string line;
        while (std::getline(header, line)) {
            if (line.rfind("#pragma", 0) == 0) continue;
            if (line.rfind("//", 0) != 0 && !line.empty()) break;  // the end of the header's comment block
            api += line.size() > 2 ? line.substr(3) : std::string();
            api += '\n';
        }
        if (api.empty()) api = "(engine/script/include/lsd/script/script.h was not found: ask before relying on an lsd.* call you are not sure of)\n";
    }
    return std::string("You are LSD Assistant, the assistant built into the LSD_Engine editor. LSD_Engine is a proprietary 3D game engine "
                       "by Kittakin Studios with an editor-first workflow like Unreal Engine: a Content Browser, Outliner, Details, visual "
                       "scripts (like Blueprints), Material Graphs, Animation Graphs, particles, game UI in HTML / CSS, multiplayer and "
                       "packaging. The developer works with you as with a small game studio; right now you answer as its ") +
           role.name + ", whose focus is " + role.focus +
           "\n\nEngine facts: Z is up, right-handed, in metres; rotations are Euler degrees (X, then Y, then Z). Gameplay scripts are .lua "
           "(LuaJIT) or .js (QuickJS) files in Content/, run by a Script component on an object, with events on_start(self), "
           "on_update(self, dt), and others listed below; `self.entity` is the object, keep state in `self`; a `properties = { speed = 5 }` "
           "table shows in the object's Details. Visual scripts (.lsdgraph) compile to Lua. C / C++ gameplay lives in Source/ and builds into "
           "Game.dll (Tools > Compile Game Code). Data assets (.lsdasset) hold Materials, Input Actions, Anim Graphs, Motion Databases and more. "
           "When the developer names an Unreal feature, map it to LSD_Engine's.\n\n"
           "How to answer: be concise and concrete. When the developer must click something, give the steps (which panel, which menu, which "
           "button). When you write a file, put the whole file in one fenced code block whose first line gives the language and the path, "
           "for example ```lua Content/Script/Door.lua - the editor shows Create File and Attach to Selected buttons under it. Paths must be "
           "inside Content/ or Source/. Never invent engine API: if a call is not in the reference below, say so and suggest another way. "
           "Each message starts with an <editor> block describing the open project (level, selected object, assets); use it, do not repeat it.\n\n"
           "Lua API reference (the engine's own documentation):\n" +
           api;
}

// The project as the assistant sees it: the level, the selected object with its components, the assets.
std::string project_context(EditorState& s) {
    std::string c = "<editor>\nLevel: ";
    c += s.level_path[0] ? s.level_path : "(unsaved)";
    c += "\n";
    if (s.world && s.world->alive(s.selected)) {
        c += "Selected object: ";
        c += s.world->name(s.selected);
        c += "\n";
        u32 count = 0;
        const reflect::TypeInfo* const* types = world::component_types(count);
        static char text[16384];
        for (u32 i = 0; i < count; ++i) {
            const void* component = s.world->get(s.selected, *types[i]);
            if (!component) continue;
            const usize n = reflect::save_text(*types[i], component, text, sizeof text);
            c += "  ";
            c += types[i]->name;
            c += ": ";
            c.append(text, n < sizeof text ? n : sizeof text - 1);
            c += "\n";
        }
    } else {
        c += "Selected object: none\n";
    }
    if (s.registry) {
        const usize total = s.registry->count();
        c += "Assets in Content/ (" + std::to_string(total) + "):\n";
        for (usize i = 0; i < total && i < 300; ++i) {
            const asset::AssetRecord& r = s.registry->get(i);
            c += "  " + std::string(r.path) + " [" + asset::to_string(r.type) + (r.data_type[0] ? std::string(" ") + r.data_type : std::string()) + "]\n";
        }
        if (total > 300) c += "  (and " + std::to_string(total - 300) + " more)\n";
    }
    return c + "</editor>\n\n";
}

// A request running on its own thread (the editor keeps drawing): curl does the HTTPS.
struct Job {
    std::atomic<bool> done{false};
    Reply reply;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

// Runs curl with the headers and body in files (the key never appears on a command line) and the
// reply to a file; returns the HTTP status (0: curl could not run or reach the server).
int run_curl(const std::string& url, const std::vector<std::string>& headers, const std::string& body, std::string& reply) {
    std::error_code error;
    const fs::path dir = "Saved/Assistant";
    fs::create_directories(dir, error);
    const std::string id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path head = dir / ("headers_" + id + ".txt"), request = dir / ("request_" + id + ".json"), response = dir / ("response_" + id + ".json");
    {
        std::ofstream h(head, std::ios::binary);
        for (const std::string& line : headers) h << line << "\n";
        std::ofstream r(request, std::ios::binary);
        r << body;
    }
    const std::string head_arg = "@" + head.string(), body_arg = "@" + request.string(), out_arg = response.string();
    const char* args[] = {"curl", "-s", "-S", "--max-time", "600", "-X", "POST", url.c_str(), "-H", head_arg.c_str(), "--data-binary", body_arg.c_str(),
                          "-o", out_arg.c_str(), "-w", "%{http_code}", nullptr};
    SDL_Process* process = SDL_CreateProcess(args, true);
    std::string status;
    if (process) {
        size_t size = 0;
        int exit_code = 0;
        if (void* out = SDL_ReadProcess(process, &size, &exit_code)) {
            status.assign(static_cast<const char*>(out), size);
            SDL_free(out);
        }
        SDL_DestroyProcess(process);
    }
    fs::remove(head, error);  // the key goes at once
    fs::remove(request, error);
    std::ifstream in(response, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    reply = text.str();
    in.close();
    fs::remove(response, error);
    return std::atoi(status.c_str());
}

struct Chat {
    std::vector<Turn> turns;
    std::vector<int> roles;  // who answered each assistant turn (kRoles index), -1 for the user's
    std::shared_ptr<Job> job;
    char input[8192] = "";
    bool include_context = true;
    int armed_overwrite = -1;  // a code block whose Create File was clicked once on an existing file
    std::string last_error;
};
Chat g_chat;

Settings settings_of(const EditorState& s) {
    Settings st;
    st.provider = s.settings.assistant_provider;
    st.endpoint = s.settings.assistant_endpoint;
    st.model = s.settings.assistant_model;
    st.key = s.settings.assistant_key;
    st.effort = s.settings.assistant_effort;
    if (st.key.empty()) {
        if (const char* env = std::getenv(st.provider == kAnthropic ? "ANTHROPIC_API_KEY" : "OPENAI_API_KEY")) st.key = env;
    }
    return st;
}

void send(EditorState& s) {
    if (!g_chat.input[0] || (g_chat.job && !g_chat.job->done)) return;
    const Settings st = settings_of(s);
    if (st.key.empty() && st.provider == kAnthropic) {
        g_chat.last_error = "Add your Anthropic API key in Editor Preferences > Plugins > LSD Assistant (or set ANTHROPIC_API_KEY).";
        return;
    }
    Turn t;
    t.user = true;
    t.text = (g_chat.include_context ? project_context(s) : std::string()) + g_chat.input;
    g_chat.turns.push_back(t);
    g_chat.roles.push_back(-1);
    g_chat.input[0] = '\0';
    g_chat.last_error.clear();
    const int role = static_cast<int>(s.settings.assistant_role) < kRoleCount ? static_cast<int>(s.settings.assistant_role) : 0;
    auto job = std::make_shared<Job>();
    g_chat.job = job;
    const std::string url = request_url(st), body = request_body(st, engine_guide(kRoles[role]), g_chat.turns);
    const std::vector<std::string> headers = request_headers(st);
    std::thread([job, st, url, body, headers] {
        std::string reply;
        const int status = run_curl(url, headers, body, reply);
        job->reply = parse_reply(st, status, reply);
        job->done = true;
    }).detach();  // shortcut: one request at a time, not cancellable; stream the reply (SSE) when long answers feel slow
}

// The reply's text, its code blocks as read-only boxes with their buttons.
void draw_reply(EditorState& s, const std::string& text, int turn) {
    const std::vector<CodeBlock> blocks = code_blocks(text);
    usize at = 0;
    int k = 0;
    for (const CodeBlock& b : blocks) {
        const usize open = text.find("```", at);
        if (open > at) ImGui::TextWrapped("%s", text.substr(at, open - at).c_str());
        ImGui::PushID(turn * 64 + k);
        const f32 lines = static_cast<f32>(std::count(b.code.begin(), b.code.end(), '\n') + 1);
        ImGui::InputTextMultiline("##code", const_cast<char*>(b.code.c_str()), b.code.size() + 1, ImVec2(-1, std::min(lines, 24.0f) * ImGui::GetTextLineHeight() + 8),
                                  ImGuiInputTextFlags_ReadOnly);
        if (with_tip(ImGui::SmallButton("Copy"), "Copies the code")) ImGui::SetClipboardText(b.code.c_str());
        if (!b.path.empty()) {
            ImGui::SameLine();
            std::error_code error;
            const bool exists = fs::exists(b.path, error);
            const bool armed = g_chat.armed_overwrite == turn * 64 + k;
            const std::string label = !writable_path(b.path) ? "(only Content/ or Source/ can be written)" : exists ? (armed ? "Click again to overwrite " : "Overwrite ") + b.path : "Create " + b.path;
            ImGui::BeginDisabled(!writable_path(b.path));
            if (with_tip(ImGui::SmallButton(label.c_str()), "Writes this code into the project (it reloads by itself)")) {
                if (exists && !armed) {
                    g_chat.armed_overwrite = turn * 64 + k;
                } else {
                    fs::create_directories(fs::path(b.path).parent_path(), error);
                    std::ofstream out(b.path, std::ios::binary);
                    out << b.code;
                    out.close();
                    g_chat.armed_overwrite = -1;
                    if (s.registry) s.registry->scan();
                    std::snprintf(s.status, sizeof s.status, out ? "LSD Assistant wrote %s" : "Could not write %s", b.path.c_str());
                }
            }
            ImGui::EndDisabled();
            // Scripts: onto the selected object (a Script component pointing at the file).
            const std::string ext = fs::path(b.path).extension().string();
            if ((ext == ".lua" || ext == ".js" || ext == ".lsdgraph") && exists && s.world && s.world->alive(s.selected) && s.registry) {
                ImGui::SameLine();
                if (with_tip(ImGui::SmallButton("Attach to Selected"), "Gives the selected object a Script component running this file")) {
                    const std::string relative = fs::path(b.path).lexically_relative("Content").generic_string();
                    for (usize i = 0; i < s.registry->count(); ++i) {
                        const asset::AssetRecord& r = s.registry->get(i);
                        if (relative != r.path) continue;
                        auto* script = s.world->get<world::Script>(s.selected) ? s.world->get<world::Script>(s.selected) : s.world->add<world::Script>(s.selected);
                        asset::to_string(r.guid, script->script);
                        std::snprintf(s.pending_edit, sizeof s.pending_edit, "Attach Script");
                        std::snprintf(s.status, sizeof s.status, "%s runs %s", s.world->name(s.selected), b.path.c_str());
                    }
                }
            }
        }
        ImGui::PopID();
        const usize close = text.find("\n```", text.find('\n', open));  // as code_blocks found it
        at = close == std::string::npos ? text.size() : close + 4;
        ++k;
    }
    if (at < text.size()) ImGui::TextWrapped("%s", text.substr(at).c_str());
}

} // namespace

void draw_settings(EditorState& s) {
    EditorSettings& set = s.settings;
    ImGui::TextWrapped("A chat with your own language model that knows LSD_Engine: ask how to do something, or have it write a script "
                       "and apply it. It answers as a studio role (engine expert, gameplay programmer, technical artist, designers, "
                       "producer, QA) and sees the open level, the selected object and the assets.");
    ImGui::Spacing();
    with_tip(ImGui::Checkbox("Enabled", &set.assistant_enabled), "Shows Tools > LSD Assistant");
    int provider = static_cast<int>(set.assistant_provider);
    ImGui::SetNextItemWidth(260);
    if (ImGui::Combo("Provider", &provider, "Anthropic (Claude)\0OpenAI-compatible (OpenAI, OpenRouter, Ollama, LM Studio...)\0")) {
        set.assistant_provider = static_cast<u32>(provider);
        std::snprintf(set.assistant_model, sizeof set.assistant_model, "%s", provider == 0 ? "claude-opus-5-5" : "gpt-4o");
        set.assistant_endpoint[0] = '\0';
    }
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("Model", set.assistant_model, sizeof set.assistant_model);
    ImGui::SetItemTooltip(provider == 0 ? "claude-opus-5-5 (best), claude-sonnet-5-5 (faster, cheaper), claude-haiku-5-5 (cheapest)"
                                        : "The model name your server knows (Ollama: llama3.1, qwen2.5-coder...)");
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("Endpoint", provider == 0 ? "https://api.anthropic.com" : "https://api.openai.com/v1", set.assistant_endpoint, sizeof set.assistant_endpoint);
    ImGui::SetItemTooltip("Empty: the provider's own. Ollama: http://localhost:11434/v1   LM Studio: http://localhost:1234/v1   OpenRouter: https://openrouter.ai/api/v1");
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("API Key", set.assistant_key, sizeof set.assistant_key, ImGuiInputTextFlags_Password);
    ImGui::SetItemTooltip("Stays on this PC (Saved/editor_settings.json, never in the project or Git). Empty: %s; local servers need none",
                          provider == 0 ? "ANTHROPIC_API_KEY" : "OPENAI_API_KEY");
    if (provider == 0) {
        int effort = static_cast<int>(set.assistant_effort);
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("Effort", &effort, "Low (quick answers)\0Medium\0High (hard problems, slower)\0")) set.assistant_effort = static_cast<u32>(effort);
    }
    ImGui::TextDisabled("Requests go from this PC to the endpoint above (curl); each message includes the project summary unless turned off in the chat.");
}

void draw_window(EditorState& s, bool* open) {
    if (!*open) return;
    ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("LSD Assistant", open)) {
        ImGui::End();
        return;
    }
    EditorSettings& set = s.settings;
    if (!set.assistant_enabled) {
        ImGui::TextDisabled("LSD Assistant is off (Editor Preferences > Plugins).");
        ImGui::End();
        return;
    }
    // Role and model.
    int role = static_cast<int>(set.assistant_role) < kRoleCount ? static_cast<int>(set.assistant_role) : 0;
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("##role", kRoles[role].name)) {
        for (int i = 0; i < kRoleCount; ++i) {
            if (ImGui::Selectable(kRoles[i].name, i == role)) set.assistant_role = static_cast<u32>(i);
            ImGui::SetItemTooltip("%s", kRoles[i].focus);
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Who in the studio answers the next message");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", set.assistant_model);
    ImGui::SameLine(ImGui::GetWindowWidth() - 170);
    if (with_tip(ImGui::SmallButton("New Chat"), "Starts over (the model forgets this conversation)") && (!g_chat.job || g_chat.job->done)) {
        g_chat.turns.clear();
        g_chat.roles.clear();
        g_chat.last_error.clear();
    }
    ImGui::SameLine();
    if (with_tip(ImGui::SmallButton("Settings"), "Provider, model and API key")) s.show_preferences = true;

    // A finished request lands here.
    if (g_chat.job && g_chat.job->done) {
        const Reply& r = g_chat.job->reply;
        if (r.ok) {
            Turn t;
            t.user = false;
            t.text = r.text;
            t.raw = r.raw;
            g_chat.turns.push_back(t);
            g_chat.roles.push_back(role);
        } else {
            g_chat.last_error = r.error;
            if (!g_chat.turns.empty() && g_chat.turns.back().user) {  // the question goes back into the box to try again
                const std::string& asked = g_chat.turns.back().text;
                const usize body = asked.find("</editor>\n\n");
                std::snprintf(g_chat.input, sizeof g_chat.input, "%s", body == std::string::npos ? asked.c_str() : asked.c_str() + body + 11);
                g_chat.turns.pop_back();
                g_chat.roles.pop_back();
            }
        }
        g_chat.job.reset();
    }

    // The conversation.
    const f32 input_height = ImGui::GetTextLineHeight() * 5 + 60;
    ImGui::BeginChild("##chat", ImVec2(0, -input_height), ImGuiChildFlags_Borders);
    if (g_chat.turns.empty()) {
        ImGui::TextWrapped("Ask LSD Assistant anything about your game or the engine, or ask it to write a script. Examples:");
        ImGui::BulletText("Make the selected door open when the player presses E near it");
        ImGui::BulletText("How do I set up a third-person character with my own animations?");
        ImGui::BulletText("Plan a first playable for a small stealth game in 4 weeks");
    }
    for (usize i = 0; i < g_chat.turns.size(); ++i) {
        const Turn& t = g_chat.turns[i];
        ImGui::Spacing();
        if (t.user) {
            ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "You");
            const usize body = t.text.find("</editor>\n\n");
            ImGui::TextWrapped("%s", body == std::string::npos ? t.text.c_str() : t.text.c_str() + body + 11);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "LSD Assistant (%s)", kRoles[g_chat.roles[i] >= 0 ? g_chat.roles[i] : 0].name);
            draw_reply(s, t.text, static_cast<int>(i));
        }
        ImGui::Separator();
    }
    if (g_chat.job) {
        const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - g_chat.job->started).count();
        ImGui::TextDisabled("LSD Assistant is thinking... %.0f s", seconds);
        ImGui::SetScrollHereY(1.0f);
    }
    if (!g_chat.last_error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", g_chat.last_error.c_str());
    ImGui::EndChild();

    // The message box: Ctrl+Enter sends.
    ImGui::InputTextMultiline("##input", g_chat.input, sizeof g_chat.input, ImVec2(-1, ImGui::GetTextLineHeight() * 5));
    const bool ctrl_enter = ImGui::IsItemFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter);
    ImGui::BeginDisabled(g_chat.job != nullptr);
    if (with_tip(ImGui::Button("Send (Ctrl+Enter)"), "Sends the message to your model") || ctrl_enter) send(s);
    ImGui::EndDisabled();
    ImGui::SameLine();
    with_tip(ImGui::Checkbox("Project context", &g_chat.include_context), "Includes the level, the selected object's components and the asset list with the message");
    ImGui::End();
}

} // namespace lsd::editor::assistant
