#include "assistant_core.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lsd::editor::assistant {

namespace {

// A JSON value from a reply, with where it sits in the text (to send an Anthropic content array back
// byte for byte). Objects keep keys and values in two vectors (a pair of a type being defined is not allowed).
struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    std::string text;  // strings
    f64 number = 0;
    bool flag = false;
    std::vector<std::string> keys;
    std::vector<Json> values;  // object values, or array items
    usize begin = 0, end = 0;
    const Json* get(const char* key) const {
        for (usize i = 0; type == Object && i < keys.size(); ++i) {
            if (keys[i] == key) return &values[i];
        }
        return nullptr;
    }
    std::string str(const char* key) const {
        const Json* v = get(key);
        return v && v->type == String ? v->text : std::string();
    }
};

struct Parser {
    const std::string& s;
    usize i = 0;
    bool ok = true;
    void ws() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    void utf8(std::string& out, u32 cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) out += static_cast<char>(0xC0 | (cp >> 6)), out += static_cast<char>(0x80 | (cp & 0x3F));
        else if (cp < 0x10000) out += static_cast<char>(0xE0 | (cp >> 12)), out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
        else out += static_cast<char>(0xF0 | (cp >> 18)), out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)), out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    std::string string() {
        std::string out;
        ++i;  // the opening quote
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) break;
            c = s[i++];
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    u32 cp = static_cast<u32>(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {  // a surrogate pair
                        const u32 low = static_cast<u32>(std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16));
                        i += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: out += c; break;  // \" \\ \/
            }
        }
        if (i >= s.size()) ok = false;
        ++i;
        return out;
    }
    Json value(int depth) {
        Json v;
        ws();
        v.begin = i;
        if (i >= s.size() || depth > 64) {
            ok = false;
            return v;
        }
        const char c = s[i];
        if (c == '{' || c == '[') {
            const bool object = c == '{';
            v.type = object ? Json::Object : Json::Array;
            ++i;
            ws();
            if (i < s.size() && s[i] == (object ? '}' : ']')) {
                ++i;
            } else {
                while (ok) {
                    ws();
                    if (object) {
                        if (i >= s.size() || s[i] != '"') {
                            ok = false;
                            break;
                        }
                        v.keys.push_back(string());
                        ws();
                        if (i >= s.size() || s[i++] != ':') {
                            ok = false;
                            break;
                        }
                    }
                    v.values.push_back(value(depth + 1));
                    ws();
                    if (i < s.size() && s[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == (object ? '}' : ']')) {
                        ++i;
                        break;
                    }
                    ok = false;
                }
            }
        } else if (c == '"') {
            v.type = Json::String;
            v.text = string();
        } else if (s.compare(i, 4, "true") == 0 || s.compare(i, 5, "false") == 0) {
            v.type = Json::Bool;
            v.flag = s[i] == 't';
            i += v.flag ? 4 : 5;
        } else if (s.compare(i, 4, "null") == 0) {
            i += 4;
        } else {
            char* stop = nullptr;
            v.type = Json::Number;
            v.number = std::strtod(s.c_str() + i, &stop);
            if (stop == s.c_str() + i) ok = false;
            i = static_cast<usize>(stop - s.c_str());
        }
        v.end = i;
        return v;
    }
};

bool starts(const std::string& s, const char* prefix) { return s.compare(0, std::strlen(prefix), prefix) == 0; }
bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// Which request fields a Claude model takes: effort (current models), server-side refusal fallback
// (the models that have one, on Anthropic's own endpoint).
bool takes_effort(const std::string& m) {
    return has(m, "opus") || has(m, "fable") || has(m, "mythos") || has(m, "sonnet-5") || has(m, "sonnet-4-6") || has(m, "haiku-5");
}
bool takes_fallbacks(const Settings& st) {
    const std::string& m = st.model;
    return st.endpoint.empty() && (starts(m, "claude-opus-5") || starts(m, "claude-fable-5") || starts(m, "claude-sonnet-5-5"));
}

} // namespace

