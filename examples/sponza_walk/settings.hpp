#pragma once

// Everything Sponza Walk can switch, in one struct, and how it is applied to a
// Scene3D each frame. A new renderer feature is a field here, a line in
// apply() and one row in the controls table (main.cpp).
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/shadow_cascades.hpp>
#include <kin/render3d/taa.hpp>

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
    bool shadow_cache = false;   // Lights3D::cache_shadows: the map is kept while nothing it depends on moves (a hit shows in the stats)
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
    // Levels of detail (meshes are made with levels at load: --lod-levels)
    bool lod = false;
    float lod_error = 1.0f;     // pixels
    float lod_bias = 1.0f;
    int lod_force = -1;         // -1: by distance
    bool lod_gpu = true;        // clusters: cut in compute where the device can
    bool lod_readback = false;  // read the GPU cut's counts back each frame (waits for the GPU)
    bool lod_occlusion = false; // clusters: those behind others are not drawn (Lod3D::occlusion; needs the GPU cut)
    // A skinned character (procedural: three joints, clips Sway, Bend and Wave), as entities with Animator3D (P1a part 2)
    bool character = false;
    int char_clip = 3;          // 0..2 a clip, 3 a 1D blend of Sway and Wave by char_blend
    float char_blend = 0.5f;    // the blend's parameter, 0 (Sway) to 1 (Wave)
    float char_speed = 1.0f;
    int char_count = 1;         // a row of up to 64 (their poses are in step)
    float char_scale = 1.0f;
    // Inverse kinematics (P1b) on a person of 18 joints (the Person rig, chosen by `ik`): a hand to a target, the head to the camera,
    // the feet on a floor and a step. The characters' clips are the person's Sway, Bend and Wave.
    bool ik = false;
    float ik_hand_x = 0.45f;    // the hand's target from the left shoulder, in the person's own axes (x to its left, y up, z forward), metres
    float ik_hand_y = 0.1f;
    float ik_hand_z = 0.45f;
    int ik_pole = 1;            // where the elbow points: 0 as animated, 1 down, 2 back, 3 up, 4 out
    float ik_weight = 1.0f;
    float ik_fade = 0.2f;       // seconds a weight takes to change
    float ik_stretch = 1.0f;    // the arm may lengthen to this factor
    bool ik_marker = true;      // a small red ball at the hand's target
    bool ik_look = false;       // the head looks at the camera
    float ik_look_weight = 1.0f;
    float ik_look_smooth = 0.15f; // seconds
    bool ik_feet = false;       // foot placement on the floor (and the step)
    bool ik_step = true;        // a step under the left foot
    float ik_step_height = 0.2f;
    bool ik_tail = false;       // a FABRIK chain on the tail, to a point of the person's own space
    float ik_tail_x = 0.5f;
    float ik_tail_y = 0.9f;
    float ik_tail_z = -0.35f;
    bool ik_limits = false;     // knee hinges with a twist limit at the hip, the arm folds to 150 degrees
    bool ik_lock = false;       // planted feet stay where they were put while the person moves
    bool ik_toes = false;       // toes lie on the ground under them
    bool ik_walk = false;       // the person drifts from side to side
    // Physics (kin/physics3d, P2b): a floor at y = 0 and boxes thrown from the camera (B, or the button), stepped at 60 Hz by
    // WorldPhysicsState3D over the viewer's ECS world, with the debug lines (the shapes, awake or asleep, and the contacts).
    bool physics = false;
    bool phys_debug = true;
    int phys_count = 10;        // boxes a throw
    float phys_size = 0.4f;     // metres a side
    float phys_speed = 8.0f;    // m/s along the view
    // Character controller (kin/physics3d, P2c): a person driven by a capsule controller over the physics floor and the test level
    // (tests/character_level.hpp, put in front of the camera when it goes on). WASD relative to the camera, Space jumps, Shift
    // sprints, Ctrl crouches; the camera follows behind. `ctrl_demo` plays a scripted walk instead of the keys (a headless run).
    bool ctrl = false;
    bool ctrl_debug = true;     // the capsule, the level's bodies and the character's contacts as lines
    bool ctrl_demo = false;
    float ctrl_speed = 4.0f;    // walk speed, m/s (sprint 1.75 times)
    float ctrl_jump = 5.0f;     // m/s up: the apex is v^2 / 2g
    float ctrl_slope = 45.0f;   // the steepest ground it stands on, degrees
    float ctrl_step = 0.4f;     // the highest step it climbs, m
    float ctrl_distance = 3.5f; // the camera behind it, m
    // Climb (kin/physics3d, P2d): a person with two hands that take hold of the world, on the climb level (tests/climb_level.hpp:
    // a wall with a ledge, an overhang, a smooth wall with holds, a platform that moves, a crate, ice, a low roof) in front of the
    // camera. WASD walk (while it hangs: along the surface and up it), the camera aims the reach, the mouse buttons (or Q and E)
    // hold with the left and the right hand, Space jumps (jumps off while it hangs), F pulls up, X lets go. `climb_demo` plays
    // a scripted climb instead of the keys (a headless run). Replaces the controller section's level when both are on.
    bool climb = false;
    bool climb_debug = true;    // the bodies, the grips (anchors, normals, the hands' lines), the hang place, what a free hand sees
    bool climb_demo = false;
    float climb_reach = 0.95f;  // the arm's reach, m
    float climb_stamina = 100.0f; // the resource's size: it drains by the mode (hanging 7 or 12 a second, sprinting 20)
    float climb_distance = 4.0f;  // the camera behind it, m
    // Ragdolls (kin/physics3d, P2e): `rag_count` people stand in a row in front of the camera on the physics floor, each with a
    // Ragdoll3D made from its skeleton. G knocks them down (their ragdolls start, blending in over `rag_blend`) and gets them up
    // (the clip blended back, put where the pelvis lies); H throws a ball along the view (what it hits starts its ragdoll).
    // `rag_demo` plays that by itself (a headless run).
    bool rag = false;
    bool rag_debug = true;      // the bones' capsules and the contacts as lines
    bool rag_demo = false;
    int rag_count = 3;
    float rag_blend = 0.15f;    // seconds the pose takes to go from the clip to the bodies
    float rag_hit = 60.0f;      // N s at a knocked-down person's chest (G)
    // Rope (kin/physics3d, P2e): a rope hung from a beam 25 degrees out from the vertical, drawn as a tube, with a person under
    // its end that takes hold with both hands (Q and E or the mouse buttons, as the climb), climbs (W, S), swings and jumps
    // off (Space). `rope_demo` plays a scripted climb and jump off, over and over.
    bool rope = false;
    bool rope_debug = false;    // the segments' capsules
    bool rope_demo = false;
    float rope_length = 6.0f;
    float rope_stamina = 400.0f;
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
        l.shadow_cascades = static_cast<u32>(std::clamp(s.shadow_cascades, 1, int(max_shadow_cascades)));
        l.shadow_distance = s.shadow_distance;
        l.cache_shadows = s.shadow_cache;
    }
    scene.ambient_occlusion = {.enabled = s.ao, .radius = s.ao_radius, .intensity = s.ao_intensity, .half_resolution = s.ao_half};
    scene.contact_shadows = {.enabled = s.contact,
                             .distance = s.contact_distance,
                             .thickness = s.contact_thickness,
                             .intensity = s.contact_intensity,
                             .steps = static_cast<u32>(std::clamp(s.contact_steps, int(min_contact_steps), int(max_contact_steps)))};
    scene.taa = {.enabled = s.taa,
                 .blend = s.taa_blend,
                 .sharpness = s.taa_sharpness,
                 .jitter_scale = s.taa_jitter_scale,
                 .jitter_samples = static_cast<u32>(std::clamp(s.taa_jitter_samples, 1, int(max_taa_jitter_samples))),
                 .freeze_jitter = s.taa_freeze};
    scene.lod = {.enabled = s.lod, .error_pixels = s.lod_error, .bias = s.lod_bias, .force_level = s.lod_force, .gpu_culling = s.lod_gpu,
                 .stats_readback = s.lod && s.lod_readback, .occlusion = {.enabled = s.lod && s.lod_occlusion}};
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
