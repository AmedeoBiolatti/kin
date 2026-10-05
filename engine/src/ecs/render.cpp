#include <kin/ecs/render.hpp>

#include <kin/core/affine.hpp>
#include <kin/core/jobs.hpp>

#include <kin/ecs/particles.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/render_profile.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

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

Vec2f abs(Vec2f v) {
    return {std::abs(v.x), std::abs(v.y)};
}

bool plain(const WorldTransform& t) {
    return t.rotation == 0.0f && t.scale == Vec2f{1.0f, 1.0f};
}

// A point in the entity's own space, in the world.
Vec2f to_world(const WorldTransform& t, Vec2f local) {
    if (plain(t)) {
        return add(t.pos, local);
    }
    return add(t.pos, Affine2::trs({}, t.rotation, t.scale).apply_vector(local));
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

// Where the sprite's pivot lands: its offsets go through the transform.
Vec2f sprite_anchor_pos(const WorldTransform& t, const SpriteRenderer& sprite, const ResolvedSprite& resolved) {
    return to_world(t, add(resolved.offset, sprite.offset));
}

// The sprite unturned about its anchor, scaled.
Rectf sprite_draw_rect(const WorldTransform& t, const SpriteRenderer& sprite, const ResolvedSprite& resolved) {
    const Vec2f size = mul(resolved_size(sprite, resolved), abs(t.scale));
    const Vec2f pivot = resolved_pivot(sprite, resolved);
    const Vec2f top_left = sub(sprite_anchor_pos(t, sprite, resolved), mul(size, pivot));
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

// The texture unturned about its anchor (offset through the transform), scaled.
Rectf texture_draw_rect(const WorldTransform& t, const TextureRenderer& texture) {
    const Vec2f size = mul(texture_size(texture), abs(t.scale));
    const Vec2f top_left = sub(to_world(t, texture.offset), mul(size, texture.pivot));
    return {top_left.x, top_left.y, size.x, size.y};
}

// What a rectangle turned by `rotation` about `pivot` (a fraction of it) may cover.
Rectf turned_bounds(Rectf rect, f32 rotation, Vec2f pivot) {
    if (rotation == 0.0f) {
        return rect;
    }
    const Vec2f p{rect.x + rect.w * pivot.x, rect.y + rect.h * pivot.y};
    const f32 dx = std::max(std::abs(p.x - rect.x), std::abs(rect.x + rect.w - p.x));
    const f32 dy = std::max(std::abs(p.y - rect.y), std::abs(rect.y + rect.h - p.y));
    const f32 r = std::sqrt(dx * dx + dy * dy);
    return {p.x - r, p.y - r, 2 * r, 2 * r};
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
      _transforms(world.query_builder<WorldTransform, const Transform2D, const WorldTransform>()
                      .term_at(2).parent().cascade().optional()
                      .cached()
                      .build()),
      _sprites(world.query_builder<const Transform2D, const WorldTransform, const SpriteRenderer>().cached().build()),
      _textures(world.query_builder<const Transform2D, const WorldTransform, const TextureRenderer>().cached().build()),
      _rects(world.query_builder<const Transform2D, const WorldTransform, const RectRenderer>().cached().build()),
      _lines(world.query_builder<const Transform2D, const WorldTransform, const LineRenderer>().cached().build()),
      _shapes(world.query_builder<const Transform2D, const WorldTransform, const ShapeRenderer>().cached().build()),
      _particle_systems(world.query_builder<const ParticleSystemComponent>().cached().build()),
      _particle_fields(world.query_builder<const ParticleFieldComponent>().cached().build()) {
}

void WorldRenderState::propagate_transforms() {
    // Tables come in hierarchy depth order (cascade), and every entity in a table
    // shares its parent, so each table is one tight loop with no per-entity lookups.
    _transforms.run([](flecs::iter& it) {
        while (it.next()) {
            const auto self = it.field<WorldTransform>(0);
            const auto local = it.field<const Transform2D>(1);
            WorldTransform parent{};
            if (it.is_set(2)) {
                parent = it.field<const WorldTransform>(2)[0];
            }
            if (plain(parent)) {
                for (const auto i : it) {
                    self[i] = {add(parent.pos, local[i].pos), parent.rotation + local[i].rotation, local[i].scale};
                }
                continue;
            }
            const Affine2 linear = Affine2::trs({}, parent.rotation, parent.scale); // once per table
            for (const auto i : it) {
                self[i] = {add(parent.pos, linear.apply_vector(local[i].pos)), parent.rotation + local[i].rotation,
                           mul(parent.scale, local[i].scale)};
            }
        }
    });
}

void WorldRenderState::collect_all(RenderQueue& queue,
                                   const EcsRenderFilter& include,
                                   SpriteRenderOptions options) {
    const u64 existing = queue.submitted(); // the caller's commands, not culled yet
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
    _shapes.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const ShapeRenderer& shape) {
        if (!include || include(entity)) {
            submit_shape(queue, entity, transform, shape, options.view);
        }
    });
    // Everything above was culled as it was submitted; only particles still need it.
    const u64 unculled = queue.submitted();
    _particle_systems.each([&](flecs::entity entity, const ParticleSystemComponent& particles) {
        if (!include || include(entity)) {
            KIN_DRAW_ENTITY(entity, "ParticleSystemComponent");
            submit_particles(queue, particles);
        }
    });
    _particle_fields.each([&](flecs::entity entity, const ParticleFieldComponent& field) {
        if (!include || include(entity)) {
            KIN_DRAW_ENTITY(entity, "ParticleFieldComponent");
            submit_particles(queue, field);
        }
    });
    if (options.view) {
        queue.cull(*options.view, unculled);
        queue.cull(*options.view, 0, existing);
    }
}

void WorldRenderState::collect_static(RenderQueue& queue, SpriteRenderOptions options) {
    const u64 existing = queue.submitted(); // the caller's commands, not culled yet
    queue.set_sort(options.sort ? options.sort_mode : RenderSortMode::Submission);
    _sprites.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const SpriteRenderer& sprite) {
        if (sprite.static_renderable) {
            submit_sprite(queue, entity, transform, sprite, options.view);
        }
    });
    collect_textures(queue, options, true);
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
    _shapes.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const ShapeRenderer& shape) {
        if (shape.static_renderable) {
            submit_shape(queue, entity, transform, shape, options.view);
        }
    });
    if (options.view) {
        queue.cull(*options.view, 0, existing); // the rest was culled as it was submitted
    }
}

