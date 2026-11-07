#include "lsd/ui/ui.h"

#include "ui_script.h"

#include "lsd/asset/texture.h"
#include "lsd/foundation/log.h"
#include "lsd/reflect/json.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <vector>

namespace lsd::ui {

namespace {

// shortcut: std containers here: per-document / per-geometry bookkeeping, not per-vertex work.

// ---- Rendering through the RHI ----

struct Geometry {
    std::vector<rhi::UiVertex> vertices;
    std::vector<u32> indices;
};

class RenderInterface final : public Rml::RenderInterface {
public:
    rhi::Device* device = nullptr;
    rhi::Device* drawing = nullptr;  // set while a context renders
    f32 viewport[4] = {};
    bool scissor_on = false;
    i32 scissor[4] = {};

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override {
        auto* g = new Geometry();
        g->vertices.resize(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            const Rml::Vertex& v = vertices[i];
            rhi::UiVertex& o = g->vertices[i];
            o.position[0] = v.position.x;
            o.position[1] = v.position.y;
            o.color[0] = v.colour.red;
            o.color[1] = v.colour.green;
            o.color[2] = v.colour.blue;
            o.color[3] = v.colour.alpha;
            o.uv[0] = v.tex_coord.x;
            o.uv[1] = v.tex_coord.y;
        }
        g->indices.assign(indices.begin(), indices.end());
        return reinterpret_cast<Rml::CompiledGeometryHandle>(g);
    }
    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) override {
        const auto* g = reinterpret_cast<const Geometry*>(handle);
        if (!drawing || !g) return;
        rhi::UiDraw d;
        d.vertices = g->vertices.data();
        d.vertex_count = static_cast<u32>(g->vertices.size());
        d.indices = g->indices.data();
        d.index_count = static_cast<u32>(g->indices.size());
        d.texture.value = texture ? static_cast<u32>(texture - 1) : rhi::TextureHandle{}.value;
        d.translation[0] = translation.x;
        d.translation[1] = translation.y;
        std::memcpy(d.viewport, viewport, sizeof viewport);
        if (scissor_on) std::memcpy(d.scissor, scissor, sizeof scissor);
        drawing->draw_ui(d);
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override { delete reinterpret_cast<Geometry*>(handle); }

    // Textures: our handle's value + 1 (0 is "none" to RmlUi). Its colours are premultiplied.
    static Rml::TextureHandle to_rml(rhi::TextureHandle t) { return t.valid() ? static_cast<Rml::TextureHandle>(t.value) + 1 : 0; }
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        asset::TextureData data(default_allocator());
        if (!asset::load_texture(source.c_str(), false, 0, data)) return 0;
        std::vector<u8> pixels(data.mip(0), data.mip(0) + static_cast<usize>(data.width) * data.height * 4);
        for (usize i = 0; i < pixels.size(); i += 4) {
            for (int c = 0; c < 3; ++c) pixels[i + c] = static_cast<u8>((pixels[i + c] * pixels[i + 3] + 127) / 255);
        }
        dimensions = {static_cast<int>(data.width), static_cast<int>(data.height)};
        const void* mips[1] = {pixels.data()};
        return to_rml(device->create_texture(data.width, data.height, 1, rhi::TextureFormat::RGBA8, mips));
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i size) override {
        const void* mips[1] = {source.data()};
        return to_rml(device->create_texture(static_cast<u32>(size.x), static_cast<u32>(size.y), 1, rhi::TextureFormat::RGBA8, mips));
    }
    void ReleaseTexture(Rml::TextureHandle texture) override {
        if (!texture) return;
        rhi::TextureHandle t;
        t.value = static_cast<u32>(texture - 1);
        device->destroy_texture(t);
    }
    void EnableScissorRegion(bool enable) override { scissor_on = enable; }
    void SetScissorRegion(Rml::Rectanglei region) override {
        scissor[0] = region.Left();
        scissor[1] = region.Top();
        scissor[2] = region.Width();
        scissor[3] = region.Height();
    }
};

class SystemInterface final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override {
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    Translator translator = nullptr;
    void* translator_user = nullptr;
    int TranslateString(Rml::String& translated, const Rml::String& input) override {
        // Only whole texts that are keys ("MENU_PLAY", surrounding spaces allowed).
        const usize first = input.find_first_not_of(" \t\r\n"), last = input.find_last_not_of(" \t\r\n");
        const char* t = translator && first != Rml::String::npos ? translator(translator_user, input.substr(first, last - first + 1).c_str()) : nullptr;
        if (!t) {
            translated = input;
            return 0;
        }
        translated = t;
        return 1;
    }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT) LSD_LOG_ERROR("ui", "%s", message.c_str());
        else if (type == Rml::Log::LT_WARNING) LSD_LOG_WARN("ui", "%s", message.c_str());
        return true;
    }
};

