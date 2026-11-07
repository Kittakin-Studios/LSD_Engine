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

// Tools the model may call (the editor runs them and sends back what they gave): the same set as the
// Claude MCP bridge, over the editor's console.
struct ToolCall {
    std::string id, name;
    std::string input;  // its arguments, JSON text
};
struct ToolResult {
    std::string id, content;
    bool error = false;
};
struct ToolSpec {
    const char* name;
    const char* description;
    const char* schema;  // JSON Schema of its arguments
};
const ToolSpec* tool_specs(u32& count);
std::string tool_arg(const std::string& input, const char* key);  // a string (or number as text) argument; "" if missing

// One turn of the chat. `raw`: an assistant turn's content exactly as the API returned it (Anthropic:
// the JSON array, thinking blocks and tool calls included), sent back unchanged so the history only
// ever grows. A user turn with `results` answers the previous turn's `calls`.
struct Turn {
    bool user = true;
    std::string text;
    std::string raw;
    std::vector<ToolCall> calls;
    std::vector<ToolResult> results;
    std::string image;  // a user turn's picture: PNG bytes as base64 (a screenshot of the editor); "" = none
};

std::string base64(const u8* data, usize size);
std::string request_url(const Settings& settings);
std::vector<std::string> request_headers(const Settings& settings);  // "Name: value" lines
// `tools`: offer the tools (the editor's setting allows them).
std::string request_body(const Settings& settings, const std::string& system, const std::vector<Turn>& turns, bool tools = false);

struct Reply {
    bool ok = false;
    std::string text;   // what to show
    std::string raw;    // the assistant turn to send back (Anthropic content array; else the text)
    std::string error;  // why not ok
    std::vector<ToolCall> calls;  // tools it asks the editor to run before it goes on
};
Reply parse_reply(const Settings& settings, int http_status, const std::string& body);
// A streamed reply (server-sent events; requests ask for `"stream":true`) read from what has arrived so
// far: `text` grows as events come, `ok` once the stream's end was seen. A body that is not a stream
// (an error reply) goes through parse_reply.
Reply parse_stream(const Settings& settings, int http_status, const std::string& body);

// A reply's fenced code blocks; `path` is set when the opening line names a file
// ("```lua Content/Script/Door.lua").
struct CodeBlock {
    std::string language, path, code;
};
std::vector<CodeBlock> code_blocks(const std::string& text);

// A path the assistant may write: relative, inside Content/ or Source/, no "..", no drive or root, and a
// code file (.lua .js .lsdgraph .rml .rcss .cpp .c .h .hpp, CMakeLists.txt): it writes code, not assets.
bool writable_path(const std::string& path);
// A path its read_file tool may open: relative, inside Content/ or Source/, no "..", no drive or root.
bool readable_path(const std::string& path);

std::string json_escape(const std::string& text);  // with the quotes

} // namespace lsd::editor::assistant
