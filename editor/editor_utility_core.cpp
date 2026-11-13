#include "editor_utility_core.h"

#include "lsd/foundation/log.h"
#include "lsd/world/level.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace lsd::editor {

namespace {

// lsd.editor on top of lsd._editor (the host callback). Each utility runs in its own environment, kept
// after the run so its panel's buttons find its functions and variables.
constexpr char kEditorApi[] = R"(
local E, envs = {}, {}
lsd.editor = E
local function lines(text)
  local t = {}
  for l in (text or ""):gmatch("[^\n]+") do t[#t + 1] = l end
  return t
end
function E.selection()
  local t = {}
  for _, l in ipairs(lines(lsd._editor("selection"))) do t[#t + 1] = tonumber(l) end
  return t
end
function E.select(list)
  local s = {}
  for i, e in ipairs(list or {}) do s[i] = string.format("%d", e) end
  lsd._editor("select", table.concat(s, "\n"))
end
function E.open_asset(name) return lsd._editor("open_asset", name) ~= nil end
function E.console(line) lsd._editor("console", line) end
function E.panel(title, widgets)
  local env = envs[lsd._editor("current")]
  local out = {tostring(title)}
  for _, w in ipairs(widgets or {}) do
    local kind, name = w[1] or "label", w[3] or ""
    if env and kind ~= "button" and kind ~= "label" and name ~= "" and env[name] == nil then env[name] = w[4] end
    local v = env and name ~= "" and kind ~= "button" and env[name]
    out[#out + 1] = table.concat({kind, tostring(w[2] or ""), name, (v == nil or v == false) and "" or tostring(v)}, "\t")
  end
  lsd._editor("panel", table.concat(out, "\n"))
end
function lsd._utility_run(path, code)
  local env = setmetatable({}, {__index = _G})
  local chunk, err = load(code, "@" .. path, "t", env)
  if not chunk then error(err, 0) end
  envs[path] = env
  chunk()
  local self = {}
  for k, v in pairs(rawget(env, "defaults") or {}) do self[k] = v end  -- a visual script's variables
  env.self = self
  local entry = rawget(env, "run") or rawget(env, "on_start")
  if entry then entry(self) end
end
function lsd._utility_call(path, fn)
  local env = envs[path]
  local f = env and rawget(env, fn)
  if type(f) ~= "function" then error(path .. " has no function " .. fn, 0) end
  f(env.self)
end
function lsd._utility_set(path, name, value)
  local env = envs[path]
  if env then env[name] = value; env.self[name] = value end
end
)";

UtilityWidget::Kind widget_kind(const std::string& text) {
    if (text == "button") return UtilityWidget::Kind::Button;
    if (text == "checkbox") return UtilityWidget::Kind::Checkbox;
    if (text == "number") return UtilityWidget::Kind::Number;
    if (text == "text") return UtilityWidget::Kind::Text;
    return UtilityWidget::Kind::Label;
}

// "title\nkind\tlabel\tname\tvalue\n..." from lsd.editor.panel.
// shortcut: tabs or line breaks inside labels and text values are cut there; escape them if a tool needs them.
UtilityPanel parse_panel(const std::string& script, const char* text) {
    UtilityPanel panel;
    panel.script = script;
    const char* line = text;
    bool first = true;
    while (*line) {
        const char* end = std::strchr(line, '\n');
        if (!end) end = line + std::strlen(line);
        const std::string row(line, end);
        line = *end ? end + 1 : end;
        if (first) {
            std::snprintf(panel.title, sizeof panel.title, "%s", row.c_str());
            first = false;
            continue;
        }
        std::string field[4];
        usize at = 0;
        for (int i = 0; i < 4; ++i) {
            const usize tab = i < 3 ? row.find('\t', at) : std::string::npos;
            field[i] = row.substr(at, tab == std::string::npos ? std::string::npos : tab - at);
            if (tab == std::string::npos) break;
            at = tab + 1;
        }
        UtilityWidget w;
        w.kind = widget_kind(field[0]);
        std::snprintf(w.label, sizeof w.label, "%s", field[1].c_str());
        std::snprintf(w.name, sizeof w.name, "%s", field[2].c_str());
        w.on = !field[3].empty();
        w.number = std::strtod(field[3].c_str(), nullptr);
        std::snprintf(w.text, sizeof w.text, "%s", field[3].c_str());
        panel.widgets.push_back(w);
    }
    return panel;
}

} // namespace

std::string lua_quote(const char* text) {
    const std::string body = text;
    std::string close = "]]";
    while ((body + close).find(close) != body.size()) close.insert(1, "=");  // ]=], ]==] ... until it first closes at the end
    std::string open = close;
    open.front() = '[';
    open.back() = '[';
    return open + "\n" + body + close;  // a long bracket drops the line break right after it
}

void EditorUtilities::start(world::World& world, const UtilityHooks& hooks) {
    world_ = &world;
    hooks_ = hooks;
    script::ScriptHost host;
    host.user = this;
    host.editor = editor_call;
    host.spawn = [](void* user, const char* name, Vec3 position) {
        const UtilityHooks& h = static_cast<EditorUtilities*>(user)->hooks_;
        return h.spawn ? h.spawn(h.user, name, position) : world::Entity{};
    };
    host.assets = [](void* user, const char* type, const char* folder, char* out, usize capacity) {
        const UtilityHooks& h = static_cast<EditorUtilities*>(user)->hooks_;
        return h.assets ? h.assets(h.user, type, folder, out, capacity) : 0u;
    };
    host.raycast = [](void* user, Vec3 origin, Vec3 direction, f32 distance, world::Entity& hit, Vec3& point, Vec3& normal) {
        const UtilityHooks& h = static_cast<EditorUtilities*>(user)->hooks_;
        return h.raycast && h.raycast(h.user, origin, direction, distance, hit, point, normal);
    };
    host.sweep = [](void* user, int shape, Vec3 size, Vec3 from, Vec3 to, world::Entity& hit, Vec3& point, Vec3& normal) {
        const UtilityHooks& h = static_cast<EditorUtilities*>(user)->hooks_;
        return h.sweep && h.sweep(h.user, shape, size, from, to, hit, point, normal);
    };
    host.overlap = [](void* user, int shape, Vec3 size, Vec3 at, world::Entity* out, u32 capacity) {
        const UtilityHooks& h = static_cast<EditorUtilities*>(user)->hooks_;
        return h.overlap ? h.overlap(h.user, shape, size, at, out, capacity) : 0u;
    };
    scripts_.start(world, host, nullptr, nullptr);
    scripts_.run(kEditorApi, "=lsd editor");
}

bool EditorUtilities::editor_call(void* user, const char* command, const char* arg, char* out, usize capacity) {
    auto& self = *static_cast<EditorUtilities*>(user);
    const UtilityHooks& h = self.hooks_;
    if (std::strcmp(command, "current") == 0) {
        std::snprintf(out, capacity, "%s", self.current_.c_str());
    } else if (std::strcmp(command, "selection") == 0) {
        world::Entity list[1024];
        const u32 n = h.selection ? h.selection(h.user, list, 1024) : 0;
        usize used = 0;
        for (u32 i = 0; i < n && used + 12 < capacity; ++i) used += static_cast<usize>(std::snprintf(out + used, capacity - used, "%u\n", list[i].value));
    } else if (std::strcmp(command, "select") == 0) {
        world::Entity list[1024];
        u32 n = 0;
        for (const char* p = arg; *p && n < 1024;) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 10);
            if (end == p) break;
            if (self.world_->alive(world::Entity{static_cast<u32>(v)})) list[n++] = world::Entity{static_cast<u32>(v)};
            p = *end ? end + 1 : end;
        }
        if (h.select) h.select(h.user, list, n);
    } else if (std::strcmp(command, "open_asset") == 0) {
        return h.open_asset && h.open_asset(h.user, arg);
    } else if (std::strcmp(command, "console") == 0) {
        if (h.console) h.console(h.user, arg);
    } else if (std::strcmp(command, "panel") == 0) {
        UtilityPanel panel = parse_panel(self.current_, arg);
        for (UtilityPanel& p : self.panels_) {
            if (p.script != panel.script) continue;
            p = std::move(panel);  // running it again rebuilds its panel (and shows it again)
            return true;
        }
        self.panels_.push_back(std::move(panel));
    } else {
        return false;
    }
    return true;
}

