#pragma once

// Everything Sponza Walk can switch, in one struct, and how it is applied to a
// Scene3D each frame. A new renderer feature is a field here, a line in
// apply() and one row in the controls table (main.cpp).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <kin/render3d/particles3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/shadow_cascades3d.hpp>
#include <kin/render3d/taa3d.hpp>
#include <kin/render3d/timeofday3d.hpp>
#include <string>
#include <vector>

namespace sponza_walk {
using namespace kin;

inline constexpr float pi = 3.14159265f;

// The reference scene's sun (tests/reference/scenes/sponza.json), as angles.
inline constexpr Vec3f reference_sun{0.4f, 1.0f, 0.12f};
inline constexpr LinearRgb reference_sun_colour{4.0f, 3.8f, 3.4f};
inline constexpr LinearRgb reference_ambient{0.36f, 0.4f, 0.48f};

inline float azimuth_of(Vec3f d) {
    return std::atan2(d.z, d.x) * 180.0f / pi;
}
inline float elevation_of(Vec3f d) {
    return std::atan2(d.y, std::hypot(d.x, d.z)) * 180.0f / pi;
}

enum class WireMode { Off, Overlay, Only };
enum class ShadowArea { Auto, AroundCamera };

// What is done to every material of the model (copies are changed, the
// model's own are kept).
struct MaterialSettings {
    bool textures = true; // base colour textures; off: flat grey
    bool normal_maps = true;
    bool metal_rough_maps = true;
    bool occlusion_maps = true;
    bool emissive = true;
    float emissive_scale = 1.0f;
    bool cutout = true;  // alpha cut-out (Blend3D::Mask); off: opaque
    bool blended = true; // alpha-blended and additive parts drawn at all
    bool force_double_sided = false;
    bool casts_shadow = true; // off: no part casts
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
    float move_speed = 4.0f;   // metres a second
    float sensitivity = 0.08f; // degrees a pixel
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
    float sun_speed = 12.0f; // degrees a second
    bool fill = false;
    float fill_intensity = 0.5f;
    // Ambient
    bool ambient = true;
    float ambient_intensity = 1.0f;
    bool ground = false;
    float ground_intensity = 1.0f;
    int environment = 0; // 0: none, else the HDRI at index - 1
    float environment_intensity = 1.0f;
    float environment_rotation = 0.0f;
    // GI is opt-in in the feature viewer (--preset gi); the interactive evaluation launcher enables it.
    bool gi = false;
    float gi_cell = 0.15f;
    int gi_levels = 3;
    int gi_rays = 64;
    int gi_per_render = 4096;
    float gi_intensity = 1.0f;
    // Point lights (at most 16 light a frame)
    std::vector<PointLightSource3D> points;
    bool light_markers = true;
    // Spot lights and the shadows of lights that are not the sun (P3b). `spots` are dropped at the camera (J) and shine
    // the way it looked; the flashlight is carried with the camera; a ring of `spot_ring` lamps hangs over the model's
    // middle, and `point_ring` point lamps (cube shadows) beside them. The maps are given to the most important
    // (`spot_shadow_count`, `point_shadow_count`); the moon's shadow is the fill light's own cascades (with the time of
    // day, faded in with the moon's height).
    std::vector<SpotLightSource3D> spots;
    bool flashlight = false;
    float flash_intensity = 45.0f; // the light a white surface shows one unit away
    float flash_inner = 7.0f;
    float flash_outer = 16.0f;
    float flash_radius = 30.0f;
    bool flash_shadow = true;
    int spot_ring = 0;
    float spot_ring_intensity = 60.0f;
    float spot_inner = 22.0f; // for the ring and the lights dropped from now on
    float spot_outer = 40.0f;
    float spot_radius = 16.0f;
    bool spot_shadows = true;       // a light made from now on asks for a shadow
    int spot_shadow_resolution = 2; // index into local_resolutions
    int spot_shadow_count = 4;      // Lights3D::max_shadowed_spots (0 to 8)
    float spot_bias = 1.0f;
    float spot_normal_offset = 1.5f;
    float spot_softness = 1.0f;
    int point_ring = 0;
    bool point_shadows = true;
    int point_shadow_count = 2; // Lights3D::max_shadowed_points (0 to 2)
    bool moon_shadow = false;   // the fill light's cascades (needs shadows on and, for the moon, the time of day)
    int moon_cascades = 2;
    // Particles and weather (P3c). Rain, snow, dust and mist (0 to 1) fall in volumes round the camera as a function of
    // the weather's clock (`weather_time`, seconds, which the walk advances); the wind blows them and the fires. The
    // weather may dim the light and thicken the fog (Weather3D::apply), the ground is the model's floor (rain splashes
    // on it). `fire` fires burn in the model's middle (each a flame and a few sparks, with a light); more are dropped
    // at the camera (F), at `fires`.
    float rain = 0.0f, snow = 0.0f, dust = 0.0f, mist = 0.0f;
    float wind_speed = 0.0f;
    float wind_direction = 20.0f; // degrees from +x toward +z
    float wind_gust = 0.4f;
    float wind_turbulence = 0.4f;
    bool weather_dims = true;
    bool weather_ground = true;
    bool rain_splash = true;
    bool soft_particles = true;
    float soft_distance = 0.6f;
    int fire = 0;
    float fire_rate = 500.0f;
    float fire_size = 0.5f;
    bool fire_sparks = true;
    bool fire_light = true;
    std::vector<Vec3f> fires;
    double weather_time = 0.0;
    // Shadows
    bool shadows = true;
    int shadow_resolution = 2; // index into shadow_resolutions
    int shadow_cascades = 1;
    float shadow_distance = 100.0f;
    int shadow_area = 0;         // ShadowArea
    float shadow_extent = 12.0f; // half a side of the box round the camera
    bool shadow_cache =
        false; // Lights3D::cache_shadows: the map is kept while nothing it depends on moves (a hit shows in the stats)
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
    // Sky and fog (P3a): a procedural atmosphere behind the model (seen through its open roof and the arches), fog that
    // takes its colour from it, and a time of day that drives the sun, the moonlight, the ambient light and the sky
    // together.
    bool sky = false;
    float sky_turbidity = 2.5f; // haze
    float sky_sun_intensity =
        3.0f; // the sun's light above the atmosphere (the time of day makes the sun's own from it)
    float sky_intensity = 1.0f;
    float sky_sun_size = 0.8f;  // degrees across
    float sky_sun_disc = 30.0f; // the disc's radiance over the sun's light (HDR: bloom picks it up)
    bool sky_stars = true;
    bool sky_moon = true;
    bool sky_lights = false; // Lights3D::sky_lights_scene: ambient light and reflections from the sky (replaces the
                             // hemisphere ambient)
    bool fog = false;
    float fog_density = 0.004f; // per metre of distance
    float fog_height_density = 0.0f;
    float fog_falloff = 0.1f; // per metre of height
    float fog_base = 0.0f;    // the height its density is measured at
    float fog_start = 0.0f;
    float fog_max = 1.0f;     // the greatest opacity
    bool fog_from_sky = true; // its colour is the sky's in the view's direction
    float fog_sun_scatter = 0.25f;
    float fog_anisotropy = 0.6f;
    bool tod = false; // the time of day sets the sun, the fill (the moon), the ambient light, the moon of the sky and
                      // the fog's colour
    float tod_hours = 12.0f; // solar time
    float tod_day = 172.0f;  // day of the year
    float tod_latitude = 45.0f;
    bool tod_play = false;
    float tod_speed = 1800.0f; // game seconds a second (1800: a game hour in two seconds)
    // Levels of detail (meshes are made with levels at load: --lod-levels)
    bool lod = false;
    float lod_error = 1.0f; // pixels
    float lod_bias = 1.0f;
    int lod_force = -1;         // -1: by distance
    bool lod_gpu = true;        // clusters: cut in compute where the device can
    bool lod_readback = false;  // read the GPU cut's counts back each frame (waits for the GPU)
    bool lod_occlusion = false; // clusters: those behind others are not drawn (Lod3D::occlusion; needs the GPU cut)
    // A skinned character (procedural: three joints, clips Sway, Bend and Wave), as entities with Animator3D (P1a part
    // 2)
    bool character = false;
    int char_clip = 3;       // 0..2 a clip, 3 a 1D blend of Sway and Wave by char_blend
    float char_blend = 0.5f; // the blend's parameter, 0 (Sway) to 1 (Wave)
    float char_speed = 1.0f;
    int char_count = 1; // a row of up to 64 (their poses are in step)
    float char_scale = 1.0f;
    // Inverse kinematics (P1b) on a person of 18 joints (the Person rig, chosen by `ik`): a hand to a target, the head
    // to the camera, the feet on a floor and a step. The characters' clips are the person's Sway, Bend and Wave.
    bool ik = false;
    float ik_hand_x = 0.45f; // the hand's target from the left shoulder, in the person's own axes (x to its left, y up,
                             // z forward), metres
    float ik_hand_y = 0.1f;
    float ik_hand_z = 0.45f;
    int ik_pole = 1; // where the elbow points: 0 as animated, 1 down, 2 back, 3 up, 4 out
    float ik_weight = 1.0f;
    float ik_fade = 0.2f;    // seconds a weight takes to change
    float ik_stretch = 1.0f; // the arm may lengthen to this factor
    bool ik_marker = true;   // a small red ball at the hand's target
    bool ik_look = false;    // the head looks at the camera
    float ik_look_weight = 1.0f;
    float ik_look_smooth = 0.15f; // seconds
    bool ik_feet = false;         // foot placement on the floor (and the step)
    bool ik_step = true;          // a step under the left foot
    float ik_step_height = 0.2f;
    bool ik_tail = false; // a FABRIK chain on the tail, to a point of the person's own space
    float ik_tail_x = 0.5f;
    float ik_tail_y = 0.9f;
    float ik_tail_z = -0.35f;
    bool ik_limits = false; // knee hinges with a twist limit at the hip, the arm folds to 150 degrees
    bool ik_lock = false;   // planted feet stay where they were put while the person moves
    bool ik_toes = false;   // toes lie on the ground under them
    bool ik_walk = false;   // the person drifts from side to side
    // Physics (kin/physics3d, P2b): a floor at y = 0 and boxes thrown from the camera (B, or the button), stepped at 60
    // Hz by WorldPhysicsState3D over the viewer's ECS world, with the debug lines (the shapes, awake or asleep, and the
    // contacts).
    bool physics = false;
    bool phys_debug = true;
    int phys_count = 10;     // boxes a throw
    float phys_size = 0.4f;  // metres a side
    float phys_speed = 8.0f; // m/s along the view
    // Character controller (kin/physics3d, P2c): a person driven by a capsule controller over the physics floor and the
    // test level (tests/character_level.hpp, put in front of the camera when it goes on). WASD relative to the camera,
    // Space jumps, Shift sprints, Ctrl crouches; the camera follows behind. `ctrl_demo` plays a scripted walk instead
    // of the keys (a headless run).
    bool ctrl = false;
    bool ctrl_debug = true; // the capsule, the level's bodies and the character's contacts as lines
    bool ctrl_demo = false;
    float ctrl_speed = 4.0f;    // walk speed, m/s (sprint 1.75 times)
    float ctrl_jump = 5.0f;     // m/s up: the apex is v^2 / 2g
    float ctrl_slope = 45.0f;   // the steepest ground it stands on, degrees
    float ctrl_step = 0.4f;     // the highest step it climbs, m
    float ctrl_distance = 3.5f; // the camera behind it, m
    // Climb (kin/physics3d, P2d): a person with two hands that take hold of the world, on the climb level
    // (tests/climb_level.hpp: a wall with a ledge, an overhang, a smooth wall with holds, a platform that moves, a
    // crate, ice, a low roof) in front of the camera. WASD walk (while it hangs: along the surface and up it), the
    // camera aims the reach, the mouse buttons (or Q and E) hold with the left and the right hand, Space jumps (jumps
    // off while it hangs), F pulls up, X lets go. `climb_demo` plays a scripted climb instead of the keys (a headless
    // run). Replaces the controller section's level when both are on.
    bool climb = false;
    bool climb_debug =
        true; // the bodies, the grips (anchors, normals, the hands' lines), the hang place, what a free hand sees
    bool climb_demo = false;
    float climb_reach = 0.95f;    // the arm's reach, m
    float climb_stamina = 100.0f; // the resource's size: it drains by the mode (hanging 7 or 12 a second, sprinting 20)
    float climb_distance = 4.0f;  // the camera behind it, m
    // Ragdolls (kin/physics3d, P2e): `rag_count` people stand in a row in front of the camera on the physics floor,
    // each with a Ragdoll3D made from its skeleton. G knocks them down (their ragdolls start, blending in over
    // `rag_blend`) and gets them up (the clip blended back, put where the pelvis lies); H throws a ball along the view
    // (what it hits starts its ragdoll). `rag_demo` plays that by itself (a headless run).
    bool rag = false;
    bool rag_debug = true; // the bones' capsules and the contacts as lines
    bool rag_demo = false;
    int rag_count = 3;
    float rag_blend = 0.15f; // seconds the pose takes to go from the clip to the bodies
    float rag_hit = 60.0f;   // N s at a knocked-down person's chest (G)
    // Rope (kin/physics3d, P2e): a rope hung from a beam 25 degrees out from the vertical, drawn as a tube, with a
    // person under its end that takes hold with both hands (Q and E or the mouse buttons, as the climb), climbs (W, S),
    // swings and jumps off (Space). `rope_demo` plays a scripted climb and jump off, over and over.
    bool rope = false;
    bool rope_debug = false; // the segments' capsules
    bool rope_demo = false;
    float rope_length = 6.0f;
    float rope_stamina = 400.0f;
    // View
    int msaa = 2;                  // index into the device's sample counts
    int anisotropy = 1;            // Scene3D::anisotropy (1: off, to 16)
    float texture_lod_bias = 0.0f; // Scene3D::texture_lod_bias (-2 to 2)
    float background[3] = {120, 150, 190};
    bool cull = true;
    int wireframe = 0; // WireMode
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
inline constexpr unsigned local_resolutions[] = {256, 512, 1024, 2048};

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
inline void apply(const Settings& s, Scene3D& scene, const Environment3D& environment, const Bounds3D& area,
                  Vec3f eye) {
    Lights3D& l = scene.lights;
    l = Lights3D{};
    l.sun = {.direction = sun_direction(s), .color = s.sun ? reference_sun_colour * s.sun_intensity : LinearRgb{}};
    if (s.fill)
        l.fill.color = kin::LinearRgb{0.55f, 0.6f, 0.75f} * s.fill_intensity;
    l.ambient = s.ambient ? reference_ambient * s.ambient_intensity : LinearRgb{};
    if (s.ground)
        l.ground_ambient = {true, kin::LinearRgb{0.12f, 0.1f, 0.085f} * s.ground_intensity};
    l.environment = environment;
    l.environment_intensity = s.environment_intensity;
    l.environment_rotation = s.environment_rotation;
    l.points = s.points;
    // Spot lights and local shadows (P3b), none unless asked for.
    {
        const unsigned local_res = local_resolutions[std::clamp(s.spot_shadow_resolution, 0, 3)];
        const auto configure_shadow = [&](auto& light, bool wanted) {
            light.shadow_enabled = wanted;
            light.shadow_resolution = local_res;
            light.shadow_bias = s.spot_bias;
            light.shadow_normal_offset = s.spot_normal_offset;
            light.shadow_softness = s.spot_softness;
        };
        l.spots = s.spots;
        if (s.flashlight) {
            const Camera3D& cam = scene.camera;
            SpotLightSource3D torch;
            torch.position = cam.position + cam.right() * 0.18f - cam.up() * 0.14f + cam.forward() * 0.1f;
            torch.direction = cam.forward();
            torch.color = kin::LinearRgb{1.0f, 0.96f, 0.88f} * s.flash_intensity;
            torch.radius = s.flash_radius;
            torch.size = 0.04f;
            torch.inner_angle = s.flash_inner;
            torch.outer_angle = std::max(s.flash_outer, s.flash_inner + 0.5f);
            configure_shadow(torch, s.flash_shadow);
            torch.id = 0x7F000001u;
            l.spots.push_back(torch);
        }
        const Vec3f centre = area.center(), half = area.size() * 0.5f;
        for (int i = 0; i < s.spot_ring; ++i) {
            const float t = s.spot_ring > 1 ? float(i) / float(s.spot_ring - 1) * 2.0f - 1.0f : 0.0f;
            SpotLightSource3D lamp;
            lamp.position = {centre.x + t * half.x * 0.7f, area.min.y + std::min(half.y * 1.1f, 7.0f),
                             centre.z + (i % 2 ? 0.25f : -0.25f) * half.z};
            lamp.direction = {0.0f, -1.0f, i % 2 ? -0.2f : 0.2f};
            lamp.color = kin::LinearRgb{1.0f, 0.82f, 0.6f} * s.spot_ring_intensity;
            lamp.radius = s.spot_radius;
            lamp.size = 0.15f;
            lamp.inner_angle = s.spot_inner;
            lamp.outer_angle = std::max(s.spot_outer, s.spot_inner + 0.5f);
            configure_shadow(lamp, s.spot_shadows);
            lamp.id = 0x7E000000u + static_cast<unsigned>(i);
            l.spots.push_back(lamp);
        }
        for (int i = 0; i < s.point_ring; ++i) {
            const float t = s.point_ring > 1 ? float(i) / float(s.point_ring - 1) * 2.0f - 1.0f : 0.0f;
            PointLightSource3D lamp;
            lamp.position = {centre.x + t * half.x * 0.45f, area.min.y + std::min(half.y * 0.55f, 3.2f),
                             centre.z + (i % 2 ? 0.3f : -0.3f) * half.z};
            lamp.color = kin::LinearRgb{0.9f, 0.95f, 1.0f} * (s.spot_ring_intensity * 0.5f);
            lamp.radius = s.spot_radius;
            lamp.size = 0.2f;
            configure_shadow(lamp, s.point_shadows);
            lamp.id = 0x7D000000u + static_cast<unsigned>(i);
            l.points.push_back(lamp);
        }
        l.max_shadowed_spots = static_cast<u32>(std::clamp(s.spot_shadow_count, 0, 8));
        l.max_shadowed_points = static_cast<u32>(std::clamp(s.point_shadow_count, 0, 2));
        l.local_shadow_resolution = local_res;
        l.fill_shadow = {.enabled = s.moon_shadow,
                         .strength = 1.0f,
                         .cascades = static_cast<u32>(std::clamp(s.moon_cascades, 1, 4)),
                         .resolution = 1024};
    }
    if (s.shadows && (s.sun || s.moon_shadow)) {
        l.shadow_area = s.shadow_area == int(ShadowArea::Auto)
                            ? area
                            : Bounds3D{eye - Vec3f{s.shadow_extent, s.shadow_extent, s.shadow_extent},
                                       eye + Vec3f{s.shadow_extent, s.shadow_extent, s.shadow_extent}};
        l.shadow_resolution = shadow_resolutions[std::clamp(s.shadow_resolution, 0, 3)];
        l.shadow_cascades = static_cast<u32>(std::clamp(s.shadow_cascades, 1, int(max_shadow_cascades)));
        l.shadow_distance = s.shadow_distance;
        l.cache_shadows = s.shadow_cache;
    }
    // Keep the model inside the outer probes' fade band, including the floor and roof. The guard is based on the
    // coarsest requested spacing; the renderer still bounds the actual voxel/probe allocations.
    const u32 gi_levels = static_cast<u32>(std::clamp(s.gi_levels, 1, int(max_gi_levels)));
    const float gi_cell = std::clamp(s.gi_cell, 0.05f, 1.0f);
    const float guard = 3.0f * 4.0f * gi_cell * static_cast<float>(1u << (gi_levels - 1));
    const Vec3f padding{guard, guard, guard};
    scene.global_illumination = {.enabled = s.gi,
                                 .area = {area.min - padding, area.max + padding},
                                 .cell = gi_cell,
                                 .levels = gi_levels,
                                 .probes = {.rays = static_cast<u32>(std::clamp(s.gi_rays, 16, 256)),
                                            .per_render = static_cast<u32>(std::clamp(s.gi_per_render, 0, 16384)),
                                            .intensity = std::clamp(s.gi_intensity, 0.0f, 3.0f)}};
    scene.ambient_occlusion = {
        .enabled = s.ao, .radius = s.ao_radius, .intensity = s.ao_intensity, .half_resolution = s.ao_half};
    scene.contact_shadows = {
        .enabled = s.contact,
        .distance = s.contact_distance,
        .thickness = s.contact_thickness,
        .intensity = s.contact_intensity,
        .steps = static_cast<u32>(std::clamp(s.contact_steps, int(min_contact_steps), int(max_contact_steps)))};
    scene.taa = {.enabled = s.taa,
                 .blend = s.taa_blend,
                 .sharpness = s.taa_sharpness,
                 .jitter_scale = s.taa_jitter_scale,
                 .jitter_samples = static_cast<u32>(std::clamp(s.taa_jitter_samples, 1, int(max_taa_jitter_samples)))};
    scene.debug.freeze_jitter = s.taa_freeze;
    scene.lod = {.enabled = s.lod,
                 .error_pixels = s.lod_error,
                 .bias = s.lod_bias,
                 .occlusion = {.enabled = s.lod && s.lod_occlusion}};
    scene.debug.force_level = s.lod_force;
    scene.debug.gpu_culling = s.lod_gpu;
    scene.debug.stats_readback = s.lod && s.lod_readback;
    scene.cull = s.cull;
    scene.anisotropy = static_cast<u32>(std::clamp(s.anisotropy, 1, 16));
    scene.texture_lod_bias = s.texture_lod_bias;
    // The sky and the fog (the time of day, when it drives them, last: it replaces the sun's direction and colour, the
    // fill and the ambient).
    scene.sky = {.enabled = s.sky,
                 .turbidity = s.sky_turbidity,
                 .sun_intensity = s.sky_sun_intensity,
                 .intensity = s.sky_intensity,
                 .sun_size = s.sky_sun_size,
                 .sun_disc = s.sky_sun_disc,
                 .stars = s.sky_stars,
                 .moon = false};
    scene.fog = {.enabled = s.fog,
                 .density = s.fog_density,
                 .height_density = s.fog_height_density,
                 .height_falloff = s.fog_falloff,
                 .base_height = s.fog_base,
                 .start = s.fog_start,
                 .max_opacity = s.fog_max,
                 .colour = {0.62f, 0.68f, 0.76f},
                 .colour_from_sky = s.fog_from_sky,
                 .sun_scatter = s.fog_sun_scatter,
                 .sun_anisotropy = s.fog_anisotropy};
    l.sky_lights_scene = s.sky_lights;
    if (s.tod) {
        const LinearRgb own_fog = scene.fog.colour;
        const bool moon = s.sky_moon;
        kin::apply(
            {.hours = s.tod_hours, .day_of_year = s.tod_day, .latitude = s.tod_latitude, .shadows = s.moon_shadow}, l,
            scene.sky, scene.fog);
        scene.sky.moon = scene.sky.moon && moon;
        if (s.fog_from_sky)
            scene.fog.colour = own_fog;
        if (!s.sun)
            l.sun.color = {};
    }
    // Particles and weather (P3c), last: the weather dims the light the time of day has just written, and thickens the
    // fog it has.
    {
        Weather3D& w = scene.weather;
        w = Weather3D{};
        w.rain.intensity = s.rain;
        w.snow.intensity = s.snow;
        w.dust.intensity = s.dust;
        w.mist.intensity = s.mist;
        w.rain.splash = s.rain_splash ? w.rain.splash : 0.0f;
        for (WeatherLayer3D* layer : {&w.rain, &w.snow, &w.dust, &w.mist}) {
            layer->look.soft = s.soft_particles;
            layer->look.soft_distance = layer == &w.mist ? std::max(s.soft_distance, 2.0f) : s.soft_distance;
        }
        w.wind.direction = {std::cos(s.wind_direction * pi / 180.0f), 0.0f, std::sin(s.wind_direction * pi / 180.0f)};
        w.wind.speed = s.wind_speed;
        w.wind.gust_strength = s.wind_gust;
        w.wind.turbulence = s.wind_turbulence;
        w.has_ground = s.weather_ground;
        w.ground_height = area.min.y;
        w.time = s.weather_time;
        if (s.weather_dims && w.active()) {
            const Fog3D base = scene.fog;
            w.apply(l, scene.fog, base);
        }
    }
}

// A fire's flame and its sparks, as particle systems' descs (the walk makes the systems). The flame is additive, soft,
// rising and widening; the sparks are stretched along their velocity and fall, with drag.
inline ParticleDesc3D fire_flame(const Settings& s) {
    ParticleDesc3D d;
    d.shape = ParticleShape3D::Cone;
    d.extent = {0.25f * s.fire_size * 2.0f, 1.0f, 1.0f};
    d.cone_angle = 14.0f;
    d.rate = s.fire_rate;
    d.max_particles = 4000;
    d.lifetime = {0.7f, 1.4f};
    d.speed = {0.9f, 1.8f};
    d.size = {0.35f * s.fire_size * 2.0f, 0.55f * s.fire_size * 2.0f};
    d.size_start = 0.5f;
    d.size_mid = 1.0f;
    d.size_end = 0.15f;
    d.gravity = {0.0f, 1.2f, 0.0f}; // buoyancy
    d.drag = 0.8f;
    d.rotation = {0.0f, 360.0f};
    d.spin = {-60.0f, 60.0f};
    d.colour_start = {1.0f, 0.62f, 0.2f};
    d.colour_end = {0.8f, 0.12f, 0.02f};
    d.alpha_start = 0.55f;
    d.alpha_end = 0.0f;
    d.fade_in = 0.12f;
    d.colour_variation = 0.25f;
    d.look.blend = ParticleBlend3D::Additive;
    d.look.softness = 1.0f;
    d.look.soft = s.soft_particles;
    d.look.soft_distance = s.soft_distance;
    d.look.lit = 0.0f;
    return d;
}

inline ParticleDesc3D fire_sparks(const Settings& s) {
    ParticleDesc3D d;
    d.shape = ParticleShape3D::Cone;
    d.extent = {0.12f * s.fire_size * 2.0f, 1.0f, 1.0f};
    d.cone_angle = 30.0f;
    d.rate = s.fire_rate * 0.12f;
    d.max_particles = 1500;
    d.lifetime = {0.8f, 2.2f};
    d.speed = {1.5f, 4.5f};
    d.size = {0.012f, 0.025f};
    d.gravity = {0.0f, -4.0f, 0.0f};
    d.drag = 0.4f;
    d.colour_start = {1.0f, 0.75f, 0.35f};
    d.colour_end = {1.0f, 0.2f, 0.05f};
    d.alpha_start = 1.0f;
    d.alpha_end = 0.0f;
    d.look.blend = ParticleBlend3D::Additive;
    d.look.facing = ParticleFacing3D::Stretched;
    d.look.stretch = 0.045f;
    d.look.stretch_min = 0.03f;
    d.look.softness = 0.6f;
    d.look.soft = false;
    d.look.lit = 0.0f;
    return d;
}

// `m` as the settings want it.
inline Material3D adjust(Material3D m, const MaterialSettings& s) {
    if (!s.textures) {
        m.texture = {};
        m.color = Color::rgb(178, 178, 178);
    }
    if (!s.normal_maps)
        m.normal = {};
    if (!s.metal_rough_maps)
        m.metallic_roughness = {};
    if (!s.occlusion_maps)
        m.occlusion = {};
    if (!s.emissive) {
        m.emissive = 0.0f;
        m.emissive_texture = {};
    } else {
        m.emissive *= s.emissive_scale;
    }
    if (!s.cutout && m.blend == Blend3D::Mask)
        m.blend = Blend3D::Opaque;
    if (s.force_double_sided)
        m.double_sided = true;
    if (!s.casts_shadow)
        m.casts_shadow = false;
    if (!s.receives_shadow)
        m.receives_shadow = false;
    if (s.force_unlit)
        m.unlit = true;
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
