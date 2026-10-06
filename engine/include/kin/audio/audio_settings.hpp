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
// Writes an object with the volume and mute of every bus the game changed
// from its default; pauses are not settings and are left out.
void write_audio_settings(JsonWriter& json, const AudioEngine& audio);
// Sets the volumes and mutes write_audio_settings wrote. Unknown buses are
// created, so settings can be applied before any sound has played.
void apply_audio_settings(AudioEngine& audio, const JsonValue& settings);

} // namespace kin