std::string EditorUtilities::level_text() const {
    Array<char> text(default_allocator());
    world::save_level(*world_, text);
    return std::string(text.data(), text.size());
}

bool EditorUtilities::changed_since(const std::string& before) const { return level_text() != before; }

bool EditorUtilities::run_code(const char* path, const char* code) {
    if (!world_) return false;
    const std::string before = level_text();
    current_ = path;
    scripts_.run(("lsd._utility_run(" + lua_quote(path) + ", " + lua_quote(code) + ")").c_str(), "=editor utility");
    return changed_since(before);
}

bool EditorUtilities::run_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        LSD_LOG_ERROR("editor", "Editor utility: cannot read %s", path);
        return false;
    }
    const std::string code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return run_code(path, code.c_str());
}

bool EditorUtilities::press(u32 panel, u32 widget) {
    if (!world_ || panel >= panels_.size() || widget >= panels_[panel].widgets.size()) return false;
    const std::string before = level_text();
    current_ = panels_[panel].script;  // a copy: the function may declare the panel again
    const std::string call = "lsd._utility_call(" + lua_quote(current_.c_str()) + ", " + lua_quote(panels_[panel].widgets[widget].name) + ")";
    scripts_.run(call.c_str(), "=editor utility");
    return changed_since(before);
}

void EditorUtilities::push_value(u32 panel, u32 widget) {
    if (!world_ || panel >= panels_.size() || widget >= panels_[panel].widgets.size()) return;
    const UtilityWidget& w = panels_[panel].widgets[widget];
    std::string value;
    if (w.kind == UtilityWidget::Kind::Checkbox) {
        value = w.on ? "true" : "false";
    } else if (w.kind == UtilityWidget::Kind::Number) {
        char number[40];
        std::snprintf(number, sizeof number, "%.17g", w.number);
        value = number;
    } else if (w.kind == UtilityWidget::Kind::Text) {
        value = lua_quote(w.text);
    } else {
        return;
    }
    scripts_.run(("lsd._utility_set(" + lua_quote(panels_[panel].script.c_str()) + ", " + lua_quote(w.name) + ", " + value + ")").c_str(), "=editor utility");
}

} // namespace lsd::editor
