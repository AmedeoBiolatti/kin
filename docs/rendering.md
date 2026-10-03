# Rendering And Sprites

## Sprite References

ECS rendering uses sprite references instead of storing concrete sprite/texture
data in components.

```cpp
kin::SpriteCatalog catalog;
catalog.set_texture("actors", "actors.png", texture);
catalog.add_sheet({
    .id = "actors.sheet",
    .texture_id = "actors",
    .grid = {.tile_w = 16, .tile_h = 16},
});
catalog.add_sheet_sprite("hero.walk.0", "actors.sheet", 0);

entity.set(kin::SpriteRenderer{
    .sprite = catalog.ref("hero.walk.0"),
});
```

The reference chain is:

```text
SpriteRenderer -> SpriteRef(sprite id)
SpriteCatalog -> SpriteDefinition
SpriteDefinition -> texture_id or sheet_id + frame
SpriteSheetDefinition -> texture_id + grid
TextureAssetRef -> asset path + loaded Texture
```

Concrete `Sprite` values still exist for low-level rendering and UI image
helpers, but ECS/editor-facing rendering should prefer ids.

## Layers And Placement

ECS sprite rendering composes placement in this order:

```text
anchor = world_position + catalog_sprite.offset + SpriteRenderer.offset
top_left = anchor - pivot * final_size
```

`final_size` uses `SpriteRenderer.size` when set, otherwise the catalog sprite
size. `pivot` uses `SpriteRenderer.pivot` when it is non-negative, otherwise the
catalog sprite pivot.

Named layer helpers keep ordering stable:

```cpp
renderer.layer = kin::layer_value(kin::RenderLayer::Actors, 5);
renderer.y_sort = true;
renderer.sort_y_offset = 12.0f;
```

Catalogs can be saved as `.kinsprites` text assets:

```text
texture actors actors.png
sheet actors.sheet actors 16 16 0 0
sheet_sprite hero.walk.0 actors.sheet 0 24 24 0.5 1 0 0
sprite hero.icon actors 0 0 16 16 24 24 0.5 1 0 0
```

Use `save_sprite_catalog(...)` and `load_sprite_catalog(...)` for roundtrips.
The renderer-backed load overload can create textures from image assets:

```cpp
kin::SpriteCatalog catalog = kin::load_sprite_catalog(assets, renderer, "hero.kinsprites");
```

## Animation

Sprite animation is driven by the general animation system. Sprite frames are
authored as `SpriteTrack` keys inside `Animation` definitions and resolved
through an `AnimationRegistry`.

```cpp
auto fragment = assets.load<kin::AnimationRegistryFragment>("hero.kinanim");

kin::AnimationRegistry animations;
animations.merge(*fragment);

entity.set(kin::AnimationPlayer{
    .registry = &animations,
    .properties = &properties,
    .catalog = &catalog,
});

auto* player = entity.get_mut<kin::AnimationPlayer>();
kin::set_base(*player, "hero.walk");
```

Run animation before rendering:

```text
advance_animation_players(world, dt)
sample_animation_players(world)
animation_events.run(world)
```

Animation assets support `Clip`, `Ref`, `Sequence`, `Parallel`, and `Repeat`
nodes, per-frame pivots, event keys, and per-frame `SpriteBox` hitboxes and
hurtboxes.

```text
animation hero.attack
  clip 0.12
    sprite hero.attack.0 0
    sprite hero.attack.1 0.06 pivot 0.25 0.75
    event 0.06 sound name=sfx value=slash offset 0 -8
```

Raw imported frame rectangles can be expanded into sprite ids and concrete
animations:

```cpp
kin::AnimationImportSet import{
    .texture_id = "actors",
    .texture_path = "actors.png",
    .clips = {{
        .name = "hero.walk",
        .frames = {
            {.source = {0, 0, 16, 16}, .duration = 0.1f},
            {.source = {16, 0, 16, 16}, .duration = 0.1f},
        },
    }},
};

kin::AnimationRegistry animations;
kin::add_animation_sprites(catalog, assets, renderer, import, {.id_prefix = "player"}, &animations);
```

## Animation State Machines

`AnimationStateMachine` maps state names to animation ids and evaluates
bool/float/trigger parameters on each `AnimationPlayer`.

```cpp
auto machine = std::make_shared<kin::AnimationStateMachine>();
machine->initial = "idle";
machine->states["idle"] = "hero.idle";
machine->states["run"] = "hero.run";
machine->transitions.push_back({
    .from = "idle",
    .to = "run",
    .conditions = {{.param = "moving", .op = kin::CondOp::BoolEqual, .bool_value = true}},
    .mode = kin::TransitionMode::ReplaceBase,
});

player.machine = machine;
player.params.set_bool("moving", true);
```

