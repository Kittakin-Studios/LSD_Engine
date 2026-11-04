// lsd_mcp: the bridge of the Claude MCP plugin (Editor Preferences > Plugins > Claude MCP).
// Claude Code starts it as an MCP server over stdio (one JSON-RPC message per line); each tool call
// becomes one console line sent to the editor's remote console on 127.0.0.1, and what that line
// logged is the tool's answer. Usage: lsd_mcp [--port 7780]
// Nothing but protocol messages may go to stdout; diagnostics go to stderr.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
#define LSD_CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kNoSocket = -1;
#define LSD_CLOSE_SOCKET ::close
#endif

namespace {

// ---- Just enough JSON: parse a request, write strings ----

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    std::string text;  // a string's value, or a number / true / false as written
    // Objects: keys[i] names values[i] (two vectors: a pair of a type still being defined is not allowed by libstdc++).
    std::vector<std::string> keys;
    std::vector<Json> values;
    std::vector<Json> items;  // arrays

    const Json* get(const char* key) const {
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) return &values[i];
        }
        return nullptr;
    }
    std::string string(const char* key, const char* fallback = "") const {
        const Json* v = get(key);
        return v && (v->type == Type::String || v->type == Type::Number) ? v->text : fallback;
    }
};

struct Parser {
    const char* p;
    const char* end;
    bool ok = true;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }
    static void put_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    unsigned hex4() {
        if (end - p < 4) {
            ok = false;
            return 0;
        }
        const std::string digits(p, 4);
        p += 4;
        return static_cast<unsigned>(std::strtoul(digits.c_str(), nullptr, 16));
    }
    std::string string() {
        std::string out;
        ++p;  // the opening quote
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\' || p >= end) {
                out += c;
                continue;
            }
            c = *p++;
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned code = hex4();
                if (code >= 0xD800 && code < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {  // a surrogate pair
                    p += 2;
                    code = 0x10000 + ((code - 0xD800) << 10) + (hex4() - 0xDC00);
                }
                put_utf8(out, code);
                break;
            }
            default: out += c;  // \" \\ \/
            }
        }
        if (p >= end) ok = false;
        else ++p;
        return out;
    }
    Json value(int depth = 0) {
        Json v;
        ws();
        if (p >= end || depth > 64) {
            ok = false;
            return v;
        }
        if (*p == '"') {
            v.type = Json::Type::String;
            v.text = string();
        } else if (*p == '{' || *p == '[') {
            const bool object = *p++ == '{';
            v.type = object ? Json::Type::Object : Json::Type::Array;
            ws();
            if (p < end && *p == (object ? '}' : ']')) {
                ++p;
                return v;
            }
            while (ok) {
                ws();
                if (object) {
                    if (p >= end || *p != '"') {
                        ok = false;
                        break;
                    }
                    std::string key = string();
                    ws();
                    if (p >= end || *p++ != ':') {
                        ok = false;
                        break;
                    }
                    v.keys.push_back(std::move(key));
                    v.values.push_back(value(depth + 1));
                } else {
                    v.items.push_back(value(depth + 1));
                }
                ws();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == (object ? '}' : ']')) ++p;
                else ok = false;
                break;
            }
        } else {
            const char* start = p;
            while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
            v.text.assign(start, p);
            v.type = v.text == "null" ? Json::Type::Null : v.text == "true" || v.text == "false" ? Json::Type::Bool : Json::Type::Number;
            if (v.text.empty()) ok = false;
        }
        return v;
    }
};

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    return out + "\"";
}

// ---- The editor's remote console ----

int g_port = 7780;
Socket g_editor = kNoSocket;  // kept open between calls, so the plugin shows Claude as connected

