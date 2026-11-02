-- Runs on every object whose Script component points at this file (in Play).
-- self.entity is the object, self.name its name; keep your own state in self.
-- The engine API is the lsd table: see engine/script/include/lsd/script/script.h.

function on_start(self)
  lsd.log("Hello from " .. self.name)
end

function on_update(self, dt)
  -- spin around Z, 90 degrees a second
  local x, y, z = lsd.rotation(self.entity)
  lsd.set_rotation(self.entity, x, y, z + 90 * dt)
end
