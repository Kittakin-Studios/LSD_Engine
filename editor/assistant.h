#pragma once
// LSD Assistant (Tools > LSD Assistant): a chat with a language model that knows LSD_Engine and game
// development; it writes code only (gameplay programmer, engine expert, UI programmer, code reviewer),
// with cubes and spheres as placeholders, never art. Lessons teach worked examples. The user brings the model: Claude through Anthropic's
// API, or anything with an OpenAI-compatible API (OpenAI, OpenRouter, a local Ollama / LM Studio).
// Each message carries the project's state (level, selection, assets); code it writes can be saved
// into Content/ or Source/ and attached to the selected object with one click.
#include "editor_ui.h"

namespace lsd::editor::assistant {

void draw_window(EditorState& s, bool* open);
void draw_settings(EditorState& s);  // Editor Preferences > Plugins > LSD Assistant

} // namespace lsd::editor::assistant
