# Input actions and axes

A game asks for what the player does by name, not by key: actions ("jump",
held or not) and axes ("move_x", a number from -1 to 1). An `InputMap` binds
them to keys, mouse buttons and game controller controls; `Input` answers.

```cpp
kin::InputMap map = kin::load_input_map(kin::content_root() / "input.kininput");
ctx.input.set_map(map);
ctx.app.open_gamepads(); // controllers polled with the window's events

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

`App::open_gamepads()` opens the first controller SDL finds, or the first one
plugged in later, and polls it with the window's events. A stick's y is
positive pushed up.
Without it, or with no controller connected, pad bindings are never held. A
test or a replay sets the state itself with `Input::set_gamepad`.

## Local players

Several players at one screen each read their own devices through the same
`Input`. A player's controller is the one their slot of `GamepadPlayers` has
(joining, reconnecting and rumble are there: [players and
controllers](2d_workflows.md#players-and-controllers)). App gives each one to
`Input::set_player_gamepad` every frame. `set_players` says who has the
keyboard and mouse, and gives a player their own map when two share one
keyboard, one on WASD and one on the arrows:

```cpp
ctx.input.set_players({{.keyboard = true}, {}});   // player 0: the keyboard and their controller

if (ctx.input.pressed(1, "jump")) { /* player 1's controller */ }
const f32 run = ctx.input.axis(0, "move_x");
if (ctx.input.pressed("pause")) { /* anyone, on any device */ }

ctx.input.set_players({{.keyboard = true, .map = wasd}, {.keyboard = true, .map = arrows}});
```

- `pressed`, `held`, `released`, `frame_pressed`, `frame_released` and `axis`
  take a player first. Without one (or with -1) they ask about anyone: every
  device, with the Input's map, and each player with their own map.
- A player without a map reads the Input's.
- With no players set, player 0 has the keyboard and mouse. Until someone
  joins a controller, the one `set_gamepad` gives (the first) counts as player
  0's too, so a one-player game sets nothing.
- `binding_held`, `binding_pressed` and `axis_value` take a player too, and
  `pad_frame_pressed(control, player)` reads one player's controller (-1: any).

Entity scripts read the player their `ScriptComponent` names (`player`: 0
anyone, 1 the first...) ([scripting](scripting.md#scripts-on-entities)). Agents
and tests drive a player's controller with the scene server's `input.pad`
([agent interface](agent_interface.md)).

## For editors

`binding_held(input, binding)` and `axis_value(input, axis)` read a binding or an
axis that is not in the map. An editor can try a file being edited without
replacing its own actions. `pad_control_held` reads one control of a
`GamepadState`.

Kin Studio edits a project's `content/input.kininput`, and the template game
reads it again when it is saved.
