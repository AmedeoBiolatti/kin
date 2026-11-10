# Platform Services

`kin/platform/services.hpp` gives a game what a store or platform offers, through
one interface whichever runs it. The game asks for the player and their language,
achievements and stats, leaderboards, rich presence, the overlay, the on-screen
keyboard, DLC, and invitations to join a friend. Steam is the first platform. A
game written against `kin::PlatformServices` does not change for another.

Where no platform answers, the **offline** backend does. That happens with no
Steam app id, Steam not running, no Steam library beside the game, a headless or
server run, or a browser build. The offline backend:

- keeps achievements, stats and the player's best scores in their save folder;
- returns `false` from what only a platform can do (the overlay, friends,
  presence).

A game never needs to ask which backend it has.

## Using it

The simplest way: give the game's info a Steam app id, and `run_scene_app` does
the rest.

```
# project.kin
id lantern_vale
title Lantern Vale
steam_app_id 480        # Valve's test app (Spacewar) until the game has its own
```

`run_scene_app` then:

1. starts the services: Steam when it answers, otherwise the offline backend;
2. quits at once (exit code 0) if Steam starts the game again itself;
3. calls `update()` once a frame;
4. shows the language the player chose for the game in Steam, if the game has
   translations for it;
5. hands the services to scenes (`SceneContext::services`) and scripts (the
   `services` table).

On a handheld (Steam Deck), it also shows Steam's floating keyboard while a text
field takes text (`Window::set_text_input_enabled`, over
`set_text_input_area`).

`SceneAppConfig` changes that:

| Member | Default | Does |
|---|---|---|
| `services_config` | from the game info | What to start (`ServicesConfig`) |
| `services` | none | Services the game started itself, used as they are |
| `platform_language` | `true` | Use the platform's language. Turn it off when the game restores the player's own saved choice. |
| `platform_keyboard` | `true` | Show the on-screen keyboard on handhelds |

Headless, server and determinism runs always get the offline backend, in memory,
so they don't depend on the machine they run on.

`--services=offline` gives a run the offline backend too, whatever the game info
says: an editor's run that must not sign in to Steam as whoever runs it, or a
build tried without its store. It keeps what it keeps beside the saves, as a
player's copy without Steam does. `--services=memory` keeps nothing.
`kin::scene_app_services_config(config)` says what `run_scene_app` starts.

Without `run_scene_app`:

```cpp
kin::ServicesStart start{};
auto services = kin::start_platform_services({.game_id = "lantern_vale", .steam_app_id = 480,
                                              .args = {argv, argv + argc}}, &start);
if (start == kin::ServicesStart::Restarting) return 0;
kin::set_active_services(services.get()); // for scripts
// each frame
services->update();
for (const kin::ServiceEvent& event : services->events()) { /* ... */ }
```

## What a game can ask

| | |
|---|---|
| `player()` | `{id, name}`. The id is stable for the player (`steam:7656…`, `offline`). |
| `language()` | The game's language chosen on the platform, as a BCP 47 tag (`fr`, `pt-BR`, `zh-Hans`). `""` when unknown. |
| `handheld()`, `big_picture()` | Steam Deck; Big Picture (TV) mode. Show controller hints and bigger text. |
| `cloud_saves()` | Steam Cloud is on for the player and the game. |
| `unlock(id)`, `clear(id)`, `unlocked(id)` | Achievements, by the API name set on the platform. |
| `show_progress(id, 3, 10)` | Shows the player "3 of 10" for an achievement. |
| `set_stat`, `add_stat`, `stat_int`, `stat_float` | Stats, whole-number or decimal. |
| `upload_score(board, score, details, keep, sort, display)` | Leaderboards. The board is made on first use. Returns a request number. |
| `request_scores(board, Top / AroundPlayer / Friends, first, last)` | The answer comes as a `LeaderboardScores` event. |
| `set_presence(key, value)`, `clear_presence()` | What friends see. Steam reads `status`, `steam_display` and `connect`. |
| `set_join_command(cmd)` | What a friend who chooses "Join game" gets in their `JoinRequested` event. |
| `overlay_active()`, `open_overlay(page)`, `open_url`, `open_store([dlc])`, `invite_friends(lobby)` | The overlay. |
| `show_keyboard(mode, field)`, `hide_keyboard()` | The floating keyboard; its keys arrive as typing. |
| `ask_text(prompt, initial, max, password)` | A full-screen text prompt; the answer comes as a `TextEntered` or `TextCancelled` event. |
| `owns(dlc)` | DLC, by its Steam app id. |
| `join_on_launch()` | What the game was started to join: `lobby:<id>` from `+connect_lobby`, or a connect string. |
| `flush()` | Keep stats and achievements now. `update()` keeps them anyway, at most every two seconds. |

Each `update()` replaces `events()` with what happened since the one before:

