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

## Scope (V1)

Implemented: async loading, handles, `LoadState`, events, dependency loading,
deterministic drain, frame integration, and default prefab asset discovery/loading
for `.kinprefab` files.

Not yet implemented: ref-counted auto-unload (assets are released via
`AssetManager::unload`/`clear`), and an asset processing pipeline (`.meta`/import
settings/cached artifacts/multiple sources). Hot-reload `Modified` events are
reserved but not yet emitted.