State-machine evaluation runs at the start of `advance_animation_players(...)`.

State machines can be saved as `.kinanimsm` text assets:

```text
statemachine hero
  initial idle
  state idle hero.idle
  state run hero.run
  transition idle -> run when bool:moving == true mode replace
  transition run -> idle when bool:moving == false mode replace
```

## Custom Shaders

A material shader is a fragment shader drawn over a rectangle. Create it once
from precompiled bytecode, then draw it with `draw_shader_surface()`:

```cpp
kin::ShaderDesc desc;
desc.spirv = {bytes.data(), static_cast<kin::u32>(bytes.size())};
desc.num_samplers = 3;        // texture slots the shader declares
desc.num_uniform_buffers = 1; // ShaderParams, when the shader has a uniform block
kin::ShaderHandle shader = renderer.create_shader(desc);

const std::array<kin::Texture, 3> sources{albedo, heights, shadows};
kin::ShaderParams params;
params.uniforms[0] = time;
renderer.draw_shader_surface(rect, shader, params, sources);
```

`sources[i]` binds at fragment sampler slot `i`, up to `kin::MaxShaderSamplers`
(16). In GLSL that is `layout(set = 2, binding = i) uniform sampler2D ...`, and
the uniform block is `layout(set = 3, binding = 0)`. Slots the shader declares
but the draw does not fill are bound to a 1x1 white texture. Overloads taking no
texture, one texture, or two textures are shorthands for the same call.

`ShaderParams::uniforms` holds 16 floats by default. Resize it for more, up to
`kin::MaxShaderUniformFloats` (4096, 16 KiB). Declare arrays in the shader as
`vec4`s: std140 pads each element of a `float` array to 16 bytes.

### Shader geometry

`draw_shader_surface()` covers a rectangle. For a shape that is really a
polygon (a shadow, a hull, a beam), `draw_shader_geometry()` draws triangles
with the same material shader, so the rasterizer decides what is inside and
only the covered pixels run the shader:

```cpp
std::vector<kin::ShaderVertex> vertices;
std::vector<kin::u32> indices;            // three per triangle; empty: vertices are triangles
for (const Shadow& s : shadows) {
    const auto first = static_cast<kin::u32>(vertices.size());
    for (const kin::Vec2f p : s.hull) {
        vertices.push_back({.position = p, .uv = s.uv_of(p), .custom = {s.index, s.height, 0, 0}});
    }
    for (kin::u32 i = 1; i + 1 < s.hull.size(); ++i) {   // a fan over the convex hull
        indices.insert(indices.end(), {first, first + i, first + i + 1});
    }
}
const auto max = renderer.scoped_blend_mode(kin::BlendMode::Max);
renderer.draw_shader_geometry(vertices, indices, shadow_shader, params, sources);
```

- The fragment shader gets `color` and `uv` as for a surface, and each vertex's
  `custom` as `layout(location = 2) in vec4`. One draw can then carry many
  shapes, each with its own parameters, such as an index into a data texture.
- Sources, params and the blend mode work as for `draw_shader_surface()`. With
  `BlendMode::Max`, overlapping shapes combine by the larger, in any order.
- Geometry that isn't whole triangles, or indexes past the vertices, is refused
  and logged; nothing is drawn.
- It needs `capabilities().shader_geometry`: the SDL_GPU backend has it, others
  draw nothing.

### Data textures

Besides `Rgba8`, textures can hold numbers for shaders to read:

```cpp
std::vector<std::uint16_t> heights(cols * rows);    // one 16-bit value per cell
kin::Texture height_map =
    renderer.create_texture({cols, rows}, kin::TextureFormat::R16Uint, heights.data());
renderer.update_texture(height_map, {x, y}, {1, 1},
                        reinterpret_cast<const kin::u8*>(&new_height));
```

| Format | Bytes per texel | GLSL |
| --- | --- | --- |
| `R16Uint` | 2 | `usampler2D`, `.r` |
| `Rg16Uint` | 4 | `usampler2D`, `.rg` |
| `R32Float` | 4 | `sampler2D`, `.r` |

Read them with `texelFetch(tex, ivec2(x, y), 0)`: they are never filtered.

