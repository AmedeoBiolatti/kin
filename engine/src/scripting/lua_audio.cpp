#include <kin/scripting/lua_audio.hpp>

#include <kin/audio/audio_engine.hpp>

#include <stdexcept>
#include <string>
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

// Fields a Lua table sets on `effect`, named as in a .kinaudio effect line.
AudioEffect effect_from(const sol::table& table, AudioEffect effect) {
    if (const sol::optional<std::string> type = table["type"]) {
        if (!parse_audio_effect_type(*type, effect.type)) {
            throw std::runtime_error("unknown effect type '" + *type + "'");
        }
    }
    effect.enabled = table.get_or("enabled", effect.enabled);
    const std::pair<const char*, f32*> fields[] = {
        {"cutoff", &effect.cutoff}, {"q", &effect.q}, {"room", &effect.room_size}, {"damping", &effect.damping},
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
        return id_of(engine->play(*cues, request));
    };
    table["stop"] = [engine](i64 handle, sol::optional<f32> fade) { engine->stop(handle_of(handle), fade.value_or(0.0f)); };
    table["playing"] = [engine](i64 handle) { return engine->playing(handle_of(handle)); };
    table["set_volume"] = [engine](i64 handle, f32 volume, sol::optional<f32> fade) {
        engine->set_volume(handle_of(handle), volume, fade.value_or(0.0f));
    };
    table["set_pitch"] = [engine](i64 handle, f32 pitch) { engine->set_pitch(handle_of(handle), pitch); };
    table["set_position"] = [engine](i64 handle, f32 x, f32 y) { engine->set_position(handle_of(handle), {x, y}); };
    table["play_music"] = [engine, cues](const std::string& cue, sol::optional<f32> crossfade) {
        return id_of(engine->play_music(*cues, cue, crossfade.value_or(1.0f)));
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
    table["set_listener"] = [engine](f32 x, f32 y) { engine->set_listener({x, y}); };
}

} // namespace kin
