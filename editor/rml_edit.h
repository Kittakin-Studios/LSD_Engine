#pragma once
// Small edits to RML text by element id, for the UI designer canvas: the text stays the source of
// truth, so dragging an element or typing in its Details rewrites only that element's tag.
// shortcut: a text scan, not a parser; it expects id="..." in double quotes, and the inner text
// helpers only handle elements whose content is plain text. A DOM writer can replace it.
#include <string>

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
// `base` with the lowest number not yet used as an id ("button1", "button2"...).
std::string rml_unique_id(const std::string& rml, const char* base);

} // namespace lsd::editor