- `OverlayShown` / `OverlayHidden`: pause the game while the overlay is up;
- `JoinRequested`;
- `AchievementUnlocked`;
- `StatsStored` / `StatsFailed`;
- `LeaderboardUploaded` / `LeaderboardScores` / `LeaderboardFailed`;
- `TextEntered` / `TextCancelled`;
- `KeyboardClosed`;
- `DlcInstalled`;
- `Disconnected` / `Reconnected`.

`run_scene_app` calls `update()` on a frame's first fixed step, so every step of
the frame and its drawing see the same events.

Scripts get the same functions through the `services` table:

```lua
services.unlock("FIRST_WIN")
services.add_stat("lanterns_lit", 1)
services.presence("status", "Exploring the vale")
for _, e in ipairs(services.events()) do
    if e.kind == "join_requested" then join(e.text) end
end
```

Without services, every call returns `false` or `nil`. What the platform says
(the overlay, events) is not part of a game's deterministic state. Keep it out of
rollback and lockstep simulation.

## The game's list

A game can list its achievements, stats and leaderboards in
`content/services.kinservices` (`kin/platform/services_catalog.hpp`). Each has
an id of the game's own, and the id each store gave it where that differs.
Steam's API names can be the game's ids; other stores make ids of their own.
The game asks by its ids, whichever store runs it.

```json
{
  "schema": "kin.services/1",
  "achievements": [
    {"id": "ALL_COINS", "name": "achievement.all_coins", "description": "achievement.all_coins.how",
     "progress": 5, "stores": {"steam": "ACH_ALL_COINS"}},
    {"id": "SECRET_ROOM", "hidden": true}
  ],
  "stats": [{"id": "coins", "type": "int"}, {"id": "best_time", "type": "float"}],
  "leaderboards": [{"id": "speedrun", "sort": "ascending", "display": "milliseconds"}]
}
```

| Field | Of | Does |
|---|---|---|
| `id` | all | The game's id: 1 to 128 characters, once in its kind |
| `name`, `description` | all (`description`: achievements) | Translation keys, for the game's own lists and for tools. The store shows what is set up on it. |
| `hidden` | achievements | Shown only once unlocked |
| `progress` | achievements | The most `show_progress` counts to, so the game can leave it out |
| `type` | stats | `int` (default) or `float` |
| `sort`, `display` | leaderboards | `descending` (default: higher is better) or `ascending`; `number` (default), `seconds` or `milliseconds` |
| `stores` | all | A store's name (`steam`, as `backend()` says it) to its id there |

Give `run_scene_app` the catalog (`SceneAppConfig::services_catalog`, read with
`load_services_catalog`), and the game's services are asked by it
(`catalog_services`):

- each id becomes the store's, and events name the game's ids again;
- an id the list doesn't have is refused (`false`, `0`, none) and said once in
  the log, where the list has that kind. A kind it lists none of goes as it is;
- a stat is set as its type says (`set_stat("best_time", 41)` sets 41.0);
- a leaderboard is made with its sort and display, whatever `upload_score` is
  given;
- `show_progress(id, 3)` (Lua: `services.progress(id, 3)`) counts to the
  listed `progress`.

The offline backend keeps everything by the game's ids.

## Steam

kin opens Steam's API library at run time: `steam_api64.dll` on Windows,
`libsteam_api.so` on Linux, `libsteam_api.dylib` on macOS. So:

- kin builds with no Steamworks SDK;
- one executable runs with or without Steam;
- CI builds and tests the Steam backend.

kin declares the parts of Steam's flat C API it uses itself
(`engine/src/platform/steam_flat.hpp`). Valve doesn't allow the SDK in a public
repository.

