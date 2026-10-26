# Assets

Kin has two complementary asset APIs that share one cache:

- **`AssetManager`** — synchronous, eager loading. `load<T>(path)` blocks, caches,
  and returns the loaded asset (or throws). Unchanged; use it for simple, startup,
  or tooling loads.
- **`AssetServer`** — asynchronous, handle-first loading layered over an
  `AssetManager`. `load_async<T>(path)` returns immediately; background workers run
  the loader; `pump()` applies completed loads on the main thread. This is the
  Bevy-`AssetServer`-equivalent layer.

Both key the cache identically, so a sync `load<T>` and an `load_async<T>` for the
same path/type resolve to the **same storage** — there is never a double cache
entry, and a sync load will complete an in-flight async reservation in place.

## Async loading

```cpp
kin::AssetManager assets{"games/mygame/assets"};
kin::AssetServer server{assets};

// Returns immediately; the handle is not valid() yet.
kin::AssetHandle<kin::Image> hero = server.load_async<kin::Image>("hero.png");

// Once per frame, on the main thread:
server.pump(kin::AssetServer::PumpMode::Budgeted);

if (server.load_state<kin::Image>("hero.png") == kin::LoadState::Loaded) {
    use(*hero);
}
```

`LoadState` is `NotLoaded | Loading | Loaded | Failed`. A loader that throws marks
the asset `Failed` and emits a `Failed` event (it does **not** throw out of the
server).

### Events

`pump()` clears its event buffer at the start and fills it with what happened this
pump. Each `AssetEvent` carries `{ kind, path, type, request_id }`.

```cpp
for (const kin::AssetEvent& e : server.events()) {
    if (e.kind == kin::AssetEvent::Kind::Added) { /* e.path finished loading */ }
}
```

## Dependencies

A dependency-aware loader declares nested assets via `LoadContext::require<U>`. The
asset is `ready()` only once it and its whole transitive dependency tree are
`Loaded`. The headline case is a sprite catalog pulling in its textures:

```cpp
kin::register_sprite_catalog_async_loader(server);
server.load_async<kin::SpriteCatalog>("hero.kinsprites");
server.drain();
assert(server.ready<kin::SpriteCatalog>("hero.kinsprites")); // catalog + textures loaded
```

Write your own dependency-aware loader with `register_async_loader<T>`:

```cpp
server.register_async_loader<MyAsset>(
    [](const std::filesystem::path& path, kin::LoadContext& ctx) {
        MyAsset a = parse(path);
        for (const std::string& dep : a.referenced_paths) {
            ctx.require<kin::Image>(dep);
        }
        return a;
    });
```

Without a registered async loader, a type falls back to the `AssetManager`'s
synchronous loader (no dependencies).

## Determinism

`pump` has two modes:

- **`Budgeted`** (windowed) — non-blocking; applies whatever finished this frame.
- **`DrainToQuiescent`** (headless/server) — blocks until everything queued and in
  flight (including dependency jobs) is applied.

In drain mode, completed loads are applied **in ascending `request_id` order** and
processed one fully-quiesced generation at a time. Dependency jobs always receive
higher ids than the parents that spawn them, so the apply/event order is identical
run-to-run regardless of how worker threads were scheduled. This keeps headless
runs deterministic, consistent with Kin's charter.

Loads run on the shared job system (`kin::default_job_system()`, or
`AssetServerConfig::jobs`), at most `worker_count` at a time. While draining, the
calling thread runs queued loads too, so a busy job system cannot stall it.

## Frame integration

`AssetServer` is game-owned. Drive it yourself by calling `pump()` each frame, or
let the runtime do it: set `SceneAppConfig::asset_server`, and `run_scene_app`
pumps once per frame before `scenes.update` — `DrainToQuiescent` in headless mode,
`Budgeted` when windowed.

```cpp
kin::AssetServer server{assets};
kin::SceneAppConfig config;
config.asset_server = &server;
return kin::run_scene_app(config, scenes);
```

## Assets named by components (2D)

A component names an asset by its path in the game's content, with a
`ComponentAssetRef` field: what a scene file writes (`"asset": "images/hero.png"`)
and an editor offers. `kin::Assets2D` (`kin/ecs/assets2d.hpp`) gives components
what they name: it reads each asset once from a content root, shares it, and
reads it again when its file changes.

```cpp
// A scene file's entity:
//   "components": {"Transform2D": {"pos": {"x": 320, "y": 180}},
//                  "TextureRenderer": {"asset": "images/hero.png", "pivot": {"x": 0.5, "y": 0.5}}}
```

