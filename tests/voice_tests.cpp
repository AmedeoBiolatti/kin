// Voice chat (kin/voice/voice.hpp, docs/voice.md): the Opus codec, a speaker's stream, the microphone's gate, its
// echo gate and noise suppression, voices over the platform's messages, the engine's output tap, and a voice played
// through the engine.

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <kin/audio/audio.hpp>
#include <kin/platform/services.hpp>
#include <kin/voice/voice.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace kin;

constexpr f32 pi = 3.14159265358979323846f;

f32 rms_of(std::span<const f32> v) {
    double sum = 0.0;
    for (const f32 x : v)
        sum += double(x) * x;
    return v.empty() ? 0.0f : f32(std::sqrt(sum / double(v.size())));
}

// How strong one frequency is in a signal (a sinusoid's amplitude), by Goertzel's sum over whole periods.
f32 amplitude_at(std::span<const f32> x, f32 hz, f32 rate) {
    const double w = 2.0 * 3.14159265358979323846 * double(hz) / double(rate), c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (const f32 v : x) {
        const double s0 = double(v) + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - c * s1 * s2;
    return f32(2.0 * std::sqrt(std::max(0.0, power)) / double(x.size()));
}

// Opus: what goes in comes out sounding the same (each partial of a voice-like sound at its strength), in about 50
// bytes a frame; silence stays silence.
void test_voice_codec() {
    constexpr int frames = 25; // half a second
    std::vector<f32> speech(std::size_t(voice_frame * frames));
    for (std::size_t i = 0; i < speech.size(); ++i) {
        const f32 t = f32(i) / f32(voice_rate);
        speech[i] = 0.3f * std::sin(2.0f * pi * 220.0f * t) + 0.15f * std::sin(2.0f * pi * 1250.0f * t) +
                    0.05f * std::sin(2.0f * pi * 3100.0f * t);
    }
    VoiceEncoder encoder;
    VoiceDecoder decoder;
    std::vector<f32> heard, chunk(voice_out_frame);
    std::array<u8, voice_max_bytes> bytes{};
    std::size_t total = 0, most = 0;
    for (int f = 0; f < frames; ++f) {
        const std::size_t n =
            encoder.encode(std::span<const f32>(speech).subspan(std::size_t(f * voice_frame), voice_frame), bytes);
        assert(n > 0);
        total += n;
        most = std::max(most, n);
        assert(decoder.decode(std::span<const u8>(bytes.data(), n), chunk));
        heard.insert(heard.end(), chunk.begin(), chunk.end());
    }
    // A tenth of a second from the middle: whole periods of both partials.
    const std::span<const f32> middle = std::span<const f32>(heard).subspan(heard.size() / 2, voice_out_rate / 10);
    const f32 low = amplitude_at(middle, 220.0f, f32(voice_out_rate)),
              mid = amplitude_at(middle, 1250.0f, f32(voice_out_rate));
    assert(std::fabs(low - 0.3f) < 0.3f * 0.3f && std::fabs(mid - 0.15f) < 0.15f * 0.35f);
    assert(total / frames <= 70 && most <= 160);
    VoiceEncoder quiet;
    VoiceDecoder quiet_decoder;
    std::array<f32, voice_frame> silence{};
    for (int f = 0; f < 4; ++f) {
        const std::size_t n = quiet.encode(silence, bytes);
        assert(n > 0 && quiet_decoder.decode(std::span<const u8>(bytes.data(), n), chunk));
    }
    assert(rms_of(chunk) < 1e-3f);
    std::printf("  voice codec: Opus, %zu bytes a frame of 20 ms on average (%zu at most), partials at %.2f and %.2f "
                "(0.30 and 0.15 went in)\n",
                total / frames, most, low, mid);
}

// A speaker's frames as they come: the first after a short wait, in order though they came out of it; one lost made
// again from the copy the next carries; two lost in a row, the first made up; a late one dropped; after the last,
// silence.
void test_voice_stream() {
    constexpr int count = 14;
    VoiceEncoder encoder;
    std::vector<VoiceFrame> frames(count);
    for (int k = 0; k < count; ++k) {
        std::array<f32, voice_frame> samples;
        for (int i = 0; i < voice_frame; ++i)
            samples[std::size_t(i)] = 0.2f * std::sin(2.0f * pi * 300.0f * f32(k * voice_frame + i) / f32(voice_rate));
        VoiceFrame& f = frames[std::size_t(k)];
        f.seq = u16(65530 + k); // across the wrap
        f.level = 2;
        f.last = k == count - 1;
        f.size = u16(encoder.encode(samples, f.data));
        assert(f.size > 0);
    }
    VoiceStream stream;
    std::vector<f32> chunk(voice_out_frame), level;
    for (int t = 0; t < 18; ++t) {
        // Frame k comes at tick k, but frame 2 at 3; frames 3, 8 and 9 never; frame 1 again at 8.
        if (t < count && t != 2 && t != 3 && t != 8 && t != 9)
            stream.push(frames[std::size_t(t)]);
        if (t == 3)
            stream.push(frames[2]);
        if (t == 8)
            stream.push(frames[1]);
        stream.pull(chunk);
        level.push_back(rms_of(chunk));
    }
    const f32 tone = 0.2f / std::sqrt(2.0f);
    assert(level[0] == 0.0f && level[1] == 0.0f); // waiting for enough to start
    for (int t = 3; t <= 15; ++t)
        if (t < 10 || t > 13) // frame 8, made up; 9, from the coarse copy; and the two the decoder comes back in
            assert(std::fabs(level[std::size_t(t)] - tone) < tone * 0.35f);
    for (int t = 11; t <= 13; ++t)
        assert(level[std::size_t(t)] > tone * 0.45f && level[std::size_t(t)] < tone * 1.2f);
    assert(level[2] > 0.02f && level[10] > 0.01f);
    assert(level[16] == 0.0f && level[17] == 0.0f);
    const VoiceStreamStats st = stream.stats();
    assert(st.played == 11 && st.recovered == 2 && st.concealed == 1 && st.late == 1 && st.dropped == 0);
}

// The gate: a quiet room sends nothing (and nothing is heard as talk); talk opens it, a short pause keeps it open, the
// end closes it with a frame marked last; talking louder is a higher level. Push to talk sends only while held.
void test_voice_gate() {
    const auto tone = [](f32 seconds, f32 amplitude, std::vector<f32>& out) {
        for (int i = 0; i < int(seconds * voice_rate); ++i)
            out.push_back(amplitude * std::sin(2.0f * pi * 300.0f * f32(out.size()) / f32(voice_rate)));
    };
    std::vector<f32> samples;
    tone(1.0f, 0.0014f, samples); // a hiss at -60 dB
    tone(1.0f, 0.14f, samples);   // talk, at -20 dB
    tone(0.1f, 0.0014f, samples); // a breath
    tone(0.5f, 0.6f, samples);    // a shout
    tone(1.0f, 0.0014f, samples);
    const auto run = [&](VoiceMode mode, std::vector<int>& sent, std::vector<u32>& levels, int& lasts) {
        double now = 0.0;
        VoiceInput mic(make_memory_audio_capture(samples, voice_rate, [&] { return now; }),
                       {.mode = mode, .denoise = false}); // a tone: RNNoise soon takes a steady one for noise
        std::vector<VoiceFrame> out;
        for (int f = 0; f < 180; ++f) {
            now += 0.02;
            out.clear();
            mic.update(f >= 60 && f < 80, out);
            sent.push_back(int(out.size()));
            levels.push_back(mic.voice_level());
            for (const VoiceFrame& v : out)
                lasts += v.last ? 1 : 0;
        }
    };
    std::vector<int> sent;
    std::vector<u32> levels;
    int lasts = 0;
    run(VoiceMode::Open, sent, levels, lasts);
    const auto sum = [&](int from, int to) {
        int n = 0;
        for (int f = from; f < to; ++f)
            n += sent[std::size_t(f)];
        return n;
    };
    assert(sum(0, 49) == 0 && levels[40] == 0);                   // the hiss
    assert(sum(52, 100) >= 46 && levels[90] == 2);                // talk
    assert(sum(100, 106) == 6);                                   // the breath: still open
    assert(levels[125] == 3);                                     // the shout
    assert(sum(150, 180) == 0 && lasts == 1 && levels[179] == 0); // closed, once
    std::vector<int> held;
    std::vector<u32> held_levels;
    int held_lasts = 0;
    run(VoiceMode::PushToTalk, held, held_levels, held_lasts);
    int before = 0, during = 0, after = 0;
    for (int f = 0; f < 180; ++f)
        (f < 60 ? before : f < 86 ? during : after) += held[std::size_t(f)];
    assert(before == 0 && during >= 20 && after == 0 && held_lasts == 1);
}

// Something like talk: syllables of a voiced sound (a pitch and its harmonics, swelling and fading), short pauses
// between them and longer ones between phrases. `seed` picks the voice and the words.
std::vector<f32> babble(f32 seconds, u32 seed, f32 amplitude) {
    std::vector<f32> out(std::size_t(seconds * f32(voice_rate)), 0.0f);
    u32 state = seed * 2654435761u + 1u;
    const auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return f32(state >> 8) / f32(1u << 24);
    };
    const f32 voice = 100.0f + 100.0f * next();
    std::size_t at = std::size_t(0.05f * f32(voice_rate));
    while (at < out.size()) {
        const std::size_t length = std::size_t((0.12f + 0.2f * next()) * f32(voice_rate));
        const f32 pitch = voice * (0.85f + 0.3f * next());
        for (std::size_t i = 0; i < length && at + i < out.size(); ++i) {
            const f32 t = f32(i) / f32(voice_rate), swell = std::sin(pi * f32(i) / f32(length));
            f32 v = 0.0f;
            for (int k = 1; k <= 6; ++k)
                v += std::sin(2.0f * pi * pitch * f32(k) * t) / f32(k);
            out[at + i] = amplitude * swell * v * 0.5f;
        }
        const f32 pause = next() < 0.2f ? 0.4f + 0.5f * next() : 0.04f + 0.12f * next();
        at += length + std::size_t(pause * f32(voice_rate));
    }
    return out;
}

