#pragma once

// Everything Sponza Walk can switch, in one struct, and how it is applied to a
// Scene3D each frame. A new renderer feature is a field here, a line in
// apply() and one row in the controls table (main.cpp).
#include <kin/render3d/renderer3d.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace sponza_walk {
using namespace kin;

inline constexpr float pi = 3.14159265f;

// The reference scene's sun (tests/reference/scenes/sponza.json), as angles.
inline constexpr Vec3f reference_sun{0.4f, 1.0f, 0.12f};
inline constexpr Vec3f reference_sun_colour{4.0f, 3.8f, 3.4f};
inline constexpr Vec3f reference_ambient{0.36f, 0.4f, 0.48f};

inline float azimuth_of(Vec3f d) { return std::atan2(d.z, d.x) * 180.0f / pi; }
inline float elevation_of(Vec3f d) { return std::atan2(d.y, std::hypot(d.x, d.z)) * 180.0f / pi; }

enum class WireMode { Off, Overlay, Only };
enum class ShadowArea { Auto, AroundCamera };

// What is done to every material of the model (copies are changed, the
// model's own are kept).
struct MaterialSettings {
    bool textures = true;      // base colour textures; off: flat grey
    bool normal_maps = true;
    bool metal_rough_maps = true;
    bool occlusion_maps = true;
    bool emissive = true;
    float emissive_scale = 1.0f;
    bool cutout = true;        // alpha cut-out (Blend3D::Mask); off: opaque
    bool blended = true;       // alpha-blended and additive parts drawn at all
    bool force_double_sided = false;
    bool casts_shadow = true;  // off: no part casts
    bool receives_shadow = true;
    bool force_unlit = false;
    float normal_scale = 1.0f;
    bool roughness_override = false;
    float roughness = 0.6f;
    bool metallic_override = false;
    float metallic = 0.0f;
    friend bool operator==(const MaterialSettings&, const MaterialSettings&) = default;
};

struct Settings {
    // Camera
    float move_speed = 4.0f;    // metres a second
    float sensitivity = 0.08f;  // degrees a pixel
    float fov = 65.0f;
    float near_plane = 0.05f;
    bool far_limit = false;
    float far_plane = 120.0f;
    // Sun
    bool sun = true;
    float sun_intensity = 1.0f; // times the reference scene's colour
    float sun_azimuth = azimuth_of(reference_sun);
    float sun_elevation = elevation_of(reference_sun);
    bool sun_animate = false;
    float sun_speed = 12.0f;    // degrees a second
    bool fill = false;
    float fill_intensity = 0.5f;
    // Ambient
    bool ambient = true;
    float ambient_intensity = 1.0f;
    bool ground = false;
    float ground_intensity = 1.0f;
    int environment = 0;        // 0: none, else the HDRI at index - 1
    float environment_intensity = 1.0f;
    float environment_rotation = 0.0f;
    // Point lights (at most 16 light a frame)
    std::vector<PointLight> points;
    bool light_markers = true;
    // Shadows
    bool shadows = true;
    int shadow_resolution = 2; // index into shadow_resolutions
    int shadow_cascades = 1;
    float shadow_distance = 100.0f;
    int shadow_area = 0;       // ShadowArea
    float shadow_extent = 12.0f; // half a side of the box round the camera
    // Ambient occlusion
    bool ao = false;
    float ao_radius = 0.5f;
    float ao_intensity = 1.0f;
    bool ao_half = false;
    // Contact shadows
    bool contact = false;
    float contact_distance = 0.5f;
    float contact_thickness = 0.1f;
    float contact_intensity = 1.0f;
    int contact_steps = 20;
    // Temporal anti-aliasing (replaces MSAA while on)
    bool taa = false;
    float taa_blend = 0.9f;
    float taa_sharpness = 0.0f;
    float taa_jitter_scale = 1.0f;
    int taa_jitter_samples = 8;
    bool taa_freeze = false;
    // View
    int msaa = 2;              // index into the device's sample counts
    float background[3] = {120, 150, 190};
    bool cull = true;
    int wireframe = 0;         // WireMode
    // Post
    bool hdr = false;
    float exposure = 1.0f;
    float bloom = 0.0f;
    float bloom_threshold = 1.0f;
    // Materials
    MaterialSettings materials;
    // Debug
    bool stats = true;
    bool gpu_timing = true;
};

inline constexpr unsigned shadow_resolutions[] = {512, 1024, 2048, 4096};