namespace {

// render_view_visible() with the view's rectangle worked out once, for loops.
class ViewCuller {
public:
    explicit ViewCuller(const RenderView* view)
        : _rect(view ? render_view_visible_rect(*view) : Rectf{}),
          _enabled(view && view->culling_enabled && _rect.w > 0.0f && _rect.h > 0.0f) {}

    bool visible(Rectf b) const {
        return !_enabled || (b.x + b.w >= _rect.x && b.y + b.h >= _rect.y && b.x <= _rect.x + _rect.w &&
                             b.y <= _rect.y + _rect.h);
    }

private:
    Rectf _rect;
    bool _enabled;
};

// submit_texture()'s work, into `out` instead of a queue: safe on a worker.
bool prepare_texture(const WorldTransform& transform, const TextureRenderer& texture, const ViewCuller& culler,
                     PreparedSprite& out) {
    if (!texture.visible || !texture.texture) {
        return false;
    }
    const Rectf dest = texture_draw_rect(transform, texture);
    if (!culler.visible(turned_bounds(dest, transform.rotation, texture.pivot))) {
        return false;
    }
    out = {
        .key = key_for(texture.layer, texture.order, texture.y_sort,
                       to_world(transform, texture.offset).y + texture.sort_y_offset),
        .texture = &texture.texture,
        .source = texture_source_rect(texture),
        .dest = dest,
        .tint = texture.tint,
        .rotation = transform.rotation,
        .pivot = texture.pivot,
    };
    return true;
}

} // namespace

