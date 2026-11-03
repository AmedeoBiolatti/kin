#include <algorithm>
#include <bit>
#include <cmath>
#include <kin/ecs/assets2d.hpp>
#include <kin/ecs/lighting.hpp>
#include <kin/ecs/normal_sprite2d.hpp>
#include <kin/renderer/renderer2d.hpp>

namespace kin {
void register_normal_sprite2d_components(EcsWorld& w) {
    if (w.components().find("NormalSpriteRenderer2D"))
        return;
    w.components()
        .native<NormalSpriteRenderer2D>("NormalSpriteRenderer2D")
        .field("albedo_asset", &NormalSpriteRenderer2D::albedo_asset)
        .asset_type("Image")
        .field("normal_asset", &NormalSpriteRenderer2D::normal_asset)
        .asset_type("Image")
        .field("specular_asset", &NormalSpriteRenderer2D::specular_asset)
        .asset_type("Image")
        .property<Vec2f>(
            "source_pos", [](const auto& r) { return Vec2f{r.source.x, r.source.y}; },
            [](auto& r, const Vec2f& v) {
                r.source.x = v.x;
                r.source.y = v.y;
                return true;
            })
        .property<Vec2f>(
            "source_size", [](const auto& r) { return Vec2f{r.source.w, r.source.h}; },
            [](auto& r, const Vec2f& v) {
                r.source.w = v.x;
                r.source.h = v.y;
                return true;
            })
        .field("offset", &NormalSpriteRenderer2D::offset)
        .field("size", &NormalSpriteRenderer2D::size)
        .field("pivot", &NormalSpriteRenderer2D::pivot)
        .field("tint", &NormalSpriteRenderer2D::tint)
        .field("normal_strength", &NormalSpriteRenderer2D::normal_strength)
        .field("specular_strength", &NormalSpriteRenderer2D::specular_strength)
        .field("shininess", &NormalSpriteRenderer2D::shininess)
        .field("light_mask", &NormalSpriteRenderer2D::light_mask)
        .field("layer", &NormalSpriteRenderer2D::layer)
        .field("order", &NormalSpriteRenderer2D::order)
        .field("visible", &NormalSpriteRenderer2D::visible)
        .field("y_sort", &NormalSpriteRenderer2D::y_sort)
        .field("flip_x", &NormalSpriteRenderer2D::flip_x)
        .field("flip_y", &NormalSpriteRenderer2D::flip_y);
    w.components()
        .native<DirectionalLight2D>("DirectionalLight2D")
        .field("direction", &DirectionalLight2D::direction)
        .field("elevation", &DirectionalLight2D::elevation)
        .field("intensity", &DirectionalLight2D::intensity)
        .field("color", &DirectionalLight2D::color)
        .field("mask", &DirectionalLight2D::mask)
        .field("enabled", &DirectionalLight2D::enabled)
        .field("shadows", &DirectionalLight2D::shadows)
        .field("shadow_strength", &DirectionalLight2D::shadow_strength)
        .field("shadow_softness", &DirectionalLight2D::shadow_softness);
}
void bind_normal_sprites2d(EcsWorld& w, Assets2D& assets) {
    std::vector<flecs::entity> entities;
    w.raw().each([&](flecs::entity e, const NormalSpriteRenderer2D&) { entities.push_back(e); });
    for (auto raw : entities) {
        EcsEntity e{raw};
        const auto old = *e.get<NormalSpriteRenderer2D>();
        auto material = old.material;
        if (!old.albedo_asset.id.empty())
            if (auto image = assets.image(old.albedo_asset.id))
                material.albedo = image;
        if (!old.normal_asset.id.empty())
            if (auto image = assets.image(old.normal_asset.id))
                material.normal = image;
        if (!old.specular_asset.id.empty())
            if (auto image = assets.image(old.specular_asset.id))
                material.specular = image;
        auto gpu = old.gpu_textures;
        if (assets.renderer().capabilities().shader_geometry && assets.renderer().capabilities().data_buffers) {
            if (!old.albedo_asset.id.empty())
                gpu.albedo = assets.data_texture(old.albedo_asset.id);
            if (!old.normal_asset.id.empty())
                gpu.normal = assets.data_texture(old.normal_asset.id);
            if (!old.specular_asset.id.empty())
                gpu.specular = assets.data_texture(old.specular_asset.id);
        }
        if (gpu.albedo != old.gpu_textures.albedo || gpu.normal != old.gpu_textures.normal ||
            gpu.specular != old.gpu_textures.specular || material.albedo != old.material.albedo ||
            material.normal != old.material.normal || material.specular != old.material.specular || !old.cache) {
            auto* r = e.get_mut<NormalSpriteRenderer2D>();
            r->material = std::move(material);
            r->gpu_textures = std::move(gpu);
            if (!r->cache)
                r->cache = std::make_shared<NormalSpriteCache2D>();
            assets.changed();
        }
    }
}
namespace {
struct Fingerprint {
    u64 value = 14695981039346656037ull;
    void add(u64 v) {
        for (int i = 0; i < 8; ++i) {
            value ^= (v >> (i * 8)) & 255;
            value *= 1099511628211ull;
        }
    }
    void add(f32 v) { add(static_cast<u64>(std::bit_cast<u32>(v))); }
    void add(i32 v) { add(static_cast<u64>(v)); }
    void add(bool v) { add(static_cast<u64>(v)); }
    void add(Vec2f v) {
        add(v.x);
        add(v.y);
    }
    void add(Rectf r) {
        add(r.x);
        add(r.y);
        add(r.w);
        add(r.h);
    }
    void add(Color c) {
        add(static_cast<u64>(c.r) | (static_cast<u64>(c.g) << 8) | (static_cast<u64>(c.b) << 16) |
            (static_cast<u64>(c.a) << 24));
    }
    void add(const std::shared_ptr<const Image>& image) {
        add(static_cast<u64>(reinterpret_cast<std::uintptr_t>(image.get())));
    }
};
} // namespace
bool submit_normal_sprite2d(RenderQueue& queue, flecs::entity entity, const WorldTransform& t,
                            const NormalSpriteRenderer2D& r, const RenderView* view) {
    if (!r.visible || !r.material.albedo || !r.material.albedo->valid())
        return false;
    const auto source = r.source.w > 0 && r.source.h > 0 ? r.source
                                                         : Rectf{0, 0, static_cast<f32>(r.material.albedo->size.x),
                                                                 static_cast<f32>(r.material.albedo->size.y)};
    const bool fx = r.flip_x != (t.scale.x < 0), fy = r.flip_y != (t.scale.y < 0);
    const auto pivot = Vec2f{fx ? 1 - r.pivot.x : r.pivot.x, fy ? 1 - r.pivot.y : r.pivot.y};
    const auto pos = Affine2::trs(t.pos, t.rotation, t.scale).apply(r.offset);
    const auto size = Vec2f{(r.size.x > 0 ? r.size.x : source.w) * std::abs(t.scale.x),
                            (r.size.y > 0 ? r.size.y : source.h) * std::abs(t.scale.y)};
    SurfaceGeometry2D g{source,
                        {pos.x - size.x * pivot.x, pos.y - size.y * pivot.y, size.x, size.y},
                        t.rotation,
                        pivot,
                        static_cast<Flip>((fx ? static_cast<int>(Flip::X) : 0) | (fy ? static_cast<int>(Flip::Y) : 0))};
    const auto bounds = transformed_bounds(
        Affine2::translation(pos) * Affine2::rotation(t.rotation) * Affine2::translation({-pos.x, -pos.y}), g.dest);
    if (view && !render_view_visible(*view, bounds))
        return false;
    Color ambient = colors::white;
    bool forward = false;
    auto world = entity.world();
    if (r.layer < layer_value(RenderLayer::UI))
        world.each([&](const SceneLighting& l) {
            if (l.enabled && !forward && l.forward_sprites) {
                ambient = l.ambient;
                forward = true;
            }
        });
    std::vector<SurfaceLight2D> lights;
    std::vector<LightOccluder2D> occluders;
    if (forward) {
        world.each([&](flecs::entity e, const LightComponent& l) {
            if (!l.enabled || l.intensity <= 0 || l.radius <= 0)
                return;
            const auto at = current_world_transform(e);
            const f32 scale = std::max(std::abs(at.scale.x), std::abs(at.scale.y));
            lights.push_back({.position = at.pos,
                              .height = l.height * scale,
                              .radius = l.radius * scale,
                              .intensity = l.intensity,
                              .color = l.color,
                              .mask = l.mask,
                              .shadows = l.shadows,
                              .shadow_strength = l.shadow_strength,
                              .shadow_softness = l.shadow_softness * scale,
                              .shape = l.shape_image,
                              .rotation = at.rotation + l.rotation});
        });
        world.each([&](flecs::entity e, const DirectionalLight2D& l) {
            if (!l.enabled || l.intensity <= 0)
                return;
            const auto at = current_world_transform(e);
            lights.push_back({.height = l.elevation,
                              .intensity = l.intensity,
                              .color = l.color,
                              .mask = l.mask,
                              .directional = true,
                              .direction = Affine2::rotation(at.rotation).apply_vector(l.direction),
                              .shadows = l.shadows,
                              .shadow_strength = l.shadow_strength,
                              .shadow_softness = l.shadow_softness});
        });
        world.each([&](flecs::entity e, const LightOccluder&) {
            auto outlines = occluder_outlines(EcsEntity{e}, Affine2{});
            occluders.insert(occluders.end(), outlines.begin(), outlines.end());
        });
    }
    auto material = r.material;
    material.normal_strength = r.normal_strength;
    material.specular_strength = r.specular_strength;
    material.shininess = r.shininess;
    material.light_mask = r.light_mask;
    material.tint = r.tint;
    Fingerprint hash;
    hash.add(material.albedo);
    hash.add(material.normal);
    hash.add(material.specular);
    hash.add(material.normal_strength);
    hash.add(material.specular_strength);
    hash.add(material.shininess);
    hash.add(material.light_mask);
    hash.add(material.tint);
    hash.add(g.source);
    hash.add(g.dest);
    hash.add(g.rotation);
    hash.add(g.pivot);
    hash.add(static_cast<i32>(g.flip));
    hash.add(ambient);
    for (const auto& l : lights) {
        hash.add(l.position);
        hash.add(l.height);
        hash.add(l.radius);
        hash.add(l.intensity);
        hash.add(l.color);
        hash.add(l.mask);
        hash.add(l.directional);
        hash.add(l.direction);
        hash.add(l.shadows);
        hash.add(l.shadow_strength);
        hash.add(l.shadow_softness);
        hash.add(l.shape);
        hash.add(l.rotation);
    }
    for (const auto& o : occluders) {
        hash.add(o.lit);
        hash.add(static_cast<u64>(o.points.size()));
        for (const auto p : o.points)
            hash.add(p);
    }
    const auto cache = r.cache ? r.cache : std::make_shared<NormalSpriteCache2D>();
    const auto gpu = r.gpu_textures;
    const auto pass = r.layer >= layer_value(RenderLayer::Debug)     ? render_pass_mask::debug
                      : r.layer >= layer_value(RenderLayer::UI)      ? render_pass_mask::ui
                      : r.layer >= layer_value(RenderLayer::Effects) ? render_pass_mask::effects
                                                                     : render_pass_mask::world;
    queue.custom(
        {.layer = r.layer, .order = r.order, .y = pos.y, .use_y = r.y_sort, .pass_mask = pass},
        [cache, fingerprint = hash.value, material, g, ambient, lights = std::move(lights),
         occluders = std::move(occluders), gpu](Renderer2D& renderer) {
            const bool gpu_inputs_changed =
                cache->gpu.fingerprint != fingerprint || cache->gpu.renderer != renderer.id();
            if (draw_normal_sprite2d_gpu(renderer, material, g, ambient, lights, occluders, fingerprint, cache->gpu,
                                         gpu)) {
                if (gpu_inputs_changed) {
                    cache->inputs = {material.albedo, material.normal, material.specular};
                    for (const auto& light : lights)
                        if (light.shape)
                            cache->inputs.push_back(light.shape);
                }
                return;
            }
            if (!cache->texture || cache->fingerprint != fingerprint || cache->renderer != renderer.id()) {
                const auto image = shade_normal_sprite2d(material, g, ambient, lights, occluders);
                if (!image.valid())
                    return;
                auto texture = renderer.create_texture_from_rgba(image.rgba.data(), image.size);
                if (!texture)
                    return;
                cache->texture = std::move(texture);
                cache->fingerprint = fingerprint;
                cache->renderer = renderer.id();
                cache->inputs = {material.albedo, material.normal, material.specular};
                for (const auto& light : lights)
                    if (light.shape)
                        cache->inputs.push_back(light.shape);
            }
            renderer.draw_texture(cache->texture, {}, g.dest, colors::white, g.rotation, g.pivot, g.flip);
        },
        "NormalSprite2D");
    return true;
}
} // namespace kin
