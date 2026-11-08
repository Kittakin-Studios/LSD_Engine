#pragma once
// Small edits to RML text by element id, for the UI designer canvas: the text stays the source of
// truth, so dragging an element or typing in its Details rewrites only that element's tag.
// shortcut: a text scan, not a parser; it expects id="..." in double quotes, and the inner text
// helpers only handle elements whose content is plain text. A DOM writer can replace it.
#include <string>
#include <vector>

namespace lsd::editor {

// The tag that has id="<id>": its start ('<') and one past its end ('>'); false if none.
bool rml_find_tag(const std::string& rml, const char* id, size_t& begin, size_t& end);
// An inline style property ("left"), or "" if the tag's style attribute does not set it.
std::string rml_style(const std::string& rml, const char* id, const char* property);
// Sets (or, with an empty value, removes) one inline style property, keeping the others.
bool rml_set_style(std::string& rml, const char* id, const char* property, const char* value);
std::string rml_attribute(const std::string& rml, const char* id, const char* name);
bool rml_set_attribute(std::string& rml, const char* id, const char* name, const char* value);  // empty: removes
// The text between the element's tags, if it holds no other tags.
bool rml_text(const std::string& rml, const char* id, std::string& out);
bool rml_set_text(std::string& rml, const char* id, const char* text);
// Adds markup at the end of <body>; false if there is no </body>.
bool rml_append_to_body(std::string& rml, const std::string& markup);
// Removes the element and everything inside it.
bool rml_remove(std::string& rml, const char* id);
// The elements inside <body> in document order with their nesting (UMG's Hierarchy): tag name, id
// ("" when it has none) and depth (0 = a direct child of body). Comments are skipped.
struct RmlNode {
    std::string tag, id;
    int depth = 0;
};
std::vector<RmlNode> rml_outline(const std::string& rml);
// Anchors (UMG's): which edge or middle of its parent an absolutely positioned element keeps its distance to,
// per axis 0 left / top, 1 the middle, 2 right / bottom. Read from its inline style (right / bottom set: 2; a % left /
// top: 1; else 0).
struct RmlAnchor {
    int x = 0, y = 0;
};
RmlAnchor rml_anchor(const std::string& rml, const char* id);
// Re-anchors it where it is now: `box` its x, y, width, height in a parent of `parent_w` x `parent_h` pixels. Writes
// left / right / margin-left and top / bottom / margin-top (the middle: 50% plus a margin to where it sits).
bool rml_set_anchor(std::string& rml, const char* id, RmlAnchor anchor, const float box[4], float parent_w, float parent_h);
// `base` with the lowest number not yet used as an id ("button1", "button2"...).
std::string rml_unique_id(const std::string& rml, const char* base);

} // namespace lsd::editor
