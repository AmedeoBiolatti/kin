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
clip hit audio/hit.ogg
clip theme music/theme.ogg stream=true loop_start=211680

cue footstep sound sfx priority=20 spatial=true min=24 max=220 volume=0.7 clips=step
cue impact sound sfx priority=40 spatial=true clips=hit
cue theme music music loop=true clips=theme
```

### Clips

Kin plays WAV, AIFF, FLAC, Ogg Vorbis and MP3, recognising a file by its
contents. Mono files stay mono. Files with more than two channels play their
first two; Ogg Vorbis is mixed down to stereo properly. Ogg Opus is not supported.

A clip is decoded whole when it is first loaded, which suits short sounds. With
`stream=true` the file stays compressed in memory and each voice decodes it
while it plays: a three-minute stereo track takes a few megabytes instead of
about 70 MB. Use it for music and long ambience.

`loop_start` and `loop_end` are frames at the file's own sample rate. A looping
cue plays from the start, and on reaching `loop_end` (or the end of the file)
jumps back to `loop_start`, so music can have an intro that plays once. A cue
that does not loop plays the whole file.

## Runtime

`AudioEngine` owns runtime playback:

```cpp
kin::AudioPlayRequest request{
    .cue = "impact",
    .position = entity_pos,
    .has_position = true,
};
kin::AudioHandle handle = audio.play(catalog, request);
audio.stop(handle, 0.5f); // fade out over half a second
```

Call the engine from one thread. With a device, mixing happens on the device's
audio thread, so a slow frame does not starve the sound card; `update(dt)` once
a frame ages voices and picks up clips that finished loading. With the null
backend nothing consumes audio in real time, so `update(dt)` mixes `dt` seconds
itself and voices still finish on time. `render(buffer)` mixes the next frames
into a buffer on the calling thread, for offline rendering and tests.

### Cues

A cue with several clips plays a random one each time, never the same one twice
in a row, and `pitch_var=0.1` varies its pitch by up to 10% either way. Both are
drawn from `AudioEngineConfig::seed`, so the same requests sound the same on
every run.

Clips play at their own sample rate and are resampled, with linear
interpolation, to the engine's. `fade_in` in a request fades the voice in.

### Buses

Bus volumes multiply down the hierarchy: the catalog's volume for each bus,
times what the game set at runtime.

```cpp
audio.set_bus_volume("music", 0.6f);      // settings slider
audio.set_bus_volume("music", 0.2f, 1.5f); // duck over 1.5 s
audio.set_bus_muted("master", true);
audio.set_bus_paused("sfx", true);        // pause menu: UI sounds keep playing
audio.stop_bus("ambient", 2.0f);
```

A paused bus's voices fade out over a few milliseconds and keep their place
until the bus is resumed. A stopped voice fades out too, however short the fade
asked for, so nothing clicks; `playing()` is false from the moment it is stopped.

### Voices

Each category has a voice cap (`AudioEngineConfig`), and a cue can limit its own
instances. When a cap is full, a new request takes over the voice that matters
least (lowest priority, oldest, furthest away) if it matters more, and is culled
otherwise; `stats()` counts both.

Spatial cues fade with distance between `min` and `max` and pan with an
equal-power law. A limiter on the output turns the mix down when voices add up
past full scale instead of clipping.

### Loading

`play()` decodes a clip the first time a cue needs it. To keep decoding out of
gameplay, load clips up front:

```cpp
audio.preload(catalog);                                  // now, e.g. on a loading screen
audio.preload_async(catalog, kin::default_job_system()); // on worker threads
```

A `play()` that needs a clip still loading waits for that job rather than
decoding the file again.

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

`IAudioBackend` is where the mix goes. A device backend calls the engine's
render function from its audio thread whenever the device needs more. The SDL
backend does this with an SDL audio stream callback. The engine falls back to
the null backend when no device opens, so tests and headless logic do not need
an audio device.