On SDL_GPU, `create_texture()` and `update_texture()` don't each submit their
own command buffer: a frame's uploads are recorded into one, sent to the GPU
just before the frame (or a read-back), so updates made during a frame apply
to the whole frame. Replacing a whole texture that the frame hasn't drawn yet
cycles its storage, so the upload doesn't wait for earlier frames still reading
it. Texels are copied with streaming stores (the staging memory is uncached),
and with `Renderer2D::set_job_system()` uploads of 1 MB or more are copied on
the workers too. Released textures are kept a few seconds (up to 256 MB) and
reused by `create_texture()` for one of the same size and format, skipping the
driver's create and destroy.

Texels made each frame can skip the copy altogether: `write_texture()` hands
its callback the upload memory itself to write them into.

```cpp
renderer.write_texture(shadow_data, {0, 0}, {1024, rows}, [&](std::span<kin::u8> texels) {
    auto* out = reinterpret_cast<float*>(texels.data());
    for (const Caster& c : casters) {   // in order, once: the memory is uncached
        *out++ = c.height;
    }
});
```

The callback must not use the renderer. On other backends it writes a buffer
that is then uploaded as by `update_texture()`. `backend_stats().texture_uploads` and `texture_upload_submits`
count them; `kin_upload_bench [frames] [workers] [big]` measures them.

CPU time per frame on an RTX 4080 Laptop GPU (kin 0.2.3, which submitted each
upload on its own, took about 25 ms for the first row and then stalled 70 ms at
present):

| Uploads a frame | No job system | 3 workers |
| --- | --- | --- |
| 50 created and 50 replaced (256 KB each), 50 half updated | 2.1 ms | 2.1 ms |
| 4 created and 4 replaced (16 MB each), 4 half updated | 11.4 ms | 4.6 ms |
They need `capabilities().data_textures` (the SDL_GPU backend), are only for
shaders (`draw_texture()` refuses them), and every slot that expects one must be
given one, since an empty slot is bound to the white `Rgba8` texture.

Shaders need `capabilities().materials_2d`. The SDL_GPU backend supports all of
the above. The SDL renderer's `gpu` driver draws shader surfaces without source
textures, and other backends draw nothing, so check the capability and provide a
fallback. `engine/shaders/` has working examples.

## Blend Modes

`Renderer2D::set_blend_mode()` (or `scoped_blend_mode()`) sets how later draws
combine with what is under them:

| Mode | Result | For |
|---|---|---|
| `Alpha` | straight alpha over (the default) | most drawing |
| `Additive` | dst + src * src alpha | lights, glows |
| `Multiply` | dst * src | light maps, tinting |
| `Replace` | src | copying |
| `Max` | max(dst, src), each channel and alpha | overlapping shadows, fog of war, coverage, heat maps |
| `Min` | min(dst, src), each channel and alpha | the reverse |

`Max` and `Min` let overlapping shapes each be drawn on their own, in any order,
into one target: two shadows that overlap darken it once, not twice. The source
is not weighted by its alpha. The SDL_GPU backend has them; SDL's software
renderer does not (`capabilities().min_max_blend` is false) and draws them as
`Alpha`, logging a warning once.

## Lighting

`kin::LightLayer` lights a scene after it is drawn: everything in an area is
multiplied by an ambient colour plus the light of each `kin::Light2D`.

![The lighting demo at night: lamps, a campfire, a cyan crystal and a flashlight beam](images/lighting_demo.png)

```cpp
kin::LightLayer lighting;                         // keep it; it caches a texture

renderer.clear(sky);
draw_world(renderer);
const std::array<kin::Light2D, 2> lights{{
    {.position = lamp, .radius = 150.0f, .color = kin::Color::rgb(255, 200, 120)},
    {.position = fire, .radius = 190.0f, .color = kin::Color::rgb(255, 130, 50),
     .intensity = 1.3f},
}};
lighting.apply(renderer, {0.0f, 0.0f, 960.0f, 540.0f}, night_ambient, lights);
draw_ui(renderer);                                // drawn after: not darkened
```

- A light is brightest at `position` and fades smoothly to nothing at `radius`.
  `intensity` scales it, up to 4.
- Lights add up, and the result can exceed the ambient; a white ambient is
  daylight and leaves the scene as drawn.
- `shape` replaces the round falloff with a texture stretched over the light's
  `2 * radius` square, rotated by `rotation` degrees: its alpha is how much light
  reaches each point. Use it for flashlight cones, spotlights or window light.
- Positions are in the coordinates `apply()` is called in. With a camera, convert
  them to screen positions first.
- `resolution` (default 0.5) is the light map's size relative to the area. Lights
  are smooth, so half resolution is plenty.

