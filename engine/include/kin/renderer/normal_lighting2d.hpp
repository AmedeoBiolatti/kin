#pragma once

// Normal-mapped sprites lit per pixel by point and directional lights
// ("forward" lighting): Lambert diffuse, Blinn-Phong specular, receiver
// masks, light shapes and the polygon occluders 2D light maps use
// (lighting.hpp), in linear light.
//
// Albedo and light colours are sRGB; normal and specular maps are data. RGB
// normal maps: +X right, +Y down, +Z toward the viewer.
//
//     const kin::SurfaceLightBuffer2D packed = kin::pack_surface_lights2d(renderer, lights, occluders);
//     // each sprite, every frame
//     if (!kin::draw_normal_sprite2d_gpu(renderer, material, geometry, ambient, packed, cache)) {
//         // no SDL_GPU: shade it on the CPU (once, while nothing changes) and draw that
//         const kin::Image shaded = kin::shade_normal_sprite2d(material, geometry, ambient, lights, occluders);
//     }
//
// kin/ecs/normal_sprite2d.hpp does this for scenes.

#include <kin/assets/image.hpp>
#include <kin/core/affine.hpp>
#include <kin/renderer/data_buffer.hpp>
#include <kin/renderer/flip.hpp>
#include <kin/renderer/lighting.hpp>

#include <array>
#include <memory>
#include <span>

namespace kin {

struct RendererBackendCapabilities;

struct SurfaceMaterial2D {
    std::shared_ptr<const Image> albedo;
    std::shared_ptr<const Image> normal;   // none: a flat surface
    std::shared_ptr<const Image> specular; // its red channel scales the highlight; none: all of it
    f32 normal_strength = 1;
    f32 specular_strength = 0;
    f32 shininess = 32;  // the highlight's width: higher, smaller
    i32 light_mask = -1; // lit by lights sharing a bit with it
    Color tint = colors::white;
};

struct SurfaceLight2D {
    Vec2f position{};
    f32 height = 64; // above the sprites' plane
    f32 radius = 128;
    f32 intensity = 1; // up to max_light_intensity
    Color color = colors::white;
    i32 mask = -1;
    bool directional = false;
    Vec2f direction = {0, -1}; // toward a directional light; `height` is its Z
    bool shadows = false;
    f32 shadow_strength = 1;
    f32 shadow_softness = 0;
    std::shared_ptr<const Image> shape; // its alpha shapes a point light, as Light2D::shape
    f32 rotation = 0;                   // of `shape`, degrees clockwise
};

struct SurfaceGeometry2D {
    Rectf source{}; // albedo pixels; empty = whole image; maps share normalized UVs
    Rectf dest{};   // world space, before rotation about pivot
    f32 rotation = 0;
    Vec2f pivot{.5f, .5f};
    Flip flip = Flip::None;
};

// Lights are skipped past this many (and those with values that can't be
// used: not finite, no radius, a negative intensity).
inline constexpr std::size_t max_surface_lights = 256;

// Whether the renderer can draw normal-mapped sprites on the GPU
// (draw_normal_sprite2d_gpu); else they are shaded on the CPU.
bool normal_lighting2d_gpu_supported(const RendererBackendCapabilities& capabilities);

// Lights and their shadows packed for the GPU: pack them once (while they
// stay the same) and draw any number of sprites with them. Its buffer is
// invalid when the renderer can't make one.
struct SurfaceLightBuffer2D {
    DataBuffer buffer;
    u32 lights = 0; // how many of its lights a draw is lit by (0: by `ambient` only)
    u64 renderer = 0;
};
SurfaceLightBuffer2D pack_surface_lights2d(Renderer2D& renderer, std::span<const SurfaceLight2D> lights,
                                           std::span<const LightOccluder2D> occluders);

// Textures for a material's images on the GPU: Assets2D's (raw UNORM, so
// normal data does not depend on the colour space), or made by the draw.
struct SurfaceGpuTextures2D {
    Texture albedo;
    Texture normal;
    Texture specular;
};
// The textures a draw made from a material's images, kept while the images
// stay the same.
struct NormalLightingGpuCache2D {
    u64 renderer = 0;
    SurfaceGpuTextures2D textures;
    std::array<std::shared_ptr<const Image>, 3> images;
    bool used = false; // the last draw was drawn on the GPU
};

// Draws the sprite lit on the GPU (the NormalSprite2D built-in shader); false,
// drawing nothing, when the renderer can't (shade it on the CPU then) or the
// material or geometry can't be drawn.
bool draw_normal_sprite2d_gpu(Renderer2D& renderer, const SurfaceMaterial2D& material,
                              const SurfaceGeometry2D& geometry, Color ambient, const SurfaceLightBuffer2D& lights,
                              NormalLightingGpuCache2D& cache, SurfaceGpuTextures2D textures = {});

// The sprite shaded on the CPU, at source resolution (at most 4 million
// pixels), alpha kept: the portable path and the reference the GPU's matches.
// Invalid when the material or geometry can't be drawn.
Image shade_normal_sprite2d(const SurfaceMaterial2D& material, const SurfaceGeometry2D& geometry, Color ambient,
                            std::span<const SurfaceLight2D> lights, std::span<const LightOccluder2D> occluders = {});

} // namespace kin
