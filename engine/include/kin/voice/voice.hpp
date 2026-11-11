#pragma once

// Voice chat (docs/voice.md): each player's microphone, cut into frames of 20 ms, coded and sent to every other
// player, and the others' voices played where they stand.
//
//   - VoiceInput (voice_input.hpp): the microphone, cleaned of noise and of the speakers, gated, and coded.
//   - VoiceDecoder: Opus for speech (up to 8 kHz of it), 20 kbit/s while someone talks and nothing while they do not;
//     each frame carries a coarse copy of the one before it, so a frame lost on the way is made again from the next,
//     and Opus covers what is lost beyond that.
//   - VoiceStream: one speaker's frames as they arrive, in order (a jitter buffer of about 60 ms), decoded and played
//     at 48 kHz. play_voice() plays one through an AudioEngine, at a position in the world.
//   - VoiceLink: where the packets go. ServicesVoiceLink sends them as the platform's messages (on Steam through its
//     relays, beside a ServicesTransport match on its own channel); a game with its own transport implements send() and
//     receive().
//   - VoiceChat: the lot, for one player.
//
//     kin::VoiceChat chat(match.local_player, players);
//     chat.set_input(std::make_unique<kin::VoiceInput>(kin::open_audio_capture({.sample_rate = kin::voice_rate}),
//                                                      kin::VoiceInputConfig{}));
//     kin::ServicesVoiceLink link(*services, match.players); // the lobby match's players, by slot
//     chat.set_link(&link);
//     // each frame:
//     chat.update(push_to_talk_held);
//     // each other player's voice, once: kin::add_voice_clip(audio, "voice1", chat.stream(1)), played at their mouth
//
// Nothing here is part of a deterministic simulation: only voice_level(), if a game puts it in its commands.

#include <atomic>
#include <kin/audio/audio_engine.hpp>
#include <kin/core/types.hpp>
#include <kin/platform/services.hpp>
#include <kin/voice/voice_input.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

struct OpusDecoder; // opus.h's, kept out of this header

namespace kin {

class PlatformServices;

inline constexpr i32 voice_out_rate = 48000; // what is played
inline constexpr i32 voice_out_frame = voice_frame * voice_out_rate / voice_rate;
inline constexpr std::size_t voice_header_bytes = 8;
inline constexpr std::size_t voice_packet_bytes = voice_header_bytes + voice_max_bytes; // the most
// The platform message channel voice uses by default: apart from a game's own messages (0) and a ServicesTransport
// match (1).
inline constexpr u32 voice_channel = 2;

class VoiceDecoder {
public:
    VoiceDecoder();
    ~VoiceDecoder();
    VoiceDecoder(const VoiceDecoder&) = delete;
    VoiceDecoder& operator=(const VoiceDecoder&) = delete;
    // A coded frame into voice_out_frame samples at voice_out_rate; with `before`, the frame before it, from the copy
    // it carries (for one that was lost). False if it would not decode (out is then silence).
    bool decode(std::span<const u8> in, std::span<f32> out, bool before = false);
    // A frame where none came: Opus makes one up from what it has heard.
    void conceal(std::span<f32> out);
    void reset(); // before a new run of talk

private:
    OpusDecoder* _opus = nullptr;
};

// A voice packet: 'k' 'v', a version, the speaker's slot, the frame's number, its level and flags, the coded frame.
// Its size; `out` holds voice_packet_bytes.
std::size_t write_voice_packet(u32 slot, const VoiceFrame& frame, std::span<u8, voice_packet_bytes> out);
bool is_voice_packet(std::span<const u8> bytes);
std::optional<std::pair<u32, VoiceFrame>> read_voice_packet(std::span<const u8> bytes);

struct VoiceStreamStats {
    u64 played = 0;    // frames
    u64 recovered = 0; // lost, made again from the next frame's copy
    u64 concealed = 0; // lost, made up
    u64 late = 0;      // came after their turn
    u64 dropped = 0;   // thrown away to catch up
};

// One speaker's voice: push() on the game thread as frames arrive, pull() on the audio thread as it plays.
class VoiceStream {
public:
    VoiceStream();
    void push(const VoiceFrame& frame);
    // The next of what was said, at 48 kHz, filling `out` (silence where nothing was said).
    void pull(std::span<f32> out);
    // How loud it plays now, 0 to 1 (any thread).
    f32 level() const { return _level.load(std::memory_order_relaxed); }
    VoiceStreamStats stats() const;

private:
    struct Slot {
        u16 seq = 0;
        bool filled = false;
        VoiceFrame frame;
    };
    static constexpr std::size_t slots = 16;
    void next_frame();
    bool waiting_after(u16 seq) const; // a frame after `seq` is in
    void restart_waiting();            // not playing: the earliest frame in is next

