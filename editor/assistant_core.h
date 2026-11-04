#pragma once
// LSD Assistant's text side, without UI or network (tested in editor_tests): the request each
// provider expects, its reply read back, code blocks that name a file, and which paths may be written.
#include "lsd/foundation/types.h"

#include <string>
#include <vector>

namespace lsd::editor::assistant {

enum Provider : u32 { kAnthropic = 0, kOpenAICompatible = 1 };

struct Settings {
    u32 provider = kAnthropic;
    std::string endpoint;  // empty: https://api.anthropic.com or https://api.openai.com/v1
    std::string model = "claude-opus-5-5";
    std::string key;
    u32 effort = 1;  // Claude: 0 low, 1 medium, 2 high
};

// One turn of the chat. `raw`: an assistant turn's content exactly as the API returned it (Anthropic:
// the JSON array, thinking blocks included), sent back unchanged so the history only ever grows.
struct Turn {
    bool user = true;
    std::string text;
    std::string raw;
};

std::string request_url(const Settings& settings);
std::vector<std::string> request_headers(const Settings& settings);  // "Name: value" lines
std::string request_body(const Settings& settings, const std::string& system, const std::vector<Turn>& turns);

struct Reply {
    bool ok = false;
    std::string text;   // what to show
    std::string raw;    // the assistant turn to send back (Anthropic content array; else the text)
    std::string error;  // why not ok
};
Reply parse_reply(const Settings& settings, int http_status, const std::string& body);

// A reply's fenced code blocks; `path` is set when the opening line names a file
// ("```lua Content/Script/Door.lua").
struct CodeBlock {
    std::string language, path, code;
};
std::vector<CodeBlock> code_blocks(const std::string& text);

// A path the assistant may write: relative, inside Content/ or Source/, no "..", no drive or root.
bool writable_path(const std::string& path);

std::string json_escape(const std::string& text);  // with the quotes

} // namespace lsd::editor::assistant