// The speakers in the microphone: what the engine plays (its tap) comes back into the microphone 65 ms after the tap
// has it (the device's latency, the air, the microphone's), a third as loud and with the room's ring. The gate finds
// how late and how loud, and from then on a friend's voice coming back is not sent (nor counted as talk),
// while the player talking alone is, as before. With headphones the player is never held, over a friend or not; without
// the gate the echo would go out.
void test_echo_gate() {
    constexpr f32 seconds = 16.0f;
    // The friends talk for the first 8 s and again from 12 s; the player from 8 s (alone until 12, then over them).
    std::vector<f32> friends = babble(seconds, 1, 0.3f), player = babble(seconds, 2, 0.15f);
    for (std::size_t i = 0; i < friends.size(); ++i) {
        const f32 t = f32(i) / f32(voice_rate);
        if (t >= 8.0f && t < 12.0f)
            friends[i] = 0.0f;
        if (t < 8.0f)
            player[i] = 0.0f;
    }
    struct Run {
        std::vector<int> sent;    // frames sent, each 1/60 s step
        std::vector<u32> level;   // voice_level()
        std::vector<int> talking; // frames of the player's talk that came in
        std::vector<int> missed;  // of those, not sent
        i32 delay_ms = -1;
        f32 coupling_db = 0.0f;
    };
    const auto run = [&](f32 coupling, bool gate) {
        constexpr i64 ahead = 1920, late = 1200; // mixed 40 ms before it plays; in the microphone 25 ms after
        std::vector<f32> mic(friends.size());
        u32 hiss = 7u;
        for (std::size_t i = 0; i < mic.size(); ++i) {
            f32 echo = 0.0f;
            for (const auto [after, gain] : {std::pair{0, 1.0f}, {3 * 480, 0.5f}, {8 * 480, 0.25f}}) {
                const i64 j = i64(i) - late - after;
                echo += j >= 0 ? gain * friends[std::size_t(j)] : 0.0f;
            }
            hiss = hiss * 1664525u + 1013904223u;
            mic[i] = coupling * echo + player[i] + 0.001f * (f32(hiss >> 8) / f32(1u << 23) - 1.0f);
        }
        double now = 0.0;
        auto tap = std::make_shared<AudioOutputTap>(voice_rate, 2, 1.0f);
        VoiceInput input(make_memory_audio_capture(mic, voice_rate, [&] { return now; }),
                         {.mode = VoiceMode::Open, .echo_gate = gate, .denoise = false});
        input.set_speakers(tap);
        // The device takes what it plays in bursts of 1024 frames, 40 ms ahead.
        i64 written = 0;
        std::vector<f32> burst(2 * 1024);
        std::vector<VoiceFrame> out;
        Run r;
        i64 heard_to = 0;
        for (int step = 0; step < int(seconds * 60.0f) - 2; ++step) {
            now += 1.0 / 60.0;
            for (const i64 want = i64(now * voice_rate) + ahead; written < want; written += 1024) {
                for (std::size_t k = 0; k < 1024; ++k) {
                    const std::size_t i = std::size_t(written) + k;
                    burst[2 * k] = burst[2 * k + 1] = i < friends.size() ? friends[i] : 0.0f;
                }
                tap->write(burst, written);
            }
            out.clear();
            input.update(false, out);
            r.sent.push_back(int(out.size()));
            r.level.push_back(input.voice_level());
            // The player's frames that came in this step, loud enough for the gate.
            const i64 to = i64(now * voice_rate) / voice_frame;
            int talk = 0;
            for (; heard_to < to; ++heard_to)
                talk += rms_of(std::span<const f32>(player).subspan(std::size_t(heard_to * voice_frame), voice_frame)) >
                                0.008f
                            ? 1
                            : 0;
            r.talking.push_back(talk);
            r.missed.push_back(std::max(0, talk - int(out.size())));
        }
        r.delay_ms = input.echo().delay_ms();
        r.coupling_db = input.echo().coupling_db();
        return r;
    };
    const auto count = [](const std::vector<int>& v, f32 from, f32 to) {
        int n = 0;
        for (int s = int(from * 60.0f); s < int(to * 60.0f) && s < int(v.size()); ++s)
            n += v[std::size_t(s)];
        return n;
    };
    const auto heard_talk = [](const Run& r, f32 from, f32 to) {
        int n = 0;
        for (int s = int(from * 60.0f); s < int(to * 60.0f); ++s)
            n += r.level[std::size_t(s)] > 0 ? 1 : 0;
        return n;
    };
    const Run speakers = run(0.33f, true), headphones = run(0.0f, true), ungated = run(0.33f, false);
    const int echo_sent = count(speakers.sent, 2.0f, 8.0f), echo_ungated = count(ungated.sent, 2.0f, 8.0f);
    const int alone = count(speakers.talking, 8.5f, 12.0f), alone_sent = alone - count(speakers.missed, 8.5f, 12.0f);
    const int over = count(speakers.talking, 12.5f, 15.9f), over_sent = over - count(speakers.missed, 12.5f, 15.9f);
    const int over_headphones = over - count(headphones.missed, 12.5f, 15.9f);
    std::printf("  echo: found %d ms late, %.1f dB down; of 300 frames of a friend coming back, %d sent (%d without "
                "the gate); the player alone: %d of %d frames of talk sent; over a friend: %d of %d (%d with "
                "headphones)\n",
                speakers.delay_ms, -speakers.coupling_db, echo_sent, echo_ungated, alone_sent, alone, over_sent, over,
                over_headphones);
    assert(speakers.delay_ms >= 35 && speakers.delay_ms <= 110);
    assert(speakers.coupling_db < -3.0f && speakers.coupling_db > -25.0f);
    assert(echo_ungated > 150);                                       // the echo would go out
    assert(echo_sent <= 6 && heard_talk(speakers, 2.0f, 8.0f) <= 10); // it does not, nor is it counted as talk
    assert(alone_sent >= alone * 95 / 100);                           // the player alone, as before
    assert(count(headphones.sent, 0.0f, 8.0f) == 0 && over_headphones >= over * 95 / 100);
    assert(headphones.coupling_db < -40.0f);
}

