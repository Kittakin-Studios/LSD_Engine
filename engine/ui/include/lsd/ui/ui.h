#pragma once
// Game UI (Unreal's UMG, written as HTML / CSS): RmlUi documents (.rml with .rcss style sheets)
// drawn through the RHI. A UiContext is one screen of UI (the game's, or an editor preview); it
// opens documents, takes SDL input and draws over the 3D view.
//
// Fonts: every .ttf / .otf in Content/Fonts (by their own family names); the system's UI font is
// also there as "sans", so a document always has something to draw text with.
//
// Events: an inline attribute such as onclick="start_game" (also onchange, onsubmit, onmouseover...)
// calls the context's action handler with "start_game", which the game sends to Lua. A document
// can also have its own JavaScript beside it (Menu.rml + Menu.js, QuickJS): its functions answer
// first; see src/ui_script.h for its API.
#include "lsd/rhi/rhi.h"

union SDL_Event;

namespace lsd::ui {

using DocumentId = i32;  // -1: none

class UiContext;

// Called when an element's on<event> attribute fires: `action` is the attribute's text.
using ActionHandler = void (*)(void* user, UiContext& context, DocumentId document, const char* action, const char* element_id);

// The UI library for the whole program (before any context, after the device).
bool init(rhi::Device& device, const char* fonts_folder = "Content/Fonts");

// Localisation: called for every text in a document as it loads; return the translation (or
// nullptr to keep the text). Text whose whole content is a string table key gets translated.
using Translator = const char* (*)(void* user, const char* text);
void set_translator(Translator translator, void* user);
void shutdown();

class UiContext {
public:
    explicit UiContext(const char* name);
    ~UiContext();
    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    void set_actions(ActionHandler handler, void* user);
    // Documents. A path is relative to the working folder (Content/UI/Menu.rml); `base` names the
    // folder that relative links (style sheets, images) in text documents are found from.
    DocumentId open(const char* path);
    DocumentId open_text(const char* rml, const char* base_path);
    void close(DocumentId document);
    void close_all();
    void reload_all();  // every open document again, same handles (after a language change)
    bool is_open(DocumentId document) const;
    // Elements by id: their text (escaped), inner markup, or an attribute (class, style, src...).
    bool set_text(DocumentId document, const char* element_id, const char* text);
    bool set_rml(DocumentId document, const char* element_id, const char* rml);
    bool set_attribute(DocumentId document, const char* element_id, const char* name, const char* value);
    bool get_value(DocumentId document, const char* element_id, char* out, usize capacity) const;  // inputs
    // One style property ("left", "120px"), leaving the element's other styles alone.
    bool set_property(DocumentId document, const char* element_id, const char* name, const char* value);

    // The UI designer canvas: the element under a point (context pixels) that has an id, or its
    // nearest ancestor with one (never the body); an element's border box (x, y, width, height in
    // context pixels); a computed style property as text ("40px", "absolute").
    bool element_at(f32 x, f32 y, char* element_id, usize capacity) const;
    bool element_box(DocumentId document, const char* element_id, f32 box[4]) const;
    bool get_property(DocumentId document, const char* element_id, const char* name, char* out, usize capacity) const;

    // Data bindings (UMG's property bindings, RmlUi data models): a document whose root (or body) has
    // data-model="game" can show `{{game.health}}`, `data-style-width="game.health + '%'"`,
    // `data-if="game.alive"`, `data-class-low="game.health < 25"`...; these set a value by name and
    // every open document using it updates. Names not set yet read as 0.
    void set_data(const char* name, f64 number);
    void set_data(const char* name, const char* text);
    void set_data(const char* name, bool value);
    // A list or record as JSON (arrays, objects, numbers, strings, booleans): data-for="item : game.<name>" repeats
    // an element per item, {{item.field}} reads a record's field, {{game.<name>.size}} counts a list.
    void set_data_json(const char* name, const char* json);

    // An action as if an element's on<event> fired it (a document script's ui.send): to the handler.
    void send_action(DocumentId document, const char* action, const char* element_id);

    // Gamepad / keyboard menus: move the focus (RCSS nav: auto) and press the focused element.
    void navigate(i32 dx, i32 dy);
    void activate();

    // `viewport`: x, y, width, height in window pixels (where it is drawn; input is relative to it).
    void update(const f32 viewport[4]);
    void render(rhi::Device& device);
    // True if the UI used the event (a click on a button, typing into a field).
    bool process_event(const SDL_Event& event);
    bool hovering() const;  // the mouse is over an element (not just the document's empty body)
    u32 document_count() const;

    struct Impl;

private:
    Impl* impl_;
};

} // namespace lsd::ui
