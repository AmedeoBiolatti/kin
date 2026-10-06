# Audio: What Is Missing

What kin's audio does not do yet, measured against Godot 4 and Bevy (built-in
`bevy_audio`), and the limits of what it does. [Audio](audio.md) describes what
is there.

## Features

Ordered by how much a 2D game is likely to need them.

| Missing | Godot | Bevy | Cost for kin |
|---|---|---|---|
| Audio generated as it plays | `AudioStreamGenerator` | Custom `Decodable` sources | Small to medium |
| Reverb zones | `Area2D` audio bus override | None | Medium |
| More effects: distortion, chorus, phaser, pitch shift, stereo width | Built in | None | Medium in total, small each |
| Microphone input and recording | `AudioStreamMicrophone`, `AudioEffectRecord` | None | Medium |
| Text-to-speech | `DisplayServer.tts_speak` | None | Medium |
| Surround output (5.1, 7.1) | Yes | No | Medium |
| Web and mobile | Yes | Web | Part of kin's platform work |
| Bus editor and audio previews | Editor | None | Out of scope |

**Audio generated as it plays.** A game can register a clip it synthesized
(`add_clip`), but not a source the mixer pulls from as it plays: an engine hum
that follows the throttle, a procedural ambience, a chiptune synth. This would
be a clip backend whose frames come from a callback on the audio thread, read
the way a streamed clip's decoder is.

**Reverb zones.** In Godot, an `Area2D` can send the sounds inside it to
another bus, so a cave gets the cave reverb. In kin a cue names its bus, so
the same footstep cannot pick up a cave's reverb on its own. This needs a way
to move a voice to another bus, or to send part of it to an effect bus. The
zones could use kin's physics sensors.

**More effects.** Each would be another `AudioEffectProcessor` (see
`engine/src/audio/audio_effects.cpp`):

- distortion and bit-crushing;
- chorus and flanger, which are modulated short delays;
- phaser;
- stereo widening;
- pitch shift that keeps the tempo, the largest of these: it needs a phase
  vocoder or granular resampling.

**Microphone input and recording.** SDL3 can open recording devices. A
capture source would also make a recording effect possible.

**Text-to-speech.** For accessibility. Godot uses each platform's speech API;
SDL does not wrap them.

**Surround output.** The mixer works in mono or stereo. Positions are 2D, so
surround would mostly mean placing sound around the listener rather than to
the left and right. It is of little use for most 2D games.

**Web and mobile.** Kin builds for Windows and Linux. Audio itself would
follow SDL to other platforms. Browsers only start audio after the player
interacts with the page, so a web build would need to handle that.

**A bus editor and audio previews.** Godot has an editor for buses and
effects, and previews clips as waveforms. Kin is code- and agent-first: audio
is a text catalog plus `write_report` and the spectrum API. A visual editor is
out of scope, the same as for the rest of kin.

## Limits of what exists

These are not missing features, but boundaries a game may run into.

**Clips and formats**
- Ogg Opus is not supported; use Ogg Vorbis.
- Files with more than two channels play only their first two (Ogg Vorbis is
  mixed down properly).
- A clip is at most 2³¹ frames, about 12 hours at 48 kHz.
- Streamed clips decode on the audio thread, under the engine's lock. A
  dozen streams is fine; hundreds would be heavy.
- Loop points are in frames; there is no way to give them in seconds or beats.

**Mixing**
- Output is mono or stereo, at whatever rate the engine was made with.
- A voice plays on the one bus its cue names, and a bus mixes into its one
  parent. There are no sends (part of a voice to a reverb bus); Godot does not
  have them either.
- The master limiter has no lookahead, so the first sample of a loud peak is
  turned down abruptly.
- Spatial sound is 2D: distance and left-right pan only. There is no Doppler,
  no occlusion, no filtering with distance.

**Effects and analysis**
- A delay holds at most twice the first time it was made with (between one
  and five seconds). A longer time needs a new chain from `set_bus_effects`.
- The spectrum always comes from the last 2048 frames (~43 ms at 48 kHz,
  ~23 Hz per bin), so the lowest bands are coarse.
- Meters fall back over a fixed ~0.3 s.

**Music**
- Beat and bar sync needs a cue `bpm` and assumes a steady tempo, with beat
  one at `beat_offset`. There are no tempo maps or changing time signatures.
- `match_position` assumes the two pieces are laid out the same way in time.
- A playlist only moves to its next clip if `update()` runs at least once
  every couple of seconds.
- There are no transition segments (a fill or bridge played between two
  pieces), unlike Godot's `AudioStreamInteractive`. A `play_synced` stinger
  timed with `play_music` comes close.

**Devices**
- Moving to the default device when the chosen one is unplugged, and back
  when it returns, is checked about once a second. It has not been tested
  with real hardware, only with SDL's dummy driver.
- Input (recording) devices are not listed. See microphone input above.
