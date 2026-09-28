#include <kin/ecs/render.hpp>

#include <kin/ecs/particles.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/render_profile.hpp>

#include <algorithm>
#include <string>

namespace kin {
namespace {

Vec2f add(Vec2f a, Vec2f b) {
    return {a.x + b.x, a.y + b.y};
}

Vec2f sub(Vec2f a, Vec2f b) {
    return {a.x - b.x, a.y - b.y};
}

Vec2f mul(Vec2f a, Vec2f b) {
    return {a.x * b.x, a.y * b.y};
}

Vec2f resolved_size(const SpriteRenderer& sprite, const ResolvedSprite& resolved) {
    if (sprite.size.x > 0.0f && sprite.size.y > 0.0f) {
        return sprite.size;
    }
    return resolved.size;
}

Vec2f resolved_pivot(const SpriteRenderer& sprite, const ResolvedSprite& resolved) {
    if (sprite.pivot.x >= 0.0f && sprite.pivot.y >= 0.0f) {
        return sprite.pivot;
    }
    return resolved.pivot;
}

bool resolve_sprite_renderer(const SpriteRenderer& sprite, ResolvedSprite& resolved) {
    return sprite.visible && sprite.sprite.valid() && sprite.sprite.catalog->resolve(sprite.sprite.id, resolved);
}

Vec2f sprite_anchor_pos(Vec2f world_pos, const SpriteRenderer& sprite, const ResolvedSprite& resolved) {
    return add(add(world_pos, resolved.offset), sprite.offset);
}

Rectf sprite_draw_rect(Vec2f world_pos, const SpriteRenderer& sprite, const ResolvedSprite& resolved) {
    const Vec2f size = resolved_size(sprite, resolved);
    const Vec2f pivot = resolved_pivot(sprite, resolved);
    const Vec2f top_left = sub(sprite_anchor_pos(world_pos, sprite, resolved), mul(size, pivot));
    return {top_left.x, top_left.y, size.x, size.y};
}

Rectf texture_source_rect(const TextureRenderer& texture) {
    if (texture.source.w > 0.0f && texture.source.h > 0.0f) {
        return texture.source;
    }
    const Vec2i size = texture.texture.size();
    return {0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)};
}

Vec2f texture_size(const TextureRenderer& texture) {
    if (texture.size.x > 0.0f && texture.size.y > 0.0f) {
        return texture.size;
    }
    const Rectf source = texture_source_rect(texture);
    return {source.w, source.h};
}

Rectf texture_draw_rect(Vec2f world_pos, const TextureRenderer& texture) {
    const Vec2f size = texture_size(texture);
    const Vec2f top_left = sub(add(world_pos, texture.offset), mul(size, texture.pivot));
    return {top_left.x, top_left.y, size.x, size.y};
}

u64 pass_mask_for_layer(i32 layer) {
    if (layer >= layer_value(RenderLayer::Debug)) {
        return render_pass_mask::debug;
    }
    if (layer >= layer_value(RenderLayer::UI)) {
        return render_pass_mask::ui;
    }
    if (layer >= layer_value(RenderLayer::Effects)) {
        return render_pass_mask::effects;
    }
    return render_pass_mask::world;
}

RenderKey key_for(i32 layer, i32 order, bool y_sort, f32 y) {
    return {
        .layer = layer,
        .order = order,
        .y = y,
        .use_y = y_sort,
        .pass_mask = pass_mask_for_layer(layer),
    };
}

} // namespace

WorldRenderState::WorldRenderState(flecs::world& world)
    : _world(&world),
      _transform_observer(world.observer<Transform2D>("add_world_transform")
                              .event(flecs::OnAdd)
                              .yield_existing()
                              .each([](flecs::entity entity, Transform2D&) {
                                  entity.ensure<WorldTransform>();
                              })),
      _transforms(world.query_builder<WorldTransform, const Transform2D>()
                      .cached()
                      .build()),
      _sprites(world.query_builder<const Transform2D, const WorldTransform, const SpriteRenderer>().cached().build()),
      _textures(world.query_builder<const Transform2D, const WorldTransform, const TextureRenderer>().cached().build()),
      _rects(world.query_builder<const Transform2D, const WorldTransform, const RectRenderer>().cached().build()),
      _lines(world.query_builder<const Transform2D, const WorldTransform, const LineRenderer>().cached().build()),
      _particle_systems(world.query_builder<const ParticleSystemComponent>().cached().build()),
      _particle_fields(world.query_builder<const ParticleFieldComponent>().cached().build()) {
}

