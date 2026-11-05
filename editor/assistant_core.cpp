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

std::string request_body(const Settings& st, const std::string& system, const std::vector<Turn>& turns) {
    std::string b = "{\"model\":" + json_escape(st.model);
    if (st.provider == kAnthropic) {
        // The system prompt (role, engine guide, API) is the same every time: cached.
        b += ",\"max_tokens\":16000,\"system\":[{\"type\":\"text\",\"text\":" + json_escape(system) + ",\"cache_control\":{\"type\":\"ephemeral\"}}]";
        if (takes_effort(st.model)) {
            static const char* const kEffort[] = {"low", "medium", "high"};
            b += ",\"output_config\":{\"effort\":\"" + std::string(kEffort[st.effort < 3 ? st.effort : 1]) + "\"}";
        }
        if (takes_fallbacks(st)) b += ",\"fallbacks\":\"default\"";
        b += ",\"messages\":[";
        for (usize i = 0; i < turns.size(); ++i) {
            const Turn& t = turns[i];
            if (i) b += ",";
            b += t.user ? "{\"role\":\"user\",\"content\":" + json_escape(t.text) + "}"
                        : "{\"role\":\"assistant\",\"content\":" + (t.raw.empty() ? json_escape(t.text) : t.raw) + "}";
        }
        return b + "]}";
    }
    b += ",\"messages\":[{\"role\":\"system\",\"content\":" + json_escape(system) + "}";
    for (const Turn& t : turns) b += std::string(",{\"role\":\"") + (t.user ? "user" : "assistant") + "\",\"content\":" + json_escape(t.text) + "}";
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
        for (const Json& block : content->values) {  // text blocks only: thinking blocks stay in `raw`
            if (block.str("type") == "text") r.text += (r.text.empty() ? "" : "\n\n") + block.str("text");
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
    }
    r.ok = true;
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

bool writable_path(const std::string& path) {
    if (path.empty() || path.find("..") != std::string::npos || path.find(':') != std::string::npos || path[0] == '/' || path[0] == '\\') return false;
    return starts(path, "Content/") || starts(path, "Source/");
}

} // namespace lsd::editor::assistant