// The noise (RNNoise): a fan and a keyboard, loud enough to open the gate without it, open it no more, nor are they
// counted as talk; something like talk over the fan still opens it. The fan is taken out between the words
// (in a pause) and the words are left as they were.
void test_noise_suppression() {
    constexpr std::size_t n = std::size_t(voice_rate) * 4;
    u32 seed = 3u;
    const auto white = [&] {
        seed = seed * 1664525u + 1013904223u;
        return f32(seed >> 8) / f32(1u << 23) - 1.0f;
    };
    std::vector<f32> fan(n), keys(n, 0.0f);
    for (f32& s : fan)
        s = 0.05f * white(); // -31 dB
    for (std::size_t at = 1000; at + 400 < n; at += 5000 + (seed % 1500)) {
        for (std::size_t k = 0; k < 400; ++k) // a key: a few milliseconds of noise, dying away
            keys[at + k] = 0.3f * white() * std::exp(-f32(k) / 60.0f);
    }
    const std::vector<f32> words = babble(4.0f, 5, 0.3f);
    std::vector<f32> talk(n);
    for (std::size_t i = 0; i < n; ++i)
        talk[i] = words[i] + 0.2f * fan[i];
    struct Heard {
        int sent = 0;
        bool talked = false; // voice_level() said talk
    };
    const auto listen = [&](const std::vector<f32>& samples, bool denoise) {
        double now = 0.0;
        VoiceInput mic(make_memory_audio_capture(samples, voice_rate, [&] { return now; }),
                       {.mode = VoiceMode::Open, .denoise = denoise});
        std::vector<VoiceFrame> out;
        Heard h;
        for (int f = 0; f < 195; ++f) {
            now += 0.02;
            out.clear();
            mic.update(false, out);
            h.sent += int(out.size());
            h.talked = h.talked || mic.voice_level() > 0;
        }
        return h;
    };
    const Heard fan_raw = listen(fan, false), fan_clean = listen(fan, true);
    const Heard keys_raw = listen(keys, false), keys_clean = listen(keys, true);
    const Heard talk_raw = listen(talk, false), talk_clean = listen(talk, true);
    // What it leaves: the fan between the words, the words.
    NoiseSuppressor ns;
    std::vector<f32> cleaned = talk;
    for (std::size_t at = 0; at + mic_block <= n; at += mic_block)
        ns.clean(std::span<f32, mic_block>(cleaned.data() + at, mic_block));
    f64 gap_in = 0.0, gap_out = 0.0, word_in = 0.0, word_out = 0.0;
    std::size_t gaps = 0, word = 0;
    for (std::size_t at = std::size_t(voice_rate); at + 2 * mic_block <= n; at += mic_block) {
        // Each block against the one RNNoise gives back for it (a block later).
        const std::span<const f32> said(words.data() + at, mic_block);
        const f32 in = rms_of(std::span<const f32>(talk.data() + at, mic_block)),
                  out = rms_of(std::span<const f32>(cleaned.data() + at + mic_block, mic_block));
        // Between words: none within 50 ms either way (RNNoise lets the noise back in, and out, over a few blocks).
        const std::span<const f32> around(words.data() + at - 5 * mic_block, 11 * mic_block);
        if (rms_of(around) < 1e-6f) {
            gap_in += in * in, gap_out += out * out, ++gaps;
        } else if (rms_of(said) > 0.05f) {
            word_in += in * in, word_out += out * out, ++word;
        }
    }
    const f32 fan_down = 10.0f * f32(std::log10(gap_in / std::max(gap_out, 1e-12))),
              words_down = 10.0f * f32(std::log10(word_in / std::max(word_out, 1e-12)));
    std::printf("  noise: a fan opens the gate for %d frames without RNNoise, %d with; keys %d, %d; talk over the fan "
                "%d, %d; between the words the fan %.0f dB down, the words %.1f dB\n",
                fan_raw.sent, fan_clean.sent, keys_raw.sent, keys_clean.sent, talk_raw.sent, talk_clean.sent, fan_down,
                words_down);
    assert(fan_raw.sent > 150 && fan_raw.talked && fan_clean.sent <= 3 && !fan_clean.talked);
    assert(keys_raw.sent > 100 && keys_raw.talked && keys_clean.sent <= 10 && !keys_clean.talked);
    assert(talk_clean.sent >= talk_raw.sent * 7 / 10 && talk_clean.talked);
    assert(gaps > 20 && word > 20 && fan_down > 20.0f && std::fabs(words_down) < 3.0f);
}

