# Request: cheaper ui2 text and rounded shapes, frame after frame

From: the Fantasy Merchant session (a game on kin, `../fantasy-merchant`), 2026-10-05.
Status: done in kin 0.2.5 — 1 kerning from HarfBuzz, read per pair from a
table; 2 `measure()` from the same glyph walk (about 50x faster); 3 corner
arcs from a table (see docs/changelog.md). The soft glow as one quad is not done.

Context: a HUD of a dozen panels, ~70 map labels and a 16-row market table
(EB Garamond and Lato through `ui2::load_ttf_font`) spent half the main
thread on ui2 each frame on an RTX 4080 Laptop, Linux/X11, Vulkan, Release.
`perf record --call-graph dwarf` of the steady-state world map, inclusive:

| | share of main-thread samples |
|---|---|
| `MapScene::draw_hud` (all of ui2) | 50% |
| `ui2::draw_text` → `TtfFontBackend::draw` | 18–27% |
| … of which `TTF_GetGlyphKerning`, `Find_GlyphMetrics`, `TTF_FindInHashTable` | ~20% |
| `GpuRenderer2DBackend::fill_rounded_rect` → `rounded_rect_loop` (`sincosf`) | 16% |
| `ui2::Context::surface` (glows as one rounded rect a pixel) | 15% |
| `GpuRenderer2DBackend::present` | 30–38% |

The game has worked round what it can (square controls, no glows, a
measurement cache of its own, fewer shadowed strings: surfaces 15% → 5–7%).
The rest is in kin and would help every game:

## 1. Kerning from a table, not from SDL_ttf per glyph pair

`draw_from_glyph_atlas` calls `TTF_GetGlyphKerning(font, previous, value)`
for every glyph of every string, every frame; each call is two glyph-metric
hash lookups inside SDL_ttf. The atlas already knows its 95 glyphs: build a
95×95 kerning table (i8 or f32) when the atlas is built, and read it while
drawing. Text drawn would cost what the quads cost.

## 2. Cache `measure()`

`TtfFontBackend::measure` runs `TTF_GetStringSize` (HarfBuzz shaping, glyph
loads with TrueType hinting) on every call. Layout code measures the same
strings every frame (centring, right-aligning). Either measure from the
glyph atlas's advances and the kerning table of item 1 (same answer as draw,
by construction), or keep a small string → size cache per font. (The game
keeps its own cache in `look::measure` meanwhile.)

## 3. Rounded rectangles without trigonometry every call

`rounded_rect_loop` computes each corner's arc with `sincosf` per segment,
for every fill and outline, every frame. A corner table per radius (or per
segment count), or the unit arc computed once and scaled, would remove it.
And `SurfaceStyle::glow_size` draws `ceil(glow_size)` rounded rects: a
blurred-mask glow like the soft shadow's (`shadow_mask`) would be one quad.

## How to measure

```sh
cd ../fantasy-merchant
cmake --build build && timeout 14 perf record -F 499 --call-graph dwarf,32768 --delay=7000 \
    ./build/bin/fantasy_merchant --play --watch --seed=7
perf report --children --stdio -g none --percent-limit 3
```

(`--watch` keeps the camera on the world map, where most text is.) Wall-clock
frame times on this laptop vary with other work on the machine: compare
shares of samples, or `perf stat -e instructions:u`, before and after.