    mutable std::mutex _mutex;
    std::array<Slot, slots> _slots{};
    bool _playing = false;
    bool _any = false; // frames are in (not playing: waiting to start)
    u16 _next = 0;
    i32 _waited = 0;
    i32 _lost_run = 0;
    VoiceDecoder _decoder;
    std::array<f32, voice_out_frame> _out{};
    std::size_t _out_pos = voice_out_frame;
    std::atomic<f32> _level{0.0f};
    VoiceStreamStats _stats;
};

// Where a voice chat's packets go and come from.
class VoiceLink {
public:
    virtual ~VoiceLink() = default;
    // Sends a packet to the player in `slot`; false if it could not.
    virtual bool send(u32 slot, std::span<const u8> packet) = 0;
    // Appends the packets that arrived, each with the slot of the player it came from (a packet that says it is
    // another slot's voice is dropped: nobody speaks for another).
    virtual void receive(std::vector<std::pair<u32, std::vector<u8>>>& out) = 0;
};

// Voice over the platform's messages between players (PlatformServices::send_message), unreliable, on `channel`.
class ServicesVoiceLink final : public VoiceLink {
public:
    // `players`: each slot's player id (ServicePlayer::id), "" for an empty slot, as LobbyMatch::players has them.
    ServicesVoiceLink(PlatformServices& services, std::vector<std::string> players, u32 channel = voice_channel);
    // Who plays each slot now (a player joined or left).
    void set_players(std::vector<std::string> players) { _players = std::move(players); }

    bool send(u32 slot, std::span<const u8> packet) override;
    void receive(std::vector<std::pair<u32, std::vector<u8>>>& out) override;

private:
    PlatformServices& _services;
    std::vector<std::string> _players;
    u32 _channel;
    std::vector<PlayerMessage> _messages;
};

// One player's voice chat: its microphone sent to the others, and the others' voices as they arrive.
class VoiceChat {
public:
    VoiceChat(u32 local_slot, u32 players);
    void set_input(std::unique_ptr<VoiceInput> input) { _input = std::move(input); }
    void set_link(VoiceLink* link) { _link = link; }
    // Hear your own voice, as slot `slot` would be heard (alone, to try a microphone); -1 for none.
    void set_echo(i32 slot) { _echo = slot; }
    // Players nobody wants to hear (a bit each): their packets are dropped.
    void set_muted(u32 slots) { _muted = slots; }

    // Each frame: records, sends, and takes what arrived. `talk`: the push-to-talk key is held.
    void update(bool talk);

    const VoiceInput* input() const { return _input.get(); }
    u32 voice_level() const { return _input ? _input->voice_level() : 0u; }
    std::shared_ptr<VoiceStream> stream(u32 slot) const;
    u64 recorded() const { return _recorded; } // frames the microphone sent (or would have)
    u64 sent() const { return _sent; }         // packets
    u64 received() const { return _received; }

private:
    u32 _local;
    std::unique_ptr<VoiceInput> _input;
    VoiceLink* _link = nullptr;
    i32 _echo = -1;
    u32 _muted = 0;
    std::vector<std::shared_ptr<VoiceStream>> _streams;
    std::vector<VoiceFrame> _frames;
    std::vector<std::pair<u32, std::vector<u8>>> _arrived;
    u64 _recorded = 0, _sent = 0, _received = 0;
};

// A voice stream as a clip that never ends (what is said as it arrives, silence between), under `clip_id` in `engine`
// (AudioEngine::add_clip). Play it through a cue that names it (voice_cue() makes one), at a position, and move it with
// the speaker:
//
//     kin::add_voice_clip(audio, "voice1", chat.stream(1));
//     catalog.add_cue(kin::voice_cue("voice1"));
//     const kin::AudioHandle handle =
//         audio.play(catalog, {.cue = "voice1", .position_3d = mouth, .has_position_3d = true});
//     audio.set_position_3d(handle, mouth); // each frame
void add_voice_clip(AudioEngine& engine, std::string_view clip_id, std::shared_ptr<VoiceStream> stream);
// A cue for a voice clip of the same id: placed in the world (full volume within `near`, silent past `far`, in world
// units), on `bus` (a game with a bus of its own for voices, for a volume slider, names it).
AudioCue voice_cue(std::string_view clip_id, f32 near = 1.0f, f32 far = 25.0f, std::string bus = "sfx");

} // namespace kin
