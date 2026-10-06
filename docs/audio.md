# Kin Audio

Kin audio is an engine-level service with a thin ECS bridge. Games describe what
should be heard in game terms: music cues, ambient beds, one-shot effects,
looping emitters, animation events, and listener position.

[What is missing](audio_missing.md) lists what kin's audio does not do yet,
compared with Godot and Bevy, and the limits of what it does.

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

### Music

`play_music` plays one music cue at a time. Playing another crossfades to it,
and playing the cue already on keeps it going, so each scene can name its music
in `on_enter` without restarting a track that carries over:

```cpp
audio.play_music(catalog, "town", 2.0f); // crossfade over 2 s
audio.stop_music(1.0f);
```

`set_volume(handle, volume, fade)` and `set_pitch(handle, pitch)` change a
playing voice relative to its cue, and `playback_position(handle)` says how many
seconds into its clip it is. `seek(handle, seconds)` jumps within the clip,
fading out and back in over a few milliseconds so the jump does not click; it
works on streamed clips too. `set_paused(handle, true)` pauses one voice the way
a paused bus does: it goes quiet, keeps its place, and still counts as playing.

### Adaptive music

A music cue can carry its tempo, so changes land in time:

```text
cue explore music music loop=true bpm=110 beats_per_bar=4 clips=explore
cue combat  music music loop=true bpm=110 beats_per_bar=4 clips=combat
cue battle  music music loop=true bpm=140 layers=true clips=drums,bass,strings
cue radio   music music loop=true playlist=true shuffle=true clips=song1,song2,song3
```

```cpp
// Into combat on the next bar, from the same place in the piece:
audio.play_music(catalog, "combat", {.crossfade = 0.5f, .sync = kin::AudioSync::Bar, .match_position = true});
```

- `sync` is `Now`, `Beat`, `Bar` or `End` (where the current clip ends or
  loops). The new music starts on that exact frame, and the old one fades out
  from it. `beat_offset=` is the seconds before the first beat, for a pickup.
- `match_position` starts the new music where the old one is, for
  arrangements of one piece that share a tempo.
- A `layers=true` cue plays all its clips together, in step, as stems.
  `audio.set_music_layer("drums", 1.0f, 2.0f)` fades one in. The layer volumes
  carry over to later music with the same layer names.
- A `playlist=true` cue plays its clips one after another with no gap.
  `loop=true` repeats the list, and `shuffle=true` plays it in random order,
  never the same clip twice running. `update()` lines up the next clip a
  couple of seconds ahead, so call it every frame.
- `music_position()` gives seconds, beat, bar and beat within the bar, for
  gameplay or UI that moves with the music.
- `play_synced(catalog, request, kin::AudioSync::Beat)` plays any cue (a
  stinger) on the music's next beat, bar or end.

The default `music_voices` is 8, since each layer, and both sides of a
crossfade, take a voice.

### Effects

A bus can run effects over everything it plays, children included, before its
volume:

- Filters: low-pass, high-pass, band-pass and notch.
- EQ: a peak band and low and high shelves.
- A delay.
- A reverb.
- A compressor. Each bus is
mixed on its own and added into its parent, so an effect on `master` hears the
whole game, and one on `sfx` only the sound effects.

```text
effect sfx lowpass cutoff=800 q=0.7
effect ambient reverb room=0.7 damping=0.5 wet=0.3 dry=1 width=1
effect master compressor threshold=-12 ratio=4 attack=0.01 release=0.1 makeup=0
effect music peak freq=2500 gain=-3 q=1.2      # EQ: dB up or down around freq
effect music lowshelf freq=120 gain=4
effect music highshelf freq=8000 gain=-2
effect ui bandpass cutoff=1500 q=2             # also: notch
effect sfx delay time=0.3 feedback=0.4 wet=0.5 dry=1
```

A delay's new `time` glides in rather than jumping, so changing it while
playing does not crackle. Its line holds up to twice the first time it was made
with (one second at least).

`effect` lines follow the `bus` they name; `enabled=false` keeps an effect in
the chain but bypassed. At runtime, `set_bus_effects(bus, chain)` replaces a
bus's chain (the catalog's no longer applies to it), and `set_bus_effect(bus,
index, effect)` changes one while it plays: a filter's cutoff glides to the new
value over about 20 ms, and a reverb keeps its tail, so a muffled pause menu is
a cutoff sweep:

```cpp
kin::AudioEffect muffled{.type = kin::AudioEffectType::LowPass, .cutoff = 400.0f};
audio.set_bus_effects("sfx", {muffled});
// later
muffled.cutoff = 20000.0f;
audio.set_bus_effect("sfx", 0, muffled);
```

### Levels and spectrum

Every bus and the final output keep a level meter: what the bus sends to its
parent, after its effects and volume.

```cpp
kin::AudioLevel music = audio.bus_level("music"); // peak and rms, 1 is full scale
kin::AudioLevel out = audio.output_level();
```

The peak falls back over about 0.3 s after a hit, and `rms` averages over about
as long, which suits a meter display.

For a music visualizer, turn on analysis for a bus (`""` is the output) and ask
for its spectrum each frame:

```cpp
audio.enable_analysis("music");
std::vector<float> bars = audio.spectrum("music", 32);    // 20 Hz..20 kHz, spaced by pitch
float bass = audio.magnitude("music", 40.0f, 120.0f);
```