RenderInterface* g_render = nullptr;
SystemInterface* g_system = nullptr;
std::vector<std::vector<Rml::byte>>* g_font_data = nullptr;  // kept until Rml::Shutdown
std::unordered_map<Rml::Context*, std::pair<UiContext*, UiContext::Impl*>>* g_contexts = nullptr;

bool read_file(const std::filesystem::path& path, std::vector<Rml::byte>& out) {
    FILE* f = std::fopen(path.generic_string().c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? static_cast<usize>(size) : 0);
    const bool ok = size > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// ---- Inline events: onclick="start_game" ----

class ActionListener final : public Rml::EventListener {
public:
    explicit ActionListener(const Rml::String& action) : action_(action) {}
    void ProcessEvent(Rml::Event& event) override;
    void OnDetach(Rml::Element*) override { delete this; }

private:
    Rml::String action_;
};

class ActionInstancer final : public Rml::EventListenerInstancer {
public:
    Rml::EventListener* InstanceEventListener(const Rml::String& value, Rml::Element*) override { return new ActionListener(value); }
};
ActionInstancer* g_instancer = nullptr;

Rml::Input::KeyIdentifier key_of(SDL_Keycode k) {
    using namespace Rml::Input;
    if (k >= SDLK_A && k <= SDLK_Z) return static_cast<KeyIdentifier>(KI_A + (k - SDLK_A));
    if (k >= SDLK_0 && k <= SDLK_9) return static_cast<KeyIdentifier>(KI_0 + (k - SDLK_0));
    switch (k) {
        case SDLK_RETURN: case SDLK_KP_ENTER: return KI_RETURN;
        case SDLK_ESCAPE: return KI_ESCAPE;
        case SDLK_BACKSPACE: return KI_BACK;
        case SDLK_TAB: return KI_TAB;
        case SDLK_SPACE: return KI_SPACE;
        case SDLK_DELETE: return KI_DELETE;
        case SDLK_LEFT: return KI_LEFT;
        case SDLK_RIGHT: return KI_RIGHT;
        case SDLK_UP: return KI_UP;
        case SDLK_DOWN: return KI_DOWN;
        case SDLK_HOME: return KI_HOME;
        case SDLK_END: return KI_END;
        case SDLK_PAGEUP: return KI_PRIOR;
        case SDLK_PAGEDOWN: return KI_NEXT;
        default: return KI_UNKNOWN;
    }
}

int modifiers() {
    const SDL_Keymod m = SDL_GetModState();
    int out = 0;
    if (m & SDL_KMOD_CTRL) out |= Rml::Input::KM_CTRL;
    if (m & SDL_KMOD_SHIFT) out |= Rml::Input::KM_SHIFT;
    if (m & SDL_KMOD_ALT) out |= Rml::Input::KM_ALT;
    return out;
}

} // namespace

// ---- Library ----

bool init(rhi::Device& device, const char* fonts_folder) {
    if (g_render) return true;
    g_render = new RenderInterface();
    g_render->device = &device;
    g_system = new SystemInterface();
    g_font_data = new std::vector<std::vector<Rml::byte>>();
    g_contexts = new std::unordered_map<Rml::Context*, std::pair<UiContext*, UiContext::Impl*>>();
    Rml::SetRenderInterface(g_render);
    Rml::SetSystemInterface(g_system);
    if (!Rml::Initialise()) {
        LSD_LOG_ERROR("ui", "RmlUi did not start");
        return false;
    }
    g_instancer = new ActionInstancer();
    Rml::Factory::RegisterEventListenerInstancer(g_instancer);

    // Fonts: the project's, then the system's UI font as "sans" (normal and bold).
    u32 loaded = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(fonts_folder, error)) {
        const std::string ext = entry.path().extension().string();
        if ((ext == ".ttf" || ext == ".otf" || ext == ".TTF" || ext == ".OTF") && Rml::LoadFontFace(entry.path().generic_string(), loaded == 0)) ++loaded;
    }
