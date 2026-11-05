# Input actions and axes

A game asks for what the player does by name, not by key: actions ("jump",
held or not) and axes ("move_x", a number from -1 to 1). An `InputMap` binds
them to keys, mouse buttons and game controller controls; `Input` answers.

```cpp
kin::InputMap map = kin::load_input_map(kin::content_root() / "input.kininput");
ctx.input.set_map(map);
ctx.app.open_gamepads(); // every controller, polled with the window's events

if (ctx.input.pressed("jump")) { /* ... */ }
const f32 run = ctx.input.axis("move_x"); // A/D, the arrows, the d-pad or the left stick
```

## Input files

A `.kininput` file is one binding a line (`#` starts a comment):

```text
key jump Space
pad jump a
mouse fire MouseLeft
key save S ctrl
axis move_x - key:A
axis move_x + key:D
axis move_x - pad:dpad_left
axis move_x + pad:dpad_right
axis move_x stick left_x
axis move_y stick -left_y
axis move_x deadzone 0.2
```

- `key`, `mouse` and `pad` bind an action. Keys may name modifiers after them
  (`ctrl`, `alt`, `shift`).
- An axis's `-` and `+` lines give what sets it to -1 and to 1 while held (both:
  0), as `key:`, `mouse:` or `pad:` bindings.
- `stick` reads a controller's analog input as far as it is pushed (`left_x`,
  `left_y`, `right_x`, `right_y`, `left_trigger`, `right_trigger`; `-` for its
  other way). Below `deadzone` (0.2 by default) a stick counts as 0, and above it
  the value is scaled back to 0..1.
- When several of an axis's bindings are held, the one pushed furthest wins.

`parse_input_map` and `serialize_input_map` read and write the text, and
`load_input_map` and `save_input_map` do the same with files. A line that does
not read fails the whole file, and `error` names the line.

## Controller controls

`pad` bindings name a `PadControl`: the face buttons `a`, `b`, `x` and `y`,
`back`, `start`, `left_stick` and `right_stick` (pressed in), `left_shoulder`,
`right_shoulder`, `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right`, a stick
pushed past halfway (`left_stick_left`, `right_stick_up`, ...) and a trigger
pulled past halfway (`left_trigger`, `right_trigger`). Each one gets
`pressed`/`released` edges the same way keys do.

`App::open_gamepads()` opens every controller SDL finds, and those plugged in
later, and polls them with the window's events. A stick's y is positive pushed
up. Without it, or with no controller connected, pad bindings are never held. A
test or a replay sets the state itself with `Input::set_gamepad(slot, state)`.

## Several controllers and local players

Each controller gets a slot, 0 to `kin::max_gamepads - 1` (8 slots), and keeps
it while it is plugged in. A new controller takes the lowest slot not used yet.
One plugged in again takes back its old slot, so a cable pulled out by mistake
keeps its player's place. `Input::gamepad(slot)` is a slot's state, and
`gamepad_plugged(slot)` and `gamepad_unplugged(slot)` are edges that last like
`pressed()`'s. `App::gamepads()` names them (`name(slot)`).

A one-player game sets nothing. There is then one player, player 0, and every
device is theirs, so the keyboard and any controller move them. A game for
several players at one screen gives each player their devices:

```cpp
ctx.input.set_players({{.keyboard = true}, {.gamepads = {0}}, {.gamepads = {1}}});

if (ctx.input.pressed(1, "jump")) { /* the first controller's player */ }
const f32 run = ctx.input.axis(2, "move_x");
if (ctx.input.pressed("pause")) { /* anyone, on any device */ }
```

- `pressed`, `held`, `released`, `frame_pressed`, `frame_released` and `axis`
  take a player first. Without one (or with -1) they ask about anyone: every
  device, owned or not, with the Input's map, and each player with their own map.
- An `InputPlayer` has the keyboard and mouse (`keyboard`), controller slots
  (`gamepads`), and optionally a `map` of their own. Two players can share
  one keyboard, one on WASD and one on the arrows, each with their own map.
  Without a map, a player reads the Input's.
- Joining: `gamepad_pressed(PadControl::Start, true)` is the lowest slot whose
  Start was pressed and that no player has (-1: none). `add_player` returns the
  new player's number and `remove_player` takes one out (those after it move
  down). `player_of_gamepad(slot)` says whose a controller is.
- `player_connected(player)` is false when the player has no keyboard and no
  controller plugged in. That is when the game shows "reconnect your
  controller".
- `last_device(player)` (keyboard, mouse or controller) and
  `last_gamepad(player)` say what the player pressed last, for the prompts to
  show.
- `rumble(player, low, high, seconds)` shakes the controller the player used
  last. `low` is the heavy motor and `high` the light buzz, 0 to 1.
  `rumble_gamepad(slot, ...)` shakes a slot's controller. A new rumble
  replaces the one going. App sends them to the controllers next frame.
  Without `open_gamepads` they are dropped.

ui2 moves its focus with the d-pad and left stick, and with the `menu_*`,
`accept` and `quit` actions ([ui](ui.md#controllers-and-menu-actions)).

Entity scripts read the player their `ScriptComponent` names (`player`: 0
anyone, 1 the first...), and Lua's `input` table can join, leave and rumble
([scripting](scripting.md#scripts-on-entities)). Agents and tests drive a slot
with the scene server's `input.pad` ([agent interface](agent_interface.md)).

## For editors

`binding_held(input, binding[, player])` and `axis_value(input, axis[, player])`
read a binding or an axis that is not in the map. An editor can try a file being edited without
replacing its own actions. `pad_control_held` reads one control of a
`GamepadState`.

Kin Studio edits a project's `content/input.kininput`, and the template game
reads it again when it is saved.