**Shipping the library.** The library comes from the
[Steamworks SDK](https://partner.steamgames.com/downloads/steamworks_sdk.zip)
(`redistributable_bin/`). Valve lets games ship it beside their executable.
`kin_game` copies it there and into the package:

```cmake
kin_game(lantern_vale INFO content/project.kin
    STEAM_REDIST "$ENV{STEAMWORKS_SDK}/redistributable_bin")   # or a folder holding the library
```

`STEAM_APP_ID` sets the app id when the game info doesn't. Outside shipping
builds, `kin_game` also writes `steam_appid.txt` beside the executable. With it,
Steam lets the game run from the build folder, and Steam must be running and
signed in. Steam reads that file only from the current folder, so kin passes it on as
`SteamAppId`, and the game starts from anywhere. Shipping builds have no
`steam_appid.txt`. Started outside Steam, they
ask Steam to start them (`SteamAPI_RestartAppIfNecessary`), and `run_scene_app`
quits. `ServicesConfig::steam_restart_through_client = false` turns that off.

The package folder is then the Steam depot: the executable, its pack, the Steam
library and `licenses/` ([shipping](shipping.md)). Build Linux depots in the Steam
Runtime (sniper), as [shipping](shipping.md) says.

**A store for each build.** A game sold on Steam and elsewhere (itch.io, its own
site) is built once for each, in a build folder of its own, chosen with the
cache variable `KIN_GAME_STORE`:

| `KIN_GAME_STORE` | The build |
|---|---|
| `auto` (default) | For Steam when the game has a Steam app id |
| `steam` | For Steam; configuring fails without an app id |
| `none` | For no store: no app id in it, no `steam_appid.txt`, no Steam library |

A build for no store never tries Steam, even on a machine where Steam's
library can be found: one carrying the app id would ask Steam to start it, and
quit. The game's code gets `KIN_GAME_STORE`, `"steam"` or `""`. Browser builds
are for no store. `KIN_STEAM_REDIST` (a cache variable, the SDK's
`redistributable_bin`) is `STEAM_REDIST` for each Steam build of the folder
that doesn't give one, so a tool can ship the library without changing the
game's `CMakeLists.txt`:

```sh
cmake -B build-steam -DKIN_SHIPPING=ON -DKIN_GAME_STORE=steam -DKIN_STEAM_REDIST=$STEAMWORKS_SDK/redistributable_bin
cmake -B build-itch  -DKIN_SHIPPING=ON -DKIN_GAME_STORE=none
```

A build without an app id takes out a `steam_appid.txt` an earlier build left
beside the executable, and shipping builds never have one.

**Set up on the partner site:**

- **Achievements and stats.** Their API names are what the game passes to
  `unlock` and `set_stat`. Stat types matter: `set_stat(id, 3)` sets an INT stat,
  `set_stat(id, 3.0f)` a FLOAT one.
- **Leaderboards.** Optional: `upload_score` makes a missing board with the sort
  and display it is given.
- **Rich presence localization.** Only for `steam_display` tokens.
- **Steam Cloud with Auto-Cloud.** Its root is the save folder:
  - Linux: `~/.local/share/kin/<game id>` (`XDG_DATA_HOME`);
  - Windows: `%APPDATA%\kin\<game id>`.

  It holds `SaveStore`'s slots and the offline backend's `services/` file. No
  code is needed.

**Steam Deck:**

- `handheld()` says the game is on a Deck, and the floating keyboard follows text
  fields there.
- Controllers already work through SDL. Steam Input's virtual controllers look
  like an Xbox pad.
- For the Deck Verified review, the game also needs pad-driven UI focus, glyphs
  that match the pad, and legible text at 1280×800.

**Which SDKs.** kin's declarations are checked against SDK 1.65 (see
[Testing](#testing)); ship that SDK's library or a newer one. The backend uses
`SteamAPI_InitFlat` (older libraries: `SteamAPI_InitSafe`) and manual callback
dispatch. A library exports only its own SDK's interface versions. kin looks for:

- SteamUser v023–v021;
- SteamFriends v018–v017;
- SteamUtils v011–v010;
- SteamApps v009–v008;
- SteamUserStats v013–v012;
- SteamRemoteStorage v016.

Older versions are kept on a best-effort basis. On Steam Deck and similar
handhelds, `handheld()` asks `GetSteamHardwareDefaultConfig`. A library missing
a function kin needs is reported in the log, and the game plays offline.

## Testing

- **Offline in memory.** `ServicesConfig::offline_in_memory` gives the offline
  backend with nothing written. Headless runs get this.
- **`kin::FakeServices`.** Does what a test sets up (`achievements`, `dlc`,
  `handheld_`, `language_`) and records every call in `calls`. `send(event)` and
  `set_overlay(true)` deliver events on the next `update()`.
- **The Steam backend itself.** `tests/fake_steam_api.cpp` builds a stand-in
  library with the same file name and functions. `kin-platform-services` loads it
  the way it loads Steam's, then drives callbacks, call results, leaderboards,
  the floating keyboard, relaunch and a Steam that is not running.
- **Against Valve's SDK.** Configure with
  `-DKIN_STEAMWORKS_SDK_DIR=<sdk>` (the folder holding `public/steam`), then
  build `kin_steam_abi_check`. It fails to compile wherever kin's declarations
  differ from the SDK's: function signatures, callback ids, enum values, struct
  layouts. Do this when moving to a new SDK.
- **On Steam.** With the SDK given, `kin-platform-services-real-steam` loads
  Valve's library and starts it as Spacewar (480). Every function and interface
  kin needs must be in it. With Steam running and signed in
  (`SteamAppId=480`), it also reads Spacewar's achievements, stats and
  leaderboard; it changes none of them. Steam installed as a snap keeps its pipe
  and `steamclient.so` under `~/snap/steam/common`, so run games outside Steam
  with `HOME=~/snap/steam/common`, or link `~/.steam/sdk64` there. A game Steam
  starts itself doesn't need this.
- Checked on 2026-11-04 against SDK 1.65 and the Linux Steam client.

## Not yet

- **Multiplayer over Steam.** A `kin::net` transport on Steam's networking
  sockets, and lobbies. That waits for `kin::net`.
- **Steam Input action sets.** Controllers work through SDL.
- **Workshop and the Cloud file API.** Auto-Cloud covers saves.
- **Inventory, Timeline, screenshots.**
