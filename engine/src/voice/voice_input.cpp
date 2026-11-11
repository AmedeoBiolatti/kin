#include <algorithm>
#include <cmath>
#include <kin/voice/voice_input.hpp>
#include <limits>
#include <opus.h>
#include <rnnoise.h>

namespace kin {

namespace {

constexpr f32 quiet = 1e-7f;                      // -70 dB: below it nothing is played
constexpr f32 tail_fall = 0.708f;                 // a room's ring after a sound: 1.5 dB a block
constexpr f32 mean_keep = 1.0f - 1.0f / 100.0f;   // the slow mean the loudness's shape is taken from: a second
constexpr f32 search_keep = 1.0f - 1.0f / 300.0f; // the correlation's memory: 3 s of the speakers sounding
constexpr i32 search_least = 100;                 // a second of the speakers sounding before the delay is trusted
constexpr f32 search_found = 0.35f;               // the correlation that finds it

f32 rms(std::span<const f32> samples) {
    f64 sum = 0.0;
    for (const f32 s : samples)
        sum += f64(s) * f64(s);
    return samples.empty() ? 0.0f : f32(std::sqrt(sum / f64(samples.size())));
}

f32 decibels(f32 level) {
    return 20.0f * std::log10(std::max(level, 1e-5f));
}

f32 mean_square(std::span<const f32> samples) {
    f32 sum = 0.0f;
    for (const f32 s : samples)
        sum += s * s;
    return samples.empty() ? 0.0f : sum / f32(samples.size());
}

f32 db(f32 power) {
    return 10.0f * std::log10(std::max(power, 1e-12f));
}

} // namespace

// ---- the noise

NoiseSuppressor::NoiseSuppressor() : _state(rnnoise_create(nullptr)) {} // its own model, built in

NoiseSuppressor::~NoiseSuppressor() {
    if (_state)
        rnnoise_destroy(_state);
}

f32 NoiseSuppressor::clean(std::span<f32, mic_block> block) {
    static_assert(mic_block == 480); // RNNoise's frame, at 48 kHz
    if (!_state)
        return 1.0f;
    // RNNoise works in 16-bit sample units.
    for (std::size_t i = 0; i < mic_block; ++i)
        _scaled[i] = block[i] * 32768.0f;
    const f32 speech = rnnoise_process_frame(_state, _scaled.data(), _scaled.data());
    for (std::size_t i = 0; i < mic_block; ++i)
        block[i] = std::clamp(_scaled[i] / 32768.0f, -1.0f, 1.0f);
    return speech;
}

// ---- the speakers

void EchoGate::played(std::span<const f32> mono, i32 rate) {
    const std::size_t block = std::size_t(std::max(1, rate / 100)); // 10 ms at the engine's rate
    for (const f32 s : mono) {
        _pending_sum += s * s;
        if (++_pending_count < block)
            continue;
        const f32 power = _pending_sum / f32(block), amplitude = std::sqrt(power);
        _pending_sum = 0.0f;
        _pending_count = 0;
        _played_mean = _played_mean * mean_keep + amplitude * (1.0f - mean_keep);
        _last_tail = std::max(power, _last_tail * tail_fall);
        _played[std::size_t(_played_blocks % history)] = {power, _last_tail, amplitude - _played_mean};
        ++_played_blocks;
    }
}

const EchoGate::Played* EchoGate::at(i64 block) const {
    if (block < 0 || block >= _played_blocks || block < _played_blocks - history)
        return nullptr;
    return &_played[std::size_t(block % history)];
}

void EchoGate::reset_search() {
    _cross.fill(0.0f);
    _played_energy.fill(0.0f);
    _mic_energy = 0.0f;
    _updates = 0;
    _delay = -1;
    _found = false;
}

void EchoGate::align(i32 blocks) {
    if (_played_blocks == 0 || blocks <= 0)
        return;
    // The newest block played and the last of these blocks of the microphone came about together (give or take the
    // bursts each comes in). The speakers' sound is in the microphone some time after it was played: line the two up
    // with a lead, so that it is found at a lag of `lead` or more.
    const i64 offset = (_played_blocks - 1) - (_mic_blocks + blocks - 1) + lead;
    if (!_aligned || std::abs(offset - _offset) > 12) {
        _offset = offset;
        _aligned = true;
        reset_search();
    }
}

f32 EchoGate::correlation() const {
    if (_delay < 0)
        return 0.0f;
    const f32 d = std::sqrt(_mic_energy * _played_energy[std::size_t(_delay)]);
    return d > 0.0f ? std::clamp(_cross[std::size_t(_delay)] / d, 0.0f, 1.0f) : 0.0f;
}

i32 EchoGate::delay_ms() const {
    return _found ? (_delay - lead) * 10 : -1;
}

f32 EchoGate::heard(f32 power) {
    const i64 block = _mic_blocks++;
    const f32 amplitude = std::sqrt(power);
    _mic_mean = _mic_mean * mean_keep + amplitude * (1.0f - mean_keep);
    // The microphone's own hiss (its quietest lately, rising 2 dB a second), which is not the speakers.
    _mic_floor = block == 0 || power < _mic_floor ? power : _mic_floor * 1.0046f;
    if (!_aligned)
        return 0.0f;
    const i64 here = block + _offset;
    // What was played within reach of this block (at any lag), with its ring.
    f32 reach = 0.0f;
    for (i32 lag = 0; lag < lags; ++lag)
        if (const Played* p = at(here - lag))
            reach = std::max(reach, p->tail);
    _playing = reach > quiet;
    if (!_playing)
        return 0.0f;

    // Where the microphone's loudness follows the speakers' best: their delay.
    const f32 shape = amplitude - _mic_mean;
    _mic_energy = _mic_energy * search_keep + shape * shape;
    i32 best = -1;
    f32 best_score = 0.0f;
    for (i32 lag = 0; lag < lags; ++lag) {
        const Played* p = at(here - lag);
        const f32 y = p ? p->shape : 0.0f;
        const std::size_t l = std::size_t(lag);
        _cross[l] = _cross[l] * search_keep + shape * y;
        _played_energy[l] = _played_energy[l] * search_keep + y * y;
        const f32 d = std::sqrt(_mic_energy * _played_energy[l]);
        const f32 score = d > 0.0f ? _cross[l] / d : 0.0f;
        if (score > best_score) {
            best_score = score;
            best = lag;
        }
    }
    if (++_updates >= search_least && best_score > search_found) {
        _found = true; // and kept, as the best moves with the room (or until the two are lined up again)
        _delay = best;
    }

    // What of the speakers is in this block: at their delay, give or take 20 ms; before it is found, at any lag.
    f32 least = std::numeric_limits<f32>::max(), most = 0.0f, ring = 0.0f;
    const i32 from = _found ? std::max(0, _delay - 2) : 0, to = _found ? std::min(lags - 1, _delay + 2) : lags - 1;
    for (i32 lag = from; lag <= to; ++lag) {
        const Played* p = at(here - lag);
        least = std::min(least, p ? p->power : 0.0f);
        most = std::max(most, p ? p->power : 0.0f);
        ring = std::max(ring, p ? p->tail : 0.0f);
    }
    // How loud the speakers are in the microphone (above its hiss), learnt while they sound. It falls quickly to the
    // quietest the microphone is against them, so that the guest's own voice over them does not raise it; but only
    // where they sounded at every lag they may come back at (else the microphone may not have heard them yet). It rises
    // slowly, and quickly when the microphone plainly follows them (as when they are all it hears); not in their ring,
    // which a room may shorten.
    const f32 heard_db = db(power - _mic_floor);
    if (least > quiet && heard_db - db(least) < _coupling_db)
        _coupling_db -= std::min(1.5f, 0.5f * (_coupling_db - (heard_db - db(least))));
    else if (most > quiet && most * 2.0f >= ring && heard_db - db(most) > _coupling_db)
        _coupling_db += correlation() > 0.6f ? 0.3f : 0.05f;
    _coupling_db = std::clamp(_coupling_db, -90.0f, 10.0f);
    return ring * std::pow(10.0f, _coupling_db / 10.0f);
}

// ---- the codec

VoiceEncoder::VoiceEncoder() {
    int error = 0;
    _opus = opus_encoder_create(voice_rate, 1, OPUS_APPLICATION_VOIP, &error);
    if (!_opus)
        return;
    opus_encoder_ctl(_opus, OPUS_SET_BITRATE(voice_bitrate));
    opus_encoder_ctl(_opus, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(_opus, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND)); // speech's band, the bits on it
    opus_encoder_ctl(_opus, OPUS_SET_COMPLEXITY(8));
    // Each frame also carries a coarse copy of the one before it, at the cost of a little of its own quality, for
    // links that lose a frame in ten.
    opus_encoder_ctl(_opus, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(_opus, OPUS_SET_PACKET_LOSS_PERC(10));
}

VoiceEncoder::~VoiceEncoder() {
    if (_opus)
        opus_encoder_destroy(_opus);
}

std::size_t VoiceEncoder::encode(std::span<const f32> in, std::span<u8> out) {
    if (!_opus || in.size() < std::size_t(voice_frame))
        return 0;
    const opus_int32 n = opus_encode_float(_opus, in.data(), voice_frame, out.data(), opus_int32(out.size()));
    return n > 0 ? std::size_t(n) : 0;
}

void VoiceEncoder::reset() {
    if (_opus)
        opus_encoder_ctl(_opus, OPUS_RESET_STATE);
}

// ---- the microphone

VoiceInput::VoiceInput(std::unique_ptr<IAudioCapture> capture, VoiceInputConfig config)
    : _capture(std::move(capture)), _config(config) {
    _read.resize(std::size_t(voice_rate)); // a second at most each read
    if (_config.denoise)
        _noise = std::make_unique<NoiseSuppressor>();
}

f32 VoiceInput::gate_meter() const {
    return std::clamp((_config.gate_db + 60.0f) / 60.0f, 0.0f, 1.0f);
}

void VoiceInput::update(bool talk, std::vector<VoiceFrame>& out) {
    if (!_capture)
        return;
    _capture->set_paused(_config.mode == VoiceMode::Off);
    const std::size_t got = _capture->read(_read);
    _pending.insert(_pending.end(), _read.begin(), _read.begin() + std::ptrdiff_t(got));
    listen_to_speakers();
    std::size_t at = 0;
    for (; at + std::size_t(voice_frame) <= _pending.size(); at += std::size_t(voice_frame))
        frame(std::span<const f32>(_pending).subspan(at, std::size_t(voice_frame)), talk, out);
    _pending.erase(_pending.begin(), _pending.begin() + std::ptrdiff_t(at));
}

void VoiceInput::listen_to_speakers() {
    if (!_speakers || !_config.echo_gate)
        return;
    const std::size_t channels = std::size_t(_speakers->channels());
    const i32 rate = _speakers->sample_rate();
    _played.resize(std::size_t(rate) * channels); // a second at most each read
    i64 first = 0;
    const std::size_t frames = _speakers->read(_played, &first);
    // What the tap lost (the game stalled for longer than it holds) is silence to the gate, so its count stays true.
    if (frames > 0 && _played_next >= 0 && first > _played_next) {
        _played_mono.assign(std::size_t(std::min<i64>(first - _played_next, rate)), 0.0f);
        _echo.played(_played_mono, rate);
    }
    if (frames > 0)
        _played_next = first + i64(frames);
    _played_mono.resize(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        f32 sum = 0.0f;
        for (std::size_t c = 0; c < channels; ++c)
            sum += _played[f * channels + c];
        _played_mono[f] = sum / f32(channels);
    }
    _echo.played(_played_mono, rate);
    // The frames about to be taken from the microphone end about now, as what was just played does.
    _echo.align(i32(_pending.size() / std::size_t(voice_frame)) * i32(voice_frame / i32(mic_block)));
}

void VoiceInput::frame(std::span<const f32> samples, bool talk, std::vector<VoiceFrame>& out) {
    // Off the rumble and the microphone's offset (a high-pass at about 100 Hz), and the gain.
    std::array<f32, voice_frame> clean{};
    constexpr f32 hp = 0.987f;
    for (std::size_t i = 0; i < clean.size(); ++i) {
        const f32 x = samples[i] * _config.gain;
        _hp_out = hp * (_hp_out + x - _hp_in);
        _hp_in = x;
        clean[i] = std::clamp(_hp_out, -1.0f, 1.0f);
    }
    // The guest's, not the speakers': either half of the frame clearly (9 dB) louder than what of the speakers is in
    // it (as the microphone heard it, noise and all).
    bool clear = true;
    if (_speakers && _config.echo_gate) {
        clear = false;
        for (std::size_t at = 0; at < clean.size(); at += mic_block) {
            const f32 power = mean_square(std::span<const f32>(clean).subspan(at, mic_block));
            clear = power > _echo.heard(power) * 8.0f || clear;
        }
    }
    // The noise out, and whether it is talk: RNNoise's likelihood of speech, more of it to start than to go on.
    _speech = 1.0f;
    if (_noise) {
        _speech = 0.0f;
        for (std::size_t at = 0; at < clean.size(); at += mic_block)
            _speech = std::max(_speech, _noise->clean(std::span<f32, mic_block>(clean.data() + at, mic_block)));
    }
    const bool speech = _speech > (_hang > 0 ? 0.3f : 0.6f);
    const f32 db = decibels(rms(clean));
    _meter = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
    _loud = db > _loud && clear && speech ? db : _loud - 1.0f; // falls 50 dB a second

    // The gate: open while talk is loud enough (and a little after, so words are not clipped), or while the key is
    // held. The speakers louder than the guest close it within a few frames.
    bool open = false;
    const bool loud = db > _config.gate_db && speech;
    switch (_config.mode) {
    case VoiceMode::Off:
        break;
    case VoiceMode::Open:
        _hang = loud && clear ? 15 : clear ? std::max(0, _hang - 1) : std::clamp(_hang - 1, 0, 2);
        open = _hang > 0;
        break;
    case VoiceMode::PushToTalk:
        _hang = talk ? 5 : std::max(0, _hang - 1);
        open = _hang > 0;
        break;
    }
    _echo_held = !clear && (_config.mode == VoiceMode::PushToTalk ? open : loud);
    if (_echo_held)
        ++_echo_frames;
    // How loud, for the Host: a murmur, talk, a shout; held a moment so that it does not flicker. The speakers are not
    // talk, sent or not, nor is a held key with nobody talking.
    const u32 level = !open || !clear || !speech ? 0u : _loud < -30.0f ? 1u : _loud < -16.0f ? 2u : 3u;
    if (level >= _level || --_level_hold <= 0) {
        _level = level;
        _level_hold = 18;
    }
    if (!open && !_open)
        return;
    if (open && !_open)
        _encoder.reset(); // a new run of talk: the listener's decoder starts afresh too
    VoiceFrame f;
    f.seq = _seq++;
    f.level = u8(std::max(1u, _level));
    f.last = !open;
    f.size = u16(_encoder.encode(clean, f.data));
    _open = open;
    if (f.size > 0)
        out.push_back(f);
}

} // namespace kin