void WorldRenderState::collect_textures(RenderQueue& queue, const SpriteRenderOptions& options, bool statics) {
    constexpr i32 parallel_min = 8192;
    constexpr i32 chunk = 1024; // small, so fast threads take over from slow ones
    const ViewCuller culler{options.view};
    _textures.run([&](flecs::iter& it) {
        while (it.next()) {
            const auto world = it.field<const WorldTransform>(1);
            const auto textures = it.field<const TextureRenderer>(2);
            const i32 rows = static_cast<i32>(it.count());
            if (!options.jobs || rows < parallel_min) {
                for (const auto i : it) {
                    if (textures[i].static_renderable == statics) {
                        submit_texture(queue, it.entity(i), world[i], textures[i], options.view);
                    }
                }
                continue;
            }
            // In parallel, with the same result as the loop above: the workers count
            // each chunk's sprites and note its textures; the queue then makes room for
            // all of them at once; and the workers write each sprite into its slot.
            const i32 chunks = (rows + chunk - 1) / chunk;
            if (_chunks.size() < static_cast<std::size_t>(chunks)) {
                _chunks.resize(static_cast<std::size_t>(chunks));
            }
            const auto rows_of = [&](i32 c) { return std::pair{c * chunk, std::min(rows, (c + 1) * chunk)}; };
            options.jobs->parallel_for(chunks, [&](i32 c) {
                TextureChunk& out = _chunks[static_cast<std::size_t>(c)];
                out.textures.clear();
                std::size_t count = 0; // a local: a shared counter would bounce between cores
                const Texture* last = nullptr;
                PreparedSprite sprite;
                const auto [begin, end] = rows_of(c);
                for (i32 i = begin; i < end; ++i) {
                    const auto row = static_cast<std::size_t>(i);
                    if (textures[row].static_renderable == statics && prepare_texture(world[row], textures[row], culler, sprite)) {
                        ++count;
                        // Each entity holds its own handle: compare textures, not handle addresses.
                        if ((!last || !(*sprite.texture == *last)) &&
                            std::find_if(out.textures.begin(), out.textures.end(),
                                         [&](const Texture* t) { return *t == *sprite.texture; }) == out.textures.end()) {
                            out.textures.push_back(sprite.texture);
                        }
                        last = sprite.texture;
                    }
                }
                out.count = count;
            });
            std::size_t total = 0;
            for (i32 c = 0; c < chunks; ++c) {
                TextureChunk& part = _chunks[static_cast<std::size_t>(c)];
                part.first = total;
                total += part.count;
                part.slots.clear();
                for (const Texture* texture : part.textures) {
                    part.slots.push_back(queue.texture_index(*texture));
                }
            }
            const RenderQueue::SpriteBlock block = queue.reserve_sprites(total);
            options.jobs->parallel_for(chunks, [&](i32 c) {
                const TextureChunk& part = _chunks[static_cast<std::size_t>(c)];
                std::size_t slot = part.first;
                const Texture* last = nullptr;
                u32 last_slot = 0;
                PreparedSprite sprite;
                const auto [begin, end] = rows_of(c);
                for (i32 i = begin; i < end; ++i) {
                    const auto row = static_cast<std::size_t>(i);
                    if (textures[row].static_renderable == statics && prepare_texture(world[row], textures[row], culler, sprite)) {
#ifdef KIN_ENABLE_RENDER_PROBE
                        sprite.draw_source = draw_source_for(it.entity(row), "TextureRenderer");
#endif
                        if (!last || !(*sprite.texture == *last)) {
                            const auto found = std::find_if(part.textures.begin(), part.textures.end(),
                                                            [&](const Texture* t) { return *t == *sprite.texture; });
                            last_slot = part.slots[static_cast<std::size_t>(found - part.textures.begin())];
                            last = sprite.texture;
                        }
                        queue.write_sprite(block, slot++, last_slot, sprite);
                    }
                }
            });
        }
    });
}

void WorldRenderState::collect_dynamic(RenderQueue& queue, SpriteRenderOptions options) {
    const u64 existing = queue.submitted(); // the caller's commands, not culled yet
    queue.set_sort(options.sort ? options.sort_mode : RenderSortMode::Submission);
    _sprites.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const SpriteRenderer& sprite) {
        if (!sprite.static_renderable) {
            submit_sprite(queue, entity, transform, sprite, options.view);
        }
    });
    collect_textures(queue, options, false);
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
    _shapes.each([&](flecs::entity entity, const Transform2D&, const WorldTransform& transform, const ShapeRenderer& shape) {
        if (!shape.static_renderable) {
            submit_shape(queue, entity, transform, shape, options.view);
        }
    });
    // Everything above was culled as it was submitted; only particles still need it.
    const u64 unculled = queue.submitted();
    _particle_systems.each([&](flecs::entity entity, const ParticleSystemComponent& particles) {
        KIN_DRAW_ENTITY(entity, "ParticleSystemComponent");
        submit_particles(queue, particles);
    });
    _particle_fields.each([&](flecs::entity entity, const ParticleFieldComponent& field) {
        KIN_DRAW_ENTITY(entity, "ParticleFieldComponent");
        submit_particles(queue, field);
    });
    if (options.view) {
        queue.cull(*options.view, unculled);
        queue.cull(*options.view, 0, existing);
    }
}

#ifdef KIN_ENABLE_RENDER_PROBE
u32 draw_source_of_entity(DrawTrace& trace, flecs::entity entity, std::string_view component) {
    if (!entity) {
        return 0;
    }
    return trace.entity_source(static_cast<u64>(reinterpret_cast<std::uintptr_t>(entity.world().c_ptr())), entity.id(),
                                component, [&] {
                                    const flecs::string_view name = entity.name();
                                    return name.size() > 0 ? std::string{name.c_str()} : "#" + std::to_string(entity.id());
                                });
}
#endif

