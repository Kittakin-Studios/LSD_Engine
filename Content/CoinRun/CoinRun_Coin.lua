-- A coin: spins, and counts itself when the player walks into its Trigger Volume.
function on_update(self, dt)
  local x, y, z = lsd.rotation(self.entity)
  lsd.set_rotation(self.entity, x, y, z + 120 * dt)
end

function on_overlap_begin(self, other)
  if not lsd.has(other, "Character") then return end
  if _G.coin_taken then _G.coin_taken() end
  lsd.destroy(self.entity)
end
