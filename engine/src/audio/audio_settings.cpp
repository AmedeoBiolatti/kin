#include <kin/audio/audio_settings.hpp>

#include <kin/audio/audio_engine.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>

namespace kin {

void write_audio_settings(JsonWriter& json, const AudioEngine& audio) {
    json.begin_object();
    for (const AudioBusState& bus : audio.bus_states()) {
        if (bus.volume == 1.0f && !bus.muted) {
            continue;
        }
        json.key(bus.id).begin_object();
        json.field("volume", static_cast<f64>(bus.volume));
        json.field("muted", bus.muted);
        json.end_object();
    }
    json.end_object();
}

void apply_audio_settings(AudioEngine& audio, const JsonValue& settings) {
    if (!settings.is_object()) {
        return;
    }
    for (const auto& [bus, value] : settings.members()) {
        if (!value.is_object()) {
            continue;
        }
        audio.set_bus_volume(bus, static_cast<f32>(value.number_at("volume", 1.0)));
        audio.set_bus_muted(bus, value.bool_at("muted", false));
    }
}

} // namespace kin