void WorldRenderState::propagate_transforms() {
    _transforms.run([](flecs::iter& it) {
        while (it.next()) {
            for (auto i : it) {
                auto& self = it.field_at<WorldTransform>(0, i);
                const auto& local = it.field_at<const Transform2D>(1, i);
                self.pos = local.pos;
                self.rotation = local.rotation;
                const flecs::entity entity = it.entity(i);
                const flecs::entity parent_entity = entity.parent();
                if (parent_entity) {
                    const auto* parent = parent_entity.get<WorldTransform>();
                    if (parent != nullptr) {
                        self.pos = add(self.pos, parent->pos);
                        self.rotation += parent->rotation;
                    }
                }
            }
        }
    });
}

void WorldRenderState::collect_all(RenderQueue& queue,
                                   const EcsRenderFilter& include,
                                   SpriteRenderOptions options) {
    queue.set_sort(options.sort ? options.sort_mode : RenderSortMode::Submission);
    _sprites.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const SpriteRenderer& sprite) {
        if (!include || include(entity)) {
            submit_sprite(queue, entity, transform, sprite, options.view);
        }
    });
    _textures.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const TextureRenderer& texture) {
        if (!include || include(entity)) {
            submit_texture(queue, entity, transform, texture, options.view);
        }
    });
    _rects.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const RectRenderer& rect) {
        if (!include || include(entity)) {
            submit_rect(queue, entity, transform, rect, options.view);
        }
    });
    _lines.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const LineRenderer& line) {
        if (!include || include(entity)) {
            submit_line(queue, entity, transform, line, options.view);
        }
    });
    _particle_systems.each([&](flecs::entity entity, const ParticleSystemComponent& particles) {
        if (!include || include(entity)) {
            submit_particles(queue, particles);
        }
    });
    _particle_fields.each([&](flecs::entity entity, const ParticleFieldComponent& field) {
        if (!include || include(entity)) {
            submit_particles(queue, field);
        }
    });
    if (options.view) {
        queue.cull(*options.view);
    }
}

void WorldRenderState::collect_static(RenderQueue& queue, SpriteRenderOptions options) {
    queue.set_sort(options.sort ? options.sort_mode : RenderSortMode::Submission);
    _sprites.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const SpriteRenderer& sprite) {
        if (sprite.static_renderable) {
            submit_sprite(queue, entity, transform, sprite, options.view);
        }
    });
    _textures.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const TextureRenderer& texture) {
        if (texture.static_renderable) {
            submit_texture(queue, entity, transform, texture, options.view);
        }
    });
    _rects.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const RectRenderer& rect) {
        if (rect.static_renderable) {
            submit_rect(queue, entity, transform, rect, options.view);
        }
    });
    _lines.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const LineRenderer& line) {
        if (line.static_renderable) {
            submit_line(queue, entity, transform, line, options.view);
        }
    });
    if (options.view) {
        queue.cull(*options.view);
    }
}

void WorldRenderState::collect_dynamic(RenderQueue& queue, SpriteRenderOptions options) {
    queue.set_sort(options.sort ? options.sort_mode : RenderSortMode::Submission);
    _sprites.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const SpriteRenderer& sprite) {
        if (!sprite.static_renderable) {
            submit_sprite(queue, entity, transform, sprite, options.view);
        }
    });
    _textures.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const TextureRenderer& texture) {
        if (!texture.static_renderable) {
            submit_texture(queue, entity, transform, texture, options.view);
        }
    });
    _rects.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const RectRenderer& rect) {
        if (!rect.static_renderable) {
            submit_rect(queue, entity, transform, rect, options.view);
        }
    });
    _lines.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const LineRenderer& line) {
        if (!line.static_renderable) {
            submit_line(queue, entity, transform, line, options.view);
        }
    });
    _particle_systems.each([&](flecs::entity, const ParticleSystemComponent& particles) {
        submit_particles(queue, particles);
    });
    _particle_fields.each([&](flecs::entity, const ParticleFieldComponent& field) {
        submit_particles(queue, field);
    });
    if (options.view) {
        queue.cull(*options.view);
    }
}

Vec2f world_position(flecs::entity entity) {
    if (const auto* transform = entity.get<WorldTransform>()) {
        return transform->pos;
    }

    Vec2f pos{};
    for (flecs::entity current = entity; current; current = current.parent()) {
        if (const auto* transform = current.get<Transform2D>()) {
            pos = add(pos, transform->pos);
        }
    }
    return pos;
}

