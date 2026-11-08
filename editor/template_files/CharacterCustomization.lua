-- Character Customization (template): tabs of morph sliders and clothes for the character this
-- script is on. Morph targets go to the tab whose keywords their name contains ("Nose_Width" ->
-- Nose); clothes come from <clothes_folder>/<Slot> (Content/Characters/Clothes/Shirt/...).
-- Save keeps it in the save game "appearance_<character>"; CharacterAppearance.lua puts it back on
-- a character in another level. Q / E (or the gamepad shoulders) change tabs.
properties = {
  clothes_folder = "Characters/Clothes",
  click_sound = "SFX_UI_Click",
  slide_sound = "SFX_UI_Slide",
}

local TABS = {
  {name = "Body",  keys = {}},  -- and every morph no other tab claims
  {name = "Head",  keys = {"head", "skull", "neck", "scalp"}},
  {name = "Face",  keys = {"face", "cheek", "jaw", "chin", "brow", "forehead", "temple"}},
  {name = "Eyes",  keys = {"eye", "lid", "lash", "pupil", "iris"}},
  {name = "Nose",  keys = {"nose", "nostril", "septum"}},
  {name = "Mouth", keys = {"mouth", "lip", "teeth", "smile", "tongue", "viseme"}},
  {name = "Ears",  keys = {"ear"}},
  {name = "Clothes"},
}
local CLOTHES = {"Hair", "Hat", "Glasses", "Shirt", "Jacket", "Gloves", "Pants", "Belt", "Shoes", "Backpack"}
local PAIRS = {Gloves = "Glove L", Shoes = "Shoe L"}  -- rigid pieces worn on both sides: "<mesh>_L" for the left one
local ROWS = 12

local function tab_of(morph)
  local lower = string.lower(morph)
  if string.sub(lower, 1, 2) == "v_" then return 6 end  -- visemes (V_Open, V_Wide...): Mouth
  for i = 2, #TABS - 1 do
    for _, k in ipairs(TABS[i].keys) do
      if string.find(lower, k, 1, true) and not (k == "ear" and string.find(lower, "beard", 1, true)) then return i end
    end
  end
  return 1
end

local function sound(self, name, every)
  local now = lsd.time()
  if every and self.last_sound and now - self.last_sound < every then return end
  self.last_sound = now
  lsd.play_sound(name)
end

local function show(self, id, on)
  lsd.ui_set(self.doc, id, "style", on and "display: block;" or "display: none;")
end

local function piece_name(self, slot)
  local list, at = self.clothes[slot], self.worn[slot]
  return (at and at > 0 and list[at]) and list[at].name or "None"
end

