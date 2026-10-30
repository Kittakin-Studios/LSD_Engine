# ui

Game UI on RmlUi (HTML / CSS): `lsd::ui::init` once, then a `UiContext` per screen of UI (the
game's, the editor's UI preview). Documents are `.rml` files with `.rcss` style sheets in Content;
fonts are the `.ttf` / `.otf` files in Content/Fonts plus the system UI font as `sans`. Drawn through
`rhi::Device::draw_ui`. Inline `on<event>="name"` attributes become actions (Lua functions in Play).