WorldTransform compose(const WorldTransform& parent, const Transform2D& child) {
    return {to_world(parent, child.pos), parent.rotation + child.rotation, mul(parent.scale, child.scale)};
}

Transform2D to_local(const WorldTransform& parent, const WorldTransform& world) {
    const auto divide = [](f32 a, f32 b) { return b == 0.0f ? 0.0f : a / b; };
    Vec2f offset = sub(world.pos, parent.pos);
    if (parent.rotation != 0.0f) {
        offset = Affine2::rotation(-parent.rotation).apply_vector(offset);
    }
    return {{divide(offset.x, parent.scale.x), divide(offset.y, parent.scale.y)},
            world.rotation - parent.rotation,
            {divide(world.scale.x, parent.scale.x), divide(world.scale.y, parent.scale.y)}};
}

WorldTransform world_transform(flecs::entity entity) {
    if (const auto* transform = entity.get<WorldTransform>()) {
        return *transform;
    }
    return current_world_transform(entity);
}

WorldTransform current_world_transform(flecs::entity entity) {
    // Up to the root, then composed back down.
    std::vector<const Transform2D*> chain;
    for (flecs::entity current = entity; current; current = current.parent()) {
        if (const auto* transform = current.get<Transform2D>()) {
            chain.push_back(transform);
        }
    }
    WorldTransform world{};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        world = compose(world, **it);
    }
    return world;
}

WorldTransform current_world_transform(const EcsEntity& entity) {
    return current_world_transform(entity.raw());
}

WorldTransform world_transform(const EcsEntity& entity) {
    return world_transform(entity.raw());
}

Vec2f world_position(flecs::entity entity) {
    return world_transform(entity).pos;
}

Vec2f world_position(const EcsEntity& entity) {
    return world_position(entity.raw());
}

bool submit_sprite(RenderQueue& queue, flecs::entity entity) {
    const auto* sprite = entity.get<SpriteRenderer>();
    if (!sprite) {
        return false;
    }

    return submit_sprite(queue, entity, world_transform(entity), *sprite);
}

bool submit_sprite(RenderQueue& queue,
                   [[maybe_unused]] flecs::entity entity,
                   const WorldTransform& transform,
                   const SpriteRenderer& sprite,
                   const RenderView* view) {
    KIN_DRAW_ENTITY(entity, "SpriteRenderer");
    ResolvedSprite resolved;
    if (!resolve_sprite_renderer(sprite, resolved)) {
        return false;
    }

    const Rectf dest = sprite_draw_rect(transform, sprite, resolved);
    const f32 rotation = transform.rotation + sprite.rotation;
    // Turned about its pivot, the sprite stays inside the circle through its
    // farthest corner.
    if (view && !render_view_visible(*view, turned_bounds(dest, rotation, resolved_pivot(sprite, resolved)))) {
        return false;
    }
    const RenderKey key = key_for(sprite.layer,
                                  sprite.order,
                                  sprite.y_sort,
                                  sprite_anchor_pos(transform, sprite, resolved).y + sprite.sort_y_offset);
    queue.draw_sprite(key, resolved.sprite, dest, sprite.tint, {}, rotation, resolved_pivot(sprite, resolved));
    return true;
}

bool submit_texture(RenderQueue& queue, flecs::entity entity) {
    const auto* texture = entity.get<TextureRenderer>();
    if (!texture || !texture->visible || !texture->texture) {
        return false;
    }

    return submit_texture(queue, entity, world_transform(entity), *texture);
}

bool submit_texture(RenderQueue& queue,
                    [[maybe_unused]] flecs::entity entity,
                    const WorldTransform& transform,
                    const TextureRenderer& texture,
                    const RenderView* view) {
    KIN_DRAW_ENTITY(entity, "TextureRenderer");
    if (!texture.visible || !texture.texture) {
        return false;
    }

    const Rectf dest = texture_draw_rect(transform, texture);
    if (view && !render_view_visible(*view, turned_bounds(dest, transform.rotation, texture.pivot))) {
        return false;
    }
    const RenderKey key = key_for(texture.layer,
                                  texture.order,
                                  texture.y_sort,
                                  to_world(transform, texture.offset).y + texture.sort_y_offset);
    queue.draw_texture_region(key, texture.texture, texture_source_rect(texture), dest, texture.tint,
                              transform.rotation, texture.pivot);
    return true;
}

bool submit_rect(RenderQueue& queue, flecs::entity entity) {
    const auto* rect = entity.get<RectRenderer>();
    if (!rect || !rect->visible || rect->size.x <= 0.0f || rect->size.y <= 0.0f) {
        return false;
    }

    return submit_rect(queue, entity, world_transform(entity), *rect);
}

