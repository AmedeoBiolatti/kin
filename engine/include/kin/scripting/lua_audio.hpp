#pragma once

#include <sol/sol.hpp>

#include <string_view>

namespace kin {

class AudioCatalog;
class AudioEngine;

// Binds `audio` playing cues from `catalog` as a Lua table (`audio` by
// default). Both must outlive the Lua state. Handles are integers; 0 is none.
//
//   local h = audio.play("door", {volume = 0.8, x = 120, y = 40})
//   audio.stop(h, 0.3)
//   audio.play_music("town", 2.0)       -- crossfade; the same cue keeps playing
//   audio.set_bus_volume("music", 0.5, 1.0)
//   audio.set_bus_paused("sfx", true)
//
// Functions: play(cue[, opts]) -> handle (opts: volume, pitch, x, y, priority,
// fade_in), stop(h[, fade]), playing(h), set_volume(h, v[, fade]),
// set_pitch(h, p), set_position(h, x, y), seek(h, seconds), position(h),
// set_paused(h, paused), paused(h), play_music(cue[, crossfade]),
// stop_music([fade]), set_bus_volume(bus, v[, fade]), bus_volume(bus),
// set_bus_muted(bus, muted), set_bus_paused(bus, paused), stop_bus(bus[, fade]),
// set_bus_effects(bus, {{type = "lowpass", cutoff = 800}, ...}),
// set_bus_effect(bus, index, {cutoff = 400}) (1-based; fields left out keep
// their values), output_devices() -> {names}, set_output_device(name) ("" for
// the default), output_device(), set_listener(x, y). Effect fields are named as in a
// .kinaudio effect line.
//
// For a LuaScript, call it from LuaScriptOptions::setup; for a ScriptScene,
// from ScriptSceneConfig::bind.
void bind_lua_audio(sol::state_view lua, AudioEngine& audio, const AudioCatalog& catalog, std::string_view name = "audio");

} // namespace kin