#ifdef _WIN32
    const char* system_fonts[2] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/segoeuib.ttf"};
#else
    const char* system_fonts[2] = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"};
#endif
    for (int i = 0; i < 2; ++i) {
        std::vector<Rml::byte> data;
        if (!read_file(system_fonts[i], data)) continue;
        g_font_data->push_back(std::move(data));
        const auto& kept = g_font_data->back();
        if (Rml::LoadFontFace({kept.data(), kept.size()}, "sans", Rml::Style::FontStyle::Normal, i ? Rml::Style::FontWeight::Bold : Rml::Style::FontWeight::Normal,
                              loaded == 0 && i == 0)) {
            ++loaded;
        }
    }
    // Fonts/Fallback (Unreal's composite font fallbacks): their glyphs fill in what the others lack, so
    // Japanese, Chinese, Korean, Arabic or Thai text shows in any font-family (drop Noto Sans JP etc. there).
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(fonts_folder) / "Fallback", error)) {
        const std::string ext = entry.path().extension().string();
        if ((ext == ".ttf" || ext == ".otf" || ext == ".TTF" || ext == ".OTF") && Rml::LoadFontFace(entry.path().generic_string(), true)) ++loaded;
    }
    if (!loaded) LSD_LOG_WARN("ui", "No fonts: put a .ttf in %s", fonts_folder);
    return true;
}

void set_translator(Translator translator, void* user) {
    if (!g_system) return;
    g_system->translator = translator;
    g_system->translator_user = user;
}

void shutdown() {
    shutdown_scripts();
    if (!g_render) return;
    Rml::Shutdown();
    delete g_instancer;
    delete g_contexts;
    delete g_font_data;
    delete g_system;
    delete g_render;
    g_instancer = nullptr;
    g_contexts = nullptr;
    g_font_data = nullptr;
    g_system = nullptr;
    g_render = nullptr;
}

// ---- Contexts ----

// A value of the "game" data model: a scalar (number, text, boolean), a list (data-for) or a record ({{item.name}}).
struct DataNode {
    enum Kind : u8 { Scalar, List, Record } kind = Scalar;
    Rml::Variant value = Rml::Variant(0.0);
    std::vector<DataNode> items;
    std::map<std::string, DataNode> fields;
    Rml::Variant size;  // a list's {{...size}}
};
// The model's one variable: a record whose fields are made the first time a document (or set_data)
// names them, so scripts need not declare them before a document loads.
struct DataStore {
    std::unordered_map<std::string, DataNode> values;  // node-based: slots stay where they are
};
Rml::DataVariable node_variable(DataNode& node);
class DataValueDefinition final : public Rml::VariableDefinition {
public:
    DataValueDefinition() : VariableDefinition(Rml::DataVariableType::Scalar) {}
    bool Get(void* ptr, Rml::Variant& variant) override {
        variant = *static_cast<Rml::Variant*>(ptr);
        return true;
    }
    bool Set(void* ptr, const Rml::Variant& variant) override {
        *static_cast<Rml::Variant*>(ptr) = variant;
        return true;
    }
};
class DataListDefinition final : public Rml::VariableDefinition {
public:
    DataListDefinition() : VariableDefinition(Rml::DataVariableType::Array) {}
    int Size(void* ptr) override { return static_cast<int>(static_cast<DataNode*>(ptr)->items.size()); }
    Rml::DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override;
};
class DataRecordDefinition final : public Rml::VariableDefinition {
public:
    DataRecordDefinition() : VariableDefinition(Rml::DataVariableType::Struct) {}
    Rml::DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override {
        if (address.name.empty()) return {};
        auto& fields = static_cast<DataNode*>(ptr)->fields;
        return node_variable(fields[address.name]);  // a field it lacks: 0
    }
};
class DataStoreDefinition final : public Rml::VariableDefinition {
public:
    DataStoreDefinition() : VariableDefinition(Rml::DataVariableType::Struct) {}
    Rml::DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override {
        if (address.name.empty()) return {};
        auto& values = static_cast<DataStore*>(ptr)->values;
        return node_variable(values[address.name]);  // not set yet: 0 (bars, counts and data-if read it)
    }
};
DataValueDefinition g_value_definition;
DataListDefinition g_list_definition;
DataRecordDefinition g_record_definition;
DataStoreDefinition g_data_definition;