// What a view is made with: changing one means making the view again.
struct ViewKey {
    u32 samples = 4;
    Color background{};
    bool hdr = false;
    float exposure = 1.0f, bloom = 0.0f, threshold = 1.0f;
    Vec2i size{};
    friend bool operator==(const ViewKey&, const ViewKey&) = default;
};

inline Color background_colour(const Settings& s) {
    const auto c = [](float v) { return static_cast<u8>(std::clamp(v, 0.0f, 255.0f)); };
    return Color::rgb(c(s.background[0]), c(s.background[1]), c(s.background[2]));
}

// Points the sun lies toward, from its two angles.
inline Vec3f sun_direction(const Settings& s) {
    const float az = s.sun_azimuth * pi / 180.0f, el = s.sun_elevation * pi / 180.0f;
    return {std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)};
}

// The lights, shadows and occlusion for this frame. `environment` is the HDRI
// chosen (empty: none), `area` the model's bounds. The lights are made anew
// each time, so nothing set earlier lingers.
inline void apply(const Settings& s, Scene3D& scene, const Environment3D& environment, const Bounds3& area, Vec3f eye) {
    Lights3D& l = scene.lights;
    l = Lights3D{};
    l.sun = {.direction = sun_direction(s), .color = s.sun ? reference_sun_colour * s.sun_intensity : Vec3f{}};
    if (s.fill) l.fill.color = Vec3f{0.55f, 0.6f, 0.75f} * s.fill_intensity;
    l.ambient = s.ambient ? reference_ambient * s.ambient_intensity : Vec3f{};
    if (s.ground) l.ground_ambient = Vec3f{0.12f, 0.1f, 0.085f} * s.ground_intensity;
    l.environment = environment;
    l.environment_intensity = s.environment_intensity;
    l.environment_rotation = s.environment_rotation;
    l.points = s.points;
    if (s.shadows && s.sun) {
        l.shadow_area = s.shadow_area == int(ShadowArea::Auto)
                            ? area
                            : Bounds3{eye - Vec3f{s.shadow_extent, s.shadow_extent, s.shadow_extent},
                                      eye + Vec3f{s.shadow_extent, s.shadow_extent, s.shadow_extent}};
        l.shadow_resolution = shadow_resolutions[std::clamp(s.shadow_resolution, 0, 3)];
        l.shadow_cascades = static_cast<u32>(std::clamp(s.shadow_cascades, 1, 4));
        l.shadow_distance = s.shadow_distance;
    }
    scene.ambient_occlusion = {.enabled = s.ao, .radius = s.ao_radius, .intensity = s.ao_intensity, .half_resolution = s.ao_half};
    scene.contact_shadows = {.enabled = s.contact,
                             .distance = s.contact_distance,
                             .thickness = s.contact_thickness,
                             .intensity = s.contact_intensity,
                             .steps = static_cast<u32>(std::clamp(s.contact_steps, 4, 64))};
    scene.taa = {.enabled = s.taa,
                 .blend = s.taa_blend,
                 .sharpness = s.taa_sharpness,
                 .jitter_scale = s.taa_jitter_scale,
                 .jitter_samples = static_cast<u32>(std::clamp(s.taa_jitter_samples, 1, 16)),
                 .freeze_jitter = s.taa_freeze};
    scene.cull = s.cull;
}

// `m` as the settings want it.
inline Material3D adjust(Material3D m, const MaterialSettings& s) {
    if (!s.textures) {
        m.texture = {};
        m.color = Color::rgb(178, 178, 178);
    }
    if (!s.normal_maps) m.normal = {};
    if (!s.metal_rough_maps) m.metallic_roughness = {};
    if (!s.occlusion_maps) m.occlusion = {};
    if (!s.emissive) {
        m.emissive = 0.0f;
        m.emissive_texture = {};
    } else {
        m.emissive *= s.emissive_scale;
    }
    if (!s.cutout && m.blend == Blend3D::Mask) m.blend = Blend3D::Opaque;
    if (s.force_double_sided) m.double_sided = true;
    if (!s.casts_shadow) m.casts_shadow = false;
    if (!s.receives_shadow) m.receives_shadow = false;
    if (s.force_unlit) m.unlit = true;
    m.normal_scale *= s.normal_scale;
    if (s.roughness_override) {
        m.roughness = s.roughness;
        m.metallic_roughness = {};
    }
    if (s.metallic_override) {
        m.metallic = s.metallic;
        m.metallic_roughness = {};
    }
    return m;
}

} // namespace sponza_walk
