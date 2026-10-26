#pragma once

#include <kin/core/types.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/world.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/render_layer.hpp>
#include <kin/renderer/render_queue.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/shape.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

class JobSystem;

struct ParticleSystemComponent;
struct ParticleFieldComponent;
struct TilemapComponent;
struct TextRenderer;
struct ShaderEffect;

// Where an entity sits relative to its parent (flecs ChildOf), or to the world
// without one: scaled, then turned (degrees, clockwise), then moved to `pos`.
// A child's position, renderer offsets and size go through all of its
// parents' transforms. Scale is per axis; under a turned parent it stays
// along the child's own axes (no shear), and a negative scale draws as its
// size, unmirrored.
struct Transform2D {
    Vec2f pos{};
    f32 rotation = 0.0f;
    Vec2f scale{1.0f, 1.0f};
};

// Transform2D composed through the hierarchy (WorldRenderState::propagate_transforms).
struct WorldTransform {
    Vec2f pos{};
    f32 rotation = 0.0f;
    Vec2f scale{1.0f, 1.0f};
};

// `child` (a Transform2D) under `parent` (a WorldTransform).
WorldTransform compose(const WorldTransform& parent, const Transform2D& child);
// The reverse: the Transform2D that puts a child at `world` under `parent`
// (compose(parent, to_local(parent, world)) == world). An axis the parent
// scales to zero keeps the child at the parent's origin along it.
Transform2D to_local(const WorldTransform& parent, const WorldTransform& world);

// A sprite of a SpriteCatalog drawn at the entity. `sprite` is set from C++,
// or by Assets2D from `asset`, a catalog's path in the game's content (a
// .kinsprites file), keeping `sprite.id`: which of its sprites.
struct SpriteRenderer {
    SpriteRef sprite;
    ComponentAssetRef asset;
    Vec2f offset{};
    Vec2f size{};
    Vec2f pivot{-1.0f, -1.0f};
    Color tint = colors::white;
    f32 rotation = 0.0f;
    i32 layer = layer_value(RenderLayer::World);
    i32 order = 0;
    bool y_sort = false;
    f32 sort_y_offset = 0.0f;
    bool visible = true;
    bool static_renderable = false;
    // Mirrored about the pivot, which stays put: left-right, upside down. A
    // negative Transform2D scale on an axis mirrors it too (both cancel out).
    bool flip_x = false;
    bool flip_y = false;
    // How it combines with what is under it: Additive for glows, Multiply for shade.
    BlendMode blend = BlendMode::Alpha;
};

// A texture, or a region of one (`source`; empty: all of it), drawn at the
// entity. `texture` is set from C++, or by Assets2D from `asset`: an image's
// path in the game's content, as a scene file names it (EcsScene binds it
// before drawing; see kin/ecs/assets2d.hpp).
struct TextureRenderer {
    Texture texture;
    ComponentAssetRef asset;
    Rectf source{};
    Vec2f offset{};
    Vec2f size{};
    Vec2f pivot{};
    Color tint = colors::white;
    i32 layer = layer_value(RenderLayer::World);
    i32 order = 0;
    bool y_sort = false;
    f32 sort_y_offset = 0.0f;
    bool visible = true;
    bool static_renderable = false;
    // As SpriteRenderer's.
    bool flip_x = false;
    bool flip_y = false;
    BlendMode blend = BlendMode::Alpha;
};

struct RectRenderer {
    Vec2f offset{};
    Vec2f size{};
    Color color = colors::white;
    i32 layer = layer_value(RenderLayer::World);
    i32 order = 0;
    bool y_sort = false;
    f32 sort_y_offset = 0.0f;
    bool outline = false;
    bool visible = true;
    bool static_renderable = false;
    BlendMode blend = BlendMode::Alpha;
};

struct LineRenderer {
    Vec2f a{};
    Vec2f b{};
    Color color = colors::white;
    i32 layer = layer_value(RenderLayer::World);
    i32 order = 0;
    bool y_sort = false;
    f32 sort_y_offset = 0.0f;
    bool visible = true;
    bool static_renderable = false;
};

