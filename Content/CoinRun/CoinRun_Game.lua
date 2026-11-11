-- Coin Run: a menu, then take every coin before the time runs out without letting the guard catch you.
local LIMIT = 60  -- seconds

local function show_menu(self, text)
  lsd.set(self.player, "Character", {walk_speed = 0, jump_speed = 0})  -- stands still behind the menu
  if text then lsd.ui_rml(self.hud, "msg", text) end
  local best = lsd.load_game("CoinRun")
  if best then lsd.ui_set_text(self.hud, "best", string.format("Best time %.1f s", best.time)) end
  lsd.ui_style(self.hud, "menu", "display", "block")
  lsd.ui_show_cursor(true)
end

local function finish(self, text)
  if not self.playing then return end
  self.playing, self.over, _G.coin_run_playing = false, true, false
  lsd.log("Coin Run: " .. text)
  lsd.ui_set_text(self.hud, "hud", string.format("Coins %d / %d", self.got, self.total))
  lsd.ui_set_text(self.hud, "play", "Play again")
  show_menu(self, text)
end

function on_start(self)
  self.hud, self.player = lsd.ui_open("UI_CoinRun"), lsd.find("Player")
  self.speed = lsd.get(self.player, "Character")
  self.got, self.total, self.playing, self.over, _G.coin_run_playing = 0, #lsd.find_all("Coin"), false, false, false
  _G.coin_taken = function()
    self.got = self.got + 1
    if self.got < self.total then return end
    local time, best = lsd.time() - self.start, lsd.load_game("CoinRun")
    local record = not best or time < best.time
    if record then lsd.save_game("CoinRun", {time = time}) end
    finish(self, string.format("You win! %.1f s%s", time, record and "<br/>New best time!" or ""))
  end
  _G.caught = function() finish(self, "Caught!") end
  if lsd.game_instance().replay then coinrun_play(self) else show_menu(self) end
end

function coinrun_play(self)
  if self.over then  -- a round was played: load the level again (every coin back) and start at once
    lsd.game_instance().replay = true
    lsd.open_level("CoinRun")
    return
  end
  lsd.ui_style(self.hud, "menu", "display", "none")
  lsd.ui_show_cursor(false)
  lsd.set(self.player, "Character", {walk_speed = self.speed.walk_speed, jump_speed = self.speed.jump_speed})
  self.playing, self.start, _G.coin_run_playing = true, lsd.time(), true
end

function coinrun_quit(self)
  lsd.quit()  -- a packaged game closes; Play in the editor stops
end

function on_update(self, dt)
  if not self.playing then return end
  local left = math.max(0, LIMIT - (lsd.time() - self.start))
  if left == 0 then finish(self, "Out of time!") end
  lsd.ui_set_text(self.hud, "hud", string.format("Coins %d / %d     Time %d", self.got, self.total, math.ceil(left)))
end
