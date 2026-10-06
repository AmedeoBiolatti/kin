#include <kin/scripting/lua_audio.hpp>

#include <kin/audio/audio_engine.hpp>

#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace kin {
namespace {

AudioHandle handle_of(i64 id) {
    return AudioHandle{static_cast<u64>(id > 0 ? id : 0)};
}

i64 id_of(AudioHandle handle) {
    return static_cast<i64>(handle.id);
}

AudioPlayRequest request_from(const std::string& cue, const sol::optional<sol::table>& options) {
    AudioPlayRequest request{.cue = cue};
    if (options) {
        const sol::table& opts = *options;
        request.volume = opts.get_or("volume", 1.0f);
        request.pitch = opts.get_or("pitch", 1.0f);
        request.priority_boost = opts.get_or("priority", 0);
        request.fade_in = opts.get_or("fade_in", 0.0f);
        const sol::optional<f32> x = opts["x"];
        const sol::optional<f32> y = opts["y"];
        if (x || y) {
            request.position = {x.value_or(0.0f), y.value_or(0.0f)};
            request.has_position = true;
        }
    }
    return request;
}

AudioSync sync_from(std::string_view name) {
    if (name == "now") return AudioSync::Now;
    if (name == "beat") return AudioSync::Beat;
    if (name == "bar") return AudioSync::Bar;
    if (name == "end") return AudioSync::End;
    throw std::runtime_error("unknown sync '" + std::string{name} + "' (now, beat, bar or end)");
}

// Fields a Lua table sets on `effect`, named as in a .kinaudio effect line.
AudioEffect effect_from(const sol::table& table, AudioEffect effect) {
    if (const sol::optional<std::string> type = table["type"]) {
        if (!parse_audio_effect_type(*type, effect.type)) {
            throw std::runtime_error("unknown effect type '" + *type + "'");
        }
    }
    effect.enabled = table.get_or("enabled", effect.enabled);
    const std::pair<const char*, f32*> fields[] = {
        {"cutoff", &effect.cutoff}, {"freq", &effect.cutoff}, {"q", &effect.q}, {"gain", &effect.gain},
        {"time", &effect.time}, {"feedback", &effect.feedback}, {"room", &effect.room_size}, {"damping", &effect.damping},
        {"wet", &effect.wet}, {"dry", &effect.dry}, {"width", &effect.width}, {"threshold", &effect.threshold},
        {"ratio", &effect.ratio}, {"attack", &effect.attack}, {"release", &effect.release}, {"makeup", &effect.makeup},
    };
    for (const auto& [name, field] : fields) {
        *field = table.get_or(name, *field);
    }
    return effect;
}

} // namespace

