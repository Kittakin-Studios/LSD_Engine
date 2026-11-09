#pragma once
// A UI document's own script (QuickJS): Menu.rml runs Menu.js from the same folder when it opens,
// like a Widget Blueprint's event graph. Its functions answer the document's on<event>="name"
// attributes before Lua does; onOpen() runs once the document is up.
//
// JavaScript API:
//   ui.setText(id, text), ui.setAttribute(id, name, value), ui.setProperty(id, name, value)
//   ui.value(id) -> text            a text field's value
//   ui.playAnimation(id, "pop 0.4s cubic-out")   plays (replays) @keyframes; "none" stops
//   ui.animate(id, property, value [, seconds, tween])   tweens one property ("opacity", "0", 0.5, "cubic-in-out")
//   ui.close()                      closes this document
//   ui.send(action [, element_id])  passes an action on to Lua (functions in gameplay scripts)
//   console.log(...), console.error(...)   to the Output Log
#include "lsd/ui/ui.h"

#include <string>

struct JSContext;

namespace lsd::ui {

class DocumentScript {
public:
    // nullptr if there is no script beside the document (or it failed to run: see the Output Log).
    static DocumentScript* load(const std::string& script_path, UiContext& ui, DocumentId document);
    ~DocumentScript();
    DocumentScript(const DocumentScript&) = delete;
    DocumentScript& operator=(const DocumentScript&) = delete;

    bool has(const char* function) const;
    void call(const char* function, const char* element_id);  // logs errors

    UiContext& ui;
    DocumentId document;
    bool close_requested = false;  // ui.close(): closed after the event, not inside it

private:
    DocumentScript(UiContext& context, DocumentId d) : ui(context), document(d) {}
    JSContext* ctx_ = nullptr;
    std::string name_;
};

// The .js beside a document path ("UI/Menu.rml" -> "UI/Menu.js").
std::string script_path_for(const char* document_path);
void shutdown_scripts();  // frees the shared JavaScript runtime

} // namespace lsd::ui
