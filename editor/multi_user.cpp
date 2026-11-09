#include "multi_user.h"
#include "debug_draw.h"
#include "editor_ui.h"
#include "multi_user_core.h"
#include "remote_console.h"
#include "tips.h"
#include "transactions.h"

#include "lsd/core/console.h"
#include "lsd/foundation/log.h"
#include "lsd/net/join_code.h"
#include "lsd/net/net.h"
#include "lsd/world/level.h"

#include <imgui.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lsd::editor::multi_user {

namespace {

struct User {
    char name[48] = "";
    Vec3 position;
    f32 yaw = 0.0f;
    u32 selected = world::Entity::kInvalid;
    f64 heard = 0.0;  // seconds (session clock)
};

// The session. The NetSession runs over an empty world of its own: the level is never replicated
// as game objects, only through the ops below.
struct Session {
    net::NetSession net;
    world::World empty{default_allocator()};
    Baseline base;
    bool synced = false;  // host: always; joiner: once the host's level arrived
    u32 seen_version = 0;
    std::vector<u32> clients;                     // host: the joined players
    std::unordered_map<u32, std::string> inbox;   // texts arriving in pieces, by sender
    std::vector<std::string> held;                // ops that arrived during Play
    std::unordered_map<u32, User> users;          // the others, by player id
    f64 clock = 0.0;
    f32 view_timer = 0.0f;
    u16 port = 0;
};

std::unique_ptr<Session> g;
char g_name[48] = "";
std::chrono::steady_clock::time_point g_last = std::chrono::steady_clock::now();

const char* my_name() {
    if (!g_name[0]) {
        const char* user = std::getenv("USERNAME");
        if (!user) user = std::getenv("USER");
        std::snprintf(g_name, sizeof g_name, "%s", user && *user ? user : "Editor");
    }
    return g_name;
}

Vec3 user_color(u32 id) {
    static const Vec3 kColors[] = {{1.0f, 0.75f, 0.1f}, {0.2f, 0.8f, 1.0f}, {1.0f, 0.35f, 0.6f}, {0.5f, 1.0f, 0.3f}, {0.75f, 0.5f, 1.0f}, {1.0f, 0.5f, 0.2f}};
    return kColors[id % (sizeof kColors / sizeof kColors[0])];
}

// Events carry at most 16 KB: longer texts go as "mu_more" pieces first (reliable and in order).
void send_text(i32 target, const char* name, const std::string& text) {
    constexpr usize kPiece = 15000;
    usize at = 0;
    for (; text.size() - at > kPiece; at += kPiece) g->net.send_event(target, "mu_more", text.substr(at, kPiece).c_str());
    g->net.send_event(target, name, text.c_str() + at);
}

void to_others(const char* name, const std::string& text, u32 except) {  // host: every joiner but one
    for (u32 c : g->clients) {
        if (c != except) send_text(static_cast<i32>(c), name, text);
    }
}

void apply_ops(EditorState& s, const std::string& ops) {
    if (!apply(*s.world, ops.data(), ops.size(), g->base)) LSD_LOG_ERROR("multiuser", "Could not apply an edit from another user");
    s.history->commit(*s.world, "Multi-User edit");  // undoable here like any edit
    g->seen_version = s.history->version();
}

void receive_level(EditorState& s, const std::string& text) {  // "<level path>\n<level text>"
    const usize line = text.find('\n');
    if (line == std::string::npos || !world::load_level(*s.world, text.data() + line + 1, text.size() - line - 1)) {
        LSD_LOG_ERROR("multiuser", "The host's level did not load");
        return;
    }
    std::snprintf(s.level_path, sizeof s.level_path, "%s", text.substr(0, line).c_str());
    s.history->reset(*s.world, "Joined Multi-User session");
    s.capture_reflections = true;
    capture(*s.world, g->base);
    g->seen_version = s.history->version();
    g->synced = true;
    LSD_LOG_INFO("multiuser", "Joined: the host's level %s (%u objects)", s.level_path[0] ? s.level_path : "(unsaved)", static_cast<u32>(g->base.objects.size()));
}

void on_call(EditorState& s, const net::NetEvent& ev) {
    std::string text(ev.json, ev.size);
    if (std::strcmp(ev.name, "mu_more") == 0) {
        g->inbox[ev.from] += text;
        return;
    }
    if (const auto it = g->inbox.find(ev.from); it != g->inbox.end()) {
        text = it->second + text;
        g->inbox.erase(it);
    }
    const bool hosting = g->net.mode() == net::Mode::Server;
    if (std::strcmp(ev.name, "mu_level") == 0 && !hosting) {
        receive_level(s, text);
    } else if (std::strcmp(ev.name, "mu_ops") == 0) {
        if (hosting) to_others("mu_ops", text, ev.from);
        if (s.playing || !g->synced) g->held.push_back(std::move(text));
        else apply_ops(s, text);
    } else if (std::strcmp(ev.name, "mu_view") == 0) {  // "id|x y z yaw|selected|name"
        unsigned id = 0, selected = 0;
        f32 x = 0, y = 0, z = 0, yaw = 0;
        int name_at = 0;
        if (std::sscanf(text.c_str(), "%u|%f %f %f %f|%u|%n", &id, &x, &y, &z, &yaw, &selected, &name_at) < 6 || name_at <= 0) return;
        if (hosting) to_others("mu_view", text, ev.from);
        if (id == g->net.player_id()) return;
        User& u = g->users[id];
        std::snprintf(u.name, sizeof u.name, "%s", text.c_str() + name_at);
        u.position = {x, y, z};
        u.yaw = yaw;
        u.selected = selected;
        u.heard = g->clock;
    }
}

void send_view(const EditorState& s) {
    char text[160];
    std::snprintf(text, sizeof text, "%u|%.3f %.3f %.3f %.4f|%u|%s", g->net.player_id(), static_cast<double>(s.camera.position.x), static_cast<double>(s.camera.position.y),
                  static_cast<double>(s.camera.position.z), static_cast<double>(s.camera.yaw), s.selected.value, my_name());
    g->net.send_event(g->net.mode() == net::Mode::Server ? net::kToEveryone : net::kToServer, "mu_view", text);
}

void draw_markers(const EditorState& s) {
    for (const auto& [id, u] : g->users) {
        const Vec3 color = user_color(id);
        const Vec3 ahead{std::cos(u.yaw), std::sin(u.yaw), 0.0f};
        debug_draw::sphere(u.position, 0.2f, color, 0.0f);
        debug_draw::line(u.position, u.position + ahead * 0.8f, color, 0.0f);
        debug_draw::text(u.position + Vec3{0, 0, 0.35f}, u.name, color, 0.0f);
        world::Entity e;
        e.value = u.selected;
        if (s.world->alive(e)) {
            const Mat4 m = s.world->world_matrix(e);
            const Vec3 at{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
            debug_draw::box(at, {0.6f, 0.6f, 0.6f}, color, 0.0f);
            debug_draw::text(at + Vec3{0, 0, 0.8f}, u.name, color, 0.0f);
        }
    }
}

bool start(EditorState& s) {
    s.history->commit(*s.world, "Multi-User session");  // nothing pending is lost on a join
    capture(*s.world, g->base);
    g->seen_version = s.history->version();
    return true;
}

} // namespace

bool host(EditorState& s, u16 port) {
    leave();
    g = std::make_unique<Session>();
    net::NetSettings settings;
    settings.port = port;
    std::snprintf(settings.server_name, sizeof settings.server_name, "%s (Multi-User)", my_name());
    if (!g->net.host(g->empty, settings)) {
        LSD_LOG_ERROR("multiuser", "Could not host on port %u (in use?)", port);
        g.reset();
        return false;
    }
    g->port = port;
    g->synced = start(s);
    LSD_LOG_INFO("multiuser", "Hosting a Multi-User session on port %u", port);
    return true;
}

bool join(EditorState& s, const char* code_or_address) {
    char address[64];
    if (!net::parse_join_code(code_or_address, kDefaultPort, address, sizeof address)) {
        LSD_LOG_ERROR("multiuser", "Not a join code or an address: %s", code_or_address);
        return false;
    }
    leave();
    g = std::make_unique<Session>();
    if (!g->net.join(g->empty, address, net::NetSettings{})) {
        LSD_LOG_ERROR("multiuser", "Could not join %s", address);
        g.reset();
        return false;
    }
    start(s);
    LSD_LOG_INFO("multiuser", "Joining the Multi-User session at %s", address);
    return true;
}

void leave() {
    if (!g) return;
    g->net.shutdown();
    g.reset();
    LSD_LOG_INFO("multiuser", "Left the Multi-User session");
}

void update(EditorState& s) {
    const auto now = std::chrono::steady_clock::now();
    const f32 dt = std::chrono::duration<f32>(now - g_last).count();
    g_last = now;
    if (!g) return;
    g->clock += dt;
    if (!s.playing) debug_draw::tick(dt);  // Play ticks them itself; here last frame's markers go
    g->net.receive(g->empty, dt);
    net::NetEvent ev;
    while (g && g->net.take_event(ev)) {
        switch (ev.kind) {
            case net::NetEvent::Kind::PlayerJoined:
                if (g->net.mode() != net::Mode::Server) break;
                g->clients.push_back(ev.from);
                {
                    Array<char> level(default_allocator());
                    world::save_level(*s.world, level);
                    send_text(static_cast<i32>(ev.from), "mu_level", std::string(s.level_path) + "\n" + std::string(level.data(), level.size()));
                }
                LSD_LOG_INFO("multiuser", "User %u joined: sent the level", ev.from);
                break;
            case net::NetEvent::Kind::PlayerLeft:
                std::erase(g->clients, ev.from);
                if (const auto it = g->users.find(ev.from); it != g->users.end()) LSD_LOG_INFO("multiuser", "%s left", it->second.name);
                g->users.erase(ev.from);
                break;
            case net::NetEvent::Kind::Connected: LSD_LOG_INFO("multiuser", "Connected as user %u, waiting for the level", ev.from); break;
            case net::NetEvent::Kind::Disconnected:
                LSD_LOG_WARN("multiuser", "The host closed the session or the connection dropped");
                leave();
                break;
            case net::NetEvent::Kind::Call: on_call(s, ev); break;
            default: break;
        }
    }
    if (!g) return;
    if (!s.playing && g->synced) {
        for (const std::string& ops : g->held) apply_ops(s, ops);
        g->held.clear();
        if (s.history->version() != g->seen_version) {  // a finished local edit (or undo / redo / open)
            g->seen_version = s.history->version();
            const std::string ops = diff(*s.world, g->base);
            if (!ops.empty()) {
                if (g->net.mode() == net::Mode::Server) to_others("mu_ops", ops, net::kServerPlayer);
                else send_text(net::kToServer, "mu_ops", ops);
            }
        }
    }
    g->view_timer -= dt;
    if (g->net.connected() && g->view_timer <= 0.0f) {
        g->view_timer = 0.2f;
        send_view(s);
    }
    std::erase_if(g->users, [](const auto& kv) { return g->clock - kv.second.heard > 5.0; });  // silent: gone
    if (!s.playing) draw_markers(s);
    g->net.send(g->empty, dt);
}

void draw_window(EditorState& s) {
    if (!s.show_multi_user) return;
    if (!ImGui::Begin("Multi-User Editing", &s.show_multi_user)) {
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(160.0f);
    my_name();
    with_tip(ImGui::InputText("Your Name", g_name, sizeof g_name), "What the others see next to your camera and selection");
    if (!g) {
        static int port = kDefaultPort;
        static char code[64] = "";
        ImGui::SetNextItemWidth(100.0f);
        with_tip(ImGui::InputInt("Port", &port, 0), "The UDP port the session uses (the host's router or firewall must let it through)");
        if (with_tip(ImGui::Button("Host"), "Starts a session for this level; the others join with your address or join code"))
            host(s, static_cast<u16>(port < 1024 || port > 65535 ? kDefaultPort : port));
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputTextWithHint("##mu_code", "join code or address", code, sizeof code);
        ImGui::SetItemTooltip("A host's join code (10 letters and digits) or address, e.g. 192.168.1.20:7790");
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Join"), "Joins that session: your level is replaced by the host's (save yours first)")) join(s, code);
        ImGui::TextDisabled("Every editor needs the same project files (Content/).");
    } else {
        const bool hosting = g->net.mode() == net::Mode::Server;
        ImGui::Text("%s, user %u%s", hosting ? "Hosting" : "Joined", g->net.player_id(), !g->net.connected() ? " (connecting)" : !g->synced ? " (getting the level)" : "");
        if (with_tip(ImGui::Button("Leave"), hosting ? "Ends the session for everyone" : "Leaves the session; your level stays as it is")) leave();
        if (g && hosting) {
            ImGui::SeparatorText("Join Codes");
            char addresses[8][16];
            const u32 n = net::local_addresses(addresses, 8);
            for (u32 i = 0; i < n; ++i) {
                char join_code[16];
                if (!net::make_join_code(addresses[i], g->port, join_code, sizeof join_code)) continue;
                ImGui::PushID(static_cast<int>(i));
                ImGui::Text("%s  %s:%u", join_code, addresses[i], g->port);
                ImGui::SameLine();
                if (with_tip(ImGui::SmallButton("Copy"), "Copies the code for the others' Join box")) ImGui::SetClipboardText(join_code);
                ImGui::PopID();
            }
        }
        if (g) {
            ImGui::SeparatorText("Users");
            const Vec3 me = user_color(g->net.player_id());
            ImGui::TextColored(ImVec4(me.x, me.y, me.z, 1.0f), "%s (you)", my_name());
            for (const auto& [id, u] : g->users) {
                const Vec3 c = user_color(id);
                world::Entity e;
                e.value = u.selected;
                ImGui::TextColored(ImVec4(c.x, c.y, c.z, 1.0f), "%s", u.name);
                ImGui::SameLine();
                ImGui::TextDisabled("%s", s.world->alive(e) ? s.world->name(e) : "nothing selected");
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(id));
                if (with_tip(ImGui::SmallButton("Go To"), "Puts your camera where theirs is")) {
                    s.camera.position = u.position;
                    s.camera.yaw = u.yaw;
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::End();
}

void register_commands(EditorState& s) {
    console::add_command("multiuser.Host", [](void* user, const char* args) {
        const int port = std::atoi(args);
        host(*static_cast<EditorState*>(user), static_cast<u16>(port >= 1024 && port <= 65535 ? port : kDefaultPort));
    }, &s, "Host a Multi-User Editing session for this level: multiuser.Host [port]");
    console::add_command("multiuser.Join", [](void* user, const char* args) { join(*static_cast<EditorState*>(user), args); }, &s,
                         "Join a Multi-User Editing session: multiuser.Join <join code or address[:port]>");
    console::add_command("multiuser.Leave", [](void*, const char*) { leave(); }, &s, "Leave the Multi-User Editing session");
    console::add_command("multiuser.Name", [](void*, const char* args) {
        if (*args) std::snprintf(g_name, sizeof g_name, "%s", args);
    }, &s, "Your name as the other users see it: multiuser.Name <name>");
    console::add_command("multiuser.Users", [](void* user, const char*) {
        const EditorState& st = *static_cast<EditorState*>(user);
        if (!g) {
            remote::print("not in a Multi-User session");
            return;
        }
        std::string out = std::string(g->net.mode() == net::Mode::Server ? "hosting" : "joined") + (g->synced ? "" : " (getting the level)") + ", you: " + my_name() + "\n";
        for (const auto& [id, u] : g->users) {
            world::Entity e;
            e.value = u.selected;
            char line[192];
            std::snprintf(line, sizeof line, "%u %s camera (%.2f %.2f %.2f) selected %s\n", id, u.name, static_cast<double>(u.position.x), static_cast<double>(u.position.y),
                          static_cast<double>(u.position.z), st.world->alive(e) ? st.world->name(e) : "-");
            out += line;
        }
        remote::print(out.c_str());
    }, &s, "The Multi-User session's users: camera and selection");
}

} // namespace lsd::editor::multi_user
