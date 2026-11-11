#pragma once

// The microphone side of a voice chat (docs/voice.md): what is the player's voice, how loud, and when to send it.
//
//   - NoiseSuppressor: the room. RNNoise (Xiph's, a small recurrent network trained on speech and noise) takes out of
//     each 10 ms what is not speech (a fan, traffic, a hum, the keyboard, breathing) and says how likely it is to be
//     speech: an open microphone opens for speech, not for a loud keyboard.
//   - EchoGate: the speakers. Through speakers rather than headphones, the microphone hears the game and the friends'
//     voices again; sent on, the friends would hear themselves a moment later. The gate compares the microphone with
//     what the engine played (AudioEngine::open_output_tap): it finds how late the speakers come back into the
//     microphone (the correlation of their loudness over the last seconds) and how loud they are there (learnt while
//     only the speakers sound), and so how much of each block of the microphone is theirs. A block is the player's only
//     when it is clearly louder than that. With headphones the speakers are not heard at all and the gate never closes.
//     It does not take the echo out of the sound (that is an echo canceller's work): while the speakers are louder in
//     the microphone than the player, the player is not heard (a speakerphone's half duplex).
//   - VoiceInput: the microphone, its level, and the gate: open (it sends while you talk), push to talk, or off.
//     voice_level() says how loud the player talks (a murmur, talk, a shout): a game can put it in its commands, so
//     that every peer's monster hears the same talk.

#include <array>
#include <kin/audio/audio_capture.hpp>
#include <kin/core/types.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct DenoiseState; // rnnoise.h's and opus.h's, kept out of this header
struct OpusEncoder;

namespace kin {

inline constexpr i32 mic_rate = 48000;        // what the microphone is recorded at (open_audio_capture's sample_rate)
inline constexpr std::size_t mic_block = 480; // 10 ms

// RNNoise: takes the noise out of the microphone and says how likely each block is speech.
class NoiseSuppressor {
public:
    NoiseSuppressor();
    ~NoiseSuppressor();
    NoiseSuppressor(const NoiseSuppressor&) = delete;
    NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;
    // A block of mic_block samples at mic_rate, cleaned in place (10 ms late: RNNoise keeps a block back), and how
    // likely it is to be speech, 0 to 1.
    f32 clean(std::span<f32, mic_block> block);

private:
    DenoiseState* _state = nullptr;
    std::array<f32, mic_block> _scaled{};
};

// How much of each block of the microphone is the speakers playing back (see the top of this file).
class EchoGate {
public:
    static constexpr i32 lags = 64;     // how late the speakers may come back: up to 640 ms
    static constexpr i32 lead = 6;      // blocks of slack in lining up the two (see align)
    static constexpr i32 history = 128; // blocks of what was played kept

    // What the speakers played: mono, at `rate`, in order (what the tap lost, as silence).
    void played(std::span<const f32> mono, i32 rate);
    // Before the next `blocks` blocks of the microphone, the last of which ends about when the newest played block
    // does: lines the two up the first time, and again if they have drifted apart (the microphone was paused).
    void align(i32 blocks);
    // One block of the microphone (its mean square): the mean square of the speakers' sound in it, as expected. Learns
    // from it.
    f32 heard(f32 power);

    bool playing() const { return _playing; }        // the speakers played anything lately
    bool found() const { return _found; }            // the delay is known
    i32 delay_ms() const;                            // how late the speakers come back, in ms (-1 until found)
    f32 coupling_db() const { return _coupling_db; } // how loud they are in the microphone, against what was played
    f32 correlation() const;                         // how clearly the microphone follows the speakers, 0 to 1

private:
    struct Played {
        f32 power = 0.0f; // mean square
        f32 tail = 0.0f;  // with the room's ring after it (falling 1.5 dB a block)
        f32 shape = 0.0f; // its loudness less the recent mean, to correlate
    };
    const Played* at(i64 block) const; // nullptr: not played yet, or too long ago
    void reset_search();

    std::array<Played, history> _played{};
    f32 _pending_sum = 0.0f; // of the block being played
    std::size_t _pending_count = 0;
    i64 _played_blocks = 0;
    f32 _played_mean = 0.0f; // of the amplitude: the slow mean `shape` is taken from
    f32 _last_tail = 0.0f;

    i64 _mic_blocks = 0;
    i64 _offset = 0; // the microphone's block i lines up with played block i + offset
    bool _aligned = false;
    f32 _mic_mean = 0.0f;
    f32 _mic_floor = 0.0f; // its hiss: the quietest lately