Rml::DataVariable node_variable(DataNode& node) {
    if (node.kind == DataNode::List || (node.kind == DataNode::Record && node.fields.empty())) return Rml::DataVariable(&g_list_definition, &node);  // {} is an empty list too
    if (node.kind == DataNode::Record) return Rml::DataVariable(&g_record_definition, &node);
    return Rml::DataVariable(&g_value_definition, &node.value);
}

Rml::DataVariable DataListDefinition::Child(void* ptr, const Rml::DataAddressEntry& address) {
    DataNode& list = *static_cast<DataNode*>(ptr);
    if (address.name == "size") {
        list.size = Rml::Variant(static_cast<int>(list.items.size()));
        return Rml::DataVariable(&g_value_definition, &list.size);
    }
    if (address.index < 0 || static_cast<usize>(address.index) >= list.items.size()) return {};
    return node_variable(list.items[static_cast<usize>(address.index)]);
}

// JSON (what lsd.ui_data writes for a table) into a node.
void parse_node(reflect::JsonReader& r, DataNode& out, int depth) {
    out = DataNode{};
    if (depth > 16) return r.fail("lists nested too deep");
    if (r.peek('"')) {
        static char text[4096];
        r.read_string(text, sizeof text);
        out.value = Rml::Variant(Rml::String(text));
    } else if (r.accept('[')) {
        out.kind = DataNode::List;
        if (r.accept(']')) return;
        do {
            out.items.emplace_back();
            parse_node(r, out.items.back(), depth + 1);
        } while (r.ok && r.accept(','));
        r.expect(']');
    } else if (r.accept('{')) {
        out.kind = DataNode::Record;
        if (r.accept('}')) return;
        do {
            char key[128];
            r.read_string(key, sizeof key);
            r.expect(':');
            parse_node(r, out.fields[key], depth + 1);
        } while (r.ok && r.accept(','));
        r.expect('}');
    } else {
        char token[64];
        r.read_token(token, sizeof token);
        if (std::strcmp(token, "true") == 0 || std::strcmp(token, "false") == 0) out.value = Rml::Variant(token[0] == 't');
        else out.value = Rml::Variant(std::atof(token));  // null: 0
    }
}

struct UiContext::Impl {
    Rml::Context* context = nullptr;
    DataStore data;
    Rml::DataModelHandle data_model;
    std::vector<Rml::ElementDocument*> documents;  // by DocumentId (closed: nullptr)
    struct Source {
        Rml::String path;  // a file, or
        Rml::String text;  // a document in memory with `path` as its base
    };
    std::vector<Source> sources;  // what each document was opened from (reload_all)
    std::vector<DocumentScript*> scripts;  // by DocumentId: its .js, or nullptr
    ActionHandler actions = nullptr;
    void* user = nullptr;
    f32 viewport[4] = {0, 0, 1, 1};

    Rml::Element* element(DocumentId d, const char* id) const {
        return d >= 0 && static_cast<usize>(d) < documents.size() && documents[d] ? documents[d]->GetElementById(id) : nullptr;
    }
    DocumentId add(Rml::ElementDocument* doc) {
        if (!doc) return -1;
        doc->Show();
        for (usize i = 0; i < documents.size(); ++i) {
            if (!documents[i]) {
                documents[i] = doc;
                return static_cast<DocumentId>(i);
            }
        }
        documents.push_back(doc);
        return static_cast<DocumentId>(documents.size() - 1);
    }
    void attach_script(UiContext& ui, DocumentId d, const char* document_path) {
        if (scripts.size() < documents.size()) scripts.resize(documents.size(), nullptr);
        delete scripts[d];
        scripts[d] = DocumentScript::load(script_path_for(document_path), ui, d);
        if (scripts[d] && scripts[d]->close_requested) ui.close(d);  // onOpen() closed it
    }
};