bool submit_rect(RenderQueue& queue,
                 [[maybe_unused]] flecs::entity entity,
                 const WorldTransform& transform,
                 const RectRenderer& rect,
                 const RenderView* view) {
    KIN_DRAW_ENTITY(entity, "RectRenderer");
    if (!rect.visible || rect.size.x <= 0.0f || rect.size.y <= 0.0f) {
        return false;
    }

    // Scaled about the entity's origin (a negative scale flips it over), then
    // turned about it.
    const Vec2f pos = transform.pos;
    const Vec2f s = transform.scale;
    const f32 x0 = pos.x + s.x * rect.offset.x, x1 = pos.x + s.x * (rect.offset.x + rect.size.x);
    const f32 y0 = pos.y + s.y * rect.offset.y, y1 = pos.y + s.y * (rect.offset.y + rect.size.y);
    const Rectf bounds{std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0), std::abs(y1 - y0)};
    if (bounds.w <= 0.0f || bounds.h <= 0.0f) {
        return false;
    }
    const Vec2f pivot{(pos.x - bounds.x) / bounds.w, (pos.y - bounds.y) / bounds.h};
    if (view && !render_view_visible(*view, turned_bounds(bounds, transform.rotation, pivot))) {
        return false;
    }
    const RenderKey key = key_for(rect.layer,
                                  rect.order,
                                  rect.y_sort,
                                  to_world(transform, rect.offset).y + rect.sort_y_offset);
    if (transform.rotation != 0.0f) {
        queue.submit({.type = rect.outline ? RenderCommandType::DrawRect : RenderCommandType::FillRect,
                      .key = key,
                      .rect = bounds,
                      .color = rect.color,
                      .rotation = transform.rotation,
                      .pivot = pivot});
    } else if (rect.outline) {
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

    return submit_line(queue, entity, world_transform(entity), *line);
}

bool submit_line(RenderQueue& queue,
                 [[maybe_unused]] flecs::entity entity,
                 const WorldTransform& transform,
                 const LineRenderer& line,
                 const RenderView* view) {
    KIN_DRAW_ENTITY(entity, "LineRenderer");
    if (!line.visible) {
        return false;
    }

    const Vec2f pos = transform.pos;
    const Vec2f a = to_world(transform, line.a);
    const Vec2f b = to_world(transform, line.b);
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

bool submit_shape(RenderQueue& queue, flecs::entity entity) {
    const auto* shape = entity.get<ShapeRenderer>();
    if (!shape || !shape->visible) {
        return false;
    }
    return submit_shape(queue, entity, world_transform(entity), *shape);
}

bool submit_shape(RenderQueue& queue,
                  [[maybe_unused]] flecs::entity entity,
                  const WorldTransform& transform,
                  const ShapeRenderer& shape,
                  const RenderView* view) {
    KIN_DRAW_ENTITY(entity, "ShapeRenderer");
    if (!shape.visible || !shape.mesh || shape.mesh->empty()) {
        return false;
    }
    // Unlike the sprite renderers, a mesh can take the whole transform: turns,
    // uneven scales and mirroring included.
    const Affine2 place = Affine2::trs(transform.pos, transform.rotation, transform.scale) *
                          Affine2::translation(shape.offset);
    if (view && !render_view_visible(*view, transformed_bounds(place, shape.mesh->bounds))) {
        return false;
    }
    const RenderKey key = key_for(shape.layer,
                                  shape.order,
                                  shape.y_sort,
                                  to_world(transform, shape.offset).y + shape.sort_y_offset);
    queue.draw_shape(key, shape.mesh, place, shape.tint);
    return true;
}

bool submit_particles(RenderQueue& queue, flecs::entity entity) {
    bool submitted = false;
    if (const auto* particles = entity.get<ParticleSystemComponent>()) {
        KIN_DRAW_ENTITY(entity, "ParticleSystemComponent");
        submitted = submit_particles(queue, *particles) || submitted;
    }
    if (const auto* field = entity.get<ParticleFieldComponent>()) {
        KIN_DRAW_ENTITY(entity, "ParticleFieldComponent");
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

bool submit_shape(Renderer2D& renderer, flecs::entity entity) {
    thread_local RenderQueue queue{RenderSortMode::Submission};
    queue.clear();
    queue.set_sort(RenderSortMode::Submission);
    if (!submit_shape(queue, entity)) {
        return false;
    }
    queue.flush(renderer);
    queue.clear(); // let go of the mesh
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
