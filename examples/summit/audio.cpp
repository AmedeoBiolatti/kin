#include "audio.hpp"

#include "environment.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace summit {

namespace {

f32 smoothstep(f32 a, f32 b, f32 x) {
    const f32 t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

const char* cue_name(CueKind k) {
    static const char* const names[cue_kind_count] = {"footstep", "jump", "land",   "grab",      "release",
                                                      "impact",   "fall", "breath", "exhausted", "rope_creak",
                                                      "hurt",     "died", "got_up", "camp",      "summit"};
    return names[std::size_t(k)];
}

f32 wind_gain(f32 altitude, f32 height, f32 wind_speed) {
    const f32 t = std::clamp(altitude / std::max(1.0f, height), 0.0f, 1.0f);
    const f32 s = std::clamp(wind_speed / 11.0f, 0.0f, 1.0f);
    return (0.10f + 0.50f * t) * (0.35f + 0.65f * s);
}

f32 wind_pitch(f32 wind_speed) {
    return 0.85f + 0.25f * std::clamp(wind_speed / 11.0f, 0.0f, 1.0f);
}

f32 fall_whoosh_gain(f32 speed_down) {
    return 0.9f * smoothstep(3.0f, 20.0f, speed_down);
}

f32 fall_whoosh_pitch(f32 speed_down) {
    return 0.8f + 0.6f * smoothstep(3.0f, 25.0f, speed_down);
}

f32 footstep_cadence(f32 ground_speed) {
    if (ground_speed < 0.5f)
        return 0.0f;
    return 1.6f + 0.25f * ground_speed;
}

f32 breath_interval(f32 usable_fraction, bool exhausted) {
    if (exhausted)
        return 0.9f;
    constexpr f32 tired = 0.4f;
    if (usable_fraction >= tired)
        return 0.0f;
    const f32 k = 1.0f - std::max(0.0f, usable_fraction) / tired;
    return 3.2f + (1.0f - 3.2f) * k;
}

f32 impact_gain(f32 damage) {
    return 0.35f + 0.65f * std::clamp(damage / 60.0f, 0.0f, 1.0f);
}

SurfaceSound surface_under(const Game& game, i32 player) {
    const CharacterState3D cs = game.character_state(player);
    const std::vector<Piece>& pieces = game.level().pieces;
    const u64 id = cs.ground_user_id;
    if (id >= 1 && id <= pieces.size()) {
        const PieceKind k = pieces[std::size_t(id - 1)].kind;
        const f32 alt = cs.position.y - game.level().start.y;
        if (k == PieceKind::Ground || k == PieceKind::Ramp) {
            return snow_cover(alt, game.level().height(), 1.0f) > 0.5f ? SurfaceSound::Snow : SurfaceSound::Grass;
        }
        return SurfaceSound::Rock;
    }
    return SurfaceSound::Rock;
}

Vec3f camp_fire_position(const Camp& camp) {
    // Beside the tent, on the side away from the spawn (the view puts the fire there).
    return camp.marker + Vec3f{1.6f, 0.15f, 0.0f};
}

// ---- cues
// ---------------------------------------------------------------------------------------------------------------------

void CueGenerator::observe(const Game& game, i32 player, std::vector<SoundCue>& out) {
    const Player& p = game.player(player);
    const CharacterState3D cs = game.character_state(player);
    const TraversalState3D ts = game.traversal(player);
    const u64 step = game.step_count();
    const auto add = [&](CueKind kind, Vec3f at, f32 gain = 1.0f, f32 pitch = 1.0f, i32 variant = 0) {
        out.push_back({step, kind, at, gain, pitch, variant});
    };

    bool hurt_now = false;
    for (const GameEvent& e : game.events()) {
        if (e.player == player && e.kind == GameEventKind::Hurt)
            hurt_now = true;
    }
    for (const GameEvent& e : game.events()) {
        if (e.player != player)
            continue;
        switch (e.kind) {
        case GameEventKind::Jumped:
            add(CueKind::Jump, e.point, 0.5f);
            break;
        case GameEventKind::Landed:
            // Damage has its own thump: no second, softer landing under it.
            if (e.value >= 2.0f && !hurt_now)
                add(CueKind::Land, e.point, std::clamp(e.value / 9.0f, 0.2f, 1.0f), 1.1f - 0.02f * e.value);
            break;
        case GameEventKind::Gripped:
            add(CueKind::Grab, e.point, 0.8f, e.value > 0.5f ? 1.06f : 1.0f, e.value > 0.5f ? 1 : 0);
            break;
        case GameEventKind::Released: {
            // Reason numbers are GripRelease3D's: the pull-up's own end and a vanished body are not heard.
            const GripRelease3D why = GripRelease3D(i32(e.value));
            if (why != GripRelease3D::Mantle && why != GripRelease3D::Lost) {
                add(CueKind::Release, e.point,
                    why == GripRelease3D::Exhausted || why == GripRelease3D::Slipped ? 0.9f : 0.5f);
            }
            break;
        }
        case GameEventKind::Fell:
            add(CueKind::FallStart, e.point, 0.8f);
            break;
        case GameEventKind::Exhausted:
            add(CueKind::Exhausted, e.point, 0.9f);
            break;
        case GameEventKind::Hurt:
            add(CueKind::Impact, e.point, impact_gain(e.value), 1.0f - 0.004f * e.value);
            add(CueKind::Hurt, e.point, std::clamp(0.4f + e.value / 60.0f, 0.4f, 1.0f));
            break;
        case GameEventKind::Died:
            add(CueKind::Died, e.point, 1.0f);
            break;
        case GameEventKind::GotUp:
            add(CueKind::GotUp, e.point, 0.6f);
            break;
        case GameEventKind::Camp:
            add(CueKind::Camp, e.point, 0.8f);
            break;
        case GameEventKind::ExpeditionStarted:
        case GameEventKind::ShelterReached:
        case GameEventKind::BeaconActivated:
            add(CueKind::Camp, e.point, 0.8f);
            break;
        case GameEventKind::Summit:
            add(CueKind::Summit, e.point, 1.0f);
            break;
        default:
            break;
        }
    }

    const Vec3f feet = cs.position;
    const bool alive = p.state == PlayerState::Alive;

    // Footsteps: a cadence from the ground speed, a foot down each time the phase passes a whole number.
    const bool walking = alive && ts.mode == TraversalMode3D::Walking && cs.ground == CharacterGround3D::OnGround;
    const f32 cadence = walking ? footstep_cadence(cs.ground_speed) : 0.0f;
    if (cadence > 0.0f) {
        _foot_phase += cadence * fixed_step;
        if (_foot_phase >= 1.0f) {
            _foot_phase -= 1.0f;
            const SurfaceSound s = surface_under(game, player);
            // A sprint is heavier; the foot alternates its pitch a little.
            const f32 heavy = std::clamp(cs.ground_speed / 7.0f, 0.5f, 1.0f);
            add(CueKind::Footstep, feet, 0.45f + 0.35f * heavy, (step & 1 ? 1.04f : 0.96f), i32(s));
        }
    } else {
        // Standing: the next step comes at once when it starts (the first foot lands as it moves off).
        _foot_phase = std::max(_foot_phase, 0.7f);
    }

    // Effort breathing, from the share of the usable bar left.
    const f32 usable = ts.resource.max > 0.0f ? ts.resource.value / ts.resource.max : 1.0f;
    const f32 breath = alive ? breath_interval(usable, ts.resource.exhausted) : 0.0f;
    if (breath > 0.0f) {
        // The first breath comes soon after the bar drops under the line (the timer starts most of the way there).
        if (_breath_timer < 0.0f)
            _breath_timer = breath * 0.7f;
        _breath_timer += fixed_step;
        if (_breath_timer >= breath) {
            _breath_timer = 0.0f;
            add(CueKind::Breath, game.head(player), 0.35f + 0.5f * (1.0f - std::min(1.0f, usable / 0.4f)),
                0.95f + 0.1f * usable);
        }
    } else {
        _breath_timer = -1.0f;
    }

    // A rope creaks under a climber: faster while it moves.
    bool on_rope = false;
    for (std::size_t i = 0; i < ts.grips.size(); ++i) {
        if (ts.grips[i].kind == GripKind3D::Rope && ts.grips[i].phase == GripPhase3D::Held)
            on_rope = true;
    }
    if (alive && on_rope && ts.mode == TraversalMode3D::Hanging) {
        const f32 moving =
            std::sqrt(cs.velocity.x * cs.velocity.x + cs.velocity.y * cs.velocity.y + cs.velocity.z * cs.velocity.z) /
            fixed_step;
        _rope_timer += fixed_step;
        const f32 interval = moving > 0.15f ? 0.6f : 1.8f;
        if (_rope_timer >= interval) {
            _rope_timer = 0.0f;
            add(CueKind::RopeCreak, ts.anchor_centre, 0.5f, 0.9f + 0.2f * std::min(1.0f, moving));
        }
    } else {
        _rope_timer = 0.0f;
    }
    _was_on_ground = cs.ground == CharacterGround3D::OnGround;
}

// ---- synthesis
// ----------------------------------------------------------------------------------------------------------------

namespace {

constexpr int rate = 24000; // mono clips at half the engine's rate: everything here is soft and low
constexpr f32 tau = 6.2831853f;

class Noise {
public:
    explicit Noise(u32 seed = 0x51e9e) : _state(seed) {}
    f32 next() {
        _state = _state * 1664525u + 1013904223u;
        return f32(_state >> 8) / 8388608.0f - 1.0f;
    }
    f32 uni() { return next() * 0.5f + 0.5f; }

private:
    u32 _state;
};

// One-pole low-pass and a high-pass made from it.
struct Lowpass {
    f32 a = 0.1f, y = 0.0f;
    explicit Lowpass(f32 hz = 1000.0f) : a(1.0f - std::exp(-tau * hz / f32(rate))) {}
    f32 operator()(f32 x) {
        y += (x - y) * a;
        return y;
    }
    void cutoff(f32 hz) { a = 1.0f - std::exp(-tau * hz / f32(rate)); }
};

// A resonant band-pass (state-variable filter).
struct Bandpass {
    f32 low = 0.0f, band = 0.0f, f = 0.1f, q = 0.2f;
    Bandpass(f32 hz, f32 resonance) : f(2.0f * std::sin(3.14159265f * hz / f32(rate))), q(1.0f / resonance) {}
    void set(f32 hz) { f = 2.0f * std::sin(3.14159265f * std::min(hz, 0.45f * f32(rate)) / f32(rate)); }
    f32 operator()(f32 x) {
        low += f * band;
        const f32 high = x - low - q * band;
        band += f * high;
        return band;
    }
};

std::vector<f32> make(f32 seconds, const std::function<f32(f32)>& signal) {
    const int n = int(seconds * f32(rate));
    std::vector<f32> out(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        out[std::size_t(i)] = std::clamp(signal(f32(i) / f32(rate)), -1.0f, 1.0f);
    return out;
}

f32 attack(f32 t, f32 s = 0.004f) {
    return std::min(1.0f, t / s);
}

// Makes the first `fade` seconds blend into the tail: the clip's end continues into its start, so that it loops without
// a click.
std::vector<f32> loop_crossfade(std::vector<f32> in, f32 fade) {
    const std::size_t n = in.size(), f = std::size_t(fade * f32(rate));
    if (f * 2 >= n)
        return in;
    std::vector<f32> out(in.begin(), in.begin() + std::ptrdiff_t(n - f));
    for (std::size_t i = 0; i < f; ++i) {
        const f32 k = f32(i) / f32(f);
        // Equal power between the tail (fading out) and the head (fading in).
        out[i] = in[n - f + i] * std::cos(k * 1.5707963f) + in[i] * std::sin(k * 1.5707963f);
    }
    return out;
}

std::vector<f32> wind_clip() {
    Noise a(11), b(23);
    Lowpass low(260.0f), low2(120.0f);
    Bandpass whistle(520.0f, 5.0f);
    constexpr f32 seconds = 9.0f;
    auto raw = make(seconds + 1.0f, [&](f32 t) {
        // Slow swells in pitch and strength: the clip is a long gust-less bed; the game's gusts are the volume.
        const f32 sw = 0.5f + 0.5f * std::sin(tau * 0.17f * t + 1.0f);
        whistle.set(430.0f + 260.0f * (0.5f + 0.5f * std::sin(tau * 0.11f * t)) + 90.0f * sw);
        const f32 bed = low(a.next()) * 1.6f + low2(b.next()) * 2.6f;
        const f32 w = whistle(a.next() * 0.5f) * 0.10f * (0.4f + sw);
        return (bed * (0.7f + 0.3f * sw) + w) * 0.9f;
    });
    return loop_crossfade(std::move(raw), 1.0f);
}

std::vector<f32> step_clip(SurfaceSound s, u32 seed) {
    Noise n(seed);
    switch (s) {
    case SurfaceSound::Grass: {
        Lowpass lp(1100.0f), thud(180.0f);
        return make(0.16f, [&](f32 t) {
            const f32 env = std::exp(-t * 26.0f) * attack(t, 0.006f);
            return (lp(n.next()) * 1.4f + thud(n.next()) * 1.2f) * env * 0.9f;
        });
    }
    case SurfaceSound::Rock: {
        Lowpass lp(2600.0f);
        return make(0.14f, [&](f32 t) {
            const f32 click = lp(n.next()) * std::exp(-t * 90.0f);
            const f32 body = std::sin(tau * (150.0f - 450.0f * t) * t) * std::exp(-t * 34.0f);
            return (click * 0.9f + body * 0.7f) * attack(t, 0.001f);
        });
    }
    case SurfaceSound::Snow: {
        Lowpass hp(1800.0f), lp(5000.0f);
        return make(0.22f, [&](f32 t) {
            // Crunch: a cluster of small clicks over a soft squeak.
            const f32 grain = (n.uni() > 0.93f ? 1.0f : 0.0f) * n.next();
            const f32 hi = lp(n.next()) - hp(n.next());
            const f32 env = std::exp(-t * 14.0f) * attack(t, 0.005f);
            return (grain * 1.5f + hi * 0.5f) * env * 0.8f;
        });
    }
    }
    return {};
}

std::vector<f32> grab_clip() {
    Noise n(5);
    Lowpass lp(1400.0f);
    return make(0.14f, [&](f32 t) {
        const f32 body = std::sin(tau * (170.0f - 600.0f * t) * t) * std::exp(-t * 28.0f);
        const f32 slap = lp(n.next()) * std::exp(-t * 70.0f);
        return (body * 0.8f + slap * 0.6f) * attack(t, 0.001f);
    });
}

std::vector<f32> release_clip() {
    Noise n(9);
    Lowpass lp(2400.0f);
    return make(0.2f, [&](f32 t) {
        const f32 env = std::exp(-t * 15.0f) * attack(t, 0.01f);
        lp.cutoff(2600.0f - 9000.0f * t);
        return lp(n.next()) * env * 0.9f;
    });
}

std::vector<f32> land_clip() {
    Noise n(13);
    Lowpass lp(700.0f);
    return make(0.26f, [&](f32 t) {
        const f32 thump = std::sin(tau * (95.0f - 150.0f * t) * t) * std::exp(-t * 17.0f);
        const f32 dust = lp(n.next()) * std::exp(-t * 20.0f);
        return (thump * 0.9f + dust * 0.6f) * attack(t, 0.002f);
    });
}

std::vector<f32> impact_clip() {
    Noise n(17);
    Lowpass lp(900.0f), rumble(110.0f);
    return make(0.7f, [&](f32 t) {
        const f32 thump = std::sin(tau * (70.0f - 80.0f * t) * t) * std::exp(-t * 7.5f);
        const f32 crunch = lp(n.next()) * std::exp(-t * 13.0f);
        const f32 body = rumble(n.next()) * 3.0f * std::exp(-t * 5.0f);
        return (thump * 1.0f + crunch * 0.8f + body * 0.6f) * attack(t, 0.002f);
    });
}

std::vector<f32> whoosh_clip() {
    Noise n(29);
    Bandpass bp(700.0f, 1.8f);
    Lowpass lp(2200.0f);
    auto raw = make(3.0f, [&](f32 t) {
        bp.set(600.0f + 300.0f * std::sin(tau * 0.5f * t));
        return (bp(n.next()) * 0.7f + lp(n.next()) * 0.5f) * 0.8f;
    });
    return loop_crossfade(std::move(raw), 0.5f);
}

std::vector<f32> breath_clip() {
    Noise n(31);
    Bandpass bp(900.0f, 2.0f);
    Lowpass lp(2200.0f);
    return make(1.0f, [&](f32 t) {
        // In (0 to 0.4 s, rising) and out (0.45 to 0.95, falling), each a breath of noise through a formant.
        const f32 in = std::sin(3.14159f * std::clamp(t / 0.4f, 0.0f, 1.0f));
        const f32 out = std::sin(3.14159f * std::clamp((t - 0.45f) / 0.5f, 0.0f, 1.0f));
        const f32 env = in * 0.8f + out;
        bp.set(800.0f + 400.0f * in);
        return (bp(n.next()) * 1.2f + lp(n.next()) * 0.4f) * env * 0.7f;
    });
}

std::vector<f32> gasp_clip() {
    Noise n(37);
    Bandpass bp(1100.0f, 2.2f);
    return make(0.6f, [&](f32 t) {
        const f32 env = std::sin(3.14159f * std::clamp(t / 0.5f, 0.0f, 1.0f));
        bp.set(1000.0f + 700.0f * t);
        return bp(n.next()) * 2.0f * env * env;
    });
}

std::vector<f32> creak_clip() {
    Noise n(41);
    Bandpass bp(300.0f, 8.0f);
    f32 phase = 0.0f;
    return make(0.8f, [&](f32 t) {
        const f32 f = 90.0f + 110.0f * std::sin(3.14159f * t / 0.8f) + 25.0f * std::sin(tau * 11.0f * t);
        phase += tau * f / f32(rate);
        const f32 saw = std::fmod(phase / tau, 1.0f) * 2.0f - 1.0f;
        bp.set(f * 3.0f);
        const f32 env = std::sin(3.14159f * t / 0.8f);
        return (bp(saw * 0.6f + n.next() * 0.15f) * 1.5f) * env * 0.8f;
    });
}

std::vector<f32> fire_clip() {
    Noise n(43), c(47);
    Lowpass rumble(170.0f), hiss(2800.0f);
    f32 pop = 0.0f;
    auto raw = make(5.0f, [&](f32) {
        if (c.uni() > 0.9985f)
            pop = 0.5f + 0.5f * c.uni();
        pop *= 0.9930f;
        const f32 click = pop * c.next();
        return (rumble(n.next()) * 1.4f + hiss(n.next()) * 0.10f + click * 0.9f) * 0.8f;
    });
    return loop_crossfade(std::move(raw), 0.6f);
}

// Decaying bell partials of each note, a stagger apart.
std::vector<f32> chord(std::initializer_list<f32> notes, f32 stagger, f32 seconds, f32 decay) {
    const std::vector<f32> freqs(notes);
    return make(seconds, [&](f32 t) {
        f32 v = 0.0f;
        for (std::size_t i = 0; i < freqs.size(); ++i) {
            const f32 local = t - stagger * f32(i);
            if (local <= 0.0f)
                continue;
            v += (std::sin(tau * freqs[i] * local) + 0.35f * std::sin(tau * freqs[i] * 2.0f * local) +
                  0.12f * std::sin(tau * freqs[i] * 3.01f * local)) *
                 std::exp(-local * decay) * attack(local, 0.006f);
        }
        return 0.26f * v;
    });
}

std::vector<f32> grunt_clip(f32 low, f32 high, f32 seconds) {
    Noise n(53);
    Bandpass bp(500.0f, 3.0f);
    f32 phase = 0.0f;
    return make(seconds, [&](f32 t) {
        const f32 f = low + (high - low) * std::exp(-t * 7.0f);
        phase += tau * f / f32(rate);
        bp.set(f * 4.0f);
        const f32 saw = std::fmod(phase / tau, 1.0f) * 2.0f - 1.0f;
        return bp(saw + n.next() * 0.2f) * 1.6f * std::exp(-t * 6.0f) * attack(t, 0.01f);
    });
}

std::vector<f32> jump_clip() {
    Noise n(59);
    Bandpass bp(700.0f, 2.0f);
    return make(0.22f, [&](f32 t) {
        const f32 env = std::sin(3.14159f * std::clamp(t / 0.22f, 0.0f, 1.0f));
        return bp(n.next()) * 1.6f * env;
    });
}

std::vector<f32> tick(f32 hz, f32 seconds) {
    return make(seconds,
                [=](f32 t) { return 0.4f * std::sin(tau * hz * t) * std::exp(-t * 80.0f) * attack(t, 0.001f); });
}

std::unique_ptr<IAudioBackend> backend(bool muted) {
    return muted ? create_null_audio_backend(48000, 2) : create_sdl_audio_backend(48000, 2);
}

AudioEngine make_engine(bool muted) {
    try {
        return AudioEngine{backend(muted), {}};
    } catch (const std::exception&) {
        return AudioEngine{create_null_audio_backend(48000, 2), {}}; // no audio device
    }
}

} // namespace

SummitAudio::SummitAudio(bool muted) : _engine(make_engine(muted)) {
    const auto clip = [&](const std::string& id, std::vector<f32> samples) {
        _engine.add_clip(id, make_memory_audio_clip(id, std::move(samples), 1, rate));
    };
    clip("wind", wind_clip());
    for (u32 v = 0; v < 2; ++v) {
        const std::string tag = std::to_string(v);
        clip("step_grass" + tag, step_clip(SurfaceSound::Grass, 100 + v));
        clip("step_rock" + tag, step_clip(SurfaceSound::Rock, 200 + v));
        clip("step_snow" + tag, step_clip(SurfaceSound::Snow, 300 + v));
    }
    clip("grab", grab_clip());
    clip("release", release_clip());
    clip("land", land_clip());
    clip("impact", impact_clip());
    clip("whoosh", whoosh_clip());
    clip("breath", breath_clip());
    clip("gasp", gasp_clip());
    clip("creak", creak_clip());
    clip("fire", fire_clip());
    clip("sting", chord({392.0f, 523.3f, 659.3f, 784.0f, 1046.5f}, 0.16f, 2.6f, 2.2f));
    clip("camp", chord({523.3f, 784.0f}, 0.14f, 1.2f, 4.0f));
    clip("hurt", grunt_clip(120.0f, 200.0f, 0.4f));
    clip("died", grunt_clip(90.0f, 180.0f, 0.9f));
    clip("jump", jump_clip());
    clip("fall", grunt_clip(150.0f, 260.0f, 0.5f));
    clip("ui_move", tick(1500.0f, 0.04f));
    clip("ui_select", chord({660.0f, 990.0f}, 0.04f, 0.18f, 24.0f));

    _catalog.add_bus({.id = "master", .volume = 0.8f});
    _catalog.add_bus({.id = "sfx", .parent = "master"});
    _catalog.add_bus({.id = "ambient", .parent = "master", .volume = 0.7f});
    _catalog.add_bus({.id = "ui", .parent = "master"});
    const auto effect = [&](const char* id, std::vector<std::string> clips, f32 volume, i32 max, f32 variance,
                            bool spatial, i32 priority, f32 near_m = 3.0f, f32 far_m = 70.0f) {
        _catalog.add_cue({.id = id,
                          .clips = std::move(clips),
                          .category = AudioCategory::Effect,
                          .bus = "sfx",
                          .volume = volume,
                          .pitch_variance = variance,
                          .priority = priority,
                          .max_instances = max,
                          .spatial = spatial,
                          .min_distance = near_m,
                          .max_distance = far_m,
                          .rolloff = AudioRolloff::Inverse});
    };
    effect("step_grass", {"step_grass0", "step_grass1"}, 0.55f, 4, 0.08f, true, 3);
    effect("step_rock", {"step_rock0", "step_rock1"}, 0.6f, 4, 0.08f, true, 3);
    effect("step_snow", {"step_snow0", "step_snow1"}, 0.6f, 4, 0.08f, true, 3);
    effect("grab", {"grab"}, 0.7f, 4, 0.06f, true, 6);
    effect("release", {"release"}, 0.6f, 3, 0.05f, true, 5);
    effect("land", {"land"}, 0.8f, 3, 0.04f, true, 7);
    effect("impact", {"impact"}, 1.0f, 2, 0.03f, true, 10);
    effect("breath", {"breath"}, 0.55f, 1, 0.04f, false, 4);
    effect("gasp", {"gasp"}, 0.6f, 1, 0.03f, false, 6);
    effect("creak", {"creak"}, 0.5f, 2, 0.1f, true, 4);
    effect("hurt", {"hurt"}, 0.7f, 1, 0.04f, false, 9);
    effect("died", {"died"}, 0.8f, 1, 0.0f, false, 10);
    effect("jump", {"jump"}, 0.4f, 2, 0.06f, false, 3);
    effect("fall", {"fall"}, 0.6f, 1, 0.04f, false, 8);
    for (const char* id : {"sting", "camp"}) {
        _catalog.add_cue(
            {.id = id, .clips = {id}, .category = AudioCategory::Ui, .bus = "sfx", .volume = 0.7f, .priority = 20});
    }
    for (const char* id : {"ui_move", "ui_select"}) {
        _catalog.add_cue(
            {.id = id, .clips = {id}, .category = AudioCategory::Ui, .bus = "ui", .volume = 0.7f, .priority = 30});
    }
    _catalog.add_cue({.id = "wind",
                      .clips = {"wind"},
                      .category = AudioCategory::Ambient,
                      .bus = "ambient",
                      .volume = 0.8f,
                      .max_instances = 1,
                      .loop = true});
    _catalog.add_cue({.id = "whoosh",
                      .clips = {"whoosh"},
                      .category = AudioCategory::Ambient,
                      .bus = "ambient",
                      .volume = 0.9f,
                      .max_instances = 1,
                      .loop = true});
    _catalog.add_cue({.id = "fire",
                      .clips = {"fire"},
                      .category = AudioCategory::Ambient,
                      .bus = "ambient",
                      .volume = 0.9f,
                      .max_instances = 0,
                      .loop = true,
                      .spatial = true,
                      .min_distance = 2.0f,
                      .max_distance = 28.0f,
                      .rolloff = AudioRolloff::Inverse});
    set_volumes(_volumes);
    _wind = _engine.play(_catalog, {.cue = "wind", .volume = 0.0f});
    _whoosh = _engine.play(_catalog, {.cue = "whoosh", .volume = 0.0f});
}

SummitAudio::~SummitAudio() = default;

void SummitAudio::set_volumes(const AudioVolumes& v) {
    _volumes = v;
    _engine.set_bus_volume("master", std::clamp(v.master, 0.0f, 1.0f));
    _engine.set_bus_volume("sfx", std::clamp(v.sfx, 0.0f, 1.0f));
    _engine.set_bus_volume("ui", std::clamp(v.sfx, 0.0f, 1.0f));
    _engine.set_bus_volume("ambient", std::clamp(v.ambience, 0.0f, 1.0f));
}

void SummitAudio::play(const SoundCue& cue) {
    _played[std::size_t(cue.kind)]++;
    const auto at = [&](const char* name, bool positional = true) {
        AudioPlayRequest r;
        r.cue = name;
        r.position_3d = cue.position;
        r.has_position_3d = positional;
        r.volume = std::clamp(cue.gain, 0.0f, 1.5f);
        r.pitch = std::clamp(cue.pitch, 0.5f, 2.0f);
        _engine.play(_catalog, r);
    };
    switch (cue.kind) {
    case CueKind::Footstep:
        at(cue.variant == i32(SurfaceSound::Snow)   ? "step_snow"
           : cue.variant == i32(SurfaceSound::Rock) ? "step_rock"
                                                    : "step_grass");
        break;
    case CueKind::Jump:
        at("jump", false);
        break;
    case CueKind::Land:
        at("land");
        break;
    case CueKind::Grab:
        at("grab");
        break;
    case CueKind::Release:
        at("release");
        break;
    case CueKind::Impact:
        at("impact");
        break;
    case CueKind::FallStart:
        at("fall", false);
        break;
    case CueKind::Breath:
        at("breath", false);
        break;
    case CueKind::Exhausted:
        at("gasp", false);
        break;
    case CueKind::RopeCreak:
        at("creak");
        break;
    case CueKind::Hurt:
        at("hurt", false);
        break;
    case CueKind::Died:
        at("died", false);
        break;
    case CueKind::GotUp:
        at("breath", false);
        break;
    case CueKind::Camp:
        at("camp", false);
        break;
    case CueKind::Summit:
        at("sting", false);
        break;
    case CueKind::Count:
        break;
    }
}

void SummitAudio::play_ui(const char* name) {
    _engine.play(_catalog, {.cue = name});
}

i32 SummitAudio::played_total() const {
    i32 n = 0;
    for (i32 v : _played)
        n += v;
    return n;
}

void SummitAudio::update(const Game& game, i32 player, const AudioListener3D& listener, f32 dt, bool paused) {
    _engine.set_listener_3d(listener);
    const CharacterState3D cs = game.character_state(player);
    const f32 altitude = game.altitude(player);
    _wind_level = wind_gain(altitude, game.level().height(), game.wind().speed) *
                  (in_expedition_shelter(game.level(), game.feet(player)) ? 0.2f : 1.0f);
    _engine.set_volume(_wind, _wind_level, 0.1f);
    _engine.set_pitch(_wind, wind_pitch(game.wind().speed));

    const TraversalState3D ts = game.traversal(player);
    const f32 down = game.player(player).state == PlayerState::Alive && ts.mode == TraversalMode3D::Walking &&
                             cs.ground == CharacterGround3D::InAir
                         ? std::max(0.0f, -cs.vertical_speed)
                         : 0.0f;
    _whoosh_level = fall_whoosh_gain(down);
    _engine.set_volume(_whoosh, _whoosh_level, 0.05f);
    _engine.set_pitch(_whoosh, fall_whoosh_pitch(down));

    // Each camp's fire is a voice at the fire (the engine fades it with distance and culls what is out of earshot at
    // the start, so a fire is started when the listener comes within range of it, spread through the clip).
    const std::vector<Camp>& camps = game.level().camps;
    if (_fires.size() != camps.size())
        _fires.assign(camps.size(), {});
    for (std::size_t i = 0; i < camps.size(); ++i) {
        if (_fires[i] && _engine.playing(_fires[i]))
            continue;
        const Vec3f at = camp_fire_position(camps[i]);
        const Vec3f d = at - listener.position;
        if (length(d) > 26.0f)
            continue;
        AudioPlayRequest r;
        r.cue = "fire";
        r.position_3d = at;
        r.has_position_3d = true;
        _fires[i] = _engine.play(_catalog, r);
        if (_fires[i])
            _engine.seek(_fires[i], 0.7f * f32(i));
    }
    if (paused != _paused) {
        _paused = paused;
        _engine.set_bus_paused("sfx", paused);
        _engine.set_bus_volume("ambient", _volumes.ambience * (paused ? 0.35f : 1.0f), 0.2f);
    }
    _engine.update(dt);
}

} // namespace summit