`EcsScene` does it for its world before each frame is drawn
(`EcsSceneRenderConfig::assets`, on by default), from `content_root()` (or
`asset_root`), checking for changed files every `asset_check_frames` frames: a
scene that names an image shows it, and an image saved again shows at once.

kin binds its own components' assets:

| Component | `asset` names | It gets |
| --- | --- | --- |
| `TextureRenderer` | an image | `texture` |
| `SpriteRenderer` | a sprite catalog (`.kinsprites`; its images named from the content root) | `sprite.catalog`; `sprite.id` (the `sprite` field) picks the sprite |
| `AudioEmitter` | a sound file, or an audio catalog (`.kinaudio`) | `catalog`: a sound file's has one cue, named by its path, which an empty `cue` plays; a catalog's `cue` picks one |
| `ParticleSystemComponent` | a particle catalog (`.kinparticles`) | `system`'s catalog; `effect` picks the effect it bursts (`sprites`, a sprite catalog, gives `sprites`) |
| `ParticleFieldComponent` | a particle catalog | `state`, made from the catalog's field `field` (`name` in C++), its area moved by where the entity is |
| `SpriteAnimator` | (none: its `SpriteRenderer`'s catalog) | `animation` names one of the catalog's animations; `update_sprite_animators(world, dt)` sets the renderer's sprite to its frames in turn |
| `TilemapComponent` | a tilemap (`.kintilemap`) | `tilemap`: the map, with the tileset file it names (`.kintileset`) and that tileset's image cut in frames (`sheet`); read again when any of them changes |
| `ShapeRenderer` | an SVG-lite file (`.svg`), when its `shape` is `svg` | `mesh`, made from its `spec` (a box, rounded box, ellipse or polygon, or the file's shape) and made again when either changes |
| `TextRenderer` | a font file (`.ttf`, `.otf`) as `font`; empty: the system's UI font | `face`: the font at its `size` |

Sounds still need a game's `AudioEngine`: `update_audio_emitters(world, audio)`
plays the emitters whose catalogs are bound. Particles move in
`update_particles(world, dt)`, which also bursts each emitter's `effect` where
its entity is (`offset` from it): once as it starts (`burst_at_start`), and
`rate` times a second while `emitting`. An emitter with no effect is the
game's to emit from, in code, as before.

A particle catalog's `effect` line gives a burst: how many particles, their
speed and lifetime ranges, start and end sizes and colours, acceleration
(gravity), layer, order and sprite. The settings after those are optional
`name=value` words: `direction` and `spread` (degrees, clockwise from right; a
spread of 360, the default, goes every way); `shape` (`point`, `circle` or
`box`) and `shape_size`, where the particles start; `drag` (the part of their
speed they lose each second); `rotation` and `spin` (a degree range, or
`a,b`); and `mid_at`, `mid_size` and `mid_color`, a size and colour they pass
through partway through their lives. Only settings that differ from their
defaults are written, so effects that don't use them read as they always did.
`parse_particle_catalog` and `serialize_particle_catalog` read and write the
text, for editors.

```text
effect fire 14 30 70 0.4 0.8 8 2 255 240 150 255 140 20 10 0 0 -40 300 0 - direction=270 spread=40 shape=circle shape_size=12,6 mid_at=0.4 mid_size=6 mid_color=255,130,30,220
```

Tilemaps draw with the rest (`WorldRenderState`), their top left where their
entity is, on `RenderLayer::TileBack` unless their `layer` says otherwise. A
map's cells are squares, or hexagons (`TileMap::grid`: pointy-top in rows or
flat-top in columns, every other one shifted, kept as `HexLayout`'s offset
cells), each box `tile_w` x `tile_h` bounding its hexagon; `world_to_map`,
`tile_center`, `neighbor_cells` (six), collision, `find_path` and drawing
follow the shape. A tileset kept in a file of its own (`.kintileset`, format
`kin.tileset`: its tiles, and the image their `frame`s are cut from) is shared
by the maps naming it (`"tileset": "tilesets/ground.kintileset"`, read from
the content root by `load_tilemap(path, content_root)` and `Assets2D`).
Terrain autotiling (four sides) stays square-only.

A component of the game's own gets a binder, a function over the world:

```cpp
scene.add_asset_binder("Speaker", [](kin::EcsWorld& world, kin::Assets2D& assets) {
    world.each([&](Speaker& speaker) {
        if (!speaker.asset.id.empty() && !speaker.sound) {
            speaker.sound = assets.manager().load<Sound>(speaker.asset.id);
            assets.changed();
        }
    });
});
```

- `texture(path)` makes an image a texture (PNG, JPEG, BMP, TGA, QOI, GIF; KTX2
  and DDS as compressed textures), once. One that cannot be read is invalid, with
  why in `problems()`, and is tried again when its file changes (or appears).
  `sprite_catalog(path)` and `audio_catalog(path)` read catalogs the same way; a
  catalog is read again when it or a file it was made of (a sprite catalog's
  images) changes, and the one before is kept for what still points at it.
- `manager()` is an `AssetManager` on the same root, for every other kind (its
  loaders, and the game's). `reload_changed()` forgets what it read whose files
  changed too, so the binder's next load reads them again.
- `reload_changed()` compares each file's time with the one read: call it now
  and then. Assets in a `.kinpak` never change.
- A field says what kind of asset it names with `.asset_type("Image")` (a name
  of `asset_type_name`, or several separated by commas), so editors offer only
  those; `"asset_type"` in a `.kincomponents` schema. A field naming an entry of
  that asset (a sprite of a catalog, a cue) says so with `.entry_of("asset")`,
  the asset field's name, so editors offer the asset's entries. `infer_asset_type` gives a file's kind from its name,
  among them `Image`, `Audio`, `Font`, `Scene`, `Prefab` and `Model`.

## Content and packs

`AssetManager`'s root is usually the game's content root, from
`kin::find_content_root` (`kin/assets/content.hpp`). While the game is made,
that is the source folder. Once it ships, it is a `.kinpak` archive mounted in
the folder's place. Every loader reads through the content layer, so the same
relative paths work either way. `discover()` lists a pack's files as it lists
a folder's. See [shipping](shipping.md).

## Compressed textures

PNG and JPEG textures are decoded to RGBA8 (4 bytes a texel) and given their
mips on the GPU. A texture can instead come already compressed, as the GPU
keeps it: a **KTX2** or **DDS** file of a block format, which the GPU decodes as
it samples, in a quarter to an eighth of the memory, with the mip chain the
tool made. Kin reads these files; it has no encoder, so they are made by a tool.

| Format | `TextureFormat` | Bytes a 4 x 4 block | For |
| --- | --- | --- | --- |
| BC1 | `Bc1RgbaUnorm`, `Bc1RgbaSrgb` | 8 | colour, 1-bit alpha |
| BC3 | `Bc3RgbaUnorm`, `Bc3RgbaSrgb` | 16 | colour with alpha |
| BC4 | `Bc4RUnorm` | 8 | one channel: roughness, masks |
| BC5 | `Bc5RgUnorm` | 16 | two channels: normal maps |
| BC7 | `Bc7RgbaUnorm`, `Bc7RgbaSrgb` | 16 | colour and alpha, the best of the BC formats |
| ASTC 4x4 | `Astc4x4RgbaUnorm`, `Astc4x4RgbaSrgb` | 16 | phones and tablets |

```cpp
#include <kin/assets/content.hpp>
#include <kin/assets/texture_file.hpp>

const auto bytes = kin::read_content_file(root / "rock.ktx2");
std::string error;
const auto image = bytes ? kin::decode_compressed_image(
    {reinterpret_cast<const kin::u8*>(bytes->data()), bytes->size()}, &error) : std::nullopt;
if (image) {
    kin::Texture rock = renderer.create_texture_compressed(*image);
    renderer.set_scale_mode(rock, kin::ScaleMode::Mipmapped); // the file's own levels
}
```

In 3D, `Assets3D::load_texture` reads a `.ktx2` or `.dds` path this way, and a
glTF model's image that is a KTX2 or DDS file is uploaded as it is (see
[3D rendering](rendering_3d.md#direct-loading-and-reload)).

- A KTX2 file holds one 2D texture (not an array, a cube map or a 3D texture)
  of a format above, without supercompression. Basis Universal files (ETC1S or
  UASTC: `KHR_texture_basisu`, `basisu`, `toktx --encode etc1s` or `uastc`) and
  Zstandard or ZLIB supercompressed ones are refused: they need a transcoder,
  which kin does not ship. A DDS file has the DX10 header with a BC format, or
  the older DXT1, DXT5, ATI1 (BC4U) or ATI2 (BC5U) one.
- Every level must be the size its place in the chain gives it; a file whose
  header, level index or levels do not fit is refused with the reason in
  `error`, and never read past its end.
- The format says what the texels are. An sRGB one is colour, used as
  `create_texture_from_rgba`'s colour is; a UNORM one holds numbers, as
  `create_texture`'s RGBA8 data does. Make base colour and emissive maps sRGB,
  and normal, metal-roughness and occlusion maps UNORM. A texture asked for the
  other way round (`load_texture`'s `srgb`, a glTF map) is said once in the log,
  as it loads, and sampled as the file says.
- The mips are the file's: `ScaleMode::Mipmapped` samples them and never makes
  more, so make the chain when compressing; a one-level file is sampled without
  mips. Rows run from the top, the way these tools write them.
- A compressed texture is drawn and sampled like any other, but never updated
  in place (`update_texture` and `write_texture` return false).
- Only SDL_GPU makes them (the SDL_Renderer backend has none), and only in
  formats the device samples: desktop GPUs have BC and few have ASTC; phones the
  other way round. Where it cannot, `create_texture_compressed` returns an
  invalid texture and says so once a format, so keep a PNG to fall back to.

Some tools that write them:

```sh
# BC7 colour and BC5 normals, as DDS with every mip (DirectXTex's texconv)
texconv -f BC7_UNORM_SRGB -m 0 -dx10 rock.png
texconv -f BC5_UNORM -m 0 -dx10 rock_normal.png
# ASTC 4x4 as KTX2 (KTX-Software's toktx; not --encode etc1s or uastc, which make Basis files)
toktx --t2 --encode astc --astc_blk_d 4x4 --genmipmap rock.ktx2 rock.png
```

AMD's Compressonator (`compressonatorcli -fd BC7 ...`) and NVIDIA's Texture
Tools write KTX2 and DDS too; check that a colour map comes out in an sRGB
format.

## Hot-reloading game data

`kin::FileWatcher` (`kin/assets/file_watcher.hpp`) reloads a game's own data
files (text, balance tables, themes, levels) while it runs: edit a file, save,
and the change is in the game a moment later, without a restart.

```cpp
kin::FileWatcher files;
files.load_and_watch(root / "data/balance.kinbalance",
                     [&](std::string_view text, std::vector<std::string>& errors) {
                         return balance.load(text, errors); // apply only on success
                     });
config.file_watcher = &files; // run_scene_app polls it once per frame
```

- `load_and_watch(path, load)` loads the file now and again whenever it changes.
  `load` gets the whole text and returns false, with messages in `errors`, to
  reject it. A rejected or unreadable file is logged and the loader should keep
  what it had, so a typo in an edit leaves the game running on the last good
  version; fix the file and it reloads.
- `watch(path, on_change)` just calls back, for files a game reads itself or
  whose change means more than one reload (for example data files that refer to
  each other, reloaded together).
- Changes are found by polling modification time and size, at most every
  `Options::interval` (250 ms), and reported only once a file has stayed the
  same for two polls, so editors that write in steps or save through a temporary
  file are seen once, after they finish. A deleted file is not reported until it
  comes back.
- Callbacks run inside `poll()`, on the thread that calls it: with
  `SceneAppConfig::file_watcher`, the main thread between frames, so they may
  change game state directly. Headless and server runs never poll, so their data
  stays what it was when the run began and they stay deterministic. A game that
  runs its own loop calls `poll()` itself.
- Files in a pack never change, so nothing is reported for them.

## Scope (V1)

Implemented: async loading, handles, `LoadState`, events, dependency loading,
deterministic drain, frame integration, and default prefab asset discovery/loading
for `.kinprefab` files.

Not yet implemented: ref-counted auto-unload (assets are released via
`AssetManager::unload`/`clear`), and an asset processing pipeline (`.meta`/import
settings/cached artifacts/multiple sources). Hot-reload `Modified` events are
reserved but not yet emitted.

## Shipping inventory and dependency declarations

The runtime dependency cache is separate from the build-time shipping graph.
Games can declare roots, dynamic groups and asset dependencies in a
`kin.asset-manifest/1` JSON manifest and pass it as `MANIFEST` to
`kin_configure_game_assets`. With `discover_dependencies: true`, supported scene and catalog references are
tracked automatically using their loader path rules. Lua/custom component loads
still require declarations. Only explicit roots and their transitive dependencies
become reachable; recognized files are not automatically rooted. Reports expose
edge provenance and root-to-file chains, and all files are still packaged. See
[asset manifests and dry-run reachability](shipping.md#asset-manifests-and-dry-run-reachability)
for the schema, commands and completeness rules.

For export-time processing, enable `KIN_ENABLE_ASSET_COOKING`. The portable cooker
minifies known Kin JSON assets and losslessly recompresses PNG while retaining
all files and paths. Other assets pass through unchanged. Per-file hashed caches
and ZIP-bound cooking reports are separate from runtime asset caches. See
[lossless asset cooking](shipping.md#lossless-asset-cooking).
