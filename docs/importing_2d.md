# Importing 2D assets

`tools/import_2d_assets.py` builds native Kin content from authoring files. Games
read the generated PNG, `.kinsprites`, `.kintilemap` and `.kinscene` files through
their normal content root, including a mounted `.kinpak`. Python and Pillow are
build dependencies only:

```sh
python3 -m pip install -r tools/requirements-imports.txt
python3 tools/import_2d_assets.py art/imports.json --output build/imported --content content
```

Source paths are relative to the manifest. Output paths are relative to the
generated content root. Use a dedicated output directory, outside the source
content directory. `--content` copies existing runtime content alongside the
generated files; output collisions are errors.

## Build integration

```cmake
add_executable(my_game src/main.cpp)
target_link_libraries(my_game PRIVATE kin::engine)
kin_game(my_game CONTENT content IMPORTS art/imports.json)
```

Configuration imports once so the packaging pipeline can discover generated
files. Builds then track the manifest, source images, external tilesets/levels
and copied content. JSON source/manifest changes also reconfigure the inventory.
The merged content lives in `build/imported-content/my_game`; `KIN_GAME_CONTENT`
and the existing pack, web and desktop package targets use that directory.
`IMPORTS` cannot be combined with `NO_CONTENT`. `MANIFEST` remains the separate
shipping reachability/cooking manifest described in [shipping](shipping.md).

For custom targets, include `kin_import_2d.cmake` and call
`kin_import_2d_assets(target MANIFEST file OUTPUT directory [CONTENT directory])`.
It creates `target_import_2d`, adds the dependency and sets target property
`KIN_IMPORTED_CONTENT_DIR`.

## Watching and incremental processing

Add `--watch` to keep development content up to date, or build the long-running
`<target>_watch_2d` terminal target. Changes are debounced with `--debounce` (0.1
seconds); `--interval` controls polling (0.25 seconds). Ordinary running scenes
pick up generated files through Assets2D::reload_changed. Preloaded scenes retain
their immutable snapshots.

Per-job output blobs are cached using tool/options/version keys, with verified
source and artifact hashes. Unrelated jobs remain cache hits; unchanged outputs
keep their mtimes. The default cache is `.kin-import-cache` in the output directory;
`--cache directory` selects another store. Deleting it is safe. Hit/miss metrics
are CLI diagnostics; generated reports remain deterministic.

Invalid source files and edits during transforms retain the last good output;
corrupt cache artifacts regenerate. Watch mode retries on the next source change.
All transforms validate before publishing; individual file replacements are atomic.
Keep authoring and output directories separate. Native Aseprite still needs its CLI.

## Manifest

```json
{
  "schema": "kin.imports/1",
  "imports": [
    {
      "kind": "aseprite",
      "source": "hero.json",
      "prefix": "hero",
      "output": "sprites/hero.kinsprites",
      "texture": "images/hero.png",
      "padding": 2,
      "max_size": 2048
    },
    {
      "kind": "atlas",
      "sprites": [
        {"id": "coin", "source": "coin.png", "pivot": [0.5, 0.5]},
        {"id": "door", "source": "door.png", "pivot": [0.5, 1.0]}
      ],
      "output": "sprites/props.kinsprites",
      "texture": "images/props.png"
    },
    {
      "kind": "tiled",
      "source": "level.tmj",
      "output": "maps/level.kintilemap",
      "entities": {"Spawn": "prefabs/spawn.kinprefab"}
    },
    {
      "kind": "ldtk",
      "source": "world.ldtk",
      "level": "Start",
      "output": "maps/start.kintilemap",
      "solid_values": {"Collisions": [1]},
      "entities": {"Spawn": "prefabs/spawn.kinprefab"}
    }
  ]
}
```

Atlas packing uses a deterministic shelf layout, without sprite rotation or
alpha trimming. `padding` defaults to 2 pixels and extrudes edges and corners
while preserving alpha. `max_size` defaults to 2048, with an 8192-pixel limit.
An atlas that does not fit is rejected; split it into separate imports or raise
the limit. Native sprite/catalog tokens cannot contain whitespace, `#` or `*`.
Pivots are normalized coordinates; they default to `[0.5, 0.5]`.

## Supported authoring formats

| Input | Imported data | Current limits |
|---|---|---|
| Aseprite JSON + PNG | Array/hash frames, original canvases restored from trim rectangles, frame durations, forward/reverse/ping-pong tags, finite repeats, optional slice pivots | No rotated export frames; the flattened sheet is the sprite source |
| Native `.ase` / `.aseprite` | Same data, exported through the Aseprite CLI | Requires `aseprite` on PATH, or `--aseprite /path/to/executable` |
| PNG sprite sets | Atlas, sprite dimensions, alpha and pivots | One output texture per import; no rotation or trimming |
| Tiled `.tmj` / JSON | Finite orthogonal or regular half-side hex maps, sheet-based tilesets, external JSON `.tsj`, layers, tile animation, scalar tile metadata and point-to-prefab placement | Uniform map/tile dimensions, uncompressed JSON arrays, right-down order; no transformed GIDs, image/group layers, TSX, isometric/staggered maps, layer offsets, opacity, tint or parallax |
| LDtk `.ldtk` | One selected level, external levels, tile/auto layers, stacked tiles, IntGrid values and point-to-prefab placement | One aligned grid size; no flipped tiles, tile/layer opacity, offsets or custom entity fields; world/background presentation stays game-authored |

With Aseprite, `slice: "feet"` selects a named slice's per-frame pivot. Without a
slice, use a manifest `pivot` or the centered default. Untagged exports get a
`prefix.default` looping animation. Frame duration milliseconds become native
hold values at 1000 fps, preserving variable timing. The native exporter passes
`--batch --list-tags --list-slices` and never starts the editor UI.

Map imports also emit a scene beside the tilemap. It contains the tilemap and
mapped prefab instances at authoring pixel coordinates within that map/level.
Mapped prefab paths name runtime content already supplied by the game. Tiled
`solid`, `cost` and `blocks_sight` tile properties become native tile settings;
one polygon or rectangle per tile can supply collision. Layer `collision` and
`navigation` properties default to true. LDtk IntGrid collision is explicit:
`solid_values` selects solid values by layer name. IntGrid values remain available
as `int_grid_value` tile metadata on an invisible collision/navigation layer.
Custom entity properties/fields belong on the mapped prefab in this version.

Format references: [Aseprite CLI](https://www.aseprite.org/docs/cli/),
[Tiled JSON](https://doc.mapeditor.org/en/stable/reference/json-map-format/),
[LDtk JSON](https://ldtk.io/json/).

## Rebuild behavior

Imports are validated before existing output is changed. Replacements are
atomic per file, unchanged bytes preserve modification times, and unsupported
input reports an error instead of silently dropping an unsupported transform.
The hidden `.kin-import-report.json` records output hashes and source
dependencies. A later import removes stale files from that inventory and keeps
unmanaged files. Output paths and existing parent symlinks must remain within
the generated root. The report is excluded from packs and shipped directories.

The importer builds all outputs in memory before writing. It is intended for
game-sized batches with bounded textures/maps, rather than a streaming importer
for arbitrary-sized worlds. There is no editor import database or persistent
artifact cache yet. The Aseprite subprocess path is covered with an exporter
fixture; actual `.aseprite` export requires a locally installed Aseprite.
