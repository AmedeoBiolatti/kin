#include <kin/ecs/normal_sprite2d.hpp>

#include "render_internal.hpp"

#include <kin/ecs/assets2d.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace kin {
namespace {

using render_detail::flip_for;
using render_detail::key_for;
using render_detail::mirrored_pivot;
using render_detail::to_world;
using render_detail::turned_bounds;

// FNV-1a over the values a draw is made from.
struct Fingerprint {
    u64 value = 14695981039346656037ull;

    void add(u64 v) {
        for (int i = 0; i < 8; ++i) {
            value ^= (v >> (i * 8)) & 255;
            value *= 1099511628211ull;
        }
    }
    void add(f32 v) { add(static_cast<u64>(std::bit_cast<u32>(v))); }
    void add(i32 v) { add(static_cast<u64>(static_cast<u32>(v))); }
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
    // By identity: those hashed are kept alive while the hash is (so no other image takes their address).
    void add(const std::shared_ptr<const Image>& image) {
        add(static_cast<u64>(reinterpret_cast<std::uintptr_t>(image.get())));
    }
};

u64 lighting_fingerprint(const NormalSpriteLighting2D& lighting) {
    Fingerprint hash;
    hash.add(lighting.ambient);
    for (const SurfaceLight2D& light : lighting.lights) {
        hash.add(light.position);
        hash.add(light.height);
        hash.add(light.radius);
        hash.add(light.intensity);
        hash.add(light.color);
        hash.add(light.mask);
        hash.add(light.directional);
        hash.add(light.direction);
        hash.add(light.shadows);
        hash.add(light.shadow_strength);
        hash.add(light.shadow_softness);
        hash.add(light.shape);
        hash.add(light.rotation);
    }
    for (const LightOccluder2D& occluder : lighting.occluders) {
        hash.add(occluder.lit);
        hash.add(static_cast<u64>(occluder.points.size()));
        for (const Vec2f point : occluder.points) {
            hash.add(point);
        }
    }
    return hash.value;
}

SurfaceMaterial2D material_of(const NormalSpriteRenderer2D& sprite) {
    return {.albedo = sprite.albedo,
            .normal = sprite.normal,
            .specular = sprite.specular,
            .normal_strength = sprite.normal_strength,
            .specular_strength = sprite.specular_strength,
            .shininess = sprite.shininess,
            .light_mask = sprite.light_mask,
            .tint = sprite.tint};
}

// What the CPU path's image of a sprite is shaded from.
u64 shading_fingerprint(const SurfaceMaterial2D& material, const SurfaceGeometry2D& geometry, u64 lighting) {
    Fingerprint hash;
    hash.add(material.albedo);
    hash.add(material.normal);
    hash.add(material.specular);
    hash.add(material.normal_strength);
    hash.add(material.specular_strength);
    hash.add(material.shininess);
    hash.add(material.light_mask);
    hash.add(material.tint);
    hash.add(geometry.source);
    hash.add(geometry.dest);
    hash.add(geometry.rotation);
    hash.add(geometry.pivot);
    hash.add(static_cast<i32>(geometry.flip));
    hash.add(lighting);
    return hash.value;
}

// The sprite drawn: on the GPU where the renderer can, else from its image
// shaded on the CPU (kept while nothing it is shaded from changes).
void draw_normal_sprite(Renderer2D& renderer, NormalSpriteCache2D& cache,
                        const std::shared_ptr<NormalSpriteLighting2D>& lighting, bool lit,
                        const SurfaceMaterial2D& material, const SurfaceGeometry2D& geometry,
                        const SurfaceGpuTextures2D& textures, flecs::entity entity) {
    const Color ambient = lit ? lighting->ambient : colors::white;
    if (normal_lighting2d_gpu_supported(renderer.capabilities())) {
        if (lighting->gpu.renderer != renderer.id()) {
            lighting->gpu = pack_surface_lights2d(renderer, lighting->lights, lighting->occluders);
        }
        SurfaceLightBuffer2D lights = lighting->gpu;
        if (!lit) {
            lights.lights = 0;
        }
        if (draw_normal_sprite2d_gpu(renderer, material, geometry, ambient, lights, cache.gpu, textures)) {
            return;
        }
    }
    const u64 id = entity.id();
    auto shaded = std::ranges::find(cache.shaded, id, &NormalSpriteCache2D::Shaded::entity);
    if (shaded == cache.shaded.end()) {
        // A new entity drawn with this component: those gone are let go first.
        const flecs::world world = entity.world();
        std::erase_if(cache.shaded,
                      [&](const NormalSpriteCache2D::Shaded& entry) { return !world.is_alive(entry.entity); });
        cache.shaded.push_back({.entity = id});
        shaded = std::prev(cache.shaded.end());
    }
    const u64 fingerprint = shading_fingerprint(material, geometry, lit ? lighting->fingerprint : 0);
    if (!shaded->texture || shaded->fingerprint != fingerprint || shaded->renderer != renderer.id()) {
        const Image image =
            lit ? shade_normal_sprite2d(material, geometry, ambient, lighting->lights, lighting->occluders)
                : shade_normal_sprite2d(material, geometry, ambient, {});
        if (!image.valid()) {
            return;
        }
        Texture texture = renderer.create_texture_from_rgba(image.rgba.data(), image.size);
        if (!texture) {
            return;
        }
        shaded->texture = std::move(texture);
        shaded->fingerprint = fingerprint;
        shaded->renderer = renderer.id();
        shaded->lighting = lighting;
        shaded->material = material;
    }
    renderer.draw_texture(shaded->texture, {}, geometry.dest, colors::white, geometry.rotation, geometry.pivot,
                          geometry.flip);
}

} // namespace