namespace {
void ActionListener::ProcessEvent(Rml::Event& event) {
    Rml::Element* target = event.GetCurrentElement();
    Rml::Context* context = target ? target->GetContext() : nullptr;
    auto found = context && g_contexts ? g_contexts->find(context) : decltype(g_contexts->end()){};
    if (!g_contexts || found == g_contexts->end()) return;
    // A range slider sends "change" before it stores its new value: store it now, so scripts read it.
    if (event.GetId() == Rml::EventId::Change && target->GetTagName() == "input" && target->GetAttribute<Rml::String>("type", "") == "range")
        target->SetAttribute("value", event.GetParameter<float>("value", target->GetAttribute<float>("value", 0.0f)));
    UiContext& ui = *found->second.first;
    UiContext::Impl& impl = *found->second.second;
    DocumentId doc = -1;
    for (usize i = 0; i < impl.documents.size(); ++i) {
        if (impl.documents[i] && impl.documents[i] == target->GetOwnerDocument()) doc = static_cast<DocumentId>(i);
    }
    // The document's own script first (Menu.js), then the game's Lua.
    DocumentScript* script = doc >= 0 && static_cast<usize>(doc) < impl.scripts.size() ? impl.scripts[doc] : nullptr;
    if (script && script->has(action_.c_str())) {
        script->call(action_.c_str(), target->GetId().c_str());
        if (script->close_requested) ui.close(doc);
        return;
    }
    if (impl.actions) impl.actions(impl.user, ui, doc, action_.c_str(), target->GetId().c_str());
}
} // namespace

UiContext::UiContext(const char* name) : impl_(new Impl()) {
    impl_->context = g_render ? Rml::CreateContext(name, {1, 1}) : nullptr;
    if (impl_->context) (*g_contexts)[impl_->context] = {this, impl_};
    if (impl_->context) {  // before any document loads: data-model="game" finds it
        if (Rml::DataModelConstructor model = impl_->context->CreateDataModel("game")) {
            model.BindCustomDataVariable("game", Rml::DataVariable(&g_data_definition, &impl_->data));
            impl_->data_model = model.GetModelHandle();
        }
    }
}

void UiContext::set_data(const char* name, f64 number) {
    impl_->data.values[name] = DataNode{DataNode::Scalar, Rml::Variant(number), {}, {}, {}};
    if (impl_->data_model) impl_->data_model.DirtyVariable("game");
}

void UiContext::set_data(const char* name, const char* text) {
    impl_->data.values[name] = DataNode{DataNode::Scalar, Rml::Variant(Rml::String(text)), {}, {}, {}};
    if (impl_->data_model) impl_->data_model.DirtyVariable("game");
}

void UiContext::set_data(const char* name, bool value) {
    impl_->data.values[name] = DataNode{DataNode::Scalar, Rml::Variant(value), {}, {}, {}};
    if (impl_->data_model) impl_->data_model.DirtyVariable("game");
}

void UiContext::set_data_json(const char* name, const char* json) {
    reflect::JsonReader reader{json, json + std::strlen(json)};
    DataNode node;
    parse_node(reader, node, 0);
    if (!reader.ok) return;  // logged; the old value stays
    impl_->data.values[name] = std::move(node);
    if (impl_->data_model) impl_->data_model.DirtyVariable("game");
}

UiContext::~UiContext() {
    for (DocumentScript* s : impl_->scripts) delete s;
    if (impl_->context) {
        g_contexts->erase(impl_->context);
        Rml::RemoveContext(impl_->context->GetName());
    }
    delete impl_;
}

void UiContext::set_actions(ActionHandler handler, void* user) {
    impl_->actions = handler;
    impl_->user = user;
}

