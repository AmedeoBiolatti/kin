#include "siege_audio.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace examples {
namespace {

constexpr int rate = 48000;
constexpr float tau = 6.2831853f;

// Renders `seconds` of a mono signal to interleaved stereo at `rate`.
std::vector<f32> render(float seconds, const std::function<float(float)>& signal) {
    const int frames = static_cast<int>(seconds * rate);
    std::vector<f32> out(static_cast<std::size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const float v = std::clamp(signal(static_cast<float>(i) / rate), -1.0f, 1.0f);
        out[static_cast<std::size_t>(i) * 2] = v;
        out[static_cast<std::size_t>(i) * 2 + 1] = v;
    }
    return out;
}

// Deterministic white noise, and a one-pole low-pass to shape it.
class Noise {
public:
    float next() {
        _state = _state * 1664525u + 1013904223u;
        return static_cast<float>(_state >> 8) / 8388608.0f - 1.0f;
    }
    float filtered(float cutoff_hz) {
        const float a = 1.0f - std::exp(-tau * cutoff_hz / rate);
        _low += (next() - _low) * a;
        return _low;
    }

private:
    u32 _state = 0x51e9e;
    float _low = 0;
};

// A frequency sweep from `from` to `to` Hz over `seconds`, as a phase accumulator.
class Sweep {
public:
    Sweep(float from, float to, float seconds) : _from(from), _to(to), _seconds(seconds) {}
    float phase(float t) {
        const float k = std::clamp(t / _seconds, 0.0f, 1.0f);
        _phase += tau * (_from * std::pow(_to / _from, k)) / rate;
        return _phase;
    }

private:
    float _from, _to, _seconds, _phase = 0;
};

float attack(float t, float seconds = .003f) { return std::min(1.0f, t / seconds); }

std::vector<f32> shot() {
    Sweep sweep(1500, 520, .07f);
    return render(.075f, [&](float t) {
        const float p = sweep.phase(t);
        return (.55f * std::sin(p) + .25f * (std::sin(p) > 0 ? 1.0f : -1.0f)) * std::exp(-t * 48) * attack(t);
    });
}

std::vector<f32> enemy_shot() {
    Sweep sweep(620, 240, .12f);
    return render(.14f, [&](float t) { return .6f * std::sin(sweep.phase(t)) * std::exp(-t * 22) * attack(t); });
}

std::vector<f32> hit() {
    Noise noise;
    return render(.07f, [&](float t) {
        return (.9f * noise.filtered(3200) + .4f * std::sin(tau * 190 * t)) * std::exp(-t * 55) * attack(t, .001f);
    });
}

std::vector<f32> kill() {
    Noise noise;
    Sweep thump(180, 45, .3f);
    return render(.42f, [&](float t) {
        const float crunch = noise.filtered(2400 * std::exp(-t * 6) + 200) * 1.4f;
        return (crunch + .7f * std::sin(thump.phase(t))) * std::exp(-t * 8) * attack(t, .002f);
    });
}

std::vector<f32> pickup() {
    return render(.18f, [](float t) {
        const bool second = t > .07f;
        const float local = second ? t - .07f : t;
        return .45f * std::sin(tau * (second ? 1318.5f : 987.8f) * t) * std::exp(-local * 22) * attack(local);
    });
}

std::vector<f32> dash() {
    Noise noise;
    return render(.24f, [&](float t) {
        const float shape = std::sin(3.14159f * t / .24f);
        return 1.3f * noise.filtered(600 + 3000 * t / .24f) * shape * shape;
    });
}

std::vector<f32> hurt() {
    return render(.2f, [](float t) {
        const float saw = std::fmod(110 * t, 1.0f) * 2 - 1 + std::fmod(117 * t, 1.0f) * 2 - 1;
        return .35f * saw * std::exp(-t * 13) * attack(t);
    });
}

// Sum of decaying bell partials for each note, starting `stagger` seconds apart.
std::vector<f32> chord(std::initializer_list<float> notes, float stagger, float seconds, float decay) {
    const std::vector<float> freqs(notes);
    return render(seconds, [&](float t) {
        float v = 0;
        for (std::size_t i = 0; i < freqs.size(); ++i) {
            const float local = t - stagger * static_cast<float>(i);
            if (local > 0) {
                v += (std::sin(tau * freqs[i] * local) + .3f * std::sin(tau * freqs[i] * 2 * local)) * std::exp(-local * decay) * attack(local);
            }
        }
        return .22f * v;
    });
}

std::vector<f32> tick(float freq, float seconds) {
    return render(seconds, [=](float t) { return .4f * std::sin(tau * freq * t) * std::exp(-t * 90) * attack(t, .001f); });
}

// Four seconds that loop seamlessly: every partial and the swell complete whole
// cycles in the loop.
std::vector<f32> drone() {
    return render(4.0f, [](float t) {
        const float swell = .75f + .25f * std::sin(tau * .25f * t);
        return swell * (.22f * std::sin(tau * 55 * t) + .12f * std::sin(tau * 82.5f * t) + .07f * std::sin(tau * 110 * t + 1));
    });
}

std::unique_ptr<IAudioBackend> backend(bool muted) {
    return muted ? create_null_audio_backend(rate, 2) : create_sdl_audio_backend(rate, 2);
}

AudioEngine make_engine(bool muted) {
    try {
        return AudioEngine{backend(muted), {}};
    } catch (const std::exception&) {
        return AudioEngine{create_null_audio_backend(rate, 2), {}}; // no audio device
    }
}

} // namespace

