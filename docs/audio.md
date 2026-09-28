# Kin Audio

Kin audio is an engine-level service with a thin ECS bridge. Games describe what
should be heard in game terms: music cues, ambient beds, one-shot effects,
looping emitters, animation events, and listener position.

## Catalogs

`AudioCatalog` maps cue ids to authored playback data and can be saved as a
`.kinaudio` text asset.

```text
bus master 1
bus music 0.8 master
bus sfx 1 master

clip step audio/step.wav
clip hit audio/hit.wav

cue footstep sound sfx priority=20 spatial=true min=24 max=220 volume=0.7 clips=step
cue impact sound sfx priority=40 spatial=true clips=hit
```

## Runtime

`AudioEngine` owns runtime playback:

```cpp
kin::AudioPlayRequest request{
    .cue = "impact",
    .position = entity_pos,
    .has_position = true,
};
audio.play(catalog, request);
```

The ECS bridge provides listeners, emitters, and one-shots:

```cpp
void update_audio_listeners(EcsWorld& world, AudioEngine& audio);
void update_audio_emitters(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog);
void consume_audio_one_shots(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog);
```

`AudioEmitter` and `AudioListener` follow `Transform2D`. `AudioOneShot` is a
transient component that plays once and is removed.

## Animation Integration

Animation clips emit `AnimationEvent`s through `EventTrack`s:

```text
animation hero.attack
  clip 0.12
    event 0.06 sound name=sfx value=slash offset 0 -8
```

The ECS audio bridge registers a `"sound"` interpreter with
`AnimationEventDispatch`:

```cpp
kin::AnimationEventDispatch dispatch;
dispatch.on("sound", kin::make_audio_event_interpreter(audio, catalog));
```

For each event, the interpreter uses `event.value` as the cue id and the
dispatched target entity's `Transform2D` plus `event.offset` as the source
position.

## Runtime Order

A typical ECS scene frame should run:

```text
gameplay update
advance_animation_players(world, dt)
sample_animation_players(world)
animation_events.run(world)
consume_audio_one_shots(world, audio, catalog)
update_audio_listeners(world, audio)
update_audio_emitters(world, audio, catalog)
audio.update(dt)
render
```

## Backend

The SDL-backed runtime hides backend details behind public Kin audio types and
falls back to a null backend when device creation fails, so tests and headless
logic do not require an audio device.
