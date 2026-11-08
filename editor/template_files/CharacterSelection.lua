-- Character Selection (template): browse ready-made characters and pick one. Presets are the
-- Skeletal Meshes (rigged FBX / glTF) in <presets_folder>; the one shown is this object's mesh.
-- Select saves it as "selected_character" (Character Customization and CharacterAppearance.lua
-- read it) and opens next_level when set. A / D, arrows or the gamepad shoulders browse.
properties = {
  presets_folder = "Characters/Presets",
  next_level = "",
  click_sound = "SFX_UI_Click",
}

local function show(self)
  local p = self.presets[self.at]
  if p then lsd.set(self.entity, "SkeletalMesh", {mesh = p.guid}) end
  lsd.ui_set_text(self.doc, "preset_name", p and (string.gsub(p.name, "_", " ")) or "No presets yet")
  lsd.ui_set_text(self.doc, "preset_count", p and (self.at .. " / " .. #self.presets) or ("Add rigged characters to Content/" .. self.presets_folder))
end

function on_start(self)
  self.doc = lsd.ui_open("UI_CharacterSelection")
  lsd.ui_show_cursor(true)
  self.presets = lsd.assets("Mesh", self.presets_folder)
  self.at = 1
  local picked = lsd.load_game("selected_character")  -- start on the last pick
  for i, p in ipairs(self.presets) do
    if type(picked) == "table" and p.guid == picked.mesh then self.at = i end
  end
  show(self)
end

function on_update(self, dt)
  local key = lsd.key_pressed()
  if key == "D" or key == "Right" or key == "Gamepad Right Shoulder" then next_preset(self)
  elseif key == "A" or key == "Left" or key == "Gamepad Left Shoulder" then prev_preset(self)
  elseif key == "Enter" or key == "Gamepad Face Bottom" then select_preset(self) end
end

function next_preset(self)
  if #self.presets == 0 then return end
  self.at = self.at % #self.presets + 1
  lsd.play_sound(self.click_sound)
  show(self)
end

function prev_preset(self)
  if #self.presets == 0 then return end
  self.at = (self.at - 2) % #self.presets + 1
  lsd.play_sound(self.click_sound)
  show(self)
end

function select_preset(self)
  local p = self.presets[self.at]
  if not p then return end
  lsd.play_sound(self.click_sound)
  lsd.save_game("selected_character", {name = p.name, mesh = p.guid})
  lsd.ui_set_text(self.doc, "status", "Selected " .. (string.gsub(p.name, "_", " ")))
  if self.next_level ~= "" then lsd.open_level(self.next_level) end
end