// Voices over the platform's messages: one player's frames reach the other through a lossy network, on their own
// channel (a match's messages on another are left alone), and play there; a packet claiming to be another slot's
// voice is dropped, and so is a muted player's.
void test_voices_over_services() {
    FakeNetwork network;
    network.drop_every = 20;
    FakeServices ann(network, {.id = "fake:ann", .name = "Ann"});
    FakeServices bob(network, {.id = "fake:bob", .name = "Bob"});
    FakeServices cyd(network, {.id = "fake:cyd", .name = "Cyd"});
    ann.create_lobby({.max_members = 4});
    bob.join_lobby(ann.lobby());
    cyd.join_lobby(ann.lobby());
    const std::vector<std::string> players = {"fake:ann", "fake:bob", "fake:cyd"};
    ServicesVoiceLink link_a(ann, players), link_b(bob, players);
    VoiceChat chat_a(0, 3), chat_b(1, 3);
    double now = 0.0;
    std::vector<f32> talk(voice_rate * 3);
    for (std::size_t i = 0; i < talk.size(); ++i)
        talk[i] = 0.2f * std::sin(2.0f * pi * 300.0f * f32(i) / f32(voice_rate));
    chat_a.set_input(std::make_unique<VoiceInput>(make_memory_audio_capture(talk, voice_rate, [&] { return now; }),
                                                  VoiceInputConfig{.denoise = false}));
    chat_a.set_link(&link_a);
    chat_b.set_link(&link_b);
    const std::vector<u8> match = {'k', 'n', 1, 40};
    assert(ann.send_message("fake:bob", match, MessageDelivery::Reliable, 1));
    std::array<u8, voice_packet_bytes> forged{};
    const std::size_t forged_size = write_voice_packet(0, VoiceFrame{.seq = 9, .size = 1}, forged); // Cyd as Ann
    assert(cyd.send_message("fake:bob", std::span<const u8>(forged.data(), forged_size), MessageDelivery::Unreliable,
                            voice_channel));
    std::vector<f32> heard, chunk(voice_out_frame);
    for (int step = 0; step < 100; ++step) {
        now += 0.02;
        chat_a.update(false);
        chat_b.update(false);
        chat_b.stream(0)->pull(chunk);
        heard.insert(heard.end(), chunk.begin(), chunk.end());
    }
    std::vector<PlayerMessage> other;
    bob.receive_messages(1, other);
    assert(other.size() == 1 && other[0].bytes == match);
    // Ann's frames went to Bob and Cyd (2 a frame); one in twenty was lost on the way.
    assert(chat_a.sent() >= 190 && chat_b.received() >= 80 && chat_b.received() <= chat_a.sent() / 2);
    assert(chat_a.voice_level() == 2);
    const f32 level = rms_of(std::span<const f32>(heard).subspan(heard.size() / 2));
    const VoiceStreamStats st = chat_b.stream(0)->stats();
    assert(level > 0.11f && level < 0.16f && st.played >= 80 && st.recovered > 0);
    assert(chat_b.stream(2)->stats().played == 0); // the forged packet named slot 0 from Cyd: dropped
    // Muted: nothing more of Ann is taken.
    const u64 before = chat_b.received();
    chat_b.set_muted(1u << 0);
    for (int step = 0; step < 10; ++step) {
        now += 0.02;
        chat_a.update(false);
        chat_b.update(false);
    }
    assert(chat_b.received() == before);
    std::printf("  voices over messages: %llu packets sent, %llu came, %llu made again from the next; heard at %.3f "
                "RMS\n",
                static_cast<unsigned long long>(chat_a.sent()), static_cast<unsigned long long>(chat_b.received()),
                static_cast<unsigned long long>(st.recovered), level);
}

