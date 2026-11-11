-- The guard: walks round the arena's corners, chases the player within 10 m, and catches at 1.6 m (the two capsules keep them ~1.1-1.5 m apart).
local CORNERS = {{-14, -14}, {14, -14}, {14, 14}, {-14, 14}}

function on_start(self)
  self.player, self.corner, self.repath = lsd.find("Player"), 1, 0
end

function on_update(self, dt)
  if not _G.coin_run_playing then lsd.stop_move(self.entity) return end  -- waits while a menu is up
  local x, y = lsd.position(self.entity)
  local px, py, pz = lsd.position(self.player)
  local d = math.sqrt((px - x) ^ 2 + (py - y) ^ 2)
  if d < 1.6 then
    if _G.caught then _G.caught() end
    lsd.stop_move(self.entity)
  elseif d < 10 then
    self.repath = self.repath - dt
    if self.repath <= 0 then lsd.move_to(self.entity, px, py, pz) self.repath = 0.3 end
  elseif not lsd.is_moving(self.entity) then
    local c = CORNERS[self.corner]
    lsd.move_to(self.entity, c[1], c[2], 0)
    self.corner = self.corner % #CORNERS + 1
  end
end