`apply()` needs render targets and blend modes, which the software and GPU
backends both have; it uses no shaders. Where they are missing it returns
`false` and leaves the scene unlit. `games/lighting_demo` has lamps, a campfire, a
coloured crystal and a flashlight, with a day/night cycle.

## Performance And Data Layout Notes

Rendering should keep hot per-frame components small and push editor/debug
metadata into catalogs, assets, or cold companion components.

Current hot render components:

- `Transform2D`
- `SpriteRenderer`
- `TextureRenderer`
- `RectRenderer`
- `LineRenderer`

Prefer `SpriteRef` ids over concrete `Sprite`/`Texture` data on entity
components. Keep catalogs, animation registries, and loaded asset handles in
scene-owned resources.

### Sprite batches

`Renderer2D::draw_sprites(texture, sprites)` draws many quads from one texture
in order: each `SpriteInstance` (dest, source, tint, rotation, pivot) draws
what `draw_texture()` would. The SDL_GPU backend submits them as one instanced
draw (a 52-byte instance per quad, expanded by `sprite_instanced.vert`); other
backends draw them one by one. `RenderQueue` flushes hand every run of
consecutive same-texture sprite commands to it, so ECS sprites batch without
changes, as long as neighbours in draw order share a texture. Y-sorted sprites
of several kinds interleave, so put them in one atlas (a `SpriteCatalog` sheet,
or `TextureRenderer::source` regions of one texture): a texture change ends the
run and starts a new draw.

`RenderQueue` stores plain sprites (a valid texture, no material, not in output
pixels) compactly: the quad, its key and a texture index, about half a
`RenderCommand`, with one texture reference per distinct texture instead of one
per sprite. `flush()` sorts and draws them without ever building commands;
`commands()`, `sort_commands()` and the presorted flushes convert them, in
submission order, the first time they are asked for, so readers of the queue see
ordinary Texture and Sprite commands. `cull(view, first, last)` counts
submissions (`submitted()`), not positions.

Both ends can use worker threads for large workloads, with the same result as
a single thread: `SpriteRenderOptions::jobs` has `collect_static()` and
`collect_dynamic()` prepare `TextureRenderer` tables of 8,192 or more entities in
chunks: workers count each chunk's sprites, the queue reserves room for all of
them (`RenderQueue::reserve_sprites()`), and workers write each into its slot
(`write_sprite()`), in row order; and after
`Renderer2D::set_job_system(jobs)` the SDL_GPU backend fills `draw_sprites()`
batches of 16,384 or more in parallel.

## Fixed-Timestep Render Interpolation

`SceneContext::alpha` carries the fixed-timestep interpolation factor into
render contexts: the fraction of the next sim step elapsed at render time
(`accumulator / fixed_dt`, in `[0, 1)`). It defaults to `1.0` so contexts that
do not interpolate render the latest stepped state, and is `0.0` in update
contexts and headless fixed-frame renders.

To render smooth motion at sim rates below the display rate (e.g. a 30Hz sim
on a 60Hz display), store the previous stepped value and lerp during render:

- capture `prev_pos = pos` at the start of each sim step (before any system
  moves the entity);
- in render-phase sync, position visuals at `lerp(prev_pos, pos, ctx.alpha)`;
- interpolate the camera the same way, or panning will stutter even when
  entities are smooth.

The pattern: a `ParallelEligible`
native captures `prev_pos` each step, `begin_interpolated_view(ctx.alpha)`
lerps the camera, and all `world_to_screen` consumers pick the interpolated
view up automatically. Death/teleport visuals must force one final sync so the
last pose is not interpolated from a stale previous position.

### Frame pacing

A display's frame times jitter around the step: at 60 steps a second, 16.5 ms
then 16.9 ms. Counted as measured, the accumulator crosses a step boundary early
or late, and some frames run 0 updates and the next 2, which shows as judder in
whatever `update()` moves. `App::run` therefore counts a frame time within
`AppConfig::snap_tolerance` (1 ms by default; `WindowedAppConfig` has it too) of
a whole number of steps as exactly that many (`kin::FrameTimeSnapper`).

- The time snapped away is kept and paid back a whole step at a time, so game
  time keeps up with real time: a 59.94 Hz display drops one step every 17 s.
- Frame times not near a whole number of steps (hitches, unpaced frames shorter
  than a step) are counted as measured.
- `AppFrameStats::snapped_frame_time` is what the accumulator was given.
- Set `snap_tolerance` to 0 to count every frame as measured.