Vec2f world_position(const EcsEntity& entity) {
    return world_position(entity.raw());
}

bool submit_sprite(RenderQueue& queue, flecs::entity entity) {
    const auto* sprite = entity.get<SpriteRenderer>();
    if (!sprite) {
        return false;
    }

    const Vec2f pos = world_position(entity);
    const WorldTransform transform{pos};
    return submit_sprite(queue, entity, transform, *sprite);
}

bool submit_sprite(RenderQueue& queue,
                   flecs::entity,
                   const WorldTransform& transform,
                   const SpriteRenderer& sprite,
                   const RenderView* view) {
    ResolvedSprite resolved;
    if (!resolve_sprite_renderer(sprite, resolved)) {
        return false;
    }

    const Vec2f pos = transform.pos;
    const Rectf dest = sprite_draw_rect(pos, sprite, resolved);
    const f32 rotation = transform.rotation + sprite.rotation;
    if (view && rotation == 0.0f && !render_view_visible(*view, dest)) {
        return false;
    }
    const RenderKey key = key_for(sprite.layer,
                                  sprite.order,
                                  sprite.y_sort,
                                  sprite_anchor_pos(pos, sprite, resolved).y + sprite.sort_y_offset);
    queue.draw_sprite(key, resolved.sprite, dest, sprite.tint, {}, rotation, resolved_pivot(sprite, resolved));
    return true;
}

bool submit_texture(RenderQueue& queue, flecs::entity entity) {
    const auto* texture = entity.get<TextureRenderer>();
    if (!texture || !texture->visible || !texture->texture) {
        return false;
    }

    const WorldTransform transform{world_position(entity)};
    return submit_texture(queue, entity, transform, *texture);
}

bool submit_texture(RenderQueue& queue,
                    flecs::entity,
                    const WorldTransform& transform,
                    const TextureRenderer& texture,
                    const RenderView* view) {
    if (!texture.visible || !texture.texture) {
        return false;
    }

    const Vec2f pos = transform.pos;
    const Rectf dest = texture_draw_rect(pos, texture);
    if (view && !render_view_visible(*view, dest)) {
        return false;
    }
    const RenderKey key = key_for(texture.layer,
                                  texture.order,
                                  texture.y_sort,
                                  pos.y + texture.offset.y + texture.sort_y_offset);
    queue.submit({
        .type = RenderCommandType::Texture,
        .key = key,
        .rect = dest,
        .source = texture_source_rect(texture),
        .color = texture.tint,
        .texture = texture.texture,
    });
    return true;
}

bool submit_rect(RenderQueue& queue, flecs::entity entity) {
    const auto* rect = entity.get<RectRenderer>();
    if (!rect || !rect->visible || rect->size.x <= 0.0f || rect->size.y <= 0.0f) {
        return false;
    }

    const WorldTransform transform{world_position(entity)};
    return submit_rect(queue, entity, transform, *rect);
}

bool submit_rect(RenderQueue& queue,
                 flecs::entity,
                 const WorldTransform& transform,
                 const RectRenderer& rect,
                 const RenderView* view) {
    if (!rect.visible || rect.size.x <= 0.0f || rect.size.y <= 0.0f) {
        return false;
    }

    const Vec2f pos = transform.pos;
    const Rectf bounds{pos.x + rect.offset.x, pos.y + rect.offset.y, rect.size.x, rect.size.y};
    if (view && !render_view_visible(*view, bounds)) {
        return false;
    }
    const RenderKey key = key_for(rect.layer,
                                  rect.order,
                                  rect.y_sort,
                                  pos.y + rect.offset.y + rect.sort_y_offset);
    if (rect.outline) {
        queue.draw_rect(key, bounds, rect.color);
    } else {
        queue.fill_rect(key, bounds, rect.color);
    }
    return true;
}

bool submit_line(RenderQueue& queue, flecs::entity entity) {
    const auto* line = entity.get<LineRenderer>();
    if (!line || !line->visible) {
        return false;
    }

    const WorldTransform transform{world_position(entity)};
    return submit_line(queue, entity, transform, *line);
}

