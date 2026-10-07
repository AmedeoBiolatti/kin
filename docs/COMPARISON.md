# Kin Compared With Godot And Bevy (2D)

What kin is missing next to Godot and Bevy, leaving 3D out. Kin's column is
taken from its headers and docs at v0.2.5; Godot (4.5) and Bevy (about 0.17)
are described as of mid-2026, so their newest releases may differ.

## Gaps

| Area | Godot | Bevy | Kin |
|---|---|---|---|
| **Gamepad and touch** | Gamepads, rumble, touch, UI focus moved by a controller | Gamepads, rumble, touch | **Missing.** Bindings are keyboard and mouse only, and kin never opens a gamepad or reads touch. The manifest names Steam Deck as a target. |
| **Localization** | Translations from CSV or gettext, plurals, pseudo-localization, right-to-left text, font fallback | Not built in (third-party crates) | **Strong core:** text by key from JSON or CSV files that hot-reload, a fallback chain, ICU-style plurals and selects, pseudo-locale, missing-text reports and `--fail-on-missing-text`, fallback fonts, bidi and HarfBuzz-shaped Arabic and Indic text, CJK line breaking, mirrored layouts. IME input, right-to-left editing and mirrored widgets, ellipsis and shrink-to-fit with overflow reports, fonts per language. Ordinals, gettext catalogues, assets by language. **Missing:** date, currency and list formatting; translator tooling (key extraction). |
| **Audio formats and effects** | Ogg, MP3 and WAV; streamed music; effects per bus (reverb, EQ, compressor, filters); adaptive music | Ogg, MP3, FLAC and WAV; no effects | **WAV only** (`SDL_LoadWAV`), no streaming, no effects. Buses, cue priority, positional sound and pitch variation are there. |
| **Platforms and shipping** | Windows, Linux, macOS, Android, iOS, web; assets packed into one file, optionally encrypted | Same platforms, web through WebAssembly | **Windows and Linux only.** No macOS in CI, no web or mobile. Games ship as a folder: the executable, one `.kinpak` content pack (uncompressed, not encrypted) and license texts, archived by a `<game>_package` target; Windows builds are windowed apps on the static runtime. No installers or code signing. |
| **Editor** | Full editor: scenes, inspector, tilemap painting, animation timeline, debugger | No official editor yet; inspector crates and a remote protocol | **No visual editor.** There is a play/edit session API and live world inspection through flecs. That fits kin's code-and-agent-first design, but there is no level or tilemap painting. |
| **Physics** | Many shapes, joints, a character controller (`move_and_slide`), one-way platforms, gravity areas | None built in (the Avian and Rapier crates are rich) | **Box2D, with only boxes and circles exposed.** No polygon, capsule or chain shapes, no joints, no shape casts, no character controller. Raycasts, point and box queries, sensors and collision filtering are there. |
| **Navigation** | Navigation meshes, agents that avoid each other, obstacles | Not built in | **Grids only:** A* on square and hex grids through a path server. No navigation mesh or avoidance. |
| **2D lighting** | Shadows from occluders, normal maps, directional lights | No 2D lighting built in | **Point and shaped lights**, with no shadows or normal maps. |
| **Animation** | Animation player and blend tree, 2D skeletons and bones, IK, mesh deformation, tweens | Animation graph that drives any field; no built-in tweens or 2D skeletons | **Strong core:** tracks, state machines, layers, blending, events. **Missing:** 2D skeletons and mesh deformation, and a one-line tween API. |
| **Particles** | GPU and CPU particles, with an editor | Not built in (the Hanabi crate) | Particles run on the CPU only, which is fine at kin's scale. |
| **Networking** | High-level multiplayer (remote calls, replication), ENet, WebSocket, WebRTC, HTTP requests | Not built in (the Lightyear and Replicon crates) | **Missing.** The HTTP server is only for agents. |
| **Asset pipeline** | Import settings per asset, a cache of processed files, compressed textures | Asset preprocessing with `.meta` files; assets freed when no longer used | **Missing:** no import pipeline, assets are never freed automatically, and the hot-reload `Modified` event is reserved but never sent ([assets](assets.md#scope-v1)). No importers for Aseprite, Tiled or LDtk. |
| **Video playback** | Yes (Theora) | No | No |
| **Accessibility** | Screen reader support (AccessKit, experimental) | AccessKit built in | Missing |
| **Ecosystem** | Asset library, GDScript and C# | crates.io | Young; Lua for code outside the ECS |

## Where Kin Is Level Or Ahead

- **Agent tooling:** deterministic headless runs with a JSON report, a control
  server, screenshots, render probes and determinism checks
  ([agent interface](agent_interface.md), [testing](testing.md)). Neither engine
  builds this in.
- **2D rendering:** SVG and vector shapes, masks of any shape, HDR with
  tonemapping and grading, compute shaders, cached render targets
  ([rendering](rendering.md)).
- **Built in where Bevy has nothing:** physics, hex grids, dialogue, save slots,
  a rich immediate-mode UI (tables, trees, a text editor with undo, rich text)
  and themes from data files ([UI](ui.md)).
- **ECS:** flecs is about as capable as Bevy's ECS, and stronger than Godot's
  node tree for games heavy on systems.

## What To Add First

Kin targets turn-based, card and puzzle games on Windows, Linux and Steam Deck
([manifest](manifest.md)). For those games most of the gaps above don't matter.
These do:

1. **Gamepad input, with UI focus moved by the D-pad.** Steam Deck can't really
   be supported without it.
2. **Ogg playback and streamed music.** Music shipped as WAV is large.
3. **Shipping:** probably a macOS build, and installers or code signing once a
   game is sold.
4. **A tween API** for UI and card motion, built on the existing easing curves.

Navigation meshes, joints, 2D skeletons, shadows and networking can wait until a
game needs them, as the manifest's "Kin earns its features" rule says.
