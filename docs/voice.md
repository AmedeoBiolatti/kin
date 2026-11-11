# Voice chat

`kin/voice/voice.hpp` lets players talk to each other while they play: each player's microphone is cleaned, gated,
coded with Opus and sent to every other player, and the others' voices play where their characters stand. It is
built with the VOICE component (`-DKIN_ENABLE_VOICE=ON`, off by default; it fetches Opus and RNNoise), and a game
links `kin::voice` beside `kin::engine`.

```cpp
#include <kin/voice/voice.hpp>

// Once a match starts (kin/net/lobby_match.hpp): who plays which slot.
kin::VoiceChat chat(match.local_player, static_cast<kin::u32>(match.players.size()));
std::string error;
if (auto mic = kin::open_audio_capture({.sample_rate = kin::voice_rate}, &error)) {
    auto input = std::make_unique<kin::VoiceInput>(std::move(mic), kin::VoiceInputConfig{.mode = kin::VoiceMode::Open});
    input->set_speakers(audio.open_output_tap()); // so the speakers playing the others back are not sent
    chat.set_input(std::move(input));
}
kin::ServicesVoiceLink link(*services, match.players); // over Steam's relays, on its own channel
chat.set_link(&link);
for (kin::u32 slot = 0; slot < match.players.size(); ++slot) {
    if (slot != match.local_player) {
        const std::string id = "voice" + std::to_string(slot);
        kin::add_voice_clip(audio, id, chat.stream(slot));
        catalog.add_cue(kin::voice_cue(id));
        handles[slot] = audio.play(catalog, {.cue = id, .position_3d = mouth(slot), .has_position_3d = true});
    }
}

// Each frame:
chat.update(push_to_talk_held);
for (each other slot) audio.set_position_3d(handles[slot], mouth(slot));
```

## The microphone

`open_audio_capture()` (`kin/audio/audio_capture.hpp`, in the AUDIO component) records mono samples at the rate asked
for; `list_audio_input_devices()` names the devices for a settings menu. It returns nothing, with a reason, when there
is no microphone or the system refused access to it: a game then plays on without sending its player's voice.
`make_memory_audio_capture()` plays samples from memory as if recorded, for tests and for trying the chat without a
microphone.

`VoiceInput` turns what the microphone hears into frames to send:

| `VoiceInputConfig` | Default | |
|---|---|---|
| `mode` | `Open` | `Open` sends while the player talks; `PushToTalk` while `update(true)`; `Off` records nothing and lets the device go |
| `gate_db` | -42 | how loud talk must be to open the gate, in dB below full scale |
| `gain` | 1 | the microphone's volume |
| `denoise` | true | RNNoise takes the room's noise out (a fan, traffic, a keyboard, breathing) and opens the gate for speech only |
| `echo_gate` | true | with `set_speakers()`: what the speakers play back into the microphone is not sent |

`meter()` and `gate_meter()` draw a level meter with the gate on it; `speech()` says how likely the last frame was
speech.

**How loud the player talks.** `voice_level()` is 0 (silent), 1 (a murmur), 2 (talk) or 3 (a shout), held a moment
so it does not flicker. A game can put it in its commands: then it is part of the deterministic simulation, and every
peer's monster hears the same talk. The voices themselves are not simulated.

**Speakers and headphones.** Through speakers, the microphone hears the game and the friends' voices again; sent on,
the friends would hear themselves a moment later. The echo gate compares the microphone with what the engine played
(`AudioEngine::open_output_tap()`): it finds how late the speakers come back into the microphone and how loud they
are there, and lets a block through only when the player is clearly louder than that. It does not take the echo out
of the sound: while the speakers are louder than the player, the player is not heard (a speakerphone's half duplex).
With headphones the gate never closes.

## Over the network

Each talking player sends a 20 ms frame to every other player: about 50 bytes, 20 kbit/s while they talk and nothing
while they do not. Each frame carries a coarse copy of the one before it, so a lost frame is made again from the next,
and Opus covers longer losses.

- **`ServicesVoiceLink`** sends the frames as the platform's messages (`PlatformServices::send_message`, unreliable),
  on channel 2 (`voice_channel`): apart from a game's own messages (0) and a `ServicesTransport` match (1). On Steam
  they go peer to peer or through Valve's relays, as the match does. `set_players()` follows the match as players
  join and leave.
- **A game's own transport** implements `VoiceLink`: `send(slot, bytes)` and `receive()`, which says which slot each
  packet came from. A packet that says it is another slot's voice is dropped: nobody speaks for another.

`VoiceStream` is one speaker's voice on the receiving side: a jitter buffer of about 60 ms that puts frames back in
order, makes lost ones again, and skips a frame now and then when the speaker's clock runs ahead. `stats()` counts
frames played, recovered, concealed, late and dropped; `level()` says how loud it plays now (to light up who is
talking).

`VoiceChat::set_muted()` drops the packets of players nobody wants to hear; `set_echo(slot)` plays a player's own
voice back as that slot, to try a microphone alone.

## Playing voices

`add_voice_clip()` makes a stream a clip that never ends: what is said as it arrives, silence between.
`voice_cue()` makes a spatial cue for it (full volume within a metre, silent past 25 by default). Play it at the
speaker's mouth and move it with them, like any positioned sound: it fades with distance, pans, and goes through the
game's buses and effects.

## Cost

RNNoise takes a few percent of a core for each microphone; Opus less. Each talking player sends 20 kbit/s to every
other player: with 6 players all talking at once, about 100 kbit/s up for each.

## Tests

`kin-voice` (with VOICE) codes speech-like sound and checks its partials come out at their strength; plays a stream
with frames out of order, lost and late; opens and closes the gate on talk, a breath and a shout, in both modes;
plays a friend's voice back through speakers into the microphone and checks the gate finds its delay and keeps it
out while the player alone is still sent; checks RNNoise keeps a fan and a keyboard from opening the gate; sends
voices over a lossy fake network; and plays one through the engine.

## Not yet

- No echo cancellation: through speakers the chat is half duplex.
- Each talking player sends to every other (a mesh): fine for a few friends, not for a crowd.
- No Lua or Python bindings.
