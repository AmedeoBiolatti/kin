#pragma once
#include <kin/ecs/render.hpp>
#include <kin/renderer/normal_lighting2d.hpp>
namespace kin {
class Assets2D;
struct NormalSpriteCache2D {
    u64 fingerprint = 0;
    Texture texture;
    u64 renderer = 0;
    // Keep hashed image identities alive until replaced, including light shapes.
    std::vector<std::shared_ptr<const Image>> inputs;
    NormalLightingGpuCache2D gpu;
};
// Forward-lit sprite, always collected dynamically. SceneLighting's
// forward_sprites enables this path and disables the scene-wide light map.
struct NormalSpriteRenderer2D {
    ComponentAssetRef albedo_asset, normal_asset, specular_asset;
    SurfaceMaterial2D material;
    SurfaceGpuTextures2D gpu_textures;
    Rectf source{};
    Vec2f offset{}, size{}, pivot{.5f, .5f};
    Color tint = colors::white;
    f32 normal_strength = 1, specular_strength = 0, shininess = 32;
    i32 light_mask = -1, layer = layer_value(RenderLayer::World), order = 0;
    bool visible = true, y_sort = false, flip_x = false, flip_y = false;
    std::shared_ptr<NormalSpriteCache2D> cache = std::make_shared<NormalSpriteCache2D>();
};
struct DirectionalLight2D {
    Vec2f direction{-.3f, -.5f}; // toward light, in world space, rotated by entity
    f32 elevation = 1, intensity = 1;
    Color color = colors::white;
    i32 mask = -1;
    bool enabled = true, shadows = false;
    f32 shadow_strength = 1, shadow_softness = 0;
};
void register_normal_sprite2d_components(EcsWorld&);
void bind_normal_sprites2d(EcsWorld&, Assets2D&);
bool submit_normal_sprite2d(RenderQueue&, flecs::entity, const WorldTransform&, const NormalSpriteRenderer2D&,
                            const RenderView* view = nullptr);
} // namespace kin
