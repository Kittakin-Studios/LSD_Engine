-- Puts the chosen character on this object in any level: put it on the object with the Skeletal
-- Mesh (a Character's body). The body picked in Character Selection, then the look saved in
-- Character Customization (morph targets and clothes per body area).
function on_start(self)
  local picked = lsd.load_game("selected_character")
  if type(picked) == "table" and picked.mesh then lsd.set(self.entity, "SkeletalMesh", {mesh = picked.mesh}) end
  local look = lsd.load_game("appearance_" .. ((type(picked) == "table" and picked.name) or "Character"))
  if type(look) ~= "table" then return end
  for name, w in pairs(look.morphs or {}) do lsd.set_morph(self.entity, name, w) end
  for slot, guid in pairs(look.outfit or {}) do lsd.set_outfit(self.entity, slot, guid) end
end
