# How to code in LSD_Engine

Step by step: where to click to write game code, attach it to an object, run it and debug it.
There are three ways to script, and you can mix them in one level:

| Way | Unreal equivalent | Where |
| --- | --- | --- |
| **Lua script** (`.lua`) | A Blueprint event graph written as text | Sections 1-6, 8 |
| **Visual Script** (`.lsdgraph`) | The Blueprint event graph | Section 7 |
| **C / C++ script** (`Source/*.cpp` / `*.c` -> `Game.dll`) | A C++ Actor Component class | Section 11 |

They get the same events (start, update, hit, overlap...), use commands with the same names
(`lsd.find` in Lua, `find` in C++, `lsd_find` in C) and can send each other events.

Contents: [1. One-time setup](#1-one-time-setup) · [2. Make a script](#2-make-a-script) ·
[3. Write the code](#3-write-the-code) · [4. Put it on an object](#4-put-it-on-an-object) ·
[5. Run it](#5-run-it) · [6. Debug it](#6-debug-it) · [7. Visual scripts instead of code](#7-visual-scripts-instead-of-code) ·
[8. Recipes](#8-recipes) · [9. When something goes wrong](#9-when-something-goes-wrong) ·
[10. Code with Claude Code](#10-code-with-claude-code) · [11. C / C++ instead of Lua](#11-c--c-instead-of-lua)

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

### Worked example: press a key to interact with what you look at

The Unreal "interact" pattern: on a key press, trace a line in front of the player; if it hits
something, tell that object it was used.

**A first try that does not work**, and why:

```text
Input Pressed (IA_Interact) --Pressed--> Branch.Condition
Branch True  --> Call Function "WBP_Interact" --Result--> Line Trace.Start (End 10, 4.8, 4.2)
             --> Play Sound 2D "Click" --> Print String "Worked"
Branch False --> Play Sound 2D "Click" --> Print String "Failed"
Get Variable "bInteract" (not wired)
```

| Problem | Why | Fix |
| --- | --- | --- |
| Nothing ever runs | **Input Pressed** is a value (green, no white pins), not an event. The Branch has no white wire coming in, so nothing starts it | Start from **Event Update** (Tick) and wire its white pin into the Branch |
| `Call Function` fails | It runs a **Function inside this graph** with that name. There is no Function called `WBP_Interact`, so the script stops with an error in the Output Log. It does not open a widget | Showing a UI document is **Open UI** (Document = the `.rml` name). Telling the hit object is **Call Event on Object** (below) |
| Trace starts in the wrong place | **Start** is wired to the function's Result, which is not a location. **End** is a fixed point in the world (10, 4.8, 4.2), the same wherever the player is | Start = the player's location at eye height; End = Start + the facing direction × reach |
| "Worked" even when nothing was hit | Line Trace's white output always continues | Add a **Branch** on the trace's **Hit** |
| "Failed" and a click **every frame** | Once driven by Event Update, the False path runs every frame the key is *not* pressed (60 times a second) | Leave Branch False empty |
| `bInteract` does nothing | A Get node that is not wired into anything is never read | Delete it, or wire it into a Branch to switch interacting on and off |
| No click sound | Play Sound 2D looks the sound up by **asset name** | Make sure a sound asset named `Click` exists (or type the real name) |

**The working graph** (on the player's prefab, or any object; it uses the player either way):

```text
Event Update ──▶ Branch (Condition ◀ Input Pressed: IA_Interact)
                   True ──▶ Line Trace
                              Start ◀ Vector + Vector ( Get Location ◀ Get Player Character ,  Make Vector 0, 0, 1.6 )
                              End   ◀ Vector + Vector ( that Start ,  Vector * Float ( Get Forward Vector ◀ Get Player Character , 3 ) )
                            ──▶ Branch (Condition ◀ Line Trace.Hit)
                                  True  ──▶ Call Event on Object (Target ◀ Hit Object, Name "on_interact")
                                          ──▶ Play Sound 2D "Click" ──▶ Print String "Worked"
                                  False ──▶ Print String "Nothing in reach"
```

Step by step in the graph window:

1. Right-click the canvas > **Event Update**. Then **Branch**; wire Event Update's white pin to it.
2. **Input Pressed**, Action `IA_Interact`; wire **Pressed** to the Branch's **Condition**.
3. **Get Player Character**, then **Get Location** (wire Player into its Target) and **Make Vector**
   (0, 0, 1.6: eye height). **Vector + Vector** adds them: that is the trace **Start**.
4. **Get Forward Vector** (Target ◀ Player) into **Vector * Float** with Scale `3` (reach in metres);
   a second **Vector + Vector** adds it to the Start: that is the trace **End**.
5. **Line Trace**: wire Branch **True** to its white input, Start and End from steps 3 and 4.
6. A second **Branch** after it with Condition ◀ **Hit**.
7. True: **Call Event on Object**, Target ◀ **Hit Object**, Name `on_interact`; then **Play Sound 2D**
   and **Print String** if you want them. False: a Print String, or nothing.
8. **Save + Compile** (Ctrl+S). Put the graph on an object with a Script component and press Play.

**The object being used** (a door, a lever...) gets its own visual script with a **Custom Event**
named `on_interact` and whatever should happen after it (Unreal: an Interact interface event). An
object without that event ignores the call. The line must hit it: tick **Collision** on its mesh (or give it a Rigid Body).

The same in Lua:

```lua
function on_update(self, dt)
  if not lsd.pressed("IA_Interact") then return end
  local p = lsd.player()
  local x, y, z = lsd.position(p)
  local _, _, yaw = lsd.rotation(p)
  local dx, dy = math.cos(math.rad(yaw)), math.sin(math.rad(yaw))
  local hit = lsd.raycast(x, y, z + 1.6, dx, dy, 0, 3)   -- 3 m in front, at eye height
  if hit then
    lsd.call(hit, "on_interact")
    lsd.play_sound("Click")
  end
end
```

Notes: the player's facing is its turn (yaw) only, so the line is level; looking up and down does
not tilt it. The trace never hits the player's own capsule.

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

## 10. Code with Claude Code

Claude Code can write the scripts for you and test them in the running editor, through the
**Claude MCP** plugin.

**Turn it on (once):**

1. **Edit > Editor Preferences**, scroll to **Plugins**.
2. Tick the box next to **Claude MCP**, then click the name **Claude MCP** to open its page.
   It says *Waiting for Claude Code on 127.0.0.1:7780*. (If it says the port is in use, type another
   Port, e.g. 7781.)
3. Click **Copy Command**, open a terminal (VS Code: **Ctrl+`**), paste and press Enter. Or click
   **Write .mcp.json** to set it up for this project only.
4. Start Claude Code in the project folder (`claude`). Type `/mcp`: **lsd-engine** is listed as
   connected, and the plugin page says *Claude is connected*.

**Then just ask**, in Unreal terms if you like:

- "Make a Lua script that opens the door named Door when the player presses E within 2 m, put it
  on the Door and test it."
- "List the assets in Content/Script and explain what Interact_System does."
- "Press Play, check the Output Log for script errors, and fix them."

Claude writes `.lua` / `.lsdgraph` files into Content/ (they reload by themselves) and uses the
plugin's tools: `console` (play, stop, save, open...), `lua` (while playing), `list_objects`,
`list_assets` and `read_log`. Keep the editor open while it works. Untick the plugin when you are
done: while it is on, any program on this PC could send the editor commands.

## 11. C / C++ instead of Lua

The same jobs in compiled C or C++ (Unreal: a C++ Actor Component). Full reference: [CODING.md](CODING.md).

**One-time:** Visual Studio 2022 or newer with **Desktop development with C++** (already there if
you build the engine).

**Make a script:**

1. **Tools > New C / C++ Script...**. Pick the language with the **C++ | C** switch, type a name
   (letters and digits, e.g. `Door`), click **Create**.
2. The editor writes `Source/Door.cpp` (or `Source/Door.c`), plus `Source/CMakeLists.txt` and
   `Source/GameModule.cpp` the first time, and compiles. The **Output Log** shows the build, then
   *C / C++ scripts loaded: 1*.
3. Open the file in VS Code (File > Open Folder on the project, or Ctrl+P and type its name).

C and C++ scripts can sit side by side in `Source/`; pick per script. C++ reads shorter (the example
below); C is the plain-C version of the same thing ([CODING.md](CODING.md#writing-it-in-plain-c)
shows a whole C script).

**Put it on an object:**

1. Select the object, **Details > Add Component > Native Script**.
2. In **Script**, pick `Door`. Its properties appear below as **Script properties**; change
   them per object like a Lua script's.

**Write the code** (the template spins the object; here is the door from section 8 in C++):

```cpp
#include <lsd/game.hpp>
using namespace lsd::game;

struct Door {
    float open_angle = 90.0f;
    static void properties(Properties<Door>& p) { p.add("open_angle", &Door::open_angle); }

    void on_overlap_begin(Self self, Entity other) {
        if (other == player()) {
            Vec3 r = rotation(self);
            set_rotation(self, {r.x, r.y, r.z + open_angle});
            play_sound("S_DoorOpen");
        }
    }
};
LSD_SCRIPT(Door)
```

| Lua | C++ |
| --- | --- |
| `function on_start(self)` | `void on_start(Self self)` |
| `function on_update(self, dt)` | `void on_update(Self self, float dt)` |
| `self.entity` | `self` |
| `self.speed = 2` (state) | a field: `float speed = 2;` |
| `properties = { speed = 2 }` | `static void properties(Properties<T>& p) { p.add("speed", &T::speed); }` |
| `lsd.find("Lift")` | `find("Lift")` |
| `local x, y, z = lsd.position(e)` | `Vec3 p = position(e);` |
| `lsd.raycast(x, y, z, dx, dy, dz, 3)` | `if (auto hit = raycast(from, dir, 3)) { hit->entity ... }` |
| `lsd.call(e, "on_interact")` | `call(e, "on_interact");` |
| `lsd.log("hi", n)` | `log("hi %d", n);` |

**Compile and test:**

1. Save the file, press **Ctrl+Alt+F11** (Tools > Compile Game Code).
2. Errors show in red in the Output Log with file and line; fix and compile again. The old code
   keeps running meanwhile.
3. When it succeeds the editor loads the new code by itself, **even during Play** (scripts restart
   with fresh state).

**Debug:** Visual Studio: **Debug > Attach to Process > lsd_editor.exe**. VS Code: Run and Debug >
create a launch.json > **C++ (Windows): Attach**, pick `lsd_editor.exe`. Set breakpoints in your
`.cpp`, then Play.

**Mixing:** a C or C++ script can `call(e, "unlock")` an object's Lua script (`function unlock(self,
other)`) or a visual script's Custom Event `unlock`; Lua's `lsd.call(e, "unlock")` and Call Event on
Object nodes reach the script's `on_event(self, name, other)`.

**Claude Code** can do all of this through the Claude MCP plugin (section 10): it writes
`Source/*.cpp` / `*.c` and runs `game.Compile`, `game.NewScript <Name> [c]` and `game.Scripts` as console commands.
