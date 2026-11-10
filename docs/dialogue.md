# Dialogue

A dialogue is a `.kindialogue` file (`kin/dialogue/dialogue.hpp`): speakers, and
nodes that lead from one to the next.

```json
{"id": "smith", "start_node": "hi",
 "speakers": [{"id": "smith", "name": "Smith", "portrait": "images/smith.png", "color": "e0a050"}],
 "nodes": [
   {"id": "hi", "kind": "line", "line": {"speaker": "smith", "text": "Need a [b]sword[/b]?", "next": "ask"}},
   {"id": "ask", "kind": "choice", "choices": [
     {"id": "buy", "text": "Yes ({gold} gold)", "target": "sold",
      "conditions": [{"kind": "number", "key": "gold", "compare": "ge", "value": 10}],
      "effects": [{"kind": "add_number", "key": "gold", "value": -10}, {"kind": "emit_event", "key": "bought_sword"}]},
     {"id": "no", "text": "No", "target": "end"}]},
   {"id": "sold", "kind": "line", "line": {"speaker": "smith", "text": "Mind the edge.", "next": "end"}},
   {"id": "end", "kind": "end"}]}
```

**Node kinds**
- **`line`:** someone says something, then goes on to its `next`.
- **`choice`:** the player picks. Each choice may have conditions on the
  variables or on what was seen before, and effects.
- **`jump`:** goes straight on to its `target`.
- **`event`:** tells the game, may have effects, then goes on.
- **`end`:** the dialogue is over.

**Effects** set, add to or text the variables, emit an event, or jump.
**Text** is markup (`[b]`, `{gold}` from the variables) and is translated
under `dialogue.<id>.<node>` (see [localization](localization.md)).

**Other starts:** `entries` lists the nodes a game also starts the dialogue
at (a `TalkComponent`'s `start`, `start(path, node)`), so what they lead to
counts as reached.

**Editor fields:** a node's `place` is where an editor draws it, and a
speaker's `color` is the colour of their name.

`dialogue_problems(doc)` lists what is wrong with a document:
- ids given twice;
- a start or a target that is no node;
- a speaker no speaker is;
- choices that lead nowhere;
- nodes never reached from a start.

`DialoguePlayer` plays a document, its state kept by `snapshot()` and
`restore()`.

## In a scene

`SceneDialogue` (`kin/ecs/dialogue.hpp`) runs the dialogue the game started,
one at a time:

```cpp
kin::SceneDialogue dialogue{kin::content_root()};
dialogue.start("dialogue/smith.kindialogue");   // or a TalkComponent starts it
// each step:
kin::update_talk(world, dialogue, ctx.input);   // "talk": start one, or on through it
dialogue.update(ctx.dt);                        // the text revealed, a changed file read again
for (const kin::DialogueEvent& e : dialogue.events()) { /* "bought_sword", "dialogue.ended" ... */ }
dialogue.clear_events();
```

- **Reveal:** a line's text is revealed `letters_per_second`.
  - `advance()` shows the rest of a line still being revealed, then goes on.
  - `choose(index or id)` picks a choice.
  - `view()` is what shows now. At a choice, it is the line before it with
    the choices.
- **Variables:** they belong to the game and are kept from one dialogue to
  the next. Read and set them with `variable` and `set_variable`.
- **Events:** `events()` holds what the dialogue told this step, with
  `dialogue.started` and `dialogue.ended` around it.
- **Saving:** `snapshot()` and `restore(path, snapshot)` save it.
- **Hot reload:** a file saved while it plays is read again, and play goes on
  from the same node when that node is still there.

A character the player talks to has a `TalkComponent`:
- `asset`: the dialogue;
- `start`: the node to start at;
- `radius`: how near the player must be;
- `prompt`: the word to show for it;
- `enabled`.

The player has a `Talker`.

`update_talk(world, dialogue, input, "talk")` starts the dialogue of the nearest
TalkComponent in reach when the action is pressed, and moves a running dialogue
on. `talk_target(world)` tells which one is in reach, for a "Talk" prompt.

**Showing it:** a UI screen's `dialogue` element shows it (see
[UI screens](ui.md#screens-from-data)):
- who speaks, in their colour;
- their portrait;
- the text as it is revealed;
- the choices, as buttons.

It is drawn on the theme's panel, wherever the screen places it.
`SceneScreens::set_dialogue(&dialogue)` connects them. Then a click on the
box goes on, and a click on a choice picks it.

**Lua:** entity scripts reach the dialogue as `dialogue`
(`EntityScripts::set_dialogue`):
- `dialogue.start(path, node)`, `stop()`, `active()`, `advance()`;
- `choose(i or id)`, `node()`, `get(name)`, `set(name, value)`.

A script's `on_dialogue_event(self, id, node)` hears what the dialogue
tells.

Kin Studio edits dialogues as a graph, reads them as a script, and plays them.