std::string json_escape(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof hex, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += hex;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

std::string base64(const u8* data, usize size) {
    static const char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (usize i = 0; i < size; i += 3) {
        const u32 n = static_cast<u32>(data[i]) << 16 | (i + 1 < size ? static_cast<u32>(data[i + 1]) << 8 : 0u) | (i + 2 < size ? data[i + 2] : 0u);
        out += kDigits[(n >> 18) & 63];
        out += kDigits[(n >> 12) & 63];
        out += i + 1 < size ? kDigits[(n >> 6) & 63] : '=';
        out += i + 2 < size ? kDigits[n & 63] : '=';
    }
    return out;
}

std::string request_url(const Settings& st) {
    std::string base = st.endpoint.empty() ? (st.provider == kAnthropic ? "https://api.anthropic.com" : "https://api.openai.com/v1") : st.endpoint;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return st.provider == kAnthropic ? base + "/v1/messages" : base + "/chat/completions";
}

std::vector<std::string> request_headers(const Settings& st) {
    std::vector<std::string> h{"Content-Type: application/json"};
    if (st.provider == kAnthropic) {
        h.push_back("x-api-key: " + st.key);
        h.push_back("anthropic-version: 2026-10-21");
        if (takes_fallbacks(st)) h.push_back("anthropic-beta: server-side-fallback-2026-07-01");
    } else if (!st.key.empty()) {  // local servers (Ollama, LM Studio) take none
        h.push_back("Authorization: Bearer " + st.key);
    }
    return h;
}

const ToolSpec* tool_specs(u32& count) {
    static const ToolSpec kTools[] = {
        {"console", "Run one LSD_Engine editor console command and get what it logged. Examples: play, stop, save, open <level>, help [prefix], "
                    "asset.Open <name>, scene.List, validate, stat dump. play / open take effect on the next frame: read_log after.",
         R"({"type":"object","properties":{"command":{"type":"string","description":"The console line, e.g. \"play\""}},"required":["command"]})"},
        {"lua", "Run Lua in the running game (only while playing: run the console command play first). The lsd API is there; lsd.log output comes back.",
         R"({"type":"object","properties":{"code":{"type":"string","description":"Lua code; several lines allowed"}},"required":["code"]})"},
        {"list_objects", "Every object in the open level (or the playing world): name, parent, world position, components.", R"({"type":"object","properties":{}})"},
        {"list_assets", "Assets in Content/: name, type, path. The filter matches any part of a line.",
         R"({"type":"object","properties":{"filter":{"type":"string","description":"Text to match, e.g. \"Script\""}}})"},
        {"read_log", "The latest Output Log lines (script errors, warnings, lsd.log output), oldest first.",
         R"({"type":"object","properties":{"lines":{"type":"integer","description":"How many lines, 50 if left out"}}})"},
        {"read_file", "A text file in Content/ or Source/ (a script, a UI document, game code), up to 64 KB.",
         R"({"type":"object","properties":{"path":{"type":"string","description":"e.g. Content/Weapon/Weapon.lua"}},"required":["path"]})"},
        {"write_file", "Write a whole code file in Content/ or Source/ (.lua .js .lsdgraph .rml .rcss .cpp .c .h .hpp, CMakeLists.txt). Saved scripts reload by themselves.",
         R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]})"},
    };
    count = static_cast<u32>(sizeof kTools / sizeof kTools[0]);
    return kTools;
}

std::string tool_arg(const std::string& input, const char* key) {
    Parser p{input};
    const Json v = p.value(0);
    const Json* field = p.ok ? v.get(key) : nullptr;
    if (!field) return {};
    if (field->type == Json::Number) return std::to_string(static_cast<long long>(field->number));
    return field->type == Json::String ? field->text : input.substr(field->begin, field->end - field->begin);
}

