#pragma once

#include <kin/core/types.hpp>
#include <kin/ecs/world.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/render_layer.hpp>
#include <kin/renderer/render_queue.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace kin {

struct ParticleSystemComponent;
struct ParticleFieldComponent;

struct Transform2D {
    Vec2f pos{};
    f32 rotation = 0.0f;
};

struct WorldTransform {
    Vec2f pos{};
    f32 rotation = 0.0f;
};

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

struct StaticRenderable {
    i32 marker = 1;
};

struct SpriteRenderOptions {
    bool sort = true;
    RenderSortMode sort_mode = RenderSortMode::LayerThenY;
    const RenderView* view = nullptr;
};

struct TopDownRenderOptions {
    const Camera2D* camera = nullptr;
    bool sort = true;
    f32 cull_padding = 32.0f;
    bool culling_enabled = true;
    Rectf viewport{};
};

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
    flecs::world* _world = nullptr;
    flecs::observer _transform_observer;
    flecs::query<WorldTransform, const Transform2D> _transforms;
    flecs::query<const Transform2D, const WorldTransform, const SpriteRenderer> _sprites;
    flecs::query<const Transform2D, const WorldTransform, const TextureRenderer> _textures;
    flecs::query<const Transform2D, const WorldTransform, const RectRenderer> _rects;
    flecs::query<const Transform2D, const WorldTransform, const LineRenderer> _lines;
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

bool submit_sprite(Renderer2D& renderer, flecs::entity entity);
bool submit_texture(Renderer2D& renderer, flecs::entity entity);
bool submit_rect(Renderer2D& renderer, flecs::entity entity);
bool submit_line(Renderer2D& renderer, flecs::entity entity);
bool submit_sprite(RenderQueue& queue, flecs::entity entity);
bool submit_texture(RenderQueue& queue, flecs::entity entity);
bool submit_rect(RenderQueue& queue, flecs::entity entity);
bool submit_line(RenderQueue& queue, flecs::entity entity);
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