SiegeAudio::SiegeAudio(bool muted) : _engine(make_engine(muted)) {
    const std::array<std::pair<const char*, std::vector<f32>>, 13> clips{{
        {"shot", shot()},
        {"enemy_shot", enemy_shot()},
        {"hit", hit()},
        {"kill", kill()},
        {"pickup", pickup()},
        {"dash", dash()},
        {"hurt", hurt()},
        {"wave", chord({880, 1108.7f, 1318.5f}, .09f, 1.0f, 4)},
        {"win", chord({523.3f, 659.3f, 784, 1046.5f}, .12f, 1.6f, 3)},
        {"lose", chord({392, 311.1f, 261.6f, 196}, .16f, 1.6f, 3)},
        {"ui_move", tick(1800, .04f)},
        {"ui_select", chord({660, 990}, .04f, .16f, 26)},
        {"drone", drone()},
    }};
    for (const auto& [id, samples] : clips) {
        _engine.add_clip(id, make_memory_audio_clip(id, samples, 2, rate));
    }

    _catalog.add_bus({.id = "master", .volume = .8f});
    _catalog.add_bus({.id = "sfx", .parent = "master"});
    _catalog.add_bus({.id = "ui", .parent = "master"});
    _catalog.add_bus({.id = "ambient", .parent = "master", .volume = .5f});
    // Positioned cues fade out over a screen's width from the listener.
    const auto effect = [&](const char* id, float volume, int max, float pitch_variance, bool spatial, int priority) {
        _catalog.add_cue({.id = id, .clips = {id}, .category = AudioCategory::Effect, .bus = "sfx", .volume = volume,
                          .pitch_variance = pitch_variance, .priority = priority, .max_instances = max,
                          .spatial = spatial, .min_distance = 160, .max_distance = 1100});
    };
    effect("shot", .28f, 3, .05f, false, 5);
    effect("enemy_shot", .35f, 4, .08f, true, 4);
    effect("hit", .45f, 6, .18f, true, 6);
    effect("kill", .7f, 6, .12f, true, 8);
    effect("pickup", .5f, 4, .04f, false, 7);
    effect("dash", .6f, 1, 0, false, 9);
    effect("hurt", .8f, 1, 0, false, 10);
    for (const char* id : {"wave", "win", "lose", "ui_move", "ui_select"}) {
        _catalog.add_cue({.id = id, .clips = {id}, .category = AudioCategory::Ui, .bus = "ui", .volume = .7f, .priority = 20});
    }
    _catalog.add_cue({.id = "drone", .clips = {"drone"}, .category = AudioCategory::Ambient, .bus = "ambient",
                      .max_instances = 1, .loop = true});
}

void SiegeAudio::play_events(std::span<const ArenaEvent> events, Vec2f listener) {
    _engine.set_listener(listener);
    for (const ArenaEvent& e : events) {
        const char* cue = nullptr;
        switch (e.kind) {
        case ArenaEvent::Kind::Fire: cue = "shot"; break;
        case ArenaEvent::Kind::EnemyFire: cue = "enemy_shot"; break;
        case ArenaEvent::Kind::Hit: cue = "hit"; break;
        case ArenaEvent::Kind::Kill: cue = "kill"; break;
        case ArenaEvent::Kind::Pickup: cue = "pickup"; break;
        case ArenaEvent::Kind::Dash: cue = "dash"; break;
        case ArenaEvent::Kind::Hurt: cue = "hurt"; break;
        }
        _engine.play(_catalog, {.cue = cue, .position = e.pos, .has_position = true});
    }
}

void SiegeAudio::play(std::string_view cue) {
    _engine.play(_catalog, {.cue = std::string{cue}});
}

void SiegeAudio::set_drone(bool playing) {
    if (playing && !_engine.playing(_drone)) {
        _drone = _engine.play(_catalog, {.cue = "drone"});
    } else if (!playing && _drone) {
        _engine.stop(_drone, .6f);
        _drone = {};
    }
}

void SiegeAudio::update(float dt) {
    _engine.update(dt);
}

} // namespace examples