std::string request_body(const Settings& st, const std::string& system, const std::vector<Turn>& turns, bool tools) {
    std::string b = "{\"model\":" + json_escape(st.model) + ",\"stream\":true";
    u32 tool_count = 0;
    const ToolSpec* specs = tool_specs(tool_count);
    if (st.provider == kAnthropic) {
        // The system prompt (role, engine guide, API) is the same every time: cached.
        b += ",\"max_tokens\":16000,\"system\":[{\"type\":\"text\",\"text\":" + json_escape(system) + ",\"cache_control\":{\"type\":\"ephemeral\"}}]";
        if (takes_effort(st.model)) {
            static const char* const kEffort[] = {"low", "medium", "high"};
            b += ",\"output_config\":{\"effort\":\"" + std::string(kEffort[st.effort < 3 ? st.effort : 1]) + "\"}";
        }
        if (takes_fallbacks(st)) b += ",\"fallbacks\":\"default\"";
        if (tools) {
            b += ",\"tools\":[";
            for (u32 i = 0; i < tool_count; ++i) {
                b += std::string(i ? "," : "") + "{\"name\":\"" + specs[i].name + "\",\"description\":" + json_escape(specs[i].description) +
                     ",\"input_schema\":" + specs[i].schema + "}";
            }
            b += "]";
        }
        b += ",\"messages\":[";
        for (usize i = 0; i < turns.size(); ++i) {
            const Turn& t = turns[i];
            if (i) b += ",";
            if (t.user && !t.results.empty()) {  // what the tools answered, one block per call
                b += "{\"role\":\"user\",\"content\":[";
                for (usize k = 0; k < t.results.size(); ++k) {
                    b += std::string(k ? "," : "") + "{\"type\":\"tool_result\",\"tool_use_id\":" + json_escape(t.results[k].id) + ",\"content\":" +
                         json_escape(t.results[k].content) + (t.results[k].error ? ",\"is_error\":true}" : "}");
                }
                b += "]}";
            } else if (t.user && !t.image.empty()) {  // a picture, then the words about it
                b += "{\"role\":\"user\",\"content\":[{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/png\",\"data\":\"" + t.image +
                     "\"}},{\"type\":\"text\",\"text\":" + json_escape(t.text) + "}]}";
            } else {
                b += t.user ? "{\"role\":\"user\",\"content\":" + json_escape(t.text) + "}"
                            : "{\"role\":\"assistant\",\"content\":" + (t.raw.empty() ? json_escape(t.text) : t.raw) + "}";
            }
        }
        return b + "]}";
    }
    if (tools) {
        b += ",\"tools\":[";
        for (u32 i = 0; i < tool_count; ++i) {
            b += std::string(i ? "," : "") + "{\"type\":\"function\",\"function\":{\"name\":\"" + specs[i].name + "\",\"description\":" +
                 json_escape(specs[i].description) + ",\"parameters\":" + specs[i].schema + "}}";
        }
        b += "]";
    }
    b += ",\"messages\":[{\"role\":\"system\",\"content\":" + json_escape(system) + "}";
    for (const Turn& t : turns) {
        if (t.user && !t.results.empty()) {
            for (const ToolResult& r : t.results) b += ",{\"role\":\"tool\",\"tool_call_id\":" + json_escape(r.id) + ",\"content\":" + json_escape(r.content) + "}";
            continue;
        }
        if (t.user && !t.image.empty()) {  // OpenAI-compatible vision: the text, then the picture as a data URL
            b += ",{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":" + json_escape(t.text) +
                 "},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64," + t.image + "\"}}]}";
            continue;
        }
        b += std::string(",{\"role\":\"") + (t.user ? "user" : "assistant") + "\",\"content\":" + json_escape(t.text);
        if (!t.user && !t.calls.empty()) {
            b += ",\"tool_calls\":[";
            for (usize k = 0; k < t.calls.size(); ++k) {
                b += std::string(k ? "," : "") + "{\"id\":" + json_escape(t.calls[k].id) + ",\"type\":\"function\",\"function\":{\"name\":" +
                     json_escape(t.calls[k].name) + ",\"arguments\":" + json_escape(t.calls[k].input.empty() ? "{}" : t.calls[k].input) + "}}";
            }
            b += "]";
        }
        b += "}";
    }
    return b + "]}";
}

