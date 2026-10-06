#pragma once

namespace kin {

class AudioEngine;
class JsonValue;
class JsonWriter;

// The player's audio settings inside a game's settings file (SaveStore::write_settings):
//
//   store.write_settings([&](kin::JsonWriter& json) {
//       json.begin_object();
//       json.key("audio");
//       kin::write_audio_settings(json, audio);
//       json.end_object();
//   });
//   const kin::SaveLoadResult loaded = store.read_settings();
//   if (const kin::JsonValue* settings = loaded.result.ok ? loaded.payload.find("audio") : nullptr) {
//       kin::apply_audio_settings(audio, *settings);
//   }
//
// Writes the output device the player chose (if not the default) and the
// volume and mute of every bus the game changed from its default:
//   {"device": "Headphones", "buses": {"music": {"volume": 0.6, "muted": false}}}
// Pauses are not settings and are left out.
void write_audio_settings(JsonWriter& json, const AudioEngine& audio);
// Sets the device, volumes and mutes write_audio_settings wrote. Unknown buses
// are created, so settings can be applied before any sound has played; a
// device that is no longer plugged in leaves the default.
void apply_audio_settings(AudioEngine& audio, const JsonValue& settings);

} // namespace kin
