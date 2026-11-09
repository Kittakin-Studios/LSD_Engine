#include "ui_script.h"

#include "lsd/foundation/log.h"

#include <quickjs.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace lsd::ui {

namespace {

JSRuntime* g_runtime = nullptr;
std::chrono::steady_clock::time_point g_deadline;

// A script that runs too long (an endless loop) is stopped instead of freezing the game.
int interrupt(JSRuntime*, void*) { return std::chrono::steady_clock::now() > g_deadline ? 1 : 0; }

JSRuntime* runtime() {
    if (!g_runtime) {
        g_runtime = JS_NewRuntime();
        JS_SetMaxStackSize(g_runtime, 256 * 1024);  // well inside the main thread's stack
        JS_SetMemoryLimit(g_runtime, 64 * 1024 * 1024);
        JS_SetInterruptHandler(g_runtime, interrupt, nullptr);
    }
    return g_runtime;
}

void start_clock() { g_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500); }

DocumentScript& self(JSContext* ctx) { return *static_cast<DocumentScript*>(JS_GetContextOpaque(ctx)); }

std::string arg(JSContext* ctx, int argc, JSValueConst* argv, int i) {
    if (i >= argc) return {};
    const char* s = JS_ToCString(ctx, argv[i]);
    std::string out = s ? s : "";
    JS_FreeCString(ctx, s);
    return out;
}

void log_exception(JSContext* ctx, const std::string& where) {
    JSValue e = JS_GetException(ctx);
    const char* text = JS_ToCString(ctx, e);
    JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
    const char* trace = JS_IsUndefined(stack) ? nullptr : JS_ToCString(ctx, stack);
    LSD_LOG_ERROR("ui", "%s: %s\n%s", where.c_str(), text ? text : "(error)", trace ? trace : "");
    JS_FreeCString(ctx, trace);
    JS_FreeValue(ctx, stack);
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, e);
}

JSValue js_set_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DocumentScript& s = self(ctx);
    return JS_NewBool(ctx, s.ui.set_text(s.document, arg(ctx, argc, argv, 0).c_str(), arg(ctx, argc, argv, 1).c_str()));
}
JSValue js_set_attribute(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DocumentScript& s = self(ctx);
    return JS_NewBool(ctx, s.ui.set_attribute(s.document, arg(ctx, argc, argv, 0).c_str(), arg(ctx, argc, argv, 1).c_str(), arg(ctx, argc, argv, 2).c_str()));
}
JSValue js_set_property(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DocumentScript& s = self(ctx);
    return JS_NewBool(ctx, s.ui.set_property(s.document, arg(ctx, argc, argv, 0).c_str(), arg(ctx, argc, argv, 1).c_str(), arg(ctx, argc, argv, 2).c_str()));
}
JSValue js_play_animation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DocumentScript& s = self(ctx);
    return JS_NewBool(ctx, s.ui.play_animation(s.document, arg(ctx, argc, argv, 0).c_str(), arg(ctx, argc, argv, 1).c_str()));
}
JSValue js_animate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DocumentScript& s = self(ctx);
    double seconds = 0.3;
    if (argc > 3) JS_ToFloat64(ctx, &seconds, argv[3]);
    return JS_NewBool(ctx, s.ui.animate(s.document, arg(ctx, argc, argv, 0).c_str(), arg(ctx, argc, argv, 1).c_str(), arg(ctx, argc, argv, 2).c_str(),
                                        static_cast<f32>(seconds), arg(ctx, argc, argv, 4).c_str()));
}
JSValue js_value(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DocumentScript& s = self(ctx);
    char out[512] = "";
    if (!s.ui.get_value(s.document, arg(ctx, argc, argv, 0).c_str(), out, sizeof out)) return JS_UNDEFINED;
    return JS_NewString(ctx, out);
}
JSValue js_close(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    self(ctx).close_requested = true;
    return JS_UNDEFINED;
}
JSValue js_send(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DocumentScript& s = self(ctx);
    s.ui.send_action(s.document, arg(ctx, argc, argv, 0).c_str(), arg(ctx, argc, argv, 1).c_str());
    return JS_UNDEFINED;
}
template <bool kError>
JSValue js_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string line;
    for (int i = 0; i < argc; ++i) line += (i ? " " : "") + arg(ctx, argc, argv, i);
    if (kError) LSD_LOG_ERROR("ui", "%s", line.c_str());
    else LSD_LOG_INFO("ui", "%s", line.c_str());
    return JS_UNDEFINED;
}

} // namespace

