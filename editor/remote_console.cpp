#include "remote_console.h"
#include "assistant_core.h"
#include "tips.h"

#include "lsd/core/console.h"
#include "lsd/foundation/log.h"

#include <imgui.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
#define LSD_CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kNoSocket = -1;
#define LSD_CLOSE_SOCKET ::close
#endif

namespace lsd::editor::remote {

namespace {

bool g_started = false;

bool start_sockets() {
#ifdef _WIN32
    if (!g_started) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    }
#endif
    g_started = true;
    return true;
}

void set_non_blocking(Socket s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

bool would_block() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

sockaddr_in localhost(u16 port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // this PC only
    return a;
}

void send_all(Socket s, const std::string& text) {
    usize sent = 0;
    while (sent < text.size()) {
        const int n = static_cast<int>(::send(s, text.data() + sent, static_cast<int>(text.size() - sent), 0));  // ssize_t on POSIX
        if (n <= 0) {
            if (n < 0 && would_block()) continue;  // shortcut: spins while the socket is full, fine for console replies
            return;
        }
        sent += static_cast<usize>(n);
    }
}

// Reads what arrived into `buffer`; false when the other side closed.
bool receive(Socket s, std::string& buffer) {
    char chunk[1024];
    for (;;) {
        const int n = static_cast<int>(::recv(s, chunk, sizeof chunk, 0));
        if (n > 0) {
            buffer.append(chunk, static_cast<usize>(n));
            continue;
        }
        return n < 0 && would_block();
    }
}

// Game side.
Socket g_listener = kNoSocket, g_client = kNoSocket;
std::string g_in;
std::string g_password;  // "" = this PC only, no login
bool g_authed = false;
u32 g_failures = 0;

// While a remote line runs, its log lines go back to the editor (and on to the Output Log).
std::string* g_capture = nullptr;
log::Sink g_previous_sink = nullptr;
void* g_previous_user = nullptr;
void capture_sink(void*, log::Level level, const char* channel, const char* message) {
    if (g_capture) *g_capture += std::string("[") + channel + "] " + message + "\n";
    if (g_previous_sink) g_previous_sink(g_previous_user, level, channel, message);
}

// Editor side.
Socket g_server = kNoSocket;
std::string g_out, g_reply;
char g_line[256] = "";
int g_port = kDefaultPort;
char g_host[64] = "127.0.0.1";
char g_pass[64] = "";
bool g_push = false;  // Push Changes
std::unordered_map<std::string, std::filesystem::file_time_type> g_pushed;  // Content file -> its time when last seen
u64 g_push_scan = 0;

} // namespace

void register_content_commands() {
    namespace fs = std::filesystem;
    console::add_command("content.Put", [](void*, const char* args) {
        char path[512] = "";
        unsigned long long offset = 0;
        int used = 0;
        if (std::sscanf(args, "%511s %llu %n", path, &offset, &used) < 2) return print("content.Put <path> <offset> <base64>");
        std::string rel, bytes;
        if (!assistant::safe_content_path(path, rel) || !assistant::base64_decode(args + used, bytes)) return print("content.Put: not a Content path, or not base64");
        const fs::path part = fs::path("Content") / (rel + ".lsdpart");
        std::error_code error;
        fs::create_directories(part.parent_path(), error);
        std::ofstream file(part, offset ? std::ios::binary | std::ios::app : std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file) print("content.Put: could not write");
    }, nullptr, "content.Put <path> <offset> <base64>: part of a file the editor pushes into Content (Remote Console > Push Changes)");
    console::add_command("content.Commit", [](void*, const char* args) {
        std::string rel;
        if (!assistant::safe_content_path(args, rel)) return print("content.Commit: not a Content path");
        const fs::path target = fs::path("Content") / rel, part = fs::path("Content") / (rel + ".lsdpart");
        std::ifstream a(part, std::ios::binary), b(target, std::ios::binary);
        if (!a) return print("content.Commit: nothing was put for that path (content.Put first)");
        const std::string fresh((std::istreambuf_iterator<char>(a)), std::istreambuf_iterator<char>());
        const std::string old = b ? std::string((std::istreambuf_iterator<char>(b)), std::istreambuf_iterator<char>()) : std::string();
        a.close();
        b.close();
        std::error_code error;
        if (b && fresh == old) {  // the same (a game running from the editor's own folder): no reload, no echo
            fs::remove(part, error);
            return;
        }
        fs::rename(part, target, error);
        if (error) return print("content.Commit: could not replace the file (open elsewhere?)");
        LSD_LOG_INFO("remote", "Pushed from the editor: %s", rel.c_str());
    }, nullptr, "content.Commit <path>: puts a pushed file in place (it hot-reloads)");
}

void push_changes() {
    namespace fs = std::filesystem;
    if (g_server == kNoSocket || !g_push) return;
    const u64 now = static_cast<u64>(ImGui::GetTime() * 1000.0);
    if (now - g_push_scan < 1000) return;  // about once a second
    g_push_scan = now;
    std::error_code error;
    const bool first = g_pushed.empty();
    for (fs::recursive_directory_iterator it("Content", error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        const std::string rel = fs::relative(it->path(), "Content", error).generic_string();
        const auto time = it->last_write_time(error);
        auto found = g_pushed.find(rel);
        if (found != g_pushed.end() && found->second == time) continue;
        g_pushed[rel] = time;
        if (first) continue;  // what is there when Push Changes is switched on is the game's already
        std::ifstream file(it->path(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        constexpr usize kChunk = 9000;  // 12000 base64 characters: under the console's 16 KB line
        for (usize at = 0; at == 0 || at < bytes.size(); at += kChunk) {
            const usize n = std::min(kChunk, bytes.size() - at);
            send_all(g_server, "content.Put " + rel + " " + std::to_string(at) + " " + assistant::base64(reinterpret_cast<const u8*>(bytes.data()) + at, n) + "\n");
            if (bytes.empty()) break;
        }
        send_all(g_server, "content.Commit " + rel + "\n");
        g_out += "Pushed " + rel + "\n";
    }
}

bool listen(u16 port, const char* password) {
    if (!start_sockets()) return false;
    g_listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listener == kNoSocket) return false;
    g_password = password ? password : "";
    g_failures = 0;
    sockaddr_in a = localhost(port);
    if (!g_password.empty()) a.sin_addr.s_addr = htonl(INADDR_ANY);  // other PCs too, behind the password
    if (::bind(g_listener, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0 || ::listen(g_listener, 1) != 0) {
        LSD_LOG_WARN("remote", "Remote console: port %u is in use", port);
        LSD_CLOSE_SOCKET(g_listener);
        g_listener = kNoSocket;
        return false;
    }
    set_non_blocking(g_listener);
    if (g_password.empty()) LSD_LOG_INFO("remote", "Remote console listening on 127.0.0.1:%u", port);
    else LSD_LOG_INFO("remote", "Remote console listening on every network, port %u (password required)", port);
    return true;
}

void poll() {
    if (g_listener == kNoSocket) return;
    if (g_client == kNoSocket) {
        g_client = ::accept(g_listener, nullptr, nullptr);
        if (g_client == kNoSocket) return;
        set_non_blocking(g_client);
        g_in.clear();
        g_authed = g_password.empty();
        LSD_LOG_INFO("remote", "Remote console connected");
    }
    if (!receive(g_client, g_in)) {
        LSD_CLOSE_SOCKET(g_client);
        g_client = kNoSocket;
        LSD_LOG_INFO("remote", "Remote console disconnected");
        return;
    }
    usize end;
    while ((end = g_in.find('\n')) != std::string::npos) {
        std::string line = g_in.substr(0, end);
        g_in.erase(0, end + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!g_authed) {  // the first line logs in
            if (line == "auth " + g_password) {
                g_authed = true;
                send_all(g_client, "Logged in\n.\n");
                continue;
            }
            send_all(g_client, "Wrong password\n.\n");
            LSD_CLOSE_SOCKET(g_client);
            g_client = kNoSocket;
            LSD_LOG_WARN("remote", "Remote console: a wrong password");
            if (++g_failures >= 5) {  // someone guessing: stop listening until the game restarts
                LSD_CLOSE_SOCKET(g_listener);
                g_listener = kNoSocket;
                LSD_LOG_WARN("remote", "Remote console closed after 5 wrong passwords");
            }
            return;
        }
        send_all(g_client, run_captured(line.c_str()) + ".\n");
    }
}

std::string run_captured(const char* line) {
    if (g_capture) {  // already capturing (a line run from inside another): it adds to that reply
        console::execute(line);
        return {};
    }
    std::string reply;
    g_capture = &reply;
    g_previous_sink = log::sink(&g_previous_user);
    log::set_sink(capture_sink, nullptr);
    console::execute(line);  // it logs unknown commands itself
    log::set_sink(g_previous_sink, g_previous_user);
    g_capture = nullptr;
    return reply;
}

void stop_listening() {
    if (g_client != kNoSocket) LSD_CLOSE_SOCKET(g_client);
    if (g_listener != kNoSocket) LSD_CLOSE_SOCKET(g_listener);
    g_client = g_listener = kNoSocket;
}

bool listening() { return g_listener != kNoSocket; }
bool client_connected() { return g_client != kNoSocket; }

void print(const char* text) {
    if (g_capture) {
        *g_capture += text;
        if (!g_capture->empty() && g_capture->back() != '\n') *g_capture += '\n';
    } else {
        LSD_LOG_INFO("console", "%s", text);
    }
}

void close() {
    if (g_client != kNoSocket) LSD_CLOSE_SOCKET(g_client);
    if (g_listener != kNoSocket) LSD_CLOSE_SOCKET(g_listener);
    if (g_server != kNoSocket) LSD_CLOSE_SOCKET(g_server);
    g_client = g_listener = g_server = kNoSocket;
#ifdef _WIN32
    if (g_started) WSACleanup();
#endif
    g_started = false;
}

void draw_window(bool* open) {
    if (!*open) return;
    ImGui::SetNextWindowSize(ImVec2(640, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Remote Console", open)) {
        ImGui::End();
        return;
    }
    if (g_server == kNoSocket) {
        ImGui::SetNextItemWidth(140);
        ImGui::InputText("Host", g_host, sizeof g_host);
        ImGui::SetItemTooltip("The PC the game runs on: 127.0.0.1 for this one, or another PC's address (192.168.x.x; its game needs -remotepassword)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("Port", &g_port, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("Password", g_pass, sizeof g_pass, ImGuiInputTextFlags_Password);
        ImGui::SetItemTooltip("The game's -remotepassword (empty for a game on this PC started with plain -remote)");
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Connect"), "Connects to a game started with -remote (this PC) or -remote -remotepassword=... (any PC on the network)") && start_sockets()) {
            g_server = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in a = localhost(static_cast<u16>(g_port));
            const bool address = inet_pton(AF_INET, g_host, &a.sin_addr) == 1;
            if (address && g_server != kNoSocket && ::connect(g_server, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0) {
                set_non_blocking(g_server);
                g_out += std::string("Connected to ") + g_host + ":" + std::to_string(g_port) + "\n";
                if (g_pass[0]) send_all(g_server, std::string("auth ") + g_pass + "\n");
            } else {
                if (g_server != kNoSocket) LSD_CLOSE_SOCKET(g_server);
                g_server = kNoSocket;
                g_out += address ? "No game is listening there. Start it with -remote (and -remotepassword=... to reach it from another PC).\n"
                                 : "The host must be an IPv4 address such as 192.168.1.20.\n";
            }
        }
    } else {
        if (!receive(g_server, g_reply)) {
            LSD_CLOSE_SOCKET(g_server);
            g_server = kNoSocket;
            g_out += "The game closed the connection.\n";
        }
        usize end;
        while ((end = g_reply.find("\n.\n")) != std::string::npos || g_reply.rfind(".\n", 0) == 0) {
            if (g_reply.rfind(".\n", 0) == 0) {  // an empty reply
                g_reply.erase(0, 2);
                continue;
            }
            g_out += g_reply.substr(0, end + 1);
            g_reply.erase(0, end + 3);
        }
        ImGui::TextUnformatted("Connected");
        ImGui::SameLine();
        if (ImGui::Checkbox("Push Changes", &g_push)) g_pushed.clear();  // starts from what is there now
        ImGui::SetItemTooltip("Content files you save here go to the game, which hot-reloads them (scripts, materials, UI, levels, textures...); files with a space in their path are not sent");
        ImGui::SameLine();
        if (with_tip(ImGui::Button("Disconnect"), "Ends the connection (the game keeps running)")) {
            LSD_CLOSE_SOCKET(g_server);
            g_server = kNoSocket;
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##line", g_line, sizeof g_line, ImGuiInputTextFlags_EnterReturnsTrue) && g_line[0] && g_server != kNoSocket) {
            g_out += std::string("> ") + g_line + "\n";
            send_all(g_server, std::string(g_line) + "\n");
            g_line[0] = '\0';
            ImGui::SetKeyboardFocusHere(-1);
        }
    }
    ImGui::BeginChild("##output", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted(g_out.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace lsd::editor::remote