Reply parse_reply(const Settings& st, int http_status, const std::string& body) {
    Reply r;
    Parser p{body};
    const Json root = p.value(0);
    if (!p.ok || root.type != Json::Object) {
        r.error = http_status ? "The reply could not be read (HTTP " + std::to_string(http_status) + "): " + body.substr(0, 300) : "No reply: is the endpoint right, and is curl installed?";
        return r;
    }
    if (const Json* err = root.get("error")) {  // both APIs: {"error": {"message": ...}} (or a string)
        r.error = err->type == Json::String ? err->text : err->str("message");
        if (r.error.empty()) r.error = "HTTP " + std::to_string(http_status);
        return r;
    }
    if (st.provider == kAnthropic) {
        const Json* content = root.get("content");
        if (root.str("stop_reason") == "refusal") {
            const Json* details = root.get("stop_details");
            r.error = "The model declined this request" + (details && !details->str("explanation").empty() ? ": " + details->str("explanation") : std::string("."));
            return r;
        }
        if (!content || content->type != Json::Array) {
            r.error = "The reply has no content";
            return r;
        }
        for (const Json& block : content->values) {  // text blocks and tool calls: thinking blocks stay in `raw`
            if (block.str("type") == "text") r.text += (r.text.empty() ? "" : "\n\n") + block.str("text");
            const Json* input = block.get("input");
            if (block.str("type") == "tool_use") r.calls.push_back({block.str("id"), block.str("name"), input ? body.substr(input->begin, input->end - input->begin) : "{}"});
        }
        r.raw = body.substr(content->begin, content->end - content->begin);
        if (root.str("stop_reason") == "max_tokens") r.text += "\n\n(The reply was cut off at its length limit: ask it to continue.)";
    } else {
        const Json* choices = root.get("choices");
        const Json* message = choices && choices->type == Json::Array && !choices->values.empty() ? choices->values[0].get("message") : nullptr;
        if (!message) {
            r.error = "The reply has no message";
            return r;
        }
        r.text = message->str("content");
        r.raw = r.text;
        if (const Json* calls = message->get("tool_calls"); calls && calls->type == Json::Array) {
            for (const Json& c : calls->values) {
                const Json* fn = c.get("function");
                if (fn) r.calls.push_back({c.str("id"), fn->str("name"), fn->str("arguments")});
            }
        }
    }
    r.ok = true;
    return r;
}

Reply parse_stream(const Settings& st, int http_status, const std::string& body) {
    struct Block {
        std::string type, text, signature, raw;  // raw: the start event's block, for types sent back as they came
        std::string id, name, input;             // tool_use: its arguments arrive as pieces of JSON
    };
    std::vector<Block> blocks;
    std::vector<ToolCall> openai_calls;  // by the stream's index
    std::string stop_reason, explanation, error, openai_text;
    bool any = false, done = false;
    for (usize at = 0, end; (end = body.find('\n', at)) != std::string::npos; at = end + 1) {  // whole lines only
        std::string line = body.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!starts(line, "data:")) continue;
        line.erase(0, line[5] == ' ' ? 6 : 5);
        any = true;
        if (line == "[DONE]") {
            done = true;
            continue;
        }
        Parser p{line};
        const Json e = p.value(0);
        if (!p.ok || e.type != Json::Object) continue;
        if (const Json* err = e.get("error")) error = err->type == Json::String ? err->text : err->str("message");
        if (st.provider != kAnthropic) {
            const Json* choices = e.get("choices");
            if (!choices || choices->type != Json::Array || choices->values.empty()) continue;
            if (const Json* delta = choices->values[0].get("delta")) {
                openai_text += delta->str("content");
                const Json* calls = delta->get("tool_calls");
                for (usize k = 0; calls && calls->type == Json::Array && k < calls->values.size(); ++k) {
                    const Json& c = calls->values[k];
                    const Json* slot = c.get("index");
                    const usize i = slot && slot->number >= 0 && slot->number < 64 ? static_cast<usize>(slot->number) : k;
                    if (openai_calls.size() <= i) openai_calls.resize(i + 1);
                    if (!c.str("id").empty()) openai_calls[i].id = c.str("id");
                    if (const Json* fn = c.get("function")) {
                        openai_calls[i].name += fn->str("name");
                        openai_calls[i].input += fn->str("arguments");
                    }
                }
            }
            done = done || !choices->values[0].str("finish_reason").empty();
            continue;
        }
        const std::string type = e.str("type");
        const Json* delta = e.get("delta");
        const Json* index = e.get("index");
        Block* block = index && index->number >= 0 && static_cast<usize>(index->number) < blocks.size() ? &blocks[static_cast<usize>(index->number)] : nullptr;
        if (type == "content_block_start") {
            if (const Json* cb = e.get("content_block")) {
                blocks.push_back({cb->str("type"), cb->str("type") == "thinking" ? cb->str("thinking") : cb->str("text"), cb->str("signature"),
                                  line.substr(cb->begin, cb->end - cb->begin), cb->str("id"), cb->str("name"), ""});
            }
        } else if (type == "content_block_delta" && block && delta) {
            block->text += delta->str("text") + delta->str("thinking");
            block->signature += delta->str("signature");
            block->input += delta->str("partial_json");
        } else if (type == "message_delta" && delta) {
            stop_reason = delta->str("stop_reason");
            const Json* details = delta->get("stop_details") ? delta->get("stop_details") : e.get("stop_details");
            if (details) explanation = details->str("explanation");
        } else if (type == "message_stop") {
            done = true;
        }
    }
    if (!any) return parse_reply(st, http_status, body);
    Reply r;
    if (st.provider != kAnthropic) {
        r.text = r.raw = openai_text;
        for (const ToolCall& c : openai_calls) {
            if (!c.name.empty()) r.calls.push_back(c);
        }
    } else {
        r.raw = "[";
        for (const Block& b : blocks) {
            if (b.type == "text") r.text += (r.text.empty() ? "" : "\n\n") + b.text;
            const std::string input = b.input.empty() ? "{}" : b.input;
            if (b.type == "tool_use") r.calls.push_back({b.id, b.name, input});
            if (r.raw.size() > 1) r.raw += ",";
            r.raw += b.type == "text"       ? "{\"type\":\"text\",\"text\":" + json_escape(b.text) + "}"
                     : b.type == "thinking" ? "{\"type\":\"thinking\",\"thinking\":" + json_escape(b.text) + ",\"signature\":" + json_escape(b.signature) + "}"
                     : b.type == "tool_use" ? "{\"type\":\"tool_use\",\"id\":" + json_escape(b.id) + ",\"name\":" + json_escape(b.name) + ",\"input\":" + input + "}"
                                            : b.raw;  // redacted thinking and others arrive whole
        }
        r.raw += "]";
        if (stop_reason == "refusal") error = "The model declined this request" + (explanation.empty() ? std::string(".") : ": " + explanation);
        if (stop_reason == "max_tokens") r.text += "\n\n(The reply was cut off at its length limit: ask it to continue.)";
    }
    r.error = !error.empty() ? error : done ? std::string() : "The connection closed before the reply finished.";
    r.ok = r.error.empty();
    return r;
}