// What a ShapeRenderer's mesh is made of, as scene files give it: a box, a
// box with rounded corners, an ellipse, a regular polygon (`sides`), each
// `size` big and centred on the entity, or an SVG-lite file of the content
// (kin/renderer/svg.hpp; `size` zero: as big as it says, else fitted in it).
// Filled with `fill`, and outlined `stroke_width` wide in `stroke`.
enum class ShapeKind : u8 { None, Rect, RoundedRect, Ellipse, Polygon, Svg };

struct ShapeSpec {
    ShapeKind kind = ShapeKind::None; // None: `mesh` is set from C++
    Vec2f size{32.0f, 32.0f};
    f32 radius = 4.0f; // RoundedRect's corners
    i32 sides = 6;     // Polygon's (3 or more)
    Color fill = colors::white;
    Color stroke = colors::transparent;
    f32 stroke_width = 0.0f;

    friend bool operator==(const ShapeSpec&, const ShapeSpec&) = default;
};

// "none", "rect", "rounded_rect", "ellipse", "polygon", "svg".
std::string_view shape_kind_name(ShapeKind kind);
std::optional<ShapeKind> shape_kind_named(std::string_view name);
// The shape `spec` says (`svg`: the file's shape, for Svg), centred on (0, 0).
Shape make_shape(const ShapeSpec& spec, const Shape* svg = nullptr);

// A vector shape (kin/renderer/shape.hpp) drawn at the entity: its mesh is
// placed by the entity's world transform (a negative scale mirrors it) and
// `offset` in the entity's own units, and coloured by `tint`. Meshes are
// shared: many entities can draw one. Set from C++, or made from `spec` (and
// `asset`, an SVG file) by Assets2D as they change (bind_shape_renderers).
struct ShapeRenderer {
    std::shared_ptr<const ShapeMesh> mesh;
    ShapeSpec spec;
    ComponentAssetRef asset; // spec.kind Svg: an .svg file of the content
    Vec2f offset{};
    Color tint = colors::white;
    i32 layer = layer_value(RenderLayer::World);
    i32 order = 0;
    bool y_sort = false;
    f32 sort_y_offset = 0.0f;
    bool visible = true;
    bool static_renderable = false;
    BlendMode blend = BlendMode::Alpha;

    // What `mesh` was made from (kept by bind_shape_renderers).
    ShapeSpec made{};
    const void* made_from = nullptr; // the SVG's shape
};

// Cuts the renderers of its entity and of every entity below it (ChildOf) to
// `clip`, given in the entity's own space: it moves, turns and scales with it.
// They draw together, as one group at this layer and order (sorted among
// themselves by their own), so a sorted world cannot interleave other things
// between them. Groups nest. Clipped renderers are collected with the dynamic
// ones, whatever their static_renderable says.
//
//     window.set(kin::ClipGroup{.clip = kin::ClipRegion::to_path(kin::Path::rounded_rect({-60, -40, 120, 80}, 12))});
//     // the window's children only show inside its rounded frame
//
// Scene files give its clip as a shape (`shape`: a box, a rounded box or an
// ellipse, `size` big, centred `offset` from the entity); reshape_clip()
// makes `clip` of them, as the described fields change.
enum class ClipShape : u8 { None, Rect, RoundedRect, Ellipse };

struct ClipGroup {
    ClipRegion clip;
    i32 layer = layer_value(RenderLayer::World);
    i32 order = 0;
    bool y_sort = false;
    f32 sort_y_offset = 0.0f;

    ClipShape shape = ClipShape::None; // None: `clip` is set from C++
    Vec2f size{64.0f, 64.0f};
    Vec2f offset{};
    f32 radius = 8.0f; // RoundedRect's corners
};

// `group.clip` made of its shape (one of None cuts nothing).
void reshape_clip(ClipGroup& group);

struct StaticRenderable {
    i32 marker = 1;
};

