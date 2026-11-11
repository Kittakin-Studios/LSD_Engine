-- Editor utility: turns the Third Person template level into Coin Run (run once, then save the level).
local function guid(type, name)
  for _, a in ipairs(lsd.assets(type, "CoinRun")) do
    if a.name == name then return a.guid end
  end
  error("no " .. type .. " named " .. name)
end

local COINS = {{8, 0}, {-8, -2}, {0, 12}, {12, 12}, {-14, -14}, {14, -12}, {-4, -14}, {-14, 14}}

function run(self)
  local coin, guard, game = guid("Script", "CoinRun_Coin"), guid("Script", "CoinRun_Guard"), guid("Script", "CoinRun_Game")
  for _, p in ipairs(COINS) do
    local e = lsd.spawn("Sphere", p[1], p[2], 0.4)
    lsd.set_name(e, "Coin")
    lsd.set(e, "Transform", {scale = {x = 0.7, y = 0.7, z = 0.7}})
    lsd.set(e, "StaticMesh", {collision = false, color = {r = 1, g = 0.8, b = 0.1, a = 1}})
    lsd.add(e, "TriggerVolume", {})
    lsd.add(e, "Script", {script = coin})
  end

  local g = lsd.spawn("Cube", -14, 14, 0)
  lsd.set_name(g, "Guard")
  lsd.set(g, "Transform", {scale = {x = 0.8, y = 0.8, z = 1.8}})
  lsd.set(g, "StaticMesh", {collision = false, color = {r = 0.8, g = 0.1, b = 0.1, a = 1}})
  lsd.add(g, "NavAgent", {max_speed = 4})
  lsd.add(g, "Script", {script = guard})

  local nav = lsd.spawn("Cube", 0, 0, -0.5)
  lsd.set_name(nav, "Nav Mesh Bounds")
  lsd.remove(nav, "StaticMesh")
  lsd.set(nav, "Transform", {scale = {x = 40, y = 40, z = 6}})
  lsd.add(nav, "NavMeshBounds", {})

  lsd.set_world_settings({level_script = game})  -- the game rules: World Settings > Level Script
  lsd.log("Coin Run built: 8 coins, a guard, a navmesh. Save the level.")
end
