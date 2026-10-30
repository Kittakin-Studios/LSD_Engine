#pragma once
// Game UI (Unreal's UMG, written as HTML / CSS): RmlUi documents (.rml with .rcss style sheets)
// drawn through the RHI. A UiContext is one screen of UI (the game's, or an editor preview); it
// opens documents, takes SDL input and draws over the 3D view.
//
// Fonts: every .ttf / .otf in Content/Fonts (by their own family names); the system's UI font is
// also there as "sans", so a document always has something to draw text with.
//
// Events: an inline attribute such as onclick="start_game" (also onchange, onsubmit, onmouseover...)
// calls the context's action handler with "start_game", which the game sends to Lua.
#include "lsd/rhi/rhi.h"

union SDL_Event;

namespace lsd::ui {

using DocumentId = i32;  // -1: none

class UiContext;

// Called when an element's on<event> attribute fires: `action` is the attribute's text.
using ActionHandler = void (*)(void* user, UiContext& context, DocumentId document, const char* action, const char* element_id);

// The UI library for the whole program (before any context, after the device).
bool init(rhi::Device& device, const char* fonts_folder = "Content/Fonts");
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
    bool is_open(DocumentId document) const;
    // Elements by id: their text (escaped), inner markup, or an attribute (class, style, src...).
    bool set_text(DocumentId document, const char* element_id, const char* text);
    bool set_rml(DocumentId document, const char* element_id, const char* rml);
    bool set_attribute(DocumentId document, const char* element_id, const char* name, const char* value);
    bool get_value(DocumentId document, const char* element_id, char* out, usize capacity) const;  // inputs

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
