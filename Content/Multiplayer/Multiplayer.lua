-- Multiplayer menu for playtests (Unreal: a Host / Join widget over Create / Find / Join Session).
-- Put it on any object in the level the game starts on (Project Settings > Multiplayer must be on).
-- The host clicks Show join codes and sends one to the testers; they paste it into Join. No port
-- forwarding when the router does UPnP; otherwise everyone joins one Tailscale / ZeroTier network
-- and uses the "virtual LAN" code (MANUAL: Playtesting over the internet).
properties = {
  toggle_key = "M",     -- opens and closes the menu
  open_at_start = true, -- shown when the game starts
}

local function set(self, id, text)
  lsd.ui_set_text(self.menu, id, text or "")
end

local function refresh_status(self)
  local mode = lsd.net_mode()
  if mode == "server" then
    set(self, "status", "Hosting: give the testers a join code")
  elseif mode == "client" then
    set(self, "status", lsd.player_id() > 0 and ("Connected as player " .. lsd.player_id()) or "Connecting...")
  else
    set(self, "status", "Single player")
  end
end

local function show_codes(self)
  local c = lsd.net_join_code()
  if c.internet then
    set(self, "internet", "Internet: " .. c.internet)
  else
    set(self, "internet", c.upnp == "working" and "Internet: asking the router..." or "Internet: no code (see below)")
  end
  local rows = c.lan or {}
  for k = 1, 3 do
    local r = rows[k]
    set(self, "lan" .. k, r and ((r.tailscale and "Tailscale: " or "LAN / virtual LAN: ") .. r.code .. "   (" .. r.address .. ")") or "")
  end
  if c.upnp == "failed" then
    set(self, "upnp", "No internet code: " .. (c.message or "") .. ".")
  elseif c.upnp == "open" then
    set(self, "upnp", "The router forwards the game's port while you host.")
  else
    set(self, "upnp", "")
  end
  self.codes_working = c.upnp == "working"
end

local function open_menu(self)
  if self.menu then return end
  self.menu = lsd.ui_open("WBP_Multiplayer")
  if not self.menu then return end
  lsd.ui_show_cursor(true)
  set(self, "keyhint", self.toggle_key .. " opens and closes this menu")
  refresh_status(self)
  if lsd.net_mode() == "server" and self.codes_shown then show_codes(self) end
end

local function close_menu(self)
  if not self.menu then return end
  lsd.ui_close(self.menu)
  self.menu = nil
  lsd.ui_show_cursor(false)
end

function on_start(self)
  self.codes_shown = lsd.net_mode() == "server" and lsd.game_instance().mp_codes_shown == true
  if self.open_at_start then open_menu(self) end
end

function on_update(self, dt)
  if lsd.key_pressed() == self.toggle_key then
    if self.menu then close_menu(self) else open_menu(self) end
  end
  if self.menu and self.codes_working then  -- UPnP takes a couple of seconds: show the code when it lands
    self.wait = (self.wait or 0) + dt
    if self.wait > 0.5 then
      self.wait = 0
      show_codes(self)
    end
  end
end

function on_connected(self, id) if self.menu then refresh_status(self) end end
function on_disconnected(self)
  if self.menu then set(self, "status", "Could not reach the host (or it left). Check the code, or Leave.") end
end

function mp_host(self, id)
  lsd.game_instance().mp_codes_shown = true  -- the restarted game shows the codes at once
  local ok, why = lsd.net_host()
  if not ok then set(self, "status", "Cannot host: " .. why) end
end

function mp_invite(self, id)
  if lsd.net_mode() ~= "server" then
    set(self, "status", "Host a game first")
    return
  end
  self.codes_shown = true
  show_codes(self)
end

function mp_join(self, id)
  local code = lsd.ui_value(self.menu, "code")
  local ok, why = lsd.net_join(code)
  if ok then set(self, "status", "Joining...") else set(self, "status", why) end
end

function mp_find(self, id)
  local games = lsd.net_find_games(0.5)
  if #games == 0 then
    set(self, "found", "No games on this network")
    return
  end
  set(self, "found", games[1].name .. " at " .. games[1].address .. " (" .. games[1].players .. "/" .. games[1].max_players .. "): Join takes it")
  lsd.ui_set(self.menu, "code", "value", games[1].address)
end

function mp_leave(self, id)
  lsd.game_instance().mp_codes_shown = false
  lsd.net_leave()
end

function mp_close(self, id) close_menu(self) end