Each value is an amplitude: a tone of amplitude 0.5 reads about 0.5 in its band.
The mixer only records the last 2048 frames of an analysed bus, and the FFT runs
on the thread that asks.

### Ducking

A `duck` line turns one bus down while anything plays on another, or on a bus
under it:

```text
duck music when=dialogue volume=0.3 attack=0.15 release=0.8
```

`volume` is how far down it goes, and `attack` and `release` are the seconds it
takes to go down and to come back up.

### Voices

Each category has a voice cap (`AudioEngineConfig`), and a cue can limit its own
instances. When a cap is full, a new request takes over the voice that matters
least (lowest priority, oldest, furthest away) if it matters more, and is culled
otherwise; `stats()` counts both.

Spatial cues fade with distance between `min` (full volume) and `max`
(silent), along the cue's `rolloff` curve:

- `smooth` (the default): holds near the source and eases out at the edge.
- `linear`.
- `inverse`: falls fast near the source and slowly far away, like real sound,
  still reaching silence at `max`.

`rolloff_power=2` squares the curve (it falls sooner), and `0.5` makes it fall
later. They pan with an equal-power law; `pan=0.5` halves how far a cue pans,
and `pan=0` keeps it centred. A limiter on the output turns the mix down when voices add up
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

Loaded clips stay cached until the game frees them:

```cpp
audio.unload(level_catalog); // drop a level's clips (voices playing them keep them)
audio.unload_unused();       // free every cached clip nothing is playing
audio.loaded_clip_bytes();   // what the cache holds now
```

A voice that finishes hands its clip back to the game thread, and `update()`
frees it there, so the audio thread never frees a clip's samples.

### Reloading

While a game runs, edited audio files can be picked up without a restart:

```cpp
audio.watch(files);                                     // clip files, as they load
audio.watch_catalog(files, root / "audio.kinaudio", catalog);
```

Polling the `FileWatcher` (which `run_scene_app` does every frame) runs the
reloads. A changed clip is decoded again: new plays use it, and voices already
playing finish with the old version. A changed catalog replaces `catalog` and
applies its buses, effects and duck rules at once. A file that fails to load
keeps the last good version. `apply_catalog(catalog)` applies a catalog edited in
code the same way.

### Output devices

```cpp
for (const std::string& name : kin::list_audio_output_devices()) { /* a settings menu */ }
audio.set_output_device("Headphones"); // "" is the system default
```

The default follows whatever the system chooses. If the device the game chose
is unplugged, playback moves to the default and moves back when it returns
(checked about once a second in `update()`).

The ECS bridge provides listeners, emitters, and one-shots:

```cpp
void update_audio_listeners(EcsWorld& world, AudioEngine& audio);
void update_audio_emitters(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog);
void consume_audio_one_shots(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog);
```

`AudioEmitter` and `AudioListener` follow `Transform2D`. An emitter's `volume`,
`pitch` and `paused` apply to its sound every update, and `when_done` says what
happens when the sound ends:

- `Restart` (the default): play it again.
- `Keep`: stay, silent.
- `Remove`: remove the emitter.
- `Despawn`: destroy the entity, for a sound fired and forgotten.

Removing an emitter, or destroying its entity, fades its sound out.
`AudioOneShot` is a transient component that plays once (with its own `volume`
and `pitch`) and is removed.

```cpp
world.entity().set(kin::Transform2D{.pos = door})
              .set(kin::AudioEmitter{.cue = "creak", .when_done = kin::AudioEmitterEnd::Despawn});
```

## Settings

`write_audio_settings` and `apply_audio_settings` keep the player's output
device, bus volumes and mutes in the game's settings file, as
`{"device": "Headphones", "buses": {"music": {"volume": 0.6, "muted": false}}}`:

```cpp
store.write_settings([&](kin::JsonWriter& json) {
    json.begin_object();
    json.key("audio");
    kin::write_audio_settings(json, audio);
    json.end_object();
});

const kin::SaveLoadResult loaded = store.read_settings();
if (const kin::JsonValue* settings = loaded.result.ok ? loaded.payload.find("audio") : nullptr) {
    kin::apply_audio_settings(audio, *settings);
}
```

## Reports

`audio.write_report(json)` writes the engine's stats, buses (with their volume,
gain and duck) and voices. A scene can call it from its own `write_report`, so
agents and tests can see what is playing.

## Lua

`bind_lua_audio` (`kin/scripting/lua_audio.hpp`) gives scripts an `audio` table:

```cpp
kin::ScriptSceneConfig config{
    .script_path = "scripts/town.lua",
    .bind = [&](sol::state& lua) { kin::bind_lua_audio(lua, audio, catalog); },
};
```

```lua
local door = audio.play("door", {volume = 0.8, x = 120, y = 40})
audio.play_music("town", 2.0)
audio.set_bus_paused("sfx", true)
audio.set_bus_effects("sfx", {{type = "lowpass", cutoff = 800}})
audio.set_bus_effect("sfx", 1, {cutoff = 300}) -- 1-based; other fields kept
local peak, rms = audio.bus_level("music")
audio.enable_analysis("music")
local bars = audio.spectrum("music", 16)
audio.play_music("combat", {crossfade = 0.5, sync = "bar", match_position = true})
audio.set_music_layer("drums", 0.0, 2.0)
local beat = audio.music_position().beat
audio.play_synced("stinger", "beat")
```

Handles are integers. A `LuaScript` binds it the same way from `setup`.

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