struct SpriteRenderOptions {
    bool sort = true;
    RenderSortMode sort_mode = RenderSortMode::LayerThenY;
    const RenderView* view = nullptr;
    // Workers for large tables: collect_static/collect_dynamic prepare texture
    // sprites in parallel chunks and queue them in order, so the result is the same.
    JobSystem* jobs = nullptr;
};

struct TopDownRenderOptions {
    const Camera2D* camera = nullptr;
    bool sort = true;
    f32 cull_padding = 32.0f;
    bool culling_enabled = true;
    Rectf viewport{};
};

#ifdef KIN_ENABLE_RENDER_PROBE
// The render probe's source id for draws of `component` of `entity` (0 when no
// probe is running). The submit_* functions below claim their draws this way;
// KIN_DRAW_ENTITY does it for code that draws an entity itself.
u32 draw_source_of_entity(DrawTrace& trace, flecs::entity entity, std::string_view component);
inline u32 draw_source_for(flecs::entity entity, std::string_view component) {
    DrawTrace* trace = active_draw_trace();
    return trace ? draw_source_of_entity(*trace, entity, component) : 0;
}

#define KIN_DRAW_ENTITY(entity, component)                                                             \
    const ::kin::DrawSourceScope KIN_DRAW_TRACE_CONCAT(kin_draw_entity_, __LINE__) {                   \
        ::kin::draw_source_for(entity, component)                                                      \
    }
#else
#define KIN_DRAW_ENTITY(entity, component) static_cast<void>(0)
#endif

using EcsRenderFilter = std::function<bool(flecs::entity)>;
using EcsRenderCollector = std::function<void(EcsWorld&, RenderQueue&)>;

class WorldRenderState {
public:
    explicit WorldRenderState(flecs::world& world);
    explicit WorldRenderState(EcsWorld& world)
        : WorldRenderState(world.raw()) {
    }

    flecs::world& world() const { return *_world; }
    bool matches(flecs::world& world) const { return _world == &world; }

    void propagate_transforms();
    void collect_all(RenderQueue& queue, const EcsRenderFilter& include = {}, SpriteRenderOptions options = {});
    void collect_static(RenderQueue& queue, SpriteRenderOptions options = {});
    void collect_dynamic(RenderQueue& queue, SpriteRenderOptions options = {});

private:
    // TextureRenderer entities (static or dynamic ones), in parallel when options.jobs is set.
    void collect_textures(RenderQueue& queue, const SpriteRenderOptions& options, bool statics);
    // Clip groups (ClipGroup): each one's queue, made or reused for this collect.
    RenderQueue& group_queue(flecs::entity owner, const SpriteRenderOptions& options);
    // Where row `row` of a table goes: its group's queue (its own ClipGroup or
    // the nearest above it, field `field`), or `queue`.
    RenderQueue& route(RenderQueue& queue, flecs::iter& it, std::size_t row, i8 field, const SpriteRenderOptions& options);
    // Each group used, as one command in the group above it or in `queue`.
    void submit_groups(RenderQueue& queue, const SpriteRenderOptions& options);

