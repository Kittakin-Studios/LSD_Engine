#include "assistant_lessons.h"
#include "assistant_core.h"
#include "skinned_cache.h"

#include "lsd/asset/data_asset.h"
#include "lsd/input/input.h"
#include "lsd/reflect/reflect.h"
#include "lsd/world/components.h"
#include "lsd/world/world.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace lsd::editor::assistant {

namespace fs = std::filesystem;

namespace {

constexpr char kGunLesson[] = R"LESSON(This lesson builds a gun for a rigged character, in code only: a Cube stands in for the gun model (swap it for your own mesh later). It becomes one Script component, Weapon, that you can put on the soldier or on any other rigged character.

Apply to Selected Character (above) does all the steps below on the selected character. Read on to see how each part works.

STEP 1: A socket on the hand (Unreal: Skeleton > Add Socket on hand_r)
A socket is a named point on a bone, with an offset, so things can hold on to the hand. Apply adds the socket weapon_r on the bone hand_r to the character's mesh (other rigs: RightHand, mixamorig:RightHand). To move or turn the gun in the hand, double-click the mesh > Sockets > weapon_r, and change Position / Rotation: the gun follows while you type.
IMAGE: docs/images/lesson_gun_socket.jpg

STEP 2: The input (Unreal: Input Actions + the Mapping Context)
Three Input Actions: IA_Aim (a Button, trigger Down, on Right Mouse Button), IA_Fire (trigger Pressed, on Left Mouse Button) and IA_Reload (trigger Pressed, on R). Apply makes them in Content/Weapon/ and adds the keys to the character's Mapping Context. In code, lsd.input("IA_Aim") is 1 while RMB is held, and lsd.pressed("IA_Fire") is true once per click.

STEP 3: The crosshair and ammo HUD (Unreal: a Widget Blueprint)
A UI document: a dot in the middle of the screen and the ammo count at the bottom right. The script changes the dot's style from its properties, so its size and colour are set in Details.
IMAGE: docs/images/lesson_gun_play_hip.jpg

```rml Content/Weapon/WBP_Crosshair.rml
<rml>
<head>
<title>Crosshair</title>
<style>
body { font-family: sans; width: 100%; height: 100%; }
div { display: block; }
#dot { position: absolute; left: 50%; top: 50%; width: 6px; height: 6px; margin-left: -3px; margin-top: -3px;
       border-radius: 3px; background-color: #FFFFFF; }
#ammo { position: absolute; right: 48px; bottom: 32px; color: #FFFFFF; font-size: 40px; font-weight: bold; }
</style>
</head>
<body>
<div id="dot"></div>
<div id="ammo">10/10</div>
</body>
</rml>
```

STEP 4: The Weapon component (Unreal: a Weapon Actor Component on the Character)
- properties = { ... } are the component's settings: each shows in Details under the Script, per character (Unreal's Instance Editable variables). Ammo starts at 10 of 10.
- on_start spawns the Cube, sizes it with the Transform's scale, turns its collision off, and gives it an Attach To Socket component pointing at this character's weapon_r. The engine then moves it with the hand every frame, after the animation.
- Aim (RMB held): the camera's spring arm eases in to Aim Distance and slides over the right shoulder (Unreal: Spring Arm Target Arm Length and Socket Offset); the crosshair turns red.
- The aim pose, in code: every frame while aiming, lsd.ik_reach puts the hand bone on a point Aim Reach metres from the shoulder along the camera's view, bending the elbow (Unreal: a Two Bone IK node in the Anim Blueprint). The arm comes up and the gun points where the crosshair is, on top of whatever animation plays; its weight follows the aim, so it eases in and out.
- Shoot (LMB): a ray from the camera through the crosshair (lsd.camera_location + lsd.camera_forward, like Unreal's Line Trace by Channel from the camera), one round less, a tracer line from the gun to the hit. What it hits gets on_damage(self, amount, shooter) on its own script, if it has one (Unreal: Apply Damage / Event AnyDamage).
- Montages (Unreal's Play Montage with a slot and Layered Blend per Bone): lsd.play_montage(self.entity, "Anim_Reload", 1, "spine_01") plays that animation once on the upper body (spine_01 and every bone under it) while the legs keep their own animation, blended in and out; on_montage_ended(self) runs when it ends. Each shot plays Shoot Montage the same way, the reload plays Reload Montage.
- Reload (R): lsd.after(Reload Time, ...) is the timer (Set Timer by Event). Until it fires the HUD says Reloading... and shots are refused, then the magazine is full again.
- The script only drives the player's character, so the same component on an AI character does nothing until you give it its own logic.

```lua Content/Weapon/Weapon.lua
-- Weapon: a gun for any rigged character (Unreal: a Weapon Actor Component on the Character).
-- On Play it makes a placeholder gun (a Cube), sticks it to the hand socket, and wires the mouse:
-- hold RMB (IA_Aim) to aim over the shoulder, click LMB (IA_Fire) to shoot, R (IA_Reload) reloads. Every value below shows
-- in Details under this Script and can be changed per character.
properties = {
  socket = "weapon_r",              -- the socket (or bone) on this character's skeleton the gun sits on
  ammo = 10,                        -- rounds in the magazine at the start
  max_ammo = 10,                    -- magazine size (the HUD shows ammo/max_ammo)
  range = 100,                      -- metres a shot reaches
  damage = 25,                      -- sent to what it hits: on_damage(self, amount, shooter)
  reload_time = 1.5,                -- seconds R takes to refill the magazine
  aim_distance = 1.6,               -- camera arm length while aiming, metres
  aim_shoulder = 0.5,               -- how far right the camera slides while aiming, metres
  hand_bone = "hand_r",             -- the aim pose: this bone reaches out...
  shoulder_bone = "upperarm_r",     -- ...from this one, along the view
  aim_reach = 0.55,                 -- how far from the shoulder the hand goes, metres
  crosshair_size = 6,               -- the dot's size in pixels (0 hides it)
  crosshair_color = "#FFFFFF",      -- the dot's colour (#RRGGBB)
  crosshair_aim_color = "#FF4040",  -- its colour while aiming
  shoot_montage = "Anim_Aim_Shoot", -- played on the upper body per shot (an animation file's name; "" = none)
  reload_montage = "Anim_Reload",   -- played on the upper body while reloading
}

-- The HUD: the ammo count and the crosshair dot, changed only when a value changed.
local function update_hud(self, aiming)
  if not self.hud then return end
  local text = self.reloading and "Reloading..." or self.ammo .. "/" .. self.max_ammo
  if text ~= self.shown_text then
    lsd.ui_set_text(self.hud, "ammo", text)
    self.shown_text = text
  end
  local size = math.floor(self.crosshair_size)
  local half = math.floor(size / 2)
  local style = string.format("width: %dpx; height: %dpx; margin-left: %dpx; margin-top: %dpx; border-radius: %dpx; background-color: %s; display: %s;",
    size, size, -half, -half, half, aiming and self.crosshair_aim_color or self.crosshair_color, size > 0 and "block" or "none")
  if style ~= self.shown_style then
    lsd.ui_set(self.hud, "dot", "style", style)
    self.shown_style = style
  end
end

-- Reload (R): after Reload Time the magazine is full again; no shooting meanwhile.
local function reload(self)
  if self.reloading or self.ammo >= self.max_ammo then return end
  self.reloading = true
  if self.reload_montage ~= "" then lsd.play_montage(self.entity, self.reload_montage, 1, "spine_01") end  -- the upper body reloads, the legs keep walking
  lsd.after(self.reload_time, function()
    self.ammo = self.max_ammo
    self.reloading = false
  end)
end

-- One shot: a ray from the camera through the crosshair, and a tracer from the gun to the hit.
local function shoot(self)
  if self.reloading then return end
  if self.ammo <= 0 then
    lsd.log("Out of ammo: press R to reload")
    return
  end
  self.ammo = self.ammo - 1
  local cx, cy, cz = lsd.camera_location()
  local fx, fy, fz = lsd.camera_forward()
  -- Start the ray just past the character, so it never hits the shooter (the camera is behind them).
  local px, py, pz = lsd.position(self.entity)
  local skip = math.max(0, (px - cx) * fx + (py - cy) * fy + (pz + 1 - cz) * fz) + 0.5
  local sx, sy, sz = cx + fx * skip, cy + fy * skip, cz + fz * skip
  local hit, hx, hy, hz = lsd.raycast(sx, sy, sz, fx, fy, fz, self.range)
  if not hit then hx, hy, hz = sx + fx * self.range, sy + fy * self.range, sz + fz * self.range end
  local mx, my, mz = lsd.socket(self.entity, self.socket)  -- the gun, where the tracer starts
  if not mx then mx, my, mz = sx, sy, sz end
  lsd.draw_line(mx, my, mz, hx, hy, hz, 1, 0.8, 0.2, 0.1)
  if type(hit) == "number" then
    lsd.draw_sphere(hx, hy, hz, 0.08, 1, 0.3, 0.1, 0.5)
    lsd.call(hit, "on_damage", self.damage, self.entity)
  end
  lsd.camera_shake(0.15, 0.1)
  if self.shoot_montage ~= "" then lsd.play_montage(self.entity, self.shoot_montage, 1, "spine_01", 0.05) end  -- the recoil, upper body only
end

function on_start(self)
  if self.entity ~= lsd.player() then return end  -- only the player's character takes the mouse
  -- The gun: a Cube made gun-sized. It is spawned far below the level because a spawned mesh gets a
  -- still collision box where it appears; collision goes off and the socket carries it from here.
  self.gun = lsd.spawn("Cube", 0, 0, -1000)
  if self.gun then
    lsd.set(self.gun, "Transform", {scale = {x = 0.35, y = 0.06, z = 0.12}})
    lsd.set(self.gun, "StaticMesh", {collision = false, color = {r = 0.08, g = 0.08, b = 0.08, a = 1}})
    lsd.add(self.gun, "AttachToSocket", {target = self.name, socket = self.socket})
  end
  local c = lsd.get(self.entity, "Character")
  self.arm = c and c.camera_distance or 0
  self.offset = c and c.camera_offset or {x = 0, y = 0, z = 0}
  self.aim = 0
  self.hud = lsd.ui_open("WBP_Crosshair")
  update_hud(self, false)
end

function on_update(self, dt)
  if self.aim == nil then return end  -- not the player's character
  local aiming = lsd.input("IA_Aim") > 0.5
  local goal = aiming and 1 or 0
  local aim = self.aim + (goal - self.aim) * math.min(1, dt * 12)  -- eases in and out
  if math.abs(goal - aim) < 0.001 then aim = goal end
  if aim ~= self.aim and self.arm > 0 then  -- third person: in to Aim Distance, over the right shoulder
    lsd.set(self.entity, "Character", {
      camera_distance = self.arm + (self.aim_distance - self.arm) * aim,
      camera_offset = {x = self.offset.x, y = self.offset.y - self.aim_shoulder * aim, z = self.offset.z},
    })
    lsd.set_anim_param(self.entity, "Aiming", aim)  -- an Anim Graph can blend to an aim pose on this
  end
  self.aim = aim
  if aim > 0 then  -- the aim pose: the hand out along the view, weighted by the aim (IK is asked for every frame)
    local sx, sy, sz = lsd.socket(self.entity, self.shoulder_bone)
    if sx then
      local fx, fy, fz = lsd.camera_forward()
      local r = self.aim_reach
      lsd.ik_reach(self.entity, self.hand_bone, sx + fx * r, sy + fy * r, sz + fz * r, aim)
    end
  end
  if lsd.pressed("IA_Fire") then shoot(self) end
  if lsd.pressed("IA_Reload") then reload(self) end
  update_hud(self, aiming)
end
```

The component in Details: Script = Weapon, and every value in its properties table, set per character.
IMAGE: docs/images/lesson_gun_details.jpg

In Play: hold RMB to aim (the camera comes in over the shoulder, the arm comes up, the dot turns red)...
IMAGE: docs/images/lesson_gun_play_aim.jpg
...click LMB to shoot (one round less: 9/10, a tracer from the gun to what the dot was on)...
IMAGE: docs/images/lesson_gun_play_fire.jpg
...and press R to reload (Reloading... until Reload Time has passed, the reload montage on the upper body).
IMAGE: docs/images/lesson_gun_play_reload.jpg

STEP 5: Use it on another character
Select any rigged character and press Apply again: the files are kept, the socket goes on that character's mesh, and it gets the same component. Or by hand: Details > Add Component > Script > Weapon, and a weapon_r socket in its Mesh Editor.

Make a target: give any object with collision a Script with
  function on_damage(self, amount, shooter) lsd.log(self.name .. " took " .. amount) end
)LESSON";

constexpr Lesson kLessons[] = {
    {"Gun on a hand socket (soldier)", "A Cube gun in the right hand, 10/10 ammo, RMB aims, LMB shoots, R reloads, a crosshair dot set in Details", kGunLesson},
};

// The record of a file under Content/ (relative path as the registry keeps it), or nullptr.
const asset::AssetRecord* record_of(const asset::AssetRegistry& registry, const std::string& content_path) {
    const std::string relative = fs::path(content_path).lexically_relative("Content").generic_string();
    for (usize i = 0; i < registry.count(); ++i) {
        if (relative == registry.get(i).path) return &registry.get(i);
    }
    return nullptr;
}

// An Input Action under Content/Weapon/, made if missing; its GUID text.
std::string input_action(EditorState& s, const char* name, input::Trigger trigger, const char* description, std::string& report) {
    const std::string path = std::string("Content/Weapon/") + name + asset::kDataAssetExtension;
    if (!record_of(*s.registry, path)) {
        input::InputAction action;
        action.trigger = static_cast<u32>(trigger);
        std::snprintf(action.description, sizeof action.description, "%s", description);
        asset::save_data_asset(path.c_str(), reflect::type_of<input::InputAction>(), &action);
        s.registry->scan();
        report += std::string("made ") + name + "; ";
    }
    const asset::AssetRecord* r = record_of(*s.registry, path);
    char guid[33] = "";
    if (r) asset::to_string(r->guid, guid);
    return guid;
}

bool apply_gun(EditorState& s, const Lesson& lesson, std::string& report) {
    world::World& world = *s.world;
    const world::Entity e = s.selected;
    const auto* mesh = world.alive(e) ? world.get<world::SkeletalMesh>(e) : nullptr;
    if (!mesh || !mesh->mesh[0]) {
        report = "Select a rigged character first (an object with a Skeletal Mesh).";
        return false;
    }
    const auto* existing = world.get<world::Script>(e);
    const asset::AssetRecord* weapon = record_of(*s.registry, "Content/Weapon/Weapon.lua");
    char weapon_guid[33] = "";
    if (weapon) asset::to_string(weapon->guid, weapon_guid);
    if (existing && existing->script[0] && std::strcmp(existing->script, weapon_guid) != 0) {
        report = std::string(world.name(e)) + " already runs another script; remove its Script component first (an object runs one).";
        return false;
    }
    // The bone the socket rides on: the first hand bone this rig has.
    const SkinnedEntry* model = s.skinned ? s.skinned->get(mesh->mesh) : nullptr;
    // Each rig family's right hand and upper arm (Unreal, Mixamo, others).
    const char* const kArms[][2] = {{"hand_r", "upperarm_r"}, {"RightHand", "RightArm"}, {"mixamorig:RightHand", "mixamorig:RightArm"},
                                    {"Hand_R", "UpperArm_R"}, {"hand.R", "upper_arm.R"}, {"R_Hand", "R_UpperArm"}};
    const char* bone = nullptr;
    const char* shoulder = nullptr;
    for (const auto& arm : kArms) {
        if (!bone && model && model->model && model->model->find_joint(arm[0]) >= 0) {
            bone = arm[0];
            shoulder = arm[1];
        }
    }
    if (!bone) {
        report = "No right-hand bone found on this skeleton (hand_r, RightHand...): add a socket named weapon_r on its hand in the Mesh Editor, then Apply.";
        if (!model) return false;
    }

    // The files: the lesson's code blocks, kept when they already exist (yours may be edited).
    std::error_code error;
    for (const CodeBlock& b : code_blocks(lesson.text)) {
        if (b.path.empty() || fs::exists(b.path, error)) continue;
        fs::create_directories(fs::path(b.path).parent_path(), error);
        std::ofstream out(b.path, std::ios::binary);
        out << b.code;
        report += "wrote " + b.path + "; ";
    }
    s.registry->scan();

    // Input: IA_Aim on the right mouse button, IA_Fire on the left, IA_Reload on R, in the character's Mapping Context.
    const std::string aim = input_action(s, "IA_Aim", input::Trigger::Down, "Aim the weapon (hold)", report);
    const std::string fire = input_action(s, "IA_Fire", input::Trigger::Pressed, "Fire the weapon", report);
    const std::string reload = input_action(s, "IA_Reload", input::Trigger::Pressed, "Reload the weapon", report);
    const auto* character = world.get<world::Character>(e);
    asset::Guid context_guid;
    const asset::AssetRecord* context = character && asset::parse_guid(character->mapping_context, context_guid) ? s.registry->find(context_guid) : nullptr;
    if (context) {
        char path[300];
        s.registry->source_path(*context, path, sizeof path);
        input::InputMappingContext imc;
        asset::load_data_asset(path, imc);
        bool changed = false;
        for (const auto& [action, key] : {std::pair{aim, "Right Mouse Button"}, std::pair{fire, "Left Mouse Button"}, std::pair{reload, "R"}}) {
            bool mapped = false;
            for (const input::KeyMapping& m : imc.mappings) mapped = mapped || std::strcmp(m.action, action.c_str()) == 0;
            if (mapped || action.empty()) continue;
            input::KeyMapping m;
            std::snprintf(m.action, sizeof m.action, "%s", action.c_str());
            std::snprintf(m.key, sizeof m.key, "%s", key);
            changed = imc.mappings.push_back(m) || changed;  // false when the context is full
        }
        if (changed) {
            asset::save_data_asset(path, reflect::type_of<input::InputMappingContext>(), &imc);
            report += std::string("mapped RMB / LMB / R in ") + context->name + "; ";
        }
    } else {
        report += "no Mapping Context on the Character: add IA_Aim (Right Mouse Button), IA_Fire (Left Mouse Button) and IA_Reload (R) to one; ";
    }

    // The socket on the hand, on the mesh asset (every character using the mesh gets it).
    asset::Guid mesh_guid;
    const asset::AssetRecord* mesh_record = bone && asset::parse_guid(mesh->mesh, mesh_guid) ? s.registry->find(mesh_guid) : nullptr;
    if (mesh_record) {
        asset::AssetMeta meta;
        s.registry->load_meta(*mesh_record, meta);
        bool has = false;
        for (const asset::MeshSocket& m : meta.sockets) has = has || std::strcmp(m.name, "weapon_r") == 0;
        if (!has) {
            asset::MeshSocket socket;
            std::snprintf(socket.name, sizeof socket.name, "weapon_r");
            std::snprintf(socket.bone, sizeof socket.bone, "%s", bone);
            socket.position = {-0.12f, 0.0f, 0.0f};  // in front of the palm; tune in the Mesh Editor > Sockets
            socket.rotation = {0.0f, 0.0f, 180.0f};   // Unreal-style right hands: the bone's +X runs back up the arm
            if (meta.sockets.push_back(socket) && s.registry->save_meta(*mesh_record, meta)) report += std::string("socket weapon_r on ") + bone + "; ";
            else report += "the mesh has no room for another socket; ";
        }
    }

    // The component: a Script running Weapon.lua.
    weapon = record_of(*s.registry, "Content/Weapon/Weapon.lua");
    if (!weapon) {
        report += "Content/Weapon/Weapon.lua could not be written.";
        return false;
    }
    auto* script = existing ? world.get<world::Script>(e) : world.add<world::Script>(e);
    asset::to_string(weapon->guid, script->script);
    if (bone && std::strcmp(bone, "hand_r") != 0) {  // another rig: its bones instead of the script's defaults
        for (const auto& [key, value] : {std::pair{"hand_bone", bone}, std::pair{"shoulder_bone", shoulder}}) {
            world::ScriptProperty* found = nullptr;
            for (world::ScriptProperty& p : script->properties) found = std::strcmp(p.name, key) == 0 ? &p : found;
            world::ScriptProperty added;
            if (!found && script->properties.push_back(added)) found = &script->properties[script->properties.count - 1];
            if (!found) continue;
            std::snprintf(found->name, sizeof found->name, "%s", key);
            std::snprintf(found->value, sizeof found->value, "%s", value);
        }
    }
    std::snprintf(s.pending_edit, sizeof s.pending_edit, "Apply Lesson");
    report += std::string(world.name(e)) + " has the Weapon component. Play: hold RMB to aim, click LMB to shoot, R to reload.";
    return true;
}

} // namespace

const Lesson* lessons(u32& count) {
    count = static_cast<u32>(sizeof kLessons / sizeof kLessons[0]);
    return kLessons;
}

bool apply_lesson(EditorState& s, u32 index, std::string& report) {
    report.clear();
    if (!s.world || !s.registry || index != 0) {
        report = "No such lesson.";
        return false;
    }
    return apply_gun(s, kLessons[index], report);
}

} // namespace lsd::editor::assistant