bool submit_line(RenderQueue& queue,
                 flecs::entity,
                 const WorldTransform& transform,
                 const LineRenderer& line,
                 const RenderView* view) {
    if (!line.visible) {
        return false;
    }

    const Vec2f pos = transform.pos;
    const Vec2f a = add(pos, line.a);
    const Vec2f b = add(pos, line.b);
    const Rectf bounds{
        std::min(a.x, b.x), std::min(a.y, b.y),
        std::abs(a.x - b.x), std::abs(a.y - b.y),
    };
    if (view && !render_view_visible(*view, bounds)) {
        return false;
    }
    const RenderKey key = key_for(line.layer,
                                  line.order,
                                  line.y_sort,
                                  pos.y + line.sort_y_offset);
    queue.draw_line(key, a, b, line.color);
    return true;
}

bool submit_particles(RenderQueue& queue, flecs::entity entity) {
    bool submitted = false;
    if (const auto* particles = entity.get<ParticleSystemComponent>()) {
        submitted = submit_particles(queue, *particles) || submitted;
    }
    if (const auto* field = entity.get<ParticleFieldComponent>()) {
        submitted = submit_particles(queue, *field) || submitted;
    }
    return submitted;
}

bool submit_particles(RenderQueue& queue, const ParticleSystemComponent& particles) {
    return submit_particles(queue,
                            particles.system.particles(),
                            {
                                .pixel_policy = particles.system.pixel_policy(),
                                .pixel_grid = particles.system.pixel_grid(),
                                .sprites = particles.sprites,
                                .sort = queue.sort() != RenderSortMode::Submission,
                            });
}

bool submit_particles(RenderQueue& queue, const ParticleFieldComponent& field) {
    std::vector<Particle> particles;
    particles.reserve(field.state.particles.size());
    for (const ParticleFieldParticle& field_particle : field.state.particles) {
        particles.push_back(to_particle(field.state.definition, field_particle));
    }
    return submit_particles(queue,
                            particles,
                            {
                                .pixel_policy = field.state.pixel_policy,
                                .pixel_grid = field.state.pixel_grid,
                                .sprites = field.sprites,
                                .sort = queue.sort() != RenderSortMode::Submission,
                            });
}

bool submit_sprite(Renderer2D& renderer, flecs::entity entity) {
    thread_local RenderQueue queue{RenderSortMode::Submission};
    queue.clear();
    queue.set_sort(RenderSortMode::Submission);
    if (!submit_sprite(queue, entity)) {
        return false;
    }
    queue.flush(renderer);
    return true;
}

bool submit_texture(Renderer2D& renderer, flecs::entity entity) {
    thread_local RenderQueue queue{RenderSortMode::Submission};
    queue.clear();
    queue.set_sort(RenderSortMode::Submission);
    if (!submit_texture(queue, entity)) {
        return false;
    }
    queue.flush(renderer);
    return true;
}

bool submit_rect(Renderer2D& renderer, flecs::entity entity) {
    thread_local RenderQueue queue{RenderSortMode::Submission};
    queue.clear();
    queue.set_sort(RenderSortMode::Submission);
    if (!submit_rect(queue, entity)) {
        return false;
    }
    queue.flush(renderer);
    return true;
}

bool submit_line(Renderer2D& renderer, flecs::entity entity) {
    thread_local RenderQueue queue{RenderSortMode::Submission};
    queue.clear();
    queue.set_sort(RenderSortMode::Submission);
    if (!submit_line(queue, entity)) {
        return false;
    }
    queue.flush(renderer);
    return true;
}

void collect_world(EcsWorld& world,
                   RenderQueue& queue,
                   const EcsRenderFilter& include,
                   SpriteRenderOptions options) {
    WorldRenderState state{world};
    state.propagate_transforms();
    state.collect_all(queue, include, options);
}

void collect_world(flecs::world& world,
                   RenderQueue& queue,
                   const EcsRenderFilter& include,
                   SpriteRenderOptions options) {
    WorldRenderState state{world};
    state.propagate_transforms();
    state.collect_all(queue, include, options);
}

void collect_world(WorldRenderState& state,
                   RenderQueue& queue,
                   const EcsRenderFilter& include,
                   SpriteRenderOptions options) {
    state.propagate_transforms();
    state.collect_all(queue, include, options);
}

void collect_static_world(EcsWorld& world, RenderQueue& queue, SpriteRenderOptions options) {
    WorldRenderState state{world};
    state.propagate_transforms();
    state.collect_static(queue, options);
}

void collect_static_world(flecs::world& world, RenderQueue& queue, SpriteRenderOptions options) {
    WorldRenderState state{world};
    state.propagate_transforms();
    state.collect_static(queue, options);
}