    flecs::world* _world = nullptr;
    flecs::observer _transform_observer;
    // Cache-line aligned: each is written by one worker, and neighbours must not
    // share a line (false sharing made the parallel path slower than one thread).
    struct alignas(64) TextureChunk {
        std::size_t count = 0;                // sprites this chunk queues
        std::size_t first = 0;                // its first slot in the reserved block
        std::vector<const Texture*> textures; // distinct, in first-use order
        std::vector<u32> slots;               // their queue texture indices
    };
    std::vector<TextureChunk> _chunks; // per parallel chunk, reused
    // Own transform, local transform, and the parent's world transform (optional,
    // cascaded so parents are visited before their children).
    flecs::query<WorldTransform, const Transform2D, const WorldTransform> _transforms;
    // Each renderer with its ClipGroup, on itself or the nearest entity above
    // (optional: field 3, or 1 for particles).
    flecs::query<const Transform2D, const WorldTransform, const SpriteRenderer, const ClipGroup*> _sprites;
    flecs::query<const Transform2D, const WorldTransform, const TextureRenderer, const ClipGroup*> _textures;
    flecs::query<const Transform2D, const WorldTransform, const RectRenderer, const ClipGroup*> _rects;
    flecs::query<const Transform2D, const WorldTransform, const LineRenderer, const ClipGroup*> _lines;
    flecs::query<const Transform2D, const WorldTransform, const ShapeRenderer, const ClipGroup*> _shapes;
    flecs::query<const ParticleSystemComponent, const ClipGroup*> _particle_systems;
    flecs::query<const ParticleFieldComponent, const ClipGroup*> _particle_fields;
    flecs::query<const Transform2D, const WorldTransform, const TilemapComponent, const ClipGroup*> _tilemaps;
    flecs::query<const Transform2D, const WorldTransform, const TextRenderer, const ClipGroup*> _texts;
    flecs::query<const Transform2D, const WorldTransform, const ShaderEffect, const ClipGroup*> _shader_effects;
    // Clip groups' queues by owner; those used by the current collect, in order.
    std::unordered_map<u64, std::shared_ptr<RenderQueue>> _group_queues;
    std::vector<u64> _groups_used;
};

class StaticRenderCache {
public:
    bool dirty() const { return _dirty; }
    bool empty() const { return _queue.empty(); }
    std::size_t size() const { return _queue.size(); }
    std::span<const RenderCommand> commands() const { return _queue.commands(); }

    void mark_dirty(std::string_view reason = "explicit");
    std::string_view dirty_reason() const { return _dirty_reason; }
    void clear();
    void rebuild(EcsWorld& world, const EcsRenderCollector& collector, RenderSortMode sort = RenderSortMode::LayerThenY);
    void flush(Renderer2D& renderer, const RenderView& view, u64 pass_mask = render_pass_mask::all) const;
    void flush_merged(Renderer2D& renderer,
                      const RenderQueue& dynamic_queue,
                      const RenderView& view,
                      u64 pass_mask = render_pass_mask::all) const;

private:
    RenderQueue _queue{RenderSortMode::LayerThenY};
    bool _dirty = true;
    std::string _dirty_reason = "initial";
};

Vec2f world_position(flecs::entity entity);
Vec2f world_position(const EcsEntity& entity);
// The entity's WorldTransform when it has one, else its Transform2Ds composed
// up the hierarchy.
WorldTransform world_transform(flecs::entity entity);
WorldTransform world_transform(const EcsEntity& entity);
// Its Transform2Ds composed up the hierarchy now, whatever WorldTransform says
// (that is only as fresh as the last propagate_transforms()).
WorldTransform current_world_transform(flecs::entity entity);
WorldTransform current_world_transform(const EcsEntity& entity);

bool submit_sprite(Renderer2D& renderer, flecs::entity entity);
bool submit_texture(Renderer2D& renderer, flecs::entity entity);
bool submit_rect(Renderer2D& renderer, flecs::entity entity);
bool submit_line(Renderer2D& renderer, flecs::entity entity);
bool submit_shape(Renderer2D& renderer, flecs::entity entity);
bool submit_sprite(RenderQueue& queue, flecs::entity entity);
bool submit_texture(RenderQueue& queue, flecs::entity entity);
bool submit_rect(RenderQueue& queue, flecs::entity entity);
bool submit_line(RenderQueue& queue, flecs::entity entity);
bool submit_shape(RenderQueue& queue, flecs::entity entity);
bool submit_particles(RenderQueue& queue, flecs::entity entity);
bool submit_sprite(RenderQueue& queue,
                   flecs::entity entity,
                   const WorldTransform& transform,
                   const SpriteRenderer& sprite,
                   const RenderView* view = nullptr);
bool submit_texture(RenderQueue& queue,
                    flecs::entity entity,
                    const WorldTransform& transform,
                    const TextureRenderer& texture,
                    const RenderView* view = nullptr);
