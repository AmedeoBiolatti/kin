# General Animation System

The engine animation surface is `kin::anim`: `AnimationRegistry`,
`AnimationPlayer`, `AnimationStateMachine`, and `AnimationEventDispatch`.
Frames only (a walk cycle of a sprite sheet) need none of it: a sprite
catalog's `animation` lines, played by `SpriteAnimator`
([Sprite animations](rendering.md#sprite-animations)), are what scene files and
editors use.
The old ECS sprite/state animation API and frame-based renderer animation API
have been retired from the engine build.

## Animators: Animations In Scenes

`kin::Animator` (`kin/ecs/animator.hpp`) plays an animation of an animation
file of the game's content on an entity. Scene files name the file (`asset`, a
`.kinanim`) and the animation (`animation`); `Assets2D` reads the file, and
`update_animators(world, dt)` plays it each frame. That call also advances and
samples every other `AnimationPlayer`, so call it instead of those two.
- **What tracks set:** an Animator's property tracks can set any field of the
  world's described native components that holds a number, a pair, a colour, a
  whole number, a check or text (`register_described_properties`:
  `"Transform2D.pos"`, `"TextRenderer.color"`, `"CameraComponent.zoom"`).
- **Children:** a track's `target` names a child, by its name.
- **Sprites:** sprite tracks set the entity's `SpriteRenderer` sprite, from its
  catalog.
- **Events:** go to the entity's `AnimationEventQueue`.
- **Playing:** `speed` and `playing` control playback; `time` and `finished`
  report it. Naming another animation starts it from the beginning.
- **Prefabs:** an Animator on a prefab's instance gets its own copy, as do the
  components its tracks set, so instances animate independently.

```cpp
door.set(kin::Animator{.asset = {"animations/door.kinanim"}, .animation = "open"});
kin::update_animators(world, dt); // each frame
```

## Definitions And Players

Definitions are immutable and shared. Per-entity playback state lives on
`AnimationPlayer`.

```cpp
kin::AnimationRegistry registry;
registry.add(kin::Animation{
    .name = "hero.idle",
    .root = kin::repeat_node(kin::clip_node(kin::Clip{
        .duration = 0.4f,
        .sprites = {{
            .keys = {
                {.time = 0.0f, .sprite_id = "hero.idle.0"},
                {.time = 0.2f, .sprite_id = "hero.idle.1"},
            },
        }},
    }), 0),
});

entity.set(kin::AnimationPlayer{
    .registry = &registry,
    .properties = &properties,
    .sprite_catalog = &sprites,
});

auto* player = entity.get_mut<kin::AnimationPlayer>();
kin::set_base(*player, "hero.idle");
```

Frame order:

```text
advance_animation_players(world, dt)
sample_animation_players(world)
animation_events.run(world)
```

## Node Model3D

An `Animation` root is one of:

- `Clip`: property, sprite, and event tracks over a duration.
- `Ref`: named registry animation reference.
- `Sequence`: children played in order.
- `Parallel`: children played together.
- `Repeat`: child repeated N times, with `0` meaning forever.

Sprite animation is represented by `SpriteTrack` keys. Pivots and `SpriteBox`
data live on `SpriteKey`, so sheet import keeps per-frame metadata.

## State Machines

`AnimationStateMachine` is parameter driven. Bool and float parameters are held
on `AnimationPlayer::params`; triggers are consumed only when a transition is
taken.

```cpp
auto machine = std::make_shared<kin::AnimationStateMachine>();
machine->initial = "idle";
machine->states["idle"] = "hero.idle";
machine->states["run"] = "hero.run";
machine->transitions.push_back({
    .from = "idle",
    .to = "run",
    .when = {{.param = "moving", .op = kin::CondOp::IsTrue}},
});

player.machine = machine;
player.params.set_bool("moving", true);
```

Empty transition `from` means any current state. `ReplaceBase` changes the base
animation and current state. `PushOverride` pushes an override and leaves the
current state unchanged.
- **Order:** transitions are tried in order, and the first whose conditions
  all hold is taken.
- **Any-state into itself:** a transition from any state into the state already
  running, replacing it, is not taken, since it would restart that state every
  step.
- **Finished:** `CondOp::Finished` (`finished` in a file) holds once the
  state's animation has played through. Use it to go back to idle after an
  attack or a jump.

In a `.kinanim` file:

```text
statemachine hero
  initial idle
  state idle idle
  state run run
  state jump jump
  transition idle -> run when bool:running == true mode replace
  transition run -> idle when bool:running == false mode replace
  transition any -> jump when trigger:jump mode replace
  transition jump -> idle when finished mode replace
  place idle 0 0
```

A transition's `blend` (seconds; `... mode replace blend 0.2` in a file) fades
the state it leaves into the one it enters. The old base goes to
`AnimationPlayer::fading` and fades out while the new base fades in; numeric
fields are mixed by their weights, and the new state's sprite shows at once.
A push with a blend fades its override in over that time.

### Blend spaces

A blend space (`blend_space.hpp`) mixes animations by a float parameter. Each
animation sits at a position along it, and the two either side of the value
are mixed by nearness. Their clips are kept in step: each goes through its loop
in the same share of a turn, so a walk's and a run's steps land together. Only
the heaviest sends events and sets the sprite. A state plays a blend space by
its name, as it plays an animation; `set_base` does too.

```text
blendspace locomotion speed
  at 0 walk
  at 1.5 jog
  at 4 run

statemachine hero
  initial ground
  state ground locomotion
  state jump jump
  transition any -> jump when trigger:jump mode replace blend 0.15
  transition jump -> ground when finished mode replace blend 0.25
```

`AnimationRegistry::add_blend_space`, `blend_space` and `blend_weights` give
the same from code.

`place` lines say where an editor draws each state (and `any`, the any-state).
Editors write them; nothing reads them when the machine plays.

An `Animator` runs a machine of its file when it names one (`machine`, in place
of `animation`). The game sets the parameters on the Animator
(`animator.params.set_bool("running", true)`, `set_trigger("jump")`; a trigger
is handed to the machine once). `state` reports the state it is in.

## Layers And Blending

Layer 0 is the base animation. `push(...)` adds override layers above it:

```cpp
kin::push(player, "hero.attack", {.weight = 0.5f, .blend_in = 0.15f});
```

Numeric property tracks (`float`, `vec2`, `color`, `int`) blend per property
using normalized positive layer weights. `blend_in` ramps an override's effective
weight from `0` to its target weight. Sprite tracks, events, strings, and bools
remain topmost-wins.

## Templates And Bindings

Animation names and string fields can contain placeholders:

```text
animation {X}.idle template
  repeat 0
    clip 0.4
      sprite {X}.idle.0 0
      sprite {X}.idle.1 0.2
```

Each player supplies bindings:

```cpp
player.bindings["X"] = "goblin";
kin::set_base(player, "{X}.idle");
```

The registry resolves this to `goblin.idle`, instantiates it, and caches the
concrete animation.

## Events

Events are emitted into `AnimationEventQueue` and dispatched by channel:

```cpp
kin::AnimationEventDispatch dispatch;
dispatch.on("sound", kin::make_audio_event_interpreter(audio, audio_catalog));
dispatch.on("particle", kin::make_particle_event_interpreter(particles, particle_catalog));
dispatch.run(world);
```

Event interpreters use the dispatched target entity, apply `Transform2D` plus
`event.offset`, and treat `event.value` as the cue/effect id.

## Introspection

`snapshot(player)` returns a read-only `AnimationPlayerSnapshot` for tools. It
reports state, playing flag, bindings, pending triggers, and active layer
summaries without exposing cursor internals.

`world.snapshot` scene-server responses include a top-level `animations` array
for ECS scenes by default. Pass `"animations":false` in request params to omit
it.

The default ECS component inspector includes `AnimationPlayer`. It shows runtime
state, bindings, triggers, and layers read-only; only `playing` is editable.

## Assets And Import

`.kinanim` and `.kinanim2` load as `AnimationRegistryFragment`. A scene-owned
`AnimationRegistry` merges fragments so current players keep the same registry
pointer across reloads.

```cpp
auto fragment = assets.load<kin::AnimationRegistryFragment>("actors.kinanim");
registry.merge(*fragment);
```

`.kinanimsm` loads the new `kin::AnimationStateMachine` format.

Sprite-sheet import still exists, but it now emits sprites into `SpriteCatalog`
and concrete animations into `AnimationRegistry`:

```cpp
kin::AnimationRegistry imported;
kin::add_animation_sprites(catalog, assets, renderer, import, {.id_prefix = "player"}, &imported);
```

## Diagnostics And Preview

`AnimationAssetLibrary` stores the latest non-blocking diagnostics produced while
merging or reloading fragments. Call `diagnostics()` for the last merge/reload or
`diagnostics_for(path)` for a source-specific copy.
Parse/load failures keep the previous registry definitions unchanged during
reload. Semantic validation issues such as missing sprites or unresolved refs are
reported as diagnostics but do not block the merge.

```cpp
kin::AnimationAssetLibrary library;
library.merge(fragment, [&](std::string_view sprite) {
    return sprites.contains(sprite);
});
```

`AnimationPreview` is a test/editor helper that owns a temporary ECS world and
player:

```cpp
kin::AnimationPreview preview{{.registry = &library.registry, .sprite_catalog = &sprites}};
preview.set_animation("hero.idle");
preview.advance(1.0f / 60.0f);
preview.sample();
kin::AnimationPlayerSnapshot state = preview.snapshot();
```

The scene server command `animation.diagnostics` returns the active scene's
animation library diagnostics when the scene exposes one through
`Scene::animation_library()`.