std::vector<f32> square(std::size_t frames, f32 amplitude) {
    std::vector<f32> out(frames);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = i % 2 ? -amplitude : amplitude;
    return out;
}

// The engine's output tap: what it mixes, as mixed, numbered from its first frame; what is not read in time is lost,
// oldest first, and counted.
void test_output_tap() {
    AudioEngine engine(create_null_audio_backend());
    engine.add_clip("tone", make_memory_audio_clip("tone", square(16384, 0.05f), 1, 48000));
    AudioCatalog catalog;
    catalog.add_cue({.id = "tone", .clips = {"tone"}});
    auto tap = engine.open_output_tap(0.1f); // 4800 frames
    assert(tap->sample_rate() == engine.sample_rate() && tap->channels() == engine.channels());
    assert(engine.play(catalog, {.cue = "tone"}));
    std::vector<f32> mixed(1024), read(1024);
    engine.render(mixed);
    i64 first = -1;
    assert(tap->available() == 512 && tap->read(read, &first) == 512 && first == 0 && read == mixed);
    for (int i = 0; i < 12; ++i) {
        engine.render(mixed); // 6144 frames, more than it holds
    }
    assert(tap->available() == 4800 && tap->lost() == 6144 - 4800 && tap->next_frame() == 512 + 6144 - 4800);
    engine.close_output_tap();
    engine.render(mixed);
    assert(tap->available() == 4800); // closed: nothing more
}