bool submit_rect(RenderQueue& queue,
                 flecs::entity entity,
                 const WorldTransform& transform,
                 const RectRenderer& rect,
                 const RenderView* view = nullptr);
bool submit_line(RenderQueue& queue,
                 flecs::entity entity,
                 const WorldTransform& transform,
                 const LineRenderer& line,
                 const RenderView* view = nullptr);
bool submit_shape(RenderQueue& queue,
                  flecs::entity entity,
                  const WorldTransform& transform,
                  const ShapeRenderer& shape,
                  const RenderView* view = nullptr);
// Text (kin/ecs/text.hpp) drawn through `view`'s camera, as the world is:
// its callback puts the camera back on (text is drawn in screen space).
bool submit_text(RenderQueue& queue,
                 flecs::entity entity,
                 const WorldTransform& transform,
                 const TextRenderer& text,
                 const RenderView* view = nullptr);
bool submit_shader_effect(RenderQueue& queue,
                          flecs::entity entity,
                          const WorldTransform& transform,
                          const ShaderEffect& effect,
                          const RenderView* view = nullptr);
bool submit_particles(RenderQueue& queue, const ParticleSystemComponent& particles);
bool submit_particles(RenderQueue& queue, const ParticleFieldComponent& field);
// A tilemap (kin/ecs/tilemap.hpp) with its top left at `world`'s position;
// false when it has no map to draw, or is hidden.
bool submit_tilemap(RenderQueue& queue, const WorldTransform& world, const TilemapComponent& tilemap, const RenderView* view = nullptr);
void collect_world(flecs::world& world,
                   RenderQueue& queue,
                   const EcsRenderFilter& include = {},
                   SpriteRenderOptions options = {});
void collect_world(EcsWorld& world,
                   RenderQueue& queue,
                   const EcsRenderFilter& include = {},
                   SpriteRenderOptions options = {});
void collect_world(WorldRenderState& state,
                   RenderQueue& queue,
                   const EcsRenderFilter& include = {},
                   SpriteRenderOptions options = {});
void collect_static_world(EcsWorld& world, RenderQueue& queue, SpriteRenderOptions options = {});
void collect_static_world(flecs::world& world, RenderQueue& queue, SpriteRenderOptions options = {});
void collect_static_world(WorldRenderState& state, RenderQueue& queue, SpriteRenderOptions options = {});
void collect_dynamic_world(EcsWorld& world, RenderQueue& queue, SpriteRenderOptions options = {});
void collect_dynamic_world(flecs::world& world, RenderQueue& queue, SpriteRenderOptions options = {});
void collect_dynamic_world(WorldRenderState& state, RenderQueue& queue, SpriteRenderOptions options = {});

void render_world(EcsWorld& world, Renderer2D& renderer, SpriteRenderOptions options = {});
void render_world(flecs::world& world, Renderer2D& renderer, SpriteRenderOptions options = {});
void render_filtered(EcsWorld& world,
                     Renderer2D& renderer,
                     const EcsRenderFilter& include,
                     SpriteRenderOptions options = {});
void render_filtered(flecs::world& world,
                     Renderer2D& renderer,
                     const EcsRenderFilter& include,
                     SpriteRenderOptions options = {});
void render_top_down_world(EcsWorld& world, Renderer2D& renderer, TopDownRenderOptions options = {});
void render_top_down_world(flecs::world& world, Renderer2D& renderer, TopDownRenderOptions options = {});
void render_top_down_filtered(EcsWorld& world,
                              Renderer2D& renderer,
                              const EcsRenderFilter& include,
                              TopDownRenderOptions options = {});
void render_top_down_filtered(flecs::world& world,
                              Renderer2D& renderer,
                              const EcsRenderFilter& include,
                              TopDownRenderOptions options = {});

template <typename... Components>
void render_world_with(EcsWorld& world, Renderer2D& renderer, SpriteRenderOptions options = {}) {
    render_filtered(world, renderer, [](flecs::entity entity) {
        return (entity.has<Components>() && ...);
    }, options);
}

} // namespace kin