std::string script_path_for(const char* document_path) {
    std::filesystem::path p(document_path);
    p.replace_extension(".js");
    return p.generic_string();
}

DocumentScript* DocumentScript::load(const std::string& path, UiContext& ui, DocumentId document) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return nullptr;
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    const std::string source = text.str();

    auto* script = new DocumentScript(ui, document);
    script->name_ = path;
    JSContext* ctx = JS_NewContext(runtime());
    script->ctx_ = ctx;
    JS_SetContextOpaque(ctx, script);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ui_object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ui_object, "setText", JS_NewCFunction(ctx, js_set_text, "setText", 2));
    JS_SetPropertyStr(ctx, ui_object, "setAttribute", JS_NewCFunction(ctx, js_set_attribute, "setAttribute", 3));
    JS_SetPropertyStr(ctx, ui_object, "setProperty", JS_NewCFunction(ctx, js_set_property, "setProperty", 3));
    JS_SetPropertyStr(ctx, ui_object, "value", JS_NewCFunction(ctx, js_value, "value", 1));
    JS_SetPropertyStr(ctx, ui_object, "playAnimation", JS_NewCFunction(ctx, js_play_animation, "playAnimation", 2));
    JS_SetPropertyStr(ctx, ui_object, "animate", JS_NewCFunction(ctx, js_animate, "animate", 5));
    JS_SetPropertyStr(ctx, ui_object, "close", JS_NewCFunction(ctx, js_close, "close", 0));
    JS_SetPropertyStr(ctx, ui_object, "send", JS_NewCFunction(ctx, js_send, "send", 2));
    JS_SetPropertyStr(ctx, global, "ui", ui_object);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_log<false>, "log", 1));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, js_log<true>, "error", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);

    start_clock();
    JSValue result = JS_Eval(ctx, source.c_str(), source.size(), path.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        log_exception(ctx, path);
        JS_FreeValue(ctx, result);
        delete script;
        return nullptr;
    }
    JS_FreeValue(ctx, result);
    if (script->has("onOpen")) script->call("onOpen", "");
    return script;
}

DocumentScript::~DocumentScript() {
    if (ctx_) JS_FreeContext(ctx_);
}

bool DocumentScript::has(const char* function) const {
    JSValue global = JS_GetGlobalObject(ctx_);
    JSValue f = JS_GetPropertyStr(ctx_, global, function);
    const bool found = JS_IsFunction(ctx_, f);
    JS_FreeValue(ctx_, f);
    JS_FreeValue(ctx_, global);
    return found;
}

void DocumentScript::call(const char* function, const char* element_id) {
    JSValue global = JS_GetGlobalObject(ctx_);
    JSValue f = JS_GetPropertyStr(ctx_, global, function);
    JSValue id = JS_NewString(ctx_, element_id);
    start_clock();
    JSValue result = JS_Call(ctx_, f, global, 1, &id);
    if (JS_IsException(result)) log_exception(ctx_, name_ + " " + function + "()");
    JS_FreeValue(ctx_, result);
    JS_FreeValue(ctx_, id);
    JS_FreeValue(ctx_, f);
    JS_FreeValue(ctx_, global);
}

void shutdown_scripts() {
    if (g_runtime) JS_FreeRuntime(g_runtime);
    g_runtime = nullptr;
}

} // namespace lsd::ui
