#include "source_control.h"

#include "lsd/foundation/log.h"
#include "lsd/foundation/types.h"

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define LSD_POPEN _popen
#define LSD_PCLOSE _pclose
#else
#define LSD_POPEN popen
#define LSD_PCLOSE pclose
#endif

namespace lsd::editor::source_control {

namespace {

struct Entry {
    std::string path;  // relative to Content/
    char code;
};

std::mutex g_mutex;
std::vector<Entry> g_status;  // changed files only
std::thread g_thread;
std::atomic<bool> g_running{false}, g_dirty{false}, g_enabled{false};

// Window and dialog state (UI thread only).
std::string g_title, g_output;
bool g_window = false, g_open_submit = false;
std::string g_submit_path;
char g_message[512] = "";

// Runs `git <args>` in the project folder; false if it failed (not a repository, no git...).
bool git(const std::string& args, std::string& out) {
    out.clear();
    const std::string command = "git -c core.quotepath=off " + args + " 2>&1";
    FILE* pipe = LSD_POPEN(command.c_str(), "r");
    if (!pipe) return false;
    char buffer[4096];
    while (const usize n = std::fread(buffer, 1, sizeof buffer, pipe)) out.append(buffer, n);
    return LSD_PCLOSE(pipe) == 0;
}

// Paths go to a shell: only ones without its special characters.
bool safe(const std::string& path) {
    return !path.empty() && path.find_first_of("\"%&|<>^!$`;") == std::string::npos;
}

// The asset and its .meta (if there is one), quoted for the command line.
std::string pathspec(const std::string& content_path) {
    std::string spec = "\"Content/" + content_path + "\"";
    std::error_code ec;
    if (std::filesystem::exists("Content/" + content_path + ".meta", ec)) spec += " \"Content/" + content_path + ".meta\"";
    return spec;
}

void read_status() {
    std::string out;
    if (!git("status --porcelain=v1 -uall -- Content", out)) {
        g_enabled = false;
        return;
    }
    std::vector<Entry> entries;
    usize start = 0;
    while (start < out.size()) {
        usize end = out.find('\n', start);
        if (end == std::string::npos) end = out.size();
        const std::string line = out.substr(start, end - start);
        start = end + 1;
        if (line.size() < 4) continue;
        const char x = line[0], y = line[1];
        if (x == 'D' || y == 'D') continue;  // gone: no tile to badge
        std::string path = line.substr(3);
        const usize arrow = path.find(" -> ");
        if (arrow != std::string::npos) path = path.substr(arrow + 4);  // renamed: the new name
        if (path.size() > 1 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
        if (path.rfind("Content/", 0) != 0) continue;
        const char code = x == '?' || x == 'A' ? 'A' : x == 'R' ? 'R' : 'M';
        entries.push_back({path.substr(8), code});
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.swap(entries);
    g_enabled = true;
}

void show(const char* title, const std::string& content_path, const std::string& args) {
    g_title = std::string(title) + ": Content/" + content_path;
    if (!git(args, g_output) && g_output.empty()) g_output = "git failed";
    if (g_output.empty()) g_output = "(nothing)";
    g_window = true;
}

} // namespace

void start() {
    if (g_running) return;
    g_running = true;
    g_dirty = true;
    g_thread = std::thread([] {
        auto last = std::chrono::steady_clock::now() - std::chrono::hours(1);
        while (g_running) {
            const auto now = std::chrono::steady_clock::now();
            if (g_dirty.exchange(false) || now - last > std::chrono::seconds(4)) {
                read_status();
                last = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void stop() {
    g_running = false;
    if (g_thread.joinable()) g_thread.join();
}

void refresh() { g_dirty = true; }

char status(const char* content_path) {
    if (!g_enabled) return 0;
    const usize n = std::strlen(content_path);
    char code = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Entry& e : g_status) {
        const bool meta = e.path.size() == n + 5 && e.path.compare(n, 5, ".meta") == 0;
        if ((e.path.size() == n || meta) && e.path.compare(0, n, content_path) == 0) {
            if (!meta) return e.code;  // the asset's own state wins over its .meta's
            code = e.code == 'A' ? 'A' : 'M';
        }
    }
    return code;
}

void asset_menu(const char* content_path) {
    if (!g_enabled || !ImGui::BeginMenu("Source Control")) return;
    const std::string path = content_path;
    const char code = status(content_path);
    if (!safe(path)) {
        ImGui::TextDisabled("Rename it first: its path has characters git commands cannot take");
    } else {
        if (ImGui::MenuItem("History")) show("History", path, "log -n 50 --date=short --format=\"%h  %ad  %an  %s\" -- " + pathspec(path));
        if (ImGui::MenuItem("Diff Against Last Commit", nullptr, false, code == 'M' || code == 'R')) show("Diff", path, "diff HEAD -- " + pathspec(path));
        if (ImGui::MenuItem("Submit...", nullptr, false, code != 0)) {
            g_submit_path = path;
            g_message[0] = '\0';
            g_open_submit = true;
        }
    }
    if (ImGui::MenuItem("Refresh Status")) refresh();
    ImGui::EndMenu();
}

void draw() {
    if (g_window) {
        ImGui::SetNextWindowSize(ImVec2(720, 420), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Source Control", &g_window)) {
            ImGui::TextUnformatted(g_title.c_str());
            ImGui::InputTextMultiline("##output", g_output.data(), g_output.size() + 1, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
        }
        ImGui::End();
    }
    if (g_open_submit) ImGui::OpenPopup("Submit to Source Control");
    g_open_submit = false;
    if (ImGui::BeginPopupModal("Submit to Source Control", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Commits Content/%s (and its .meta) to the local Git repository.", g_submit_path.c_str());
        ImGui::TextDisabled("Push from your Git tool to share it.");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextMultiline("Description", g_message, sizeof g_message, ImVec2(480, 90));
        const bool ok = g_message[0] != '\0';
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button("Submit")) {
            // The message goes through a file: no quoting problems whatever it says.
            const char* message_file = "Saved/commit_message.txt";
            std::error_code ec;
            std::filesystem::create_directories("Saved", ec);
            if (FILE* f = std::fopen(message_file, "wb")) {
                std::fwrite(g_message, 1, std::strlen(g_message), f);
                std::fclose(f);
            }
            std::string out;
            const std::string spec = pathspec(g_submit_path);
            const bool done = git("add -- " + spec, out) && git("commit -F \"" + std::string(message_file) + "\" --only -- " + spec, out);
            std::filesystem::remove(message_file, ec);
            if (done) LSD_LOG_INFO("source", "Submitted Content/%s", g_submit_path.c_str());
            else LSD_LOG_ERROR("source", "Submit failed: %s", out.c_str());
            refresh();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace lsd::editor::source_control
