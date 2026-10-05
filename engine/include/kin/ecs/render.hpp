#pragma once

#include <kin/core/types.hpp>
#include <kin/ecs/world.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/render_layer.hpp>
#include <kin/renderer/render_queue.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace kin {

class JobSystem;

struct ParticleSystemComponent;
struct ParticleFieldComponent;

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

struct SpriteRenderer {
    SpriteRef sprite;
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
};

struct TextureRenderer {
    Texture texture;
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

// A vector shape (kin/renderer/shape.hpp) drawn at the entity: its mesh is
// placed by the entity's world transform (a negative scale mirrors it) and
// `offset` in the entity's own units, and coloured by `tint`. Meshes are
// shared: many entities can draw one.
struct ShapeRenderer {
    std::shared_ptr<const ShapeMesh> mesh;
    Vec2f offset{};
    Color tint = colors::white;
    i32 layer = layer_value(RenderLayer::World);
    i32 order = 0;
    bool y_sort = false;
    f32 sort_y_offset = 0.0f;
    bool visible = true;
    bool static_renderable = false;
};

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
    flecs::query<const Transform2D, const WorldTransform, const SpriteRenderer> _sprites;
    flecs::query<const Transform2D, const WorldTransform, const TextureRenderer> _textures;
    flecs::query<const Transform2D, const WorldTransform, const RectRenderer> _rects;
    flecs::query<const Transform2D, const WorldTransform, const LineRenderer> _lines;
    flecs::query<const Transform2D, const WorldTransform, const ShapeRenderer> _shapes;
    flecs::query<const ParticleSystemComponent> _particle_systems;
    flecs::query<const ParticleFieldComponent> _particle_fields;
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
bool submit_particles(RenderQueue& queue, const ParticleSystemComponent& particles);
bool submit_particles(RenderQueue& queue, const ParticleFieldComponent& field);
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
