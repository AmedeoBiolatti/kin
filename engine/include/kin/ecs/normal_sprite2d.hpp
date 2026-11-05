#pragma once

// Normal-mapped sprites as entities: lit per pixel by the scene's lights when
// its SceneLighting has forward_sprites (kin/ecs/lighting.hpp), on the GPU
// where the renderer can and shaded on the CPU where it can't
// (kin/renderer/normal_lighting2d.hpp).
//
//     world.entity("lighting").set(kin::SceneLighting{.forward_sprites = true});
//     world.entity("sun").set(kin::DirectionalLight2D{.direction = {-.3f, -.5f}});
//     actor.set(kin::NormalSpriteRenderer2D{.albedo_asset = {"images/hero.png"},
//                                           .normal_asset = {"images/hero_n.png"}});
//
// They are drawn with the world's other renderers (sorted, culled, clipped,
// placed by their entities) and are never cached as static. On the UI layer
// and above, and in scenes without forward lighting, they are unlit.

#include <kin/ecs/lighting.hpp>
#include <kin/ecs/render.hpp>
#include <kin/renderer/normal_lighting2d.hpp>

#include <memory>
#include <vector>

namespace kin {

class Assets2D;

// What one collect's normal-mapped sprites are lit by, gathered once
// (WorldRenderState does) and shared by every sprite drawn with it.
struct NormalSpriteLighting2D {
    Color ambient = colors::white;
    std::vector<SurfaceLight2D> lights;
    std::vector<LightOccluder2D> occluders;
    u64 fingerprint = 0;      // of all of the above
    SurfaceLightBuffer2D gpu; // packed by the first GPU draw (for its renderer)
};

// A NormalSpriteRenderer2D's draws, kept while nothing they are made from
// changes. Prefab instances inherit their prefab's component, so the CPU
// path's shaded images are kept per entity drawn.
struct NormalSpriteCache2D {
    struct Shaded {
        u64 entity = 0;
        u64 fingerprint = 0;
        u64 renderer = 0;
        Texture texture;
        // What it was shaded with, kept so the identities it was fingerprinted by stay unique.
        std::shared_ptr<const NormalSpriteLighting2D> lighting;
        SurfaceMaterial2D material;
    };
    NormalLightingGpuCache2D gpu; // the GPU path's textures, from the images alone
    std::vector<Shaded> shaded;   // the CPU path's
};

struct NormalSpriteRenderer2D {
    ComponentAssetRef albedo_asset;
    ComponentAssetRef normal_asset;
    ComponentAssetRef specular_asset;
    // Assets2D binds them from the assets (or set them from C++).
    std::shared_ptr<const Image> albedo;
    std::shared_ptr<const Image> normal;   // none: a flat surface
    std::shared_ptr<const Image> specular; // its red channel scales the highlight; none: all of it
    SurfaceGpuTextures2D gpu_textures;     // Assets2D's textures of them, for the GPU path
    Rectf source{};                        // albedo pixels; empty: the whole image
    Vec2f offset{};
    Vec2f size{}; // empty: the source's
    Vec2f pivot{.5f, .5f};
    Color tint = colors::white;
    f32 normal_strength = 1;
    f32 specular_strength = 0;
    f32 shininess = 32;
    i32 light_mask = -1; // lit by lights whose mask shares a bit with it
    i32 layer = layer_value(RenderLayer::World);
    i32 order = 0;
    bool visible = true;
    bool y_sort = false;
    bool flip_x = false;
    bool flip_y = false;
    std::shared_ptr<NormalSpriteCache2D> cache = std::make_shared<NormalSpriteCache2D>();
};

void register_normal_sprite2d_components(EcsWorld& world);
// Binds the images (and their GPU textures) of the sprites' assets.
void bind_normal_sprites2d(EcsWorld& world, Assets2D& assets);
// The lights the world's normal-mapped sprites are lit by now: its SceneLighting's
// (scene_lighting()) ambient, lights and occluders when it has forward_sprites;
// else none (white ambient: unlit). `previous` when nothing changed, so its
// packed GPU buffer is kept.
std::shared_ptr<NormalSpriteLighting2D>
gather_normal_sprite_lighting2d(flecs::world& world, std::shared_ptr<NormalSpriteLighting2D> previous = {});
// Queues the sprite, lit by `lighting` (unlit on the UI layer and above).
bool submit_normal_sprite2d(RenderQueue& queue, flecs::entity entity, const WorldTransform& transform,
                            const NormalSpriteRenderer2D& sprite,
                            const std::shared_ptr<NormalSpriteLighting2D>& lighting, const RenderView* view = nullptr);

} // namespace kin