bool connect_editor() {
    g_editor = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<unsigned short>(g_port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (g_editor == kNoSocket || ::connect(g_editor, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
        if (g_editor != kNoSocket) LSD_CLOSE_SOCKET(g_editor);
        g_editor = kNoSocket;
        return false;
    }
#ifdef _WIN32
    const DWORD timeout = 20000;  // the editor answers on its next frame; a long stall means it is busy or paused
    setsockopt(g_editor, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
#else
    timeval timeout{20, 0};
    setsockopt(g_editor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
#endif
    return true;
}

// Sends `line` and reads the reply up to its closing "." line. False if the connection broke.
bool exchange(const std::string& line, std::string& reply, bool& timed_out) {
    for (size_t sent = 0; sent < line.size();) {
        const int n = static_cast<int>(::send(g_editor, line.data() + sent, static_cast<int>(line.size() - sent), 0));  // ssize_t on POSIX
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    char chunk[4096];
    for (;;) {
        const int n = static_cast<int>(::recv(g_editor, chunk, sizeof chunk, 0));
        if (n <= 0) {
#ifdef _WIN32
            timed_out = n < 0 && WSAGetLastError() == WSAETIMEDOUT;
#else
            timed_out = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
#endif
            return false;
        }
        reply.append(chunk, static_cast<size_t>(n));
        if (reply == ".\n" || (reply.size() >= 3 && reply.compare(reply.size() - 3, 3, "\n.\n") == 0)) return true;
    }
}

// Sends one console line and returns what it logged; false (with the reason in `out`) if the editor
// is not there.
bool run_line(std::string line, std::string& out) {
    for (char& c : line) {
        if (c == '\n' || c == '\r') c = ' ';  // one console line
    }
    line += '\n';
    std::string reply;
    bool timed_out = false;
    bool done = false;
    // A kept connection may have been closed by an editor restart: reconnect once, but never resend
    // a line the editor may already have run.
    for (int attempt = 0; attempt < 2 && !done; ++attempt) {
        if (g_editor == kNoSocket && !connect_editor()) break;
        done = exchange(line, reply, timed_out);
        if (done) break;
        LSD_CLOSE_SOCKET(g_editor);
        g_editor = kNoSocket;
        if (timed_out || !reply.empty()) break;
    }
    if (!done && reply.empty() && !timed_out) {
        out = "The LSD_Engine editor is not listening on 127.0.0.1:" + std::to_string(g_port) +
              ". Open the editor, then Edit > Editor Preferences > Plugins > Claude MCP and tick Enabled (same port).";
        return false;
    }
    if (!done) {
        out = "The editor did not answer in 20 seconds (busy, paused at a breakpoint, or a modal dialog is open). Partial output:\n" + reply;
        return false;
    }
    reply.resize(reply.size() - 2);  // the closing "."
    out = reply.empty() ? "(done; it logged nothing. Use read_log to see what happened later, e.g. after play)" : reply;
    return true;
}

// ---- MCP ----

constexpr char kInstructions[] =
    "LSD_Engine editor (Unreal-like, Z-up, metres). Gameplay code is Lua scripts (.lua) and visual scripts "
    "(.lsdgraph) in Content/: write the files directly, they reload in the editor and during Play. Attach a "
    "script with a Script component (lsd.add(e, \"Script\", ...) in Lua, or ask the user to pick it in Details). "
    "The lsd API is in engine/script/include/lsd/script/script.h; how-to in docs/How to code in LSD_Engine.md and "
    "docs/CODING.md. Use list_assets and list_objects to see the project, console \"play\" then read_log to test, "
    "lua only while playing, console \"stop\" afterwards. Never send console \"debug.Crash\".";

constexpr char kTools[] = R"json([
{"name":"console","description":"Run one LSD_Engine editor console command and return what it logged. Examples: play, stop, simulate, save, open <level>, help [prefix], graph.Open <asset>, mesh.Open <asset>, validate, HighResShot 2, viewmode wireframe, stat fps. Effects of play / open happen on the next frame: call read_log after.","inputSchema":{"type":"object","properties":{"command":{"type":"string","description":"The console line, e.g. \"play\""}},"required":["command"]}},
{"name":"lua","description":"Run Lua in the running game (only while playing: send console play first). The lsd API is available (lsd.find, lsd.position, lsd.get / lsd.set components...). lsd.log output is returned.","inputSchema":{"type":"object","properties":{"code":{"type":"string","description":"Lua code; several lines allowed"}},"required":["code"]}},
{"name":"list_objects","description":"Every object in the open level (or the playing world): name, parent, world position in metres, components.","inputSchema":{"type":"object","properties":{}}},
{"name":"list_assets","description":"Assets in Content/: name, type (Mesh, Script, Graph, Data:InputAction...), path. Optional filter text matches any part of the line.","inputSchema":{"type":"object","properties":{"filter":{"type":"string","description":"Text to match, e.g. \"Script\" or \"Door\""}}}},
{"name":"read_log","description":"The latest Output Log lines (script errors, warnings, lsd.log output), oldest first.","inputSchema":{"type":"object","properties":{"lines":{"type":"integer","description":"How many lines (default 50)"}}}}
])json";

void reply(const Json* id, const std::string& result) {
    std::cout << "{\"jsonrpc\":\"2.0\",\"id\":" << (id->type == Json::Type::String ? quote(id->text) : id->text) << ",\"result\":" << result << "}\n";
    std::cout.flush();
}

void reply_error(const Json* id, int code, const std::string& message) {
    std::cout << "{\"jsonrpc\":\"2.0\",\"id\":" << (id->type == Json::Type::String ? quote(id->text) : id->text) << ",\"error\":{\"code\":" << code
              << ",\"message\":" << quote(message) << "}}\n";
    std::cout.flush();
}

std::string tool_result(const std::string& text, bool error) {
    return std::string("{\"content\":[{\"type\":\"text\",\"text\":") + quote(text) + "}],\"isError\":" + (error ? "true" : "false") + "}";
}

std::string call_tool(const std::string& name, const Json* args, bool& error) {
    const Json none;
    const Json& a = args ? *args : none;
    std::string line;
    if (name == "console") {
        line = a.string("command");
        if (line.empty()) {
            error = true;
            return "console needs a command";
        }
    } else if (name == "lua") {
        std::string code = a.string("code");
        for (char& c : code) {
            if (c == '\n') c = '\x1f';  // the editor's lua command turns these back into line breaks
            else if (c == '\r') c = ' ';
        }
        line = "lua " + code;
    } else if (name == "list_objects") {
        line = "scene.List";
    } else if (name == "list_assets") {
        line = "asset.List " + a.string("filter");
    } else if (name == "read_log") {
        line = "log.Tail " + a.string("lines", "50");
    } else {
        error = true;
        return "Unknown tool " + name;
    }
    if (line.size() > 16000) {
        error = true;
        return "Too long for one console line (16 000 characters): write it to a .lua file in Content/ instead.";
    }
    std::string out;
    error = !run_line(line, out);
    return out;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0) g_port = std::atoi(argv[i + 1]);
    }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::fprintf(stderr, "lsd_mcp: no sockets\n");
        return 1;
    }
#endif
    std::string message;
    while (std::getline(std::cin, message)) {
        if (!message.empty() && message.back() == '\r') message.pop_back();
        if (message.empty()) continue;
        Parser parser{message.data(), message.data() + message.size()};
        const Json request = parser.value();
        const Json* id = request.get("id");
        if (!parser.ok || request.type != Json::Type::Object) {
            std::cout << "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}\n";
            std::cout.flush();
            continue;
        }
        if (!id) continue;  // a notification (initialized, cancelled...): nothing to answer
        const std::string method = request.string("method");
        const Json* params = request.get("params");
        if (method == "initialize") {
            const std::string version = params ? params->string("protocolVersion", "2025-06-18") : "2025-06-18";
            reply(id, "{\"protocolVersion\":" + quote(version) + ",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"lsd-engine\",\"version\":\"0.1.0\"},\"instructions\":" +
                          quote(kInstructions) + "}");
        } else if (method == "ping") {
            reply(id, "{}");
        } else if (method == "tools/list") {
            std::string tools = kTools;
            std::erase(tools, '\n');  // one message per line
            reply(id, "{\"tools\":" + tools + "}");
        } else if (method == "tools/call" && params) {
            bool error = false;
            const std::string text = call_tool(params->string("name"), params->get("arguments"), error);
            reply(id, tool_result(text, error));
        } else {
            reply_error(id, -32601, "Method not found: " + method);
        }
    }
    if (g_editor != kNoSocket) LSD_CLOSE_SOCKET(g_editor);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
