-- Coin Run: take every coin before the time runs out, and do not let the guard catch you.
local LIMIT = 60  -- seconds

local function finish(self, text)
  if self.over then return end
  self.over = true
  lsd.ui_rml(self.hud, "msg", text .. "<br/>Press Space to play again")
  lsd.set(lsd.find("Player"), "Character", {walk_speed = 0, jump_speed = 0})
  lsd.log("Coin Run: " .. text)
end

function on_start(self)
  self.hud = lsd.ui_open("UI_CoinRun")
  self.got, self.total, self.over, self.start = 0, #lsd.find_all("Coin"), false, lsd.time()
  _G.coin_taken = function()
    self.got = self.got + 1
    if self.got >= self.total then finish(self, "You win!") end
  end
  _G.caught = function() finish(self, "Caught!") end
end

function on_update(self, dt)
  if self.over then
    if lsd.pressed("Jump") then lsd.open_level("CoinRun") end
    return
  end
  local left = math.max(0, LIMIT - (lsd.time() - self.start))
  if left == 0 then finish(self, "Out of time!") end
  lsd.ui_set_text(self.hud, "hud", string.format("Coins %d / %d     Time %d", self.got, self.total, math.ceil(left)))
end