local function refresh(self)
  local tab = TABS[self.tab]
  lsd.ui_set_text(self.doc, "tab_title", tab.name)
  for i, t in ipairs(TABS) do lsd.ui_set(self.doc, "tab" .. i, "class", i == self.tab and "tab active" or "tab") end
  local clothes = tab.keys == nil
  show(self, "sliders", not clothes)
  show(self, "clothes", clothes)
  if clothes then
    for i, slot in ipairs(CLOTHES) do
      local n = #self.clothes[slot]
      lsd.ui_set_text(self.doc, "cname" .. i, n == 0 and "-" or piece_name(self, slot))
    end
    lsd.ui_set_text(self.doc, "clothes_folder", "Meshes in Content/" .. self.clothes_folder .. "/<area> (Shirt, Pants...)")
    show(self, "empty", false)
    return
  end
  local morphs = self.by_tab[self.tab]
  for row = 1, ROWS do
    local m = morphs[row]
    show(self, "row" .. row, m ~= nil)
    if m then
      lsd.ui_set_text(self.doc, "label" .. row, (string.gsub(m, "_", " ")))
      lsd.ui_set(self.doc, "slider" .. row, "value", tostring(self.weights[m] or 0))
    end
  end
  show(self, "empty", #morphs == 0)
end

local function wear(self, slot)
  local list, at = self.clothes[slot], self.worn[slot] or 0
  local piece = at > 0 and list[at] or nil
  lsd.set_outfit(self.entity, slot, piece and piece.guid or "")
  if PAIRS[slot] then
    local left = piece and self.left[piece.name] or nil
    lsd.set_outfit(self.entity, PAIRS[slot], left and left.guid or "")
  end
end

function on_start(self)
  self.doc = lsd.ui_open("UI_CharacterCustomization")
  lsd.ui_show_cursor(true)
  self.tab, self.weights, self.worn, self.clothes, self.left = 1, {}, {}, {}, {}
  -- The character picked in Character Selection, if any: its body, and its saved look.
  local picked = lsd.load_game("selected_character")
  if type(picked) == "table" and picked.mesh then lsd.set(self.entity, "SkeletalMesh", {mesh = picked.mesh}) end
  self.slot = "appearance_" .. ((type(picked) == "table" and picked.name) or lsd.name(self.entity))
  self.by_tab = {}
  for i = 1, #TABS do self.by_tab[i] = {} end
  for _, m in ipairs(lsd.morph_names(self.entity)) do
    local list = self.by_tab[tab_of(m)]
    if #list < ROWS then list[#list + 1] = m end
  end
  for _, slot in ipairs(CLOTHES) do
    local list = {}
    for _, a in ipairs(lsd.assets("Mesh", self.clothes_folder .. "/" .. slot)) do
      if string.sub(a.name, -2) == "_L" then self.left[string.sub(a.name, 1, -3)] = a else list[#list + 1] = a end
    end
    self.clothes[slot] = list
  end
  local saved = lsd.load_game(self.slot)
  if type(saved) == "table" then
    for name, w in pairs(saved.morphs or {}) do
      self.weights[name] = w
      lsd.set_morph(self.entity, name, w)
    end
    for slot, guid in pairs(saved.outfit or {}) do
      for i, a in ipairs(self.clothes[slot] or {}) do
        if a.guid == guid then self.worn[slot] = i end
      end
      if self.clothes[slot] then wear(self, slot) end
    end
  end
  refresh(self)
end

function on_update(self, dt)
  local key = lsd.key_pressed()
  if key == "E" or key == "Gamepad Right Shoulder" then next_tab(self)
  elseif key == "Q" or key == "Gamepad Left Shoulder" then prev_tab(self) end
end

function select_tab(self, id)
  self.tab = tonumber(string.match(id, "%d+")) or 1
  sound(self, self.click_sound)
  refresh(self)
end

function next_tab(self)
  self.tab = self.tab % #TABS + 1
  sound(self, self.click_sound)
  refresh(self)
end

function prev_tab(self)
  self.tab = (self.tab - 2) % #TABS + 1
  sound(self, self.click_sound)
  refresh(self)
end

function slider_changed(self, id)
  local row = tonumber(string.match(id, "%d+"))
  local m = row and self.by_tab[self.tab][row]
  if not m then return end
  local w = tonumber(lsd.ui_value(self.doc, id)) or 0
  self.weights[m] = w
  lsd.set_morph(self.entity, m, w)
  sound(self, self.slide_sound, 0.08)
end

local function cycle(self, id, step)
  local slot = CLOTHES[tonumber(string.match(id, "%d+")) or 0]
  if not slot then return end
  local n = #self.clothes[slot]
  self.worn[slot] = ((self.worn[slot] or 0) + step) % (n + 1)  -- 0 = None
  wear(self, slot)
  sound(self, self.click_sound)
  refresh(self)
end
function cloth_next(self, id) cycle(self, id, 1) end
function cloth_prev(self, id) cycle(self, id, -1) end

function reset_appearance(self)
  for name in pairs(self.weights) do lsd.set_morph(self.entity, name, 0) end
  self.weights = {}
  for _, slot in ipairs(CLOTHES) do
    self.worn[slot] = 0
    wear(self, slot)
  end
  sound(self, self.click_sound)
  refresh(self)
end

function save_appearance(self)
  local outfit = {}
  for _, slot in ipairs(CLOTHES) do
    local at = self.worn[slot] or 0
    if at > 0 then
      outfit[slot] = self.clothes[slot][at].guid
      local left = PAIRS[slot] and self.left[self.clothes[slot][at].name]
      if left then outfit[PAIRS[slot]] = left.guid end
    end
  end
  local ok = lsd.save_game(self.slot, {morphs = self.weights, outfit = outfit})
  lsd.ui_set_text(self.doc, "status", ok and "Saved" or "Could not save")
  sound(self, self.click_sound)
end