std::vector<CodeBlock> code_blocks(const std::string& text) {
    std::vector<CodeBlock> out;
    usize at = 0;
    while ((at = text.find("```", at)) != std::string::npos) {
        const usize line_end = text.find('\n', at);
        if (line_end == std::string::npos) break;
        const usize close = text.find("\n```", line_end);
        if (close == std::string::npos) break;
        CodeBlock b;
        std::string head = text.substr(at + 3, line_end - at - 3);
        while (!head.empty() && (head.back() == '\r' || head.back() == ' ')) head.pop_back();
        const usize space = head.find(' ');
        b.language = head.substr(0, space);
        if (space != std::string::npos) {
            b.path = head.substr(space + 1);
            while (!b.path.empty() && b.path.front() == ' ') b.path.erase(b.path.begin());
            if (b.path.find('/') == std::string::npos && b.path.find('.') == std::string::npos) b.path.clear();  // a title, not a file
        }
        b.code = text.substr(line_end + 1, close - line_end);
        out.push_back(b);
        at = close + 4;
    }
    return out;
}

bool readable_path(const std::string& path) {
    if (path.empty() || path.find("..") != std::string::npos || path.find(':') != std::string::npos || path[0] == '/' || path[0] == '\\') return false;
    return starts(path, "Content/") || starts(path, "Source/");
}

bool writable_path(const std::string& path) {
    if (!readable_path(path)) return false;
    // Code only: scripts, UI documents, game code (meshes, textures, levels, data assets are not its job).
    const usize dot = path.rfind('.');
    const std::string ext = dot == std::string::npos ? std::string() : path.substr(dot);
    for (const char* code : {".lua", ".js", ".lsdgraph", ".rml", ".rcss", ".cpp", ".c", ".h", ".hpp"}) {
        if (ext == code) return true;
    }
    return path.size() >= 14 && path.compare(path.size() - 14, 14, "CMakeLists.txt") == 0;
}

} // namespace lsd::editor::assistant