DocumentId UiContext::open(const char* path) {
    const DocumentId d = impl_->context ? impl_->add(impl_->context->LoadDocument(path)) : -1;
    if (d >= 0) {
        impl_->sources.resize(impl_->documents.size());
        impl_->sources[d] = {path, ""};
        impl_->attach_script(*this, d, path);
    }
    return d;
}

DocumentId UiContext::open_text(const char* rml, const char* base_path) {
    const DocumentId d = impl_->context ? impl_->add(impl_->context->LoadDocumentFromMemory(rml, base_path)) : -1;
    if (d >= 0) {
        impl_->sources.resize(impl_->documents.size());
        impl_->sources[d] = {base_path, rml};
        impl_->attach_script(*this, d, base_path);
    }
    return d;
}

void UiContext::reload_all() {
    if (!impl_->context) return;
    for (usize i = 0; i < impl_->documents.size(); ++i) {
        if (!impl_->documents[i]) continue;
        impl_->documents[i]->Close();
        const Impl::Source& s = impl_->sources[i];
        Rml::ElementDocument* doc = s.text.empty() ? impl_->context->LoadDocument(s.path) : impl_->context->LoadDocumentFromMemory(s.text, s.path);
        if (doc) doc->Show();
        impl_->documents[i] = doc;
        if (doc) impl_->attach_script(*this, static_cast<DocumentId>(i), s.path.c_str());
    }
}

void UiContext::send_action(DocumentId d, const char* action, const char* element_id) {
    if (impl_->actions) impl_->actions(impl_->user, *this, d, action, element_id);
}

void UiContext::close(DocumentId d) {
    if (d < 0 || static_cast<usize>(d) >= impl_->documents.size() || !impl_->documents[d]) return;
    impl_->documents[d]->Close();
    impl_->documents[d] = nullptr;
    if (static_cast<usize>(d) < impl_->scripts.size()) {
        delete impl_->scripts[d];
        impl_->scripts[d] = nullptr;
    }
}

void UiContext::close_all() {
    for (usize i = 0; i < impl_->documents.size(); ++i) close(static_cast<DocumentId>(i));
    if (impl_->context) impl_->context->Update();  // closed documents are freed on the next update
}

bool UiContext::is_open(DocumentId d) const { return d >= 0 && static_cast<usize>(d) < impl_->documents.size() && impl_->documents[d]; }

u32 UiContext::document_count() const {
    u32 n = 0;
    for (const auto* d : impl_->documents) n += d ? 1u : 0u;
    return n;
}

bool UiContext::set_text(DocumentId d, const char* id, const char* text) {
    Rml::Element* e = impl_->element(d, id);
    if (e) e->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
    return e != nullptr;
}

bool UiContext::set_rml(DocumentId d, const char* id, const char* rml) {
    Rml::Element* e = impl_->element(d, id);
    if (e) e->SetInnerRML(rml);
    return e != nullptr;
}

bool UiContext::set_attribute(DocumentId d, const char* id, const char* name, const char* value) {
    Rml::Element* e = impl_->element(d, id);
    if (e) e->SetAttribute(name, Rml::String(value));
    return e != nullptr;
}

bool UiContext::get_value(DocumentId d, const char* id, char* out, usize capacity) const {
    const Rml::Element* e = impl_->element(d, id);
    if (!e) return false;
    std::snprintf(out, capacity, "%s", e->GetAttribute<Rml::String>("value", "").c_str());
    return true;
}

bool UiContext::set_property(DocumentId d, const char* id, const char* name, const char* value) {
    Rml::Element* e = impl_->element(d, id);
    if (e) e->SetProperty(name, value);
    return e != nullptr;
}

bool UiContext::element_at(f32 x, f32 y, char* id, usize capacity) const {
    if (!impl_->context) return false;
    for (Rml::Element* e = impl_->context->GetElementAtPoint(Rml::Vector2f(x, y)); e; e = e->GetParentNode()) {
        if (e->GetTagName() == "body" || e->GetOwnerDocument() == e) return false;
        if (!e->GetId().empty()) {
            std::snprintf(id, capacity, "%s", e->GetId().c_str());
            return true;
        }
    }
    return false;
}