void collect_static_world(WorldRenderState& state, RenderQueue& queue, SpriteRenderOptions options) {
    state.propagate_transforms();
    state.collect_static(queue, options);
}

void collect_dynamic_world(EcsWorld& world, RenderQueue& queue, SpriteRenderOptions options) {
    WorldRenderState state{world};
    state.propagate_transforms();
    state.collect_dynamic(queue, options);
}

void collect_dynamic_world(flecs::world& world, RenderQueue& queue, SpriteRenderOptions options) {
    WorldRenderState state{world};
    state.propagate_transforms();
    state.collect_dynamic(queue, options);
}

void collect_dynamic_world(WorldRenderState& state, RenderQueue& queue, SpriteRenderOptions options) {
    state.propagate_transforms();
    state.collect_dynamic(queue, options);
}

void StaticRenderCache::mark_dirty(std::string_view reason) {
    _dirty = true;
    _dirty_reason = reason.empty() ? "explicit" : std::string{reason};
    KIN_LOG_DEBUG_F("render",
                    "static render cache marked dirty",
                    (LogFields{{.name = "reason", .value = _dirty_reason}}));
}

void StaticRenderCache::clear() {
    _queue.clear();
    mark_dirty("clear");
}

void StaticRenderCache::rebuild(EcsWorld& world, const EcsRenderCollector& collector, RenderSortMode sort) {
    const std::string reason = _dirty_reason;
    _queue.clear();
    _queue.set_sort(sort);
    if (collector) {
        collector(world, _queue);
    }
    _queue.sort_commands();
    _dirty = false;
    KIN_LOG_DEBUG_F("render",
                    "static render cache rebuilt",
                    (LogFields{{.name = "reason", .value = reason},
                               {.name = "count", .value = std::to_string(_queue.size())}}));
}

void StaticRenderCache::flush(Renderer2D& renderer, const RenderView& view, u64 pass_mask) const {
    _queue.flush_presorted(renderer, view, pass_mask);
}

void StaticRenderCache::flush_merged(Renderer2D& renderer,
                                     const RenderQueue& dynamic_queue,
                                     const RenderView& view,
                                     u64 pass_mask) const {
    _queue.flush_merged_presorted(renderer, dynamic_queue.commands(), view, pass_mask);
}

void render_world(EcsWorld& world, Renderer2D& renderer, SpriteRenderOptions options) {
    render_world(world.raw(), renderer, options);
}

void render_world(flecs::world& world, Renderer2D& renderer, SpriteRenderOptions options) {
    render_filtered(world, renderer, {}, options);
}

void render_filtered(EcsWorld& world,
                     Renderer2D& renderer,
                     const EcsRenderFilter& include,
                     SpriteRenderOptions options) {
    render_filtered(world.raw(), renderer, include, options);
}

void render_filtered(flecs::world& world,
                     Renderer2D& renderer,
                     const EcsRenderFilter& include,
                     SpriteRenderOptions options) {
    RenderQueue queue{options.sort ? options.sort_mode : RenderSortMode::Submission};
    collect_world(world, queue, include, options);
    queue.flush(renderer);
}

void render_top_down_world(EcsWorld& world, Renderer2D& renderer, TopDownRenderOptions options) {
    render_top_down_world(world.raw(), renderer, options);
}

void render_top_down_world(flecs::world& world, Renderer2D& renderer, TopDownRenderOptions options) {
    render_top_down_filtered(world, renderer, {}, options);
}

void render_top_down_filtered(EcsWorld& world,
                              Renderer2D& renderer,
                              const EcsRenderFilter& include,
                              TopDownRenderOptions options) {
    render_top_down_filtered(world.raw(), renderer, include, options);
}

void render_top_down_filtered(flecs::world& world,
                              Renderer2D& renderer,
                              const EcsRenderFilter& include,
                              TopDownRenderOptions options) {
    const RenderProfile profile = top_down_2d_profile();
    RenderView view = profile.make_view(options.camera, options.viewport);
    view.cull_padding = options.cull_padding;
    view.culling_enabled = options.culling_enabled;
    if (options.camera) {
        view.cull_rect = options.camera->visible_rect(options.cull_padding);
    }

    RenderQueue queue{options.sort ? profile.sort : RenderSortMode::Submission};
    collect_world(world,
                  queue,
                  include,
                  {
                      .sort = options.sort,
                      .sort_mode = profile.sort,
                      .view = &view,
                  });
    queue.flush(renderer, view);
}

} // namespace kin