// A voice played through the engine, as a clip that never ends: what the stream has, then silence while it waits.
void test_voice_clip_plays() {
    AudioEngine engine(create_null_audio_backend());
    auto stream = std::make_shared<VoiceStream>();
    add_voice_clip(engine, "voice1", stream);
    AudioCatalog catalog;
    catalog.add_cue(voice_cue("voice1"));
    engine.set_listener_3d({});
    const AudioHandle handle =
        engine.play(catalog, {.cue = "voice1", .position_3d = {0.0f, 0.0f, -2.0f}, .has_position_3d = true});
    assert(handle);
    VoiceEncoder encoder;
    for (u16 k = 0; k < 10; ++k) {
        std::array<f32, voice_frame> samples;
        for (int i = 0; i < voice_frame; ++i)
            samples[std::size_t(i)] = 0.2f * std::sin(2.0f * pi * 300.0f * f32(k * voice_frame + i) / f32(voice_rate));
        VoiceFrame f{.seq = k, .level = 2, .last = k == 9};
        f.size = u16(encoder.encode(samples, f.data));
        stream->push(f);
    }
    std::vector<f32> out(2 * 4800); // 100 ms
    f32 loudest = 0.0f;
    for (int i = 0; i < 3; ++i) {
        engine.render(out);
        loudest = std::max(loudest, rms_of(out));
    }
    assert(loudest > 0.05f && engine.playing(handle));
    for (int i = 0; i < 4; ++i) {
        engine.render(out);
    }
    assert(rms_of(out) < 1e-4f && engine.playing(handle)); // all said: silence, and it plays on for what comes next
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_voice_codec();
    test_voice_stream();
    test_voice_gate();
    test_echo_gate();
    test_noise_suppression();
    test_voices_over_services();
    test_output_tap();
    test_voice_clip_plays();
    std::printf("voice tests passed\n");
    return 0;
}