bool UiContext::element_box(DocumentId d, const char* id, f32 box[4]) const {
    Rml::Element* e = impl_->element(d, id);
    if (!e) return false;
    const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
    box[0] = at.x;
    box[1] = at.y;
    box[2] = size.x;
    box[3] = size.y;
    return true;
}

bool UiContext::get_property(DocumentId d, const char* id, const char* name, char* out, usize capacity) const {
    Rml::Element* e = impl_->element(d, id);
    const Rml::Property* p = e ? e->GetProperty(name) : nullptr;
    if (!p) return false;
    std::snprintf(out, capacity, "%s", p->ToString().c_str());
    return true;
}

void UiContext::navigate(i32 dx, i32 dy) {
    if (!impl_->context) return;
    using namespace Rml::Input;
    const KeyIdentifier key = dy < 0 ? KI_UP : dy > 0 ? KI_DOWN : dx < 0 ? KI_LEFT : KI_RIGHT;
    impl_->context->ProcessKeyDown(key, 0);
    impl_->context->ProcessKeyUp(key, 0);
}

namespace {
// Text fields keep Enter and Space for typing.
bool typing_into(const Rml::Element* e) {
    if (!e) return false;
    const Rml::String& tag = e->GetTagName();
    return tag == "textarea" || tag == "select" || (tag == "input" && e->GetAttribute<Rml::String>("type", "text") == "text");
}
} // namespace

void UiContext::activate() {
    Rml::Element* focus = impl_->context ? impl_->context->GetFocusElement() : nullptr;
    if (focus && focus->GetTagName() != "body" && !typing_into(focus)) focus->Click();
}

void UiContext::update(const f32 viewport[4]) {
    if (!impl_->context) return;
    std::memcpy(impl_->viewport, viewport, sizeof impl_->viewport);
    const Rml::Vector2i size{static_cast<int>(viewport[2]), static_cast<int>(viewport[3])};
    if (impl_->context->GetDimensions() != size && size.x > 0 && size.y > 0) impl_->context->SetDimensions(size);
    impl_->context->Update();
}

void UiContext::render(rhi::Device& device) {
    if (!impl_->context || !g_render) return;
    g_render->drawing = &device;
    std::memcpy(g_render->viewport, impl_->viewport, sizeof g_render->viewport);
    g_render->scissor_on = false;
    impl_->context->Render();
    g_render->drawing = nullptr;
}

bool UiContext::process_event(const SDL_Event& event) {
    Rml::Context* c = impl_->context;
    if (!c) return false;
    const f32* v = impl_->viewport;
    bool passed = true;  // RmlUi returns true when it did NOT use the event
    switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            passed = c->ProcessMouseMove(static_cast<int>(event.motion.x - v[0]), static_cast<int>(event.motion.y - v[1]), modifiers());
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const int button = event.button.button == SDL_BUTTON_LEFT ? 0 : event.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            c->ProcessMouseMove(static_cast<int>(event.button.x - v[0]), static_cast<int>(event.button.y - v[1]), modifiers());
            passed = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? c->ProcessMouseButtonDown(button, modifiers()) : c->ProcessMouseButtonUp(button, modifiers());
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: passed = c->ProcessMouseWheel(Rml::Vector2f{0.0f, -event.wheel.y}, modifiers()); break;
        case SDL_EVENT_KEY_DOWN: {
            const SDL_Keycode k = event.key.key;
            const Rml::Element* focus = c->GetFocusElement();
            if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && focus && focus->GetTagName() != "body" && !typing_into(focus)) {
                activate();  // a focused button, as if clicked
                return true;
            }
            passed = c->ProcessKeyDown(key_of(k), modifiers());
            break;
        }
        case SDL_EVENT_KEY_UP: passed = c->ProcessKeyUp(key_of(event.key.key), modifiers()); break;
        case SDL_EVENT_TEXT_INPUT: passed = c->ProcessTextInput(Rml::String(event.text.text)); break;
        default: break;
    }
    return !passed;
}

bool UiContext::hovering() const {
    const Rml::Element* e = impl_->context ? impl_->context->GetHoverElement() : nullptr;
    return e && e->GetTagName() != "body" && e->GetTagName() != "#root";
}

} // namespace lsd::ui