    std::array<f32, lags> _cross{}, _played_energy{};
    f32 _mic_energy = 0.0f;
    i32 _updates = 0;
    i32 _delay = -1; // lag in blocks, counting the lead
    bool _found = false;
    bool _playing = false;
    f32 _coupling_db = 0.0f; // start as if the speakers were loud: it falls within a second of hearing them
};

inline constexpr i32 voice_rate = mic_rate; // the microphone, and what is coded
inline constexpr i32 voice_frame = 960;     // 20 ms
inline constexpr i32 voice_bitrate = 20000;
inline constexpr std::size_t voice_max_bytes = 400; // a coded frame's most (at 20 kbit/s one is about 50)

// One coded frame of a player's voice.
struct VoiceFrame {
    u16 seq = 0;
    u8 level = 0;      // how loud (VoiceInput::voice_level, 1 to 3)
    bool last = false; // the speaker stopped after it
    u16 size = 0;      // the coded frame's bytes
    std::array<u8, voice_max_bytes> data{};
    std::span<const u8> bytes() const { return {data.data(), size}; }
};

// Opus in its mode for speech, a frame at a time, with in-band FEC (each frame carries a coarse copy of the last).
class VoiceEncoder {
public:
    VoiceEncoder();
    ~VoiceEncoder();
    VoiceEncoder(const VoiceEncoder&) = delete;
    VoiceEncoder& operator=(const VoiceEncoder&) = delete;
    // voice_frame samples at voice_rate into `out`: the coded frame's size (0 if Opus could not). Only speech's band
    // (up to 8 kHz) is coded.
    std::size_t encode(std::span<const f32> in, std::span<u8> out);
    void reset(); // before a new run of talk

private:
    OpusEncoder* _opus = nullptr;
};

enum class VoiceMode : u8 { Off, Open, PushToTalk };

struct VoiceInputConfig {
    VoiceMode mode = VoiceMode::Open;
    f32 gate_db = -42.0f; // open: a frame louder than this (dB below full scale) opens the gate
    f32 gain = 1.0f;
    bool echo_gate = true; // with speakers (set_speakers): what they play back is not talk
    bool denoise = true;   // RNNoise: the noise taken out, and the gate opened by speech only
};

// The microphone, cleaned and gated, as coded frames to send:
//
//     auto mic = kin::open_audio_capture({.sample_rate = kin::voice_rate});
//     kin::VoiceInput input(std::move(mic), {.mode = kin::VoiceMode::Open});
//     input.set_speakers(audio.open_output_tap()); // through speakers, not headphones: no echo of the others
class VoiceInput {
public:
    // `capture` records at voice_rate.
    VoiceInput(std::unique_ptr<IAudioCapture> capture, VoiceInputConfig config);
    // What the engine plays (AudioEngine::open_output_tap), for the echo gate.
    void set_speakers(std::shared_ptr<AudioOutputTap> tap) { _speakers = std::move(tap); }
    // Takes what was recorded since the last call and appends the frames to send to `out`. `talk`: the push-to-talk
    // key is held.
    void update(bool talk, std::vector<VoiceFrame>& out);
    u32 voice_level() const { return _level; } // 0 silent, 1 a murmur, 2 talk, 3 a shout
    f32 meter() const { return _meter; }       // the microphone's level, 0 to 1, sending or not
    bool sending() const { return _open; }
    VoiceMode mode() const { return _config.mode; }
    f32 gate_meter() const; // where the gate is on the meter
    std::string device() const { return _capture ? _capture->device() : std::string(); }
    // The speakers are louder in the microphone than the player: the gate holds (and, pushing to talk, it is not
    // counted as talk in voice_level()).
    bool echo_held() const { return _echo_held; }
    u64 echo_frames() const { return _echo_frames; } // frames held so (that would have been talk)
    f32 speech() const { return _speech; }           // how likely the last frame was speech, 0 to 1
    const EchoGate& echo() const { return _echo; }

private:
    void frame(std::span<const f32> samples, bool talk, std::vector<VoiceFrame>& out);
    void listen_to_speakers();

    std::unique_ptr<IAudioCapture> _capture;
    VoiceInputConfig _config;
    std::vector<f32> _pending;
    std::vector<f32> _read;
    VoiceEncoder _encoder;
    std::shared_ptr<AudioOutputTap> _speakers;
    std::vector<f32> _played; // read from the tap, interleaved
    std::vector<f32> _played_mono;
    i64 _played_next = -1; // the tap's next frame, as last read
    EchoGate _echo;
    std::unique_ptr<NoiseSuppressor> _noise;
    f32 _speech = 0.0f;
    bool _echo_held = false;
    u64 _echo_frames = 0;
    f32 _hp_in = 0.0f, _hp_out = 0.0f;
    bool _open = false;
    i32 _hang = 0;
    f32 _loud = -90.0f; // dB, quick to rise and slow to fall
    u32 _level = 0;
    i32 _level_hold = 0;
    f32 _meter = 0.0f;
    u16 _seq = 0;
};

} // namespace kin
