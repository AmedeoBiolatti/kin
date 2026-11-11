#include <algorithm>
#include <cmath>
#include <cstring>
#include <kin/audio/audio_decoder.hpp>
#include <kin/voice/voice.hpp>
#include <limits>
#include <opus.h>

namespace kin {

namespace {

f32 rms(std::span<const f32> samples) {
    f64 sum = 0.0;
    for (const f32 s : samples)
        sum += f64(s) * f64(s);
    return samples.empty() ? 0.0f : f32(std::sqrt(sum / f64(samples.size())));
}

} // namespace

// ---- the codec

VoiceDecoder::VoiceDecoder() {
    int error = 0;
    _opus = opus_decoder_create(voice_out_rate, 1, &error);
}

VoiceDecoder::~VoiceDecoder() {
    if (_opus)
        opus_decoder_destroy(_opus);
}

bool VoiceDecoder::decode(std::span<const u8> in, std::span<f32> out, bool before) {
    const int n = _opus && !in.empty() ? opus_decode_float(_opus, in.data(), opus_int32(in.size()), out.data(),
                                                           voice_out_frame, before ? 1 : 0)
                                       : -1;
    if (n == voice_out_frame)
        return true;
    std::fill(out.begin(), out.end(), 0.0f);
    return false;
}

void VoiceDecoder::conceal(std::span<f32> out) {
    if (!_opus || opus_decode_float(_opus, nullptr, 0, out.data(), voice_out_frame, 0) != voice_out_frame)
        std::fill(out.begin(), out.end(), 0.0f);
}

void VoiceDecoder::reset() {
    if (_opus)
        opus_decoder_ctl(_opus, OPUS_RESET_STATE);
}

// ---- packets

std::size_t write_voice_packet(u32 slot, const VoiceFrame& frame, std::span<u8, voice_packet_bytes> out) {
    out[0] = 'k';
    out[1] = 'v';
    out[2] = 1;
    out[3] = u8(slot);
    out[4] = u8(frame.seq & 0xff);
    out[5] = u8(frame.seq >> 8);
    out[6] = frame.level;
    out[7] = frame.last ? 1 : 0;
    const std::size_t size = std::min<std::size_t>(frame.size, voice_max_bytes);
    std::memcpy(out.data() + voice_header_bytes, frame.data.data(), size);
    return voice_header_bytes + size;
}

bool is_voice_packet(std::span<const u8> bytes) {
    return bytes.size() >= 3 && bytes[0] == 'k' && bytes[1] == 'v';
}

std::optional<std::pair<u32, VoiceFrame>> read_voice_packet(std::span<const u8> bytes) {
    if (bytes.size() <= voice_header_bytes || bytes.size() > voice_packet_bytes || !is_voice_packet(bytes) ||
        bytes[2] != 1)
        return std::nullopt;
    VoiceFrame frame;
    frame.seq = u16(bytes[4] | (bytes[5] << 8));
    frame.level = std::min<u8>(bytes[6], 3);
    frame.last = (bytes[7] & 1) != 0;
    frame.size = u16(bytes.size() - voice_header_bytes);
    std::memcpy(frame.data.data(), bytes.data() + voice_header_bytes, frame.size);
    return std::pair<u32, VoiceFrame>{bytes[3], frame};
}

// ---- a speaker's stream

VoiceStream::VoiceStream() = default;

void VoiceStream::push(const VoiceFrame& frame) {
    const std::scoped_lock lock{_mutex};
    if (_playing || _any) {
        const i16 ahead = i16(u16(frame.seq - _next));
        if (_playing && ahead < 0) {
            ++_stats.late;
            return;
        }
        if (ahead >= i16(slots) || ahead <= -i16(slots)) {
            // Far from where we are: the speaker started again (or we lost track). Start from this one.
            for (Slot& s : _slots)
                s.filled = false;
            _playing = false;
            _next = frame.seq;
        } else if (!_playing && ahead < 0) {
            _next = frame.seq; // an earlier one came later
        }
    } else {
        _next = frame.seq;
    }
    _any = true;
    _slots[frame.seq % slots] = {frame.seq, true, frame};
}

bool VoiceStream::waiting_after(u16 seq) const {
    for (const Slot& s : _slots)
        if (s.filled && i16(u16(s.seq - seq)) > 0)
            return true;
    return false;
}

void VoiceStream::restart_waiting() {
    _playing = false;
    _any = false;
    for (const Slot& s : _slots)
        if (s.filled && (!_any || i16(u16(s.seq - _next)) < 0)) {
            _next = s.seq;
            _any = true;
        }
    _waited = 0;
}

void VoiceStream::next_frame() {
    std::span<f32> frame(_out);
    _out_pos = 0;
    if (!_playing) {
        // Start once three frames are in a row, or the last of what was said is in, or the first has waited long.
        i32 run = 0;
        bool ends = false;
        for (u16 seq = _next; run < 3 && _any; ++seq) {
            const Slot& s = _slots[seq % slots];
            if (!s.filled || s.seq != seq)
                break;
            ++run;
            if (s.frame.last) {
                ends = true;
                break;
            }
        }
        if (!_any || (run < 3 && !ends && ++_waited < 3)) {
            std::fill(frame.begin(), frame.end(), 0.0f);
            _level.store(_level.load(std::memory_order_relaxed) * 0.7f, std::memory_order_relaxed);
            return;
        }
        _playing = true;
        _waited = 0;
        _lost_run = 0;
        _decoder.reset(); // a new run of talk, as the speaker's coder began it
    }
    Slot& slot = _slots[_next % slots];
    bool ended = false;
    if (slot.filled && slot.seq == _next) {
        _decoder.decode(slot.frame.bytes(), frame);
        slot.filled = false;
        _lost_run = 0;
        ended = slot.frame.last;
        ++_stats.played;
    } else {
        // Lost, or not here yet. The next frame, if it is in, carries a copy of this one; else Opus makes one up. With
        // nothing after it, the speaker has paused.
        const Slot& after = _slots[u16(_next + 1) % slots];
        if (after.filled && after.seq == u16(_next + 1)) {
            _decoder.decode(after.frame.bytes(), frame, true);
            ++_stats.recovered;
        } else {
            _decoder.conceal(frame);
            ++_stats.concealed;
        }
        ++_lost_run;
        ended = !waiting_after(_next) || _lost_run > 4;
    }
    ++_next;
    if (ended) {
        restart_waiting();
    } else {
        // Too far behind the speaker (its clock is a little faster than ours): skip a frame to catch up.
        i32 waiting = 0;
        for (const Slot& s : _slots)
            waiting += s.filled ? 1 : 0;
        if (waiting > 6) {
            Slot& skip = _slots[_next % slots];
            if (skip.filled && skip.seq == _next)
                skip.filled = false;
            ++_next;
            ++_stats.dropped;
        }
    }
    const f32 now = std::min(1.0f, rms(frame) * 5.0f);
    const f32 was = _level.load(std::memory_order_relaxed);
    _level.store(now > was ? now : was * 0.7f + now * 0.3f, std::memory_order_relaxed);
}

void VoiceStream::pull(std::span<f32> out) {
    const std::scoped_lock lock{_mutex};
    std::size_t at = 0;
    while (at < out.size()) {
        if (_out_pos >= _out.size())
            next_frame();
        const std::size_t n = std::min(out.size() - at, _out.size() - _out_pos);
        std::copy_n(_out.begin() + std::ptrdiff_t(_out_pos), n, out.begin() + std::ptrdiff_t(at));
        _out_pos += n;
        at += n;
    }
}

VoiceStreamStats VoiceStream::stats() const {
    const std::scoped_lock lock{_mutex};
    return _stats;
}

// ---- links

ServicesVoiceLink::ServicesVoiceLink(PlatformServices& services, std::vector<std::string> players, u32 channel)
    : _services(services), _players(std::move(players)), _channel(channel) {}

bool ServicesVoiceLink::send(u32 slot, std::span<const u8> packet) {
    return slot < _players.size() && !_players[slot].empty() &&
           _services.send_message(_players[slot], packet, MessageDelivery::Unreliable, _channel);
}

void ServicesVoiceLink::receive(std::vector<std::pair<u32, std::vector<u8>>>& out) {
    _messages.clear();
    _services.receive_messages(_channel, _messages);
    for (PlayerMessage& message : _messages) {
        const auto it = std::ranges::find(_players, message.from);
        if (it != _players.end() && !message.from.empty()) {
            out.emplace_back(static_cast<u32>(it - _players.begin()), std::move(message.bytes));
        }
    }
}

// ---- the chat

VoiceChat::VoiceChat(u32 local_slot, u32 players) : _local(local_slot) {
    for (u32 i = 0; i < players; ++i)
        _streams.push_back(std::make_shared<VoiceStream>());
}

std::shared_ptr<VoiceStream> VoiceChat::stream(u32 slot) const {
    return slot < _streams.size() ? _streams[slot] : nullptr;
}

void VoiceChat::update(bool talk) {
    _frames.clear();
    if (_input)
        _input->update(talk, _frames);
    std::array<u8, voice_packet_bytes> packet{};
    _recorded += _frames.size();
    for (const VoiceFrame& f : _frames) {
        if (_echo >= 0 && u32(_echo) < _streams.size())
            _streams[std::size_t(_echo)]->push(f);
        if (!_link)
            continue;
        const std::size_t size = write_voice_packet(_local, f, packet);
        for (u32 slot = 0; slot < _streams.size(); ++slot)
            if (slot != _local && _link->send(slot, std::span<const u8>(packet.data(), size)))
                ++_sent;
    }
    if (!_link)
        return;
    _arrived.clear();
    _link->receive(_arrived);
    for (const auto& [from, bytes] : _arrived) {
        const auto parsed = read_voice_packet(bytes);
        if (!parsed)
            continue;
        const auto& [slot, frame] = *parsed;
        // Only from the player in that slot: nobody speaks for another.
        if (slot != from || slot == _local || slot >= _streams.size() || ((_muted >> slot) & 1u))
            continue;
        _streams[slot]->push(frame);
        ++_received;
    }
}

// ---- playing

namespace {

class LiveVoiceDecoder final : public AudioDecoder {
public:
    explicit LiveVoiceDecoder(std::shared_ptr<VoiceStream> stream) : _stream(std::move(stream)) {}
    i32 channels() const override { return 1; }
    i32 sample_rate() const override { return voice_out_rate; }
    i64 frame_count() const override { return std::numeric_limits<i32>::max(); }
    i64 read(std::span<f32> out) override {
        _stream->pull(out);
        return i64(out.size());
    }
    bool seek(i64) override { return false; }

private:
    std::shared_ptr<VoiceStream> _stream;
};

class LiveVoiceClip final : public IAudioClipBackend {
public:
    explicit LiveVoiceClip(std::shared_ptr<VoiceStream> stream) : _stream(std::move(stream)) {}
    std::span<const f32> samples() const override { return {}; }
    bool streamed() const override { return true; }
    std::size_t memory_bytes() const override { return 0; }
    std::unique_ptr<AudioDecoder> open_stream() const override { return std::make_unique<LiveVoiceDecoder>(_stream); }

private:
    std::shared_ptr<VoiceStream> _stream;
};

} // namespace

AudioCue voice_cue(std::string_view clip_id, f32 near, f32 far, std::string bus) {
    return AudioCue{.id = std::string{clip_id},
                    .clips = {std::string{clip_id}},
                    .bus = std::move(bus),
                    .spatial = true,
                    .min_distance = near,
                    .max_distance = far};
}

void add_voice_clip(AudioEngine& engine, std::string_view clip_id, std::shared_ptr<VoiceStream> stream) {
    engine.add_clip(clip_id,
                    AudioClip(std::string{clip_id}, AudioFormat::F32, 1, voice_out_rate,
                              std::numeric_limits<i32>::max(), std::make_shared<LiveVoiceClip>(std::move(stream))));
}

} // namespace kin