void register_normal_sprite2d_components(EcsWorld& world) {
    if (world.components().find("NormalSpriteRenderer2D")) {
        return;
    }
    world.components()
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
    world.components()
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

void bind_normal_sprites2d(EcsWorld& world, Assets2D& assets) {
    // Read only, so nothing is marked written; those to change, changed after.
    struct Bound {
        flecs::entity entity;
        std::shared_ptr<const Image> albedo;
        std::shared_ptr<const Image> normal;
        std::shared_ptr<const Image> specular;
        SurfaceGpuTextures2D textures;
    };
    const bool gpu = normal_lighting2d_gpu_supported(assets.renderer().capabilities());
    std::vector<Bound> rebound;
    world.each([&](flecs::entity entity, const NormalSpriteRenderer2D& sprite) {
        Bound bound{entity, sprite.albedo, sprite.normal, sprite.specular, sprite.gpu_textures};
        const auto bind = [&](const ComponentAssetRef& asset, std::shared_ptr<const Image>& image, Texture& texture) {
            if (asset.id.empty()) {
                return; // set from C++, if at all
            }
            if (std::shared_ptr<const Image> loaded = assets.image(asset.id)) {
                image = std::move(loaded); // else the last one, while a reload is broken
            }
            if (gpu) {
                texture = assets.data_texture(asset.id);
            }
        };
        bind(sprite.albedo_asset, bound.albedo, bound.textures.albedo);
        bind(sprite.normal_asset, bound.normal, bound.textures.normal);
        bind(sprite.specular_asset, bound.specular, bound.textures.specular);
        if (bound.albedo != sprite.albedo || bound.normal != sprite.normal || bound.specular != sprite.specular ||
            !(bound.textures.albedo == sprite.gpu_textures.albedo) ||
            !(bound.textures.normal == sprite.gpu_textures.normal) ||
            !(bound.textures.specular == sprite.gpu_textures.specular) || !sprite.cache) {
            rebound.push_back(std::move(bound));
        }
    });
    for (Bound& bound : rebound) {
        EcsEntity owner{bound.entity}; // one from a prefab gets its own copy first
        NormalSpriteRenderer2D* sprite = owner.get_mut<NormalSpriteRenderer2D>();
        sprite->albedo = std::move(bound.albedo);
        sprite->normal = std::move(bound.normal);
        sprite->specular = std::move(bound.specular);
        sprite->gpu_textures = std::move(bound.textures);
        if (!sprite->cache) {
            sprite->cache = std::make_shared<NormalSpriteCache2D>();
        }
        owner.modified<NormalSpriteRenderer2D>();
    }
    if (!rebound.empty()) {
        assets.changed();
    }
}

std::shared_ptr<NormalSpriteLighting2D>
gather_normal_sprite_lighting2d(flecs::world& world, std::shared_ptr<NormalSpriteLighting2D> previous) {
    auto lighting = std::make_shared<NormalSpriteLighting2D>();
    if (const SceneLighting* scene = scene_lighting(world); scene && scene->forward_sprites) {
        lighting->ambient = scene->ambient;
        lighting->lights = scene_surface_lights(world);
        if (lighting->lights.size() > max_surface_lights) {
            lighting->lights.resize(max_surface_lights);
        }
        if (std::ranges::any_of(lighting->lights, &SurfaceLight2D::shadows)) {
            lighting->occluders = scene_occluders(world, Affine2{});
        }
    }
    lighting->fingerprint = lighting_fingerprint(*lighting);
    if (previous && previous->fingerprint == lighting->fingerprint) {
        return previous; // `previous` holds the images it was fingerprinted by: the same ones
    }
    return lighting;
}

bool submit_normal_sprite2d(RenderQueue& queue, flecs::entity entity, const WorldTransform& transform,
                            const NormalSpriteRenderer2D& sprite,
                            const std::shared_ptr<NormalSpriteLighting2D>& lighting, const RenderView* view) {
    KIN_DRAW_ENTITY(entity, "NormalSpriteRenderer2D");
    if (!sprite.visible || !sprite.albedo || !sprite.albedo->valid() || !lighting) {
        return false;
    }
    const Rectf source =
        sprite.source.w > 0 && sprite.source.h > 0
            ? sprite.source
            : Rectf{0, 0, static_cast<f32>(sprite.albedo->size.x), static_cast<f32>(sprite.albedo->size.y)};
    const Flip flip = flip_for(transform, sprite.flip_x, sprite.flip_y);
    const Vec2f pivot = mirrored_pivot(sprite.pivot, flip);
    const Vec2f anchor = to_world(transform, sprite.offset);
    const Vec2f size{(sprite.size.x > 0 ? sprite.size.x : source.w) * std::abs(transform.scale.x),
                     (sprite.size.y > 0 ? sprite.size.y : source.h) * std::abs(transform.scale.y)};
    const SurfaceGeometry2D geometry{.source = source,
                                     .dest = {anchor.x - size.x * pivot.x, anchor.y - size.y * pivot.y, size.x, size.y},
                                     .rotation = transform.rotation,
                                     .pivot = pivot,
                                     .flip = flip};
    if (view && !render_view_visible(*view, turned_bounds(geometry.dest, geometry.rotation, pivot))) {
        return false;
    }
    const bool lit = sprite.layer < layer_value(RenderLayer::UI);
    std::shared_ptr<NormalSpriteCache2D> cache = sprite.cache ? sprite.cache : std::make_shared<NormalSpriteCache2D>();
    queue.custom(
        key_for(sprite.layer, sprite.order, sprite.y_sort, anchor.y),
        [cache = std::move(cache), lighting, lit, material = material_of(sprite), geometry,
         textures = sprite.gpu_textures, entity](Renderer2D& renderer) {
            draw_normal_sprite(renderer, *cache, lighting, lit, material, geometry, textures, entity);
        },
        "NormalSprite2D");
    return true;
}

} // namespace kin