void bind_lua_audio(sol::state_view lua, AudioEngine& audio, const AudioCatalog& catalog, std::string_view name) {
    AudioEngine* engine = &audio;
    const AudioCatalog* cues = &catalog;
    sol::table table = lua.create_named_table(std::string{name});

    table["play"] = [engine, cues](const std::string& cue, sol::optional<sol::table> options) {
        return id_of(engine->play(*cues, request_from(cue, options)));
    };
    // play_synced(cue, "beat" | "bar" | "end" | "now"[, opts as play])
    table["play_synced"] = [engine, cues](const std::string& cue, const std::string& sync, sol::optional<sol::table> options) {
        return id_of(engine->play_synced(*cues, request_from(cue, options), sync_from(sync)));
    };
    table["stop"] = [engine](i64 handle, sol::optional<f32> fade) { engine->stop(handle_of(handle), fade.value_or(0.0f)); };
    table["playing"] = [engine](i64 handle) { return engine->playing(handle_of(handle)); };
    table["set_volume"] = [engine](i64 handle, f32 volume, sol::optional<f32> fade) {
        engine->set_volume(handle_of(handle), volume, fade.value_or(0.0f));
    };
    table["set_pitch"] = [engine](i64 handle, f32 pitch) { engine->set_pitch(handle_of(handle), pitch); };
    table["set_position"] = [engine](i64 handle, f32 x, f32 y) { engine->set_position(handle_of(handle), {x, y}); };
    // play_music(cue[, crossfade]) or play_music(cue, {crossfade, sync, match_position})
    table["play_music"] = [engine, cues](const std::string& cue, sol::object how) {
        AudioMusicTransition transition;
        if (how.is<f32>()) {
            transition.crossfade = how.as<f32>();
        } else if (how.is<sol::table>()) {
            const sol::table opts = how.as<sol::table>();
            transition.crossfade = opts.get_or("crossfade", 1.0f);
            transition.sync = sync_from(opts.get_or<std::string>("sync", "now"));
            transition.match_position = opts.get_or("match_position", false);
        }
        return id_of(engine->play_music(*cues, cue, transition));
    };
    table["set_music_layer"] = [engine](const std::string& layer, f32 volume, sol::optional<f32> fade) {
        engine->set_music_layer(layer, volume, fade.value_or(0.0f));
    };
    table["music_position"] = [engine](sol::this_state state) {
        const AudioMusicPosition position = engine->music_position();
        sol::table result = sol::state_view{state}.create_table();
        result["playing"] = position.playing;
        result["seconds"] = position.seconds;
        result["beat"] = position.beat;
        result["bar"] = position.bar;
        result["beat_in_bar"] = position.beat_in_bar;
        return result;
    };
    table["stop_music"] = [engine](sol::optional<f32> fade) { engine->stop_music(fade.value_or(1.0f)); };
    table["set_bus_volume"] = [engine](const std::string& bus, f32 volume, sol::optional<f32> fade) {
        engine->set_bus_volume(bus, volume, fade.value_or(0.0f));
    };
    table["bus_volume"] = [engine](const std::string& bus) { return engine->bus_volume(bus); };
    table["set_bus_muted"] = [engine](const std::string& bus, bool muted) { engine->set_bus_muted(bus, muted); };
    table["set_bus_paused"] = [engine](const std::string& bus, bool paused) { engine->set_bus_paused(bus, paused); };
    table["stop_bus"] = [engine](const std::string& bus, sol::optional<f32> fade) {
        engine->stop_bus(bus, fade.value_or(0.0f));
    };
    table["set_bus_effects"] = [engine](const std::string& bus, sol::table effects) {
        std::vector<AudioEffect> chain;
        for (std::size_t i = 1; i <= effects.size(); ++i) {
            chain.push_back(effect_from(effects[i], {}));
        }
        engine->set_bus_effects(bus, std::move(chain));
    };
    // 1-based; fields not given keep their current values.
    table["set_bus_effect"] = [engine](const std::string& bus, std::size_t index, sol::table fields) {
        const std::vector<AudioEffect> chain = engine->bus_effects(bus);
        if (index >= 1 && index <= chain.size()) {
            engine->set_bus_effect(bus, index - 1, effect_from(fields, chain[index - 1]));
        }
    };
    table["seek"] = [engine](i64 handle, f32 seconds) { return engine->seek(handle_of(handle), seconds); };
    table["position"] = [engine](i64 handle) { return engine->playback_position(handle_of(handle)); };
    table["set_paused"] = [engine](i64 handle, bool paused) { engine->set_paused(handle_of(handle), paused); };
    table["paused"] = [engine](i64 handle) { return engine->paused(handle_of(handle)); };
    table["output_devices"] = [] {
        return sol::as_table(list_audio_output_devices());
    };
    table["set_output_device"] = [engine](const std::string& name) { return engine->set_output_device(name); };
    table["output_device"] = [engine]() { return engine->output_device(); };
    // Two results: peak and rms, linear (1 is full scale).
    table["bus_level"] = [engine](const std::string& bus) {
        const AudioLevel level = engine->bus_level(bus);
        return std::make_tuple(level.peak, level.rms);
    };
    table["output_level"] = [engine]() {
        const AudioLevel level = engine->output_level();
        return std::make_tuple(level.peak, level.rms);
    };
    table["enable_analysis"] = [engine](const std::string& bus, sol::optional<bool> enabled) {
        engine->enable_analysis(bus, enabled.value_or(true));
    };
    table["spectrum"] = [engine](const std::string& bus, i32 bands, sol::optional<f32> min_hz, sol::optional<f32> max_hz) {
        return sol::as_table(engine->spectrum(bus, bands, min_hz.value_or(20.0f), max_hz.value_or(20000.0f)));
    };
    table["set_listener"] = [engine](f32 x, f32 y) { engine->set_listener({x, y}); };
}

} // namespace kin
