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
