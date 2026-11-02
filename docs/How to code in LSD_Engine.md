# How to code in LSD_Engine

Step by step: where to click to write game code, attach it to an object, run it and debug it.
In Unreal terms: a **Lua script** is a Blueprint's event graph written as text, a **Visual Script**
is the Blueprint event graph itself, and the **Lua Debugger** is Visual Studio's debugger.

> **C and C++:** game code in C / C++ (a `Game.dll` from `Source/`) comes with milestone 7. Its
> commands have the same names as the Lua ones below, so everything you learn here carries over.
> The C / C++ API and every command: [CODING.md](CODING.md).

Contents: [1. One-time setup](#1-one-time-setup) · [2. Make a script](#2-make-a-script) ·
[3. Write the code](#3-write-the-code) · [4. Put it on an object](#4-put-it-on-an-object) ·
[5. Run it](#5-run-it) · [6. Debug it](#6-debug-it) · [7. Visual scripts instead of code](#7-visual-scripts-instead-of-code) ·
[8. Recipes](#8-recipes) · [9. When something goes wrong](#9-when-something-goes-wrong)

---

## 1. One-time setup

Double-clicking a `.lua` file in the Content Browser opens it in Windows' program for `.lua` files.
Make that VS Code:

1. Open the project's `Content` folder in **File Explorer**.
2. Right-click any `.lua` file > **Open with** > **Choose another app**.
3. Pick **Visual Studio Code**, tick **Always use this app to open .lua files**, click **OK**.
4. Optional, in VS Code: Extensions (Ctrl+Shift+X) > install **Lua** (by sumneko) for colours and
   autocomplete.

## 2. Make a script

1. In the **Content Browser** (bottom panel), open the folder you want it in (for example
   `Content/Scripts`; right-click empty space > **New Folder** to make one).
2. **Right-click empty space** in the asset area > **New Lua Script**.
3. Type a name (for example `Door`) and press **Enter**. You get `Door.lua`, shown as a dark tile
   reading **.Lua**.

Unreal: Content Browser > Add > Blueprint Class.

## 3. Write the code

1. **Double-click** the script's tile (or right-click it > **Edit Script**). It opens in VS Code.
2. A new script already contains:

```lua
function on_start(self)            -- once, when Play starts (BeginPlay)
  lsd.log("Hello from " .. self.name)
end

function on_update(self, dt)       -- every frame; dt = seconds since the last one (Tick)
  local x, y, z = lsd.rotation(self.entity)
  lsd.set_rotation(self.entity, x, y, z + 90 * dt)   -- spin 90 degrees a second
end
```

3. Change it and **save (Ctrl+S)**. The engine picks the change up by itself, **even while the game
   is playing**: no compile button, no restart.

What the words mean:

| Word | Meaning | Unreal |
| --- | --- | --- |
| `self` | This object's own table: `self.entity` (the object), `self.name`, plus anything you store (`self.health = 100`) | `this` / the Blueprint's variables |
| `self.entity` | The object itself; pass it to `lsd.` commands | Self reference |
| `lsd.` | The engine's commands (`lsd.log`, `lsd.find`, `lsd.set_position`...) | Blueprint function library |
| `dt` | Seconds since the last frame; multiply speeds by it | Delta Seconds |
| `-- text` | A comment; ignored | Comment box |

Every command is listed in [CODING.md](CODING.md#command-reference) and in
[MANUAL.md section 19](MANUAL.md#lua-api).

**Values you can change per object** (Unreal's *Instance Editable* variables): put a `properties`
table at the top. Each object using the script then shows these in **Details**, under its Script:

```lua
properties = { speed = 90, label = "Door", locked = false }

function on_update(self, dt)
  local x, y, z = lsd.rotation(self.entity)
  lsd.set_rotation(self.entity, x, y, z + self.speed * dt)
end
```

## 4. Put it on an object

1. Click the object in the **viewport** or the **Outliner** (top right).
2. In **Details** (bottom right), scroll down and click **Add Component** > **Script**.
3. In the new Script section, click the **Script** dropdown and pick `Door` (or **drag** the script's
   tile from the Content Browser onto that field).
4. Any `properties` show under it; change them for this object only (right-click a value >
   **Reset to Default** to undo).

Many objects can use the same script; each runs its own copy with its own `self`.

## 5. Run it

1. Click **Play** on the toolbar (or **Alt+P**). **Simulate** (**Alt+S**) runs it with the editor
   camera instead of the player.
2. Watch the **Output Log** tab (bottom): `lsd.log(...)` lines appear there.
3. Edit the script and save while playing: the object keeps going with the new code.
4. **Esc** stops Play. Everything returns to how it was before Play (like Unreal's PIE).

Try a line without a script: in the console box at the bottom of the Output Log, type
`lua lsd.log(lsd.name(lsd.player()))` and press Enter.

## 6. Debug it

This is the window in the screenshot: **Tools > Lua Debugger**.

1. **File** (top right of the window): pick the script, e.g. `Content/NewScript.lua`.
2. **Click a line number** to put a breakpoint on that line (a red dot). Click again to remove it.
   Or hover the line and press **F9**.
3. Press **Play**. When that line runs the game freezes and the debugger shows:
   - **Call stack**: which function called which; click a row to look at that one.
   - **Locals**: every variable at that moment, and `self`'s fields (`self.speed`...).
   - **Watch**: type a name (`dt`, `self.health`) to keep an eye on it.
4. Then choose:

| Button | Key | Does |
| --- | --- | --- |
| Continue | F5 | Runs on to the next breakpoint |
| Step Over | F10 | Runs this line, stops on the next one |
| Step Into | F11 | Goes inside the function this line calls |
| Step Out | Shift+F11 | Finishes this function, stops where it was called |

"Set breakpoints, then Play" next to the buttons means nothing is stopped yet: that is the normal
state before you press Play.

## 7. Visual scripts instead of code

The same things with nodes and wires (Blueprints):

1. Content Browser > right-click empty space > **New Visual Script**, name it, **double-click** it.
2. **Right-click the canvas** for the node menu (or drag nodes from the **Palette** on the left).
   Start from **Event Start** / **Event Update**; white wires set the order, coloured wires carry values.
3. **My Blueprint** (left) > **Variables** > **+** adds a variable; tick **Instance Editable** to
   change it per object.
4. **Save + Compile** (**Ctrl+S**).
5. Put it on an object exactly like a `.lua`: Add Component > **Script** > pick the graph.
6. Debug: right-click a node > **Toggle Breakpoint** (or select it and press F9). While playing,
   nodes that run glow orange.

## 8. Recipes

**Open a door when the player walks in** (the object needs a **Trigger Volume** component):

```lua
properties = { open_angle = 90 }

function on_overlap_begin(self, other)
  if other == lsd.player() then
    local x, y, z = lsd.rotation(self.entity)
    lsd.set_rotation(self.entity, x, y, z + self.open_angle)
    lsd.play_sound("S_DoorOpen")
  end
end
```

**React to a key** (make an Input Action `IA_Interact` first: Content Browser > right-click >
New Data Asset > InputAction, and add it to your Mapping Context):

```lua
function on_update(self, dt)
  if lsd.pressed("IA_Interact") then lsd.log("Interact!") end
end
```

**Do something later / repeatedly** (Set Timer by Event):

```lua
function on_start(self)
  lsd.after(3, function() lsd.log("3 seconds passed") end)
  self.timer = lsd.every(1, function() lsd.log("tick") end)   -- lsd.cancel(self.timer) stops it
end
```

**Find another object and move it**:

```lua
local lift = lsd.find("Lift")            -- its name in the Outliner
lsd.set_position(lift, 0, 0, 5)          -- metres, Z is up
```

**Talk to other objects** (Event Dispatcher / Interface):

```lua
lsd.broadcast("on_alarm", 2)             -- every script with function on_alarm(self, level)
lsd.call(lsd.find("Guard"), "on_alarm", 2)  -- only the Guard's scripts
```

**Change a component's settings** (any Details field, by its name):

```lua
lsd.set(self.entity, "Character", { walk_speed = 8 })
local c = lsd.get(self.entity, "Character")
lsd.log(c.walk_speed)
```

## 9. When something goes wrong

| You see | It means | Do |
| --- | --- | --- |
| Red line in the Output Log with a file and line | The script hit an error and stopped (other scripts keep running) | Fix that line, save: it restarts |
| Nothing happens on Play | The object has no Script component, or the Script field is None | Section 4 |
| `attempt to call a nil value (field 'xyz')` | `lsd.xyz` is not a command (typo) | Check the name in [CODING.md](CODING.md#command-reference) |
| `attempt to index a nil value` | `lsd.find("Name")` found nothing, or a variable is not set yet | Check the Outliner name; set it in `on_start` |
| Double-click opens the wrong program | Windows has another app for `.lua` | Section 1 |
| `io` / `os.execute` missing | Scripts are sandboxed (no files, no programs) | Use `lsd.save_game` for saving |

Still stuck: take a screenshot of the Output Log and the debugger.
