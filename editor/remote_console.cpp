#include "remote_console.h"

#include "lsd/core/console.h"
#include "lsd/foundation/log.h"

#include <imgui.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

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
        const int n = ::send(s, text.data() + sent, static_cast<int>(text.size() - sent), 0);
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
        const int n = ::recv(s, chunk, sizeof chunk, 0);
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

} // namespace

bool listen(u16 port) {
    if (!start_sockets()) return false;
    g_listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listener == kNoSocket) return false;
    const sockaddr_in a = localhost(port);
    if (::bind(g_listener, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0 || ::listen(g_listener, 1) != 0) {
        LSD_LOG_WARN("remote", "Remote console: port %u is in use", port);
        LSD_CLOSE_SOCKET(g_listener);
        g_listener = kNoSocket;
        return false;
    }
    set_non_blocking(g_listener);
    LSD_LOG_INFO("remote", "Remote console listening on 127.0.0.1:%u", port);
    return true;
}

void poll() {
    if (g_listener == kNoSocket) return;
    if (g_client == kNoSocket) {
        g_client = ::accept(g_listener, nullptr, nullptr);
        if (g_client == kNoSocket) return;
        set_non_blocking(g_client);
        g_in.clear();
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
        std::string reply;
        g_capture = &reply;
        g_previous_sink = log::sink(&g_previous_user);
        log::set_sink(capture_sink, nullptr);
        console::execute(line.c_str());  // it logs unknown commands itself
        log::set_sink(g_previous_sink, g_previous_user);
        g_capture = nullptr;
        send_all(g_client, reply + ".\n");
    }
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
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("Port", &g_port, 0);
        ImGui::SameLine();
        if (ImGui::Button("Connect") && start_sockets()) {
            g_server = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            const sockaddr_in a = localhost(static_cast<u16>(g_port));
            if (g_server != kNoSocket && ::connect(g_server, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0) {
                set_non_blocking(g_server);
                g_out += "Connected to 127.0.0.1:" + std::to_string(g_port) + "\n";
            } else {
                if (g_server != kNoSocket) LSD_CLOSE_SOCKET(g_server);
                g_server = kNoSocket;
                g_out += "No game is listening there. Start it with -remote (File > Package Project, then run it with -remote).\n";
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
        if (ImGui::Button("Disconnect")) {
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
