#include "source_control.h"
#include "tips.h"

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
struct Lock {
    std::string path;  // relative to Content/
    std::string owner;
    bool mine;
};
std::vector<Lock> g_locks;  // Git LFS locks under Content/
std::atomic<bool> g_lfs{false}, g_locks_dirty{false};
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

// A JSON string value after `key` from `at` on ("" if none); `at` moves past it.
std::string json_value(const std::string& json, const char* key, usize& at) {
    const std::string k = std::string("\"") + key + "\":";
    usize p = json.find(k, at);
    if (p == std::string::npos) return {};
    p = json.find('"', p + k.size());
    if (p == std::string::npos) return {};
    std::string v;
    for (++p; p < json.size() && json[p] != '"'; ++p) {
        if (json[p] == '\\' && p + 1 < json.size()) ++p;
        v += json[p];
    }
    at = p;
    return v;
}

// `git lfs locks --verify --json`: {"ours": [{"path": ..., "owner": {"name": ...}}], "theirs": [...]}
// (without --verify support: a plain list, matched to this user's name).
void read_locks() {
    std::string out;
    bool verified = git("lfs locks --verify --json", out);
    if (!verified && !git("lfs locks --json", out)) return;  // no LFS remote: no locks
    std::string me;
    if (!verified && git("config user.name", me)) {
        while (!me.empty() && (me.back() == '\n' || me.back() == '\r')) me.pop_back();
    }
    const usize theirs = verified ? out.find("\"theirs\"") : std::string::npos;
    std::vector<Lock> locks;
    for (usize at = 0;;) {
        const std::string path = json_value(out, "path", at);
        if (path.empty()) break;
        const usize here = at;
        const std::string owner = json_value(out, "name", at);
        if (path.rfind("Content/", 0) != 0) continue;
        locks.push_back({path.substr(8), owner, verified ? (theirs == std::string::npos || here < theirs) : owner == me});
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_locks.swap(locks);
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
        auto last_locks = last;
        std::string version;
        g_lfs = git("lfs version", version);
        while (g_running) {
            const auto now = std::chrono::steady_clock::now();
            if (g_dirty.exchange(false) || now - last > std::chrono::seconds(4)) {
                read_status();
                last = std::chrono::steady_clock::now();
            }
            if (g_lfs && g_enabled && (g_locks_dirty.exchange(false) || now - last_locks > std::chrono::seconds(20))) {
                read_locks();
                last_locks = std::chrono::steady_clock::now();
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

std::string locked_by(const char* content_path, bool& mine) {
    mine = false;
    if (!g_lfs) return {};
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Lock& l : g_locks) {
        if (l.path == content_path) {
            mine = l.mine;
            return l.owner;  // a copy: the background thread replaces the list
        }
    }
    return {};
}

void asset_menu(const char* content_path) {
    if (!g_enabled || !with_tip(ImGui::BeginMenu("Source Control"), "Git: history, diff and submit for this asset")) return;
    const std::string path = content_path;
    const char code = status(content_path);
    if (!safe(path)) {
        ImGui::TextDisabled("Rename it first: its path has characters git commands cannot take");
    } else {
        if (with_tip(ImGui::MenuItem("History"), "The last 50 commits that changed it")) show("History", path, "log -n 50 --date=short --format=\"%h  %ad  %an  %s\" -- " + pathspec(path));
        if (with_tip(ImGui::MenuItem("Diff Against Last Commit", nullptr, false, code == 'M' || code == 'R'), "What changed since the last commit")) show("Diff", path, "diff HEAD -- " + pathspec(path));
        if (with_tip(ImGui::MenuItem("Submit...", nullptr, false, code != 0), "Commits the asset and its .meta with a message")) {
            g_submit_path = path;
            g_message[0] = '\0';
            g_open_submit = true;
        }
    }
    if (g_lfs && safe(path)) {
        ImGui::Separator();
        bool mine = false;
        const std::string owner = locked_by(content_path, mine);
        if (owner.empty()) {
            if (with_tip(ImGui::MenuItem("Lock (Check Out)"), "Git LFS: locks the asset on the server so teammates know you are changing it (they cannot push it)")) {
                show("Lock", path, "lfs lock \"Content/" + path + "\"");
                g_locks_dirty = true;
            }
        } else if (mine) {
            if (with_tip(ImGui::MenuItem("Unlock (Check In)"), "Git LFS: releases your lock (push your changes first)")) {
                show("Unlock", path, "lfs unlock \"Content/" + path + "\"");
                g_locks_dirty = true;
            }
        } else {
            ImGui::TextDisabled("Locked by %s", owner.c_str());
            if (with_tip(ImGui::MenuItem("Break Lock"), "Git LFS: removes a teammate's lock (only when they cannot: it may lose their work)")) {
                show("Break Lock", path, "lfs unlock --force \"Content/" + path + "\"");
                g_locks_dirty = true;
            }
        }
    }
    if (with_tip(ImGui::MenuItem("Refresh Status"), "Asks Git again what changed (and who holds locks)")) {
        refresh();
        g_locks_dirty = true;
    }
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
        if (with_tip(ImGui::Button("Submit"), "Commits with this message")) {
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
        if (with_tip(ImGui::Button("Cancel"), "Does not commit") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace lsd::editor::source_control
