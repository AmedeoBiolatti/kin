#pragma once

// What a 3D view shows, filled each frame (docs/3d_plan.md): a camera,
// lights, and meshes to draw. Plain data, no GPU: Renderer3D::render() draws it.
//
//     kin::Scene3D scene;
//     scene.camera = kin::Camera3D::orbit(target, 30.0f, 40.0f, 8.0f);
//     scene.draw(cube, kin::Mat4f::rotation_y(angle), {.color = red});
//     scene.draw_instanced(rock, rock_instances);
//     r3d.render(view, scene);

#include <kin/core/math3d.hpp>
#include <kin/render3d/camera3d.hpp>
#include <kin/render3d/environment.hpp>
#include <kin/render3d/mesh.hpp>
#include <kin/render3d/particles3d.hpp>
#include <kin/render3d/sky3d.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/texture.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace kin {

class Model;

// How a material's colour goes over what is behind it.
enum class Blend3D : u8 {
    Opaque,   // replaces it, hides what is further
    Alpha,    // over it by its alpha (drawn after the opaque, furthest first)
    Additive, // adds light to it: sparks, glows, flashes (in any order)
    // Opaque where the base colour's alpha reaches Material3D::alpha_cutoff,
    // not drawn where it falls short: leaves, fences, grates. Drawn with the
    // opaque, and casts the cut-out shadow.
    Mask,
};

// What a texture does past 0..1 (glTF's sampler wrap modes).
enum class Wrap3D : u8 {
    Clamp,  // the edge texel goes on
    Repeat, // tiles
    Mirror, // tiles, every other copy flipped
};

// The backend's copy of a material shader (Renderer3D::create_material_shader).
class IMaterialShaderBackend {
public:
    virtual ~IMaterialShaderBackend() = default;
};

// A fragment shader of a material's own, made by Renderer3D::create_material_shader
// or compile_material_shader; cheap to copy (copies share it).
class MaterialShader3D {
public:
    MaterialShader3D() = default;
    explicit MaterialShader3D(std::shared_ptr<IMaterialShaderBackend> backend) : _backend(std::move(backend)) {}

    bool valid() const { return _backend != nullptr; }
    explicit operator bool() const { return valid(); }
    const std::shared_ptr<IMaterialShaderBackend>& backend() const { return _backend; }

    friend bool operator==(const MaterialShader3D& a, const MaterialShader3D& b) { return a._backend == b._backend; }

private:
    std::shared_ptr<IMaterialShaderBackend> _backend;
};

struct Material3D {
    Color color = colors::white; // times the mesh's vertex colours and each instance's tint
    // Light of its own, as a share of its colour: glows in the dark, and
    // past white feeds bloom in an HDR view.
    f32 emissive = 0.0f;
    bool unlit = false;        // its colour as it is, no light or shade
    bool double_sided = false; // its back faces drawn too (else culled)
    Blend3D blend = Blend3D::Opaque;
    // Opaque meshes cast the sun's shadow (Lights3D::shadow_area) unless this
    // is off, and take it unless `receives_shadow` is.
    bool casts_shadow = true;
    bool receives_shadow = true;
    // Only its triangles' edges, a pixel wide, in its colour (no light),
    // drawn just in front of the faces they edge: a model's wireframe drawn
    // over it, or lines (triangles a, b, b) on the ground. Needs line fill
    // (desktop GPUs have it): where its pipeline can't be made, not drawn.
    bool wireframe = false;
    // The base colour's texture (the mesh's UVs), times the colours above;
    // none: white.
    Texture texture{};

    // glTF's metallic-roughness model. `color` is the base colour: a
    // dielectric's diffuse colour (it reflects 4% as a mirror would), a
    // metal's reflection. The defaults are rough plastic, as before.
    f32 metallic = 0.0f;  // 0 dielectric, 1 metal
    f32 roughness = 1.0f; // 0 a mirror, 1 chalk
    // Times the two factors above: roughness in G, metallic in B (linear
    // data, as glTF stores them); none: 1.
    Texture metallic_roughness{};
    // Tangent-space normals (linear data, green up, as glTF stores them),
    // their slope times `normal_scale`; none: the mesh's normals. The tangents
    // come from the UVs, so the mesh needs them.
    Texture normal{};
    f32 normal_scale = 1.0f;
    // Ambient light let through, in R (linear data), by `occlusion_strength`
    // (1: all the map says, 0: none); none: all.
    Texture occlusion{};
    f32 occlusion_strength = 1.0f;
    // Light of its own, as a colour (sRGB, as the base colour's texture),
    // times `emissive`; none: the base colour's share, as above.
    Texture emissive_texture{};
    // Blend3D::Mask: below this alpha (the base colour's, times the mesh's,
    // the material's and the copy's) a pixel is not drawn.
    f32 alpha_cutoff = 0.5f;
    // How all of its textures run past 0..1 along u and v (glTF's default,
    // and what models set, is Repeat).
    Wrap3D wrap_u = Wrap3D::Clamp;
    Wrap3D wrap_v = Wrap3D::Clamp;
    // A fragment shader of its own in place of kin's (docs/rendering_3d.md,
    // Custom shaders), which may light itself as kin's does or not. It reads
    // `shader_params` (four vec4s) and all the material's fields above. The
    // shadow and pick passes treat the material as they would without it.
    MaterialShader3D shader{};
    std::array<f32, 16> shader_params{};

    friend bool operator==(const Material3D&, const Material3D&) = default;
};

// One copy of a mesh in a draw_instanced() call. A transform that mirrors
// (a negative scale) turns its faces inside out: draw it double-sided.
struct MeshInstance {
    Mat4f transform{};
    Color tint = colors::white;
    u32 id = 0; // what Renderer3D::pick says is under a pixel showing it
};

// Light from far away, in one direction: the sun.
struct DirectionalLight {
    Vec3f direction{-0.4f, 1.0f, 0.3f}; // toward the light (any length but zero)
    Vec3f color{1.0f, 1.0f, 1.0f};      // linear; above 1 is brighter
};

// What a shadow-casting light that is not the sun is asked to do (P3b). Every number is a default that suits a lamp or a
// torch at a few metres: the resolution decides the sharpness, `bias` and `normal_offset` the acne against the detaching
// of a shadow from its caster (both in texels of the map at the distance of the surface, so they scale with the map).
struct LightShadow3D {
    bool enabled = false;
    // The map's side, in texels; 0: Lights3D::local_shadow_resolution. A map is drawn into a corner of the shared array
    // (whose side is the largest any light asks for), so a small one costs less to draw, not less memory.
    u32 resolution = 0;
    // A depth bias away from the light, in texels, times (1 + the surface's slope to the light): 1 is enough for most
    // surfaces; more hides acne on grazing ones and shows as a shadow that starts later than its caster.
    f32 bias = 1.0f;
    // The surface's point is pushed this many texels along its normal before the map is read (removes the acne on
    // curved surfaces that the bias would need a large number for).
    f32 normal_offset = 1.5f;
    // The map's near plane (units; a caster nearer the light than this casts nothing). Raise it to spend the depth
    // range on the scene; a small near plane costs depth precision far away.
    f32 near_plane = 0.05f;
    // Past this distance from the light its shadow fades to nothing (units; 0: the light's radius, where the light is
    // nothing anyway). The map reaches as far.
    f32 distance = 0.0f;
    // The filter's width in texels (the taps lie this far apart): 1 is the sun's, up to about 1.5 is smooth, more
    // shows the nine taps as bands.
    f32 softness = 1.0f;
};

// Light from a point: a muzzle flash, a lamp. It falls off with the square of
// the distance, as light does, and is faded to nothing at `radius` (smoothly,
// over its outer part) so that few lights reach a pixel.
struct PointLight {
    Vec3f position{};
    // Linear; the light a white surface facing it shows, at one unit away.
    // Twice as far is a quarter as bright: a flash a few units from the wall
    // takes tens.
    Vec3f color{1.0f, 1.0f, 1.0f};
    f32 radius = 5.0f;
    // The light's own size (units): nearer than this it is no brighter, as
    // the light spreads over a body that big (a flash, a fireball, a bulb).
    // At least 0.01.
    f32 size = 0.01f;
    // A shadow (P3b): drawn into a cube of six faces, at most Lights3D::max_shadowed_points a frame, chosen by importance.
    LightShadow3D shadow{};
    // Names the light across frames, for the choice of which light has a shadow map (hysteresis) and for its place in a
    // cache; 0: its index in Lights3D::points.
    u32 id = 0;
};

// Light from a point in a cone: a torch, a lamp shade, a car's headlight (P3b). The same photometry as PointLight
// (`color` is what a white surface facing it, in the cone's middle, shows at one unit) and the same radius and size,
// inside a cone: full light up to `inner_angle` from its axis, falling to none at `outer_angle`, by the glTF punctual
// light's curve, t = clamp((cos a - cos outer) / (cos inner - cos outer), 0, 1), light times t * t (so it is exactly
// zero from the outer angle on and exactly full inside the inner one). Angles are half angles, in degrees.
struct SpotLight {
    Vec3f position{};
    Vec3f direction{0.0f, -1.0f, 0.0f}; // the way it shines (any length but zero)
    Vec3f color{1.0f, 1.0f, 1.0f};
    f32 radius = 10.0f;
    f32 size = 0.01f;
    f32 inner_angle = 20.0f; // degrees from the axis (at least 0)
    f32 outer_angle = 30.0f; // degrees; below 90, above `inner_angle` (clamped to 0.1 degrees beyond it)
    // A perspective shadow map over the cone, at most Lights3D::max_shadowed_spots a frame (the most important ones).
    LightShadow3D shadow{};
    u32 id = 0; // as PointLight::id, over Lights3D::spots
};

// A second directional light's shadow: the moon's, when the fill light is the moon (P3b). Its own cascade set, drawn from
// the same casters and over the same `Lights3D::shadow_area` as the sun's, so that at night, or at dusk beside the sun's,
// there are shadows from the light that is there.
struct DirectionalShadow3D {
    bool enabled = false;
    // How dark the shadow is: 1 is as the sun's, 0 none (a map is not drawn at 0). TimeOfDay3D::apply fades it with the
    // moon's height and strength, so that a rising moon does not make shadows pop in.
    f32 strength = 1.0f;
    // 1 to 4 (the sun's `Lights3D::shadow_cascades`, for the fill). Two is enough for the moon's soft, far light.
    u32 cascades = 2;
    u32 resolution = 0; // each cascade's side; 0: Lights3D::shadow_resolution
    f32 distance = 0.0f; // how far from the camera they reach; 0: Lights3D::shadow_distance
};

struct Lights3D {
    DirectionalLight sun{};
    // A second sun that casts no shadow, to lift the far side (black: none).
    DirectionalLight fill{.direction = {0.6f, 0.4f, -0.5f}, .color = {0.0f, 0.0f, 0.0f}};
    Vec3f ambient{0.25f, 0.25f, 0.28f}; // light from everywhere, linear
    // With it, ambient light comes from the sky above (`ambient`) to the
    // ground below (this), by which way a face looks; without, from all round.
    std::optional<Vec3f> ground_ambient{};
    // Light from all round, from a picture (Renderer3D::load_environment):
    // surfaces take their diffuse light from it by their normal and reflect
    // it by roughness, in place of `ambient` (which is then not used; sun,
    // fill and point lights go on). `environment_intensity` times its
    // colours; `environment_rotation` turns it about +y, in degrees. None:
    // the hemisphere above.
    Environment3D environment{};
    f32 environment_intensity = 1.0f;
    f32 environment_rotation = 0.0f;
    // With Scene3D::sky enabled (procedural): the environment is made from the sky, in place of `environment` and
    // `ambient`: ambient light and reflections agree with the sky you see and with the time of day (P3a). It is drawn
    // into a small panorama, filtered as any environment is, and made again only when the sun or the sky moved
    // (Sky3D::light_threshold, light_interval; Render3DStats::sky_env_*). `environment_intensity` scales it;
    // `environment_rotation` does not apply (it is in world space). Off, or with the image sky, as before.
    bool sky_lights_scene = false;
    // Where the sun casts shadows: a box round what casts and receives them
    // (the level, say). Empty: no shadows. Smaller is sharper.
    Bounds3 shadow_area{};
    u32 shadow_resolution = 2048; // the shadow map's side, in texels
    // 1: one map over the whole area. 2 to 4 (more is 4): that many maps, each
    // fitted to a slice of the camera's view, nearer ones sharper, for a
    // level too big for one map to be sharp. Each is `shadow_resolution`
    // square. The area still limits what casts and receives. Without a
    // depth array to render into, one map is used.
    u32 shadow_cascades = 1;
    // With cascades: how far from the camera they reach (units; past it the
    // sun is not shadowed; never beyond the camera's far plane).
    f32 shadow_distance = 100.0f;
    // The shadow map is kept from one render to the next, and not drawn again when it would come out the same: the sun,
    // the area and the resolution, the camera (what casts into view, and the cascades' fit, depend on it), every caster's
    // mesh, material and copies, and the levels of detail all as they were. Exact (the picture is the one drawn again, to
    // the byte), and a scene that moves pays only a comparison that stops at the first difference; it hits only
    // when nothing in the shadow's reach moved: a paused game, a photo mode, a viewer at rest. A cut-out caster's texture
    // that is drawn into or written to (a render target) is never cached. Off unless asked for, like the other
    // opt-ins (a benchmark that renders one scene many times and means to time the shadows leaves it off). `Render3DStats::shadow_cached`
    // says when it hit.
    bool cache_shadows = false;
    // At most 16 light a frame: those that reach the view, the strongest
    // and nearest first. Cleared with the draws.
    std::vector<PointLight> points;
    // Spot lights (P3b): at most 16 a frame, chosen as the points are (the cone's bounding sphere reaches the view, the
    // strongest and nearest first). Cleared with the draws.
    std::vector<SpotLight> spots;
    // Shadows of lights that are not the sun (P3b), all opt-in (a light's `shadow.enabled`; `fill_shadow.enabled`) and
    // off, the picture and the cost are what they were. Of the lights that ask, the most important this many a frame
    // have a map (importance: intensity x reach x the share of the view it covers, with a hysteresis in favour of the
    // lights that had one, so the choice does not flip between frames): up to 8 spots, up to 2 point lights (six
    // faces each). Their maps share one array of depth textures of `local_shadow_resolution` (a light may ask for less).
    u32 max_shadowed_spots = 4;
    u32 max_shadowed_points = 2;
    u32 local_shadow_resolution = 1024;
    // The fill light's shadow (the moon): its own cascades and cache; see DirectionalShadow3D.
    DirectionalShadow3D fill_shadow{};
    // How dark the sun's shadow is, 1 (as always) to 0 (none: no map is drawn). Below 1 the picture is drawn by the
    // light shaders (the others know no strength). TimeOfDay3D::apply fades it through twilight when it is asked to.
    f32 sun_shadow_strength = 1.0f;
};

// Ambient occlusion (GTAO) of the scene's own surfaces: how much of the
// ambient or environment light a crease, a corner or the foot of a wall is
// shut off from. Costs a depth and normal pre-pass of the opaque meshes and
// three compute dispatches, so it is off unless asked for; off, the picture
// and the cost are as without the feature. It dims only the ambient light and
// the environment (diffuse, and reflections by a specular occlusion term),
// never the sun, the fill or the point lights, and not the overlay.
struct AmbientOcclusion3D {
    bool enabled = false;
    f32 radius = 0.5f;    // how far from a surface something can still shut light off (units)
    f32 intensity = 1.0f; // the darkening's power: 1 is the plain answer, more is stronger
    // Computed at half the view's size, blurred and read up: about a third of
    // the cost, a little blurrier at edges.
    bool half_resolution = false;
};

// The least and most samples a contact shadow's march takes (ContactShadows3D::steps is clamped to them).
constexpr u32 min_contact_steps = 4;
constexpr u32 max_contact_steps = 64;

// Contact shadows: a short march through the depth buffer toward the sun,
// for the small shadows the cascades are too coarse for (a foot, a pole, a
// plate on the ground). Needs the depth pre-pass (shared with ambient
// occlusion when both are on) and one compute dispatch and blur, so it is
// off unless asked for; off, the picture and the cost are as without it. It
// multiplies only the sun's light, after the shadow map's, and not the
// overlay, blended, additive or wireframe meshes.
struct ContactShadows3D {
    bool enabled = false;
    f32 distance = 0.5f;   // how far toward the sun a pixel looks for something in the way (units)
    f32 thickness = 0.1f;  // how deep a thing is taken to be: what is further behind the ray than this lets it pass (units)
    f32 intensity = 1.0f;  // 1: fully dark where blocked, less: lighter (0..1)
    u32 steps = 20;        // samples along the march (min_contact_steps to max_contact_steps)
};

// Temporal anti-aliasing: each frame is drawn a fraction of a pixel off (a
// jitter), and a compute pass blends it into the picture of the frames before
// (reprojected by how everything moved, and clamped to the colours near it),
// so edges and fine detail settle to what many samples a pixel would give.
// It replaces MSAA while on: the view draws one sample a pixel whatever
// View3DOptions::samples asks. Costs a velocity pre-pass (shared with ambient
// occlusion and contact shadows), a history picture and a compute dispatch, so
// it is off unless asked for; off, the picture and the cost are as without it.
//
// The history belongs to the view (made by the first frame that asks, dropped
// by the first that does not), not the scene: the scene is rebuilt each frame.
// A camera cut should call View3D::reset_history(). Moving meshes are matched
// from one frame to the next by draw order or MeshInstance::id: see
// docs/rendering_3d.md for how, and what it cannot do.
struct TemporalAA3D {
    bool enabled = false;
    f32 blend = 0.9f;        // how much of the history stays each frame (0 to 0.98): more is smoother, slower to follow
    f32 sharpness = 0.0f;    // a small local-contrast boost on the result, 0 (off) to 1; costs one more dispatch
    f32 jitter_scale = 1.0f; // the jitter's reach: 1 is a pixel; 0 turns the jitter off (no anti-aliasing, for looking at the rest)
    u32 jitter_samples = 8;  // frames before the jitter sequence repeats (1 to 16)
    bool freeze_jitter = false; // debug: the same offset every frame, to see the history working without the jitter's crawl
};

// Levels of detail: each copy of a mesh made with levels (MeshOptions::levels)
// is drawn with the simplest level whose error, projected onto the screen at
// the copy's distance from the camera, is under `error_pixels` / `bias`. One
// level a copy a frame, for every pass that draws it (the camera's, the sun's
// shadow, the depth pre-pass, the velocity, picking), so that a shadow matches
// the surface it falls from. Chosen on the CPU in the cull, from the copy's
// bounding sphere (its nearest point to the camera) and its largest scale;
// there is no hysteresis, so a copy at the edge of a threshold can change level
// as the camera moves (the bound is on error, so what changes is small). Meshes
// without levels, alpha-blended ones (drawn furthest first, which levels would
// break up) and the overlay (which has its own setting) are drawn as before.
// Off, the picture, the statistics and the cost are as without the feature.
// Occlusion culling of clustered meshes (Lod3D::occlusion): clusters hidden behind other geometry are not drawn.
struct Occlusion3D {
    bool enabled = false;
    // The two-pass method never culls what shows (a cluster is tested against this frame's own depth), so these only
    // trade a little culling for safety against rounding. `depth_margin` is relative (a cluster must be behind the
    // pyramid by this fraction of its depth to be culled); `pixel_margin` widens the cluster's screen rectangle
    // by this many pixels each side.
    f32 depth_margin = 0.001f;
    u32 pixel_margin = 1;
};

struct Lod3D {
    bool enabled = false;
    // The most a level may be wrong on the screen, in pixels (the sizes of the
    // view the scene is drawn into).
    f32 error_pixels = 1.0f;
    // Quality multiplier: 2 holds the error to half of error_pixels, 0.5 allows twice.
    f32 bias = 1.0f;
    // Debug: -1 chooses by distance; 0 or more draws every copy at that level
    // (or its coarsest, for a mesh with fewer).
    i32 force_level = -1;
    // Meshes made with clusters (MeshOptions::clusters) are cut through their hierarchy every render. With this on
    // (and a device that has compute with storage buffers and indirect draws: Renderer3DCapabilities::gpu_driven_clusters)
    // the cut, the frustum tests and the gathering of the chosen triangles into one index buffer run in compute and are
    // drawn with indirect draws, so the CPU does not look at a cluster; off, or without that, the cut runs on the CPU
    // (the picture is the same). Not a quality setting: it is for comparing the two and for tests.
    bool gpu_culling = true;
    // The most triangles the GPU's gathered index buffer holds, all passes and copies together (12 bytes a triangle of
    // video memory). Clusters that do not fit are not drawn (the picture degrades, nothing is written out of bounds),
    // counted in Render3DStats::cluster_overflow and said once in the log when stats_readback is on.
    u32 max_cluster_triangles = 4'000'000;
    // Debug: reads the GPU cut's counts back at the end of every render() (Render3DStats::clusters_drawn, triangles,
    // cluster_overflow). It waits for the GPU, as Renderer3D::pick does: for tools and tests, not for a game's frames.
    bool stats_readback = false;
    // Clusters hidden behind others are not drawn (G3d). Needs `enabled` and `gpu_culling` (else ignored, with a line in
    // the log) and a device with the GPU cut and ambient occlusion's formats (Renderer3DCapabilities::occlusion_culling).
    // The picture is the one without it; what changes is the work. Off by default: it adds a first draw of last
    // frame's visible clusters and a depth pyramid, which pay only where much of the scene is hidden.
    Occlusion3D occlusion{};
};

class Scene3D {
public:
    Scene3D() = default;
    Scene3D(Scene3D&&) noexcept = default;
    Scene3D& operator=(Scene3D&&) noexcept = default;

    Camera3D camera{};
    Lights3D lights{};
    AmbientOcclusion3D ambient_occlusion{};
    ContactShadows3D contact_shadows{};
    TemporalAA3D taa{};
    Lod3D lod{};
    // A sky behind the opaque meshes, and fog over them that agrees with it (render3d/sky3d.hpp, P3a). Off by default:
    // off, the picture and the cost are as without the features.
    Sky3D sky{};
    Fog3D fog{};
    // Rain, snow, dust and mist round the camera (render3d/particles3d.hpp, P3c): a function of `weather.time` and the camera, drawn
    // as particles (fogged, soft, lit by the scene's light as the layers' looks say). Off by default (every layer's intensity 0:
    // nothing is built, uploaded or drawn). The overlay's weather is ignored.
    Weather3D weather{};
    // Copies wholly outside the camera's view are skipped (each tested as the
    // sphere round its mesh's bounds, moved and scaled with it).
    bool cull = true;

    // What is drawn over the scene, in front of everything in it, seen by its
    // own camera and lit by this scene's lights: a first-person gun or hands,
    // which then never sink into a wall. Made on first use; its own overlay
    // is never drawn.
    Scene3D& overlay();
    const Scene3D* overlay_if_any() const { return _overlay.get(); }

    // `mesh` placed by `transform`. Draws of the same mesh and material one
    // after another are drawn as one (instanced).
    void draw(const Mesh& mesh, const Mat4f& transform, const Material3D& material = {},
              Color tint = colors::white, u32 id = 0);
    // Each part of `model` (model.hpp), placed by `transform`.
    void draw(const Model& model, const Mat4f& transform, Color tint = colors::white);
    // A skinned mesh (made from MeshData with a skin) in a pose: `joint_matrices` are the matrices of
    // compute_joint_matrices (kin/render3d/skeleton.hpp), at least Mesh::skin_joint_count() of them, copied into the scene
    // (they need not outlive the call; clear() takes them away). `transform` places the posed mesh: the matrices are in
    // the mesh's own space, so it is the model's placing without the glTF node's. Each call is one copy with its own
    // pose; copies of one mesh and material one after another are drawn as one. Drawn in every pass: the main pass, the
    // sun's shadow, the depth pre-pass (ambient occlusion, contact shadows, occlusion culling's occluders), the temporal
    // anti-aliasing's velocity (from last frame's matrices of the same copy, matched as its model matrix is: by `id`, else
    // by order) and pick. A mesh without a skin is drawn as by draw(); too few matrices draw nothing (and say so once).
    void draw_skinned(const Mesh& mesh, std::span<const Mat4f> joint_matrices, const Mat4f& transform, const Material3D& material = {},
                      Color tint = colors::white, u32 id = 0);
    // Every part of `model`: its skinned parts in the pose `joint_matrices` (of Model::skeletons[0]) placed by `transform`
    // alone, the others (not skinned) by `transform` times their node's placing, as draw(model, ...) does. The pose is copied
    // into the scene once and every skinned part points at that range (BrainStem's 59 parts keep one set of 18 matrices, not
    // 59). `id` is what pick says of each part; it is also how a copy is matched across frames for the velocity.
    void draw_skinned(const Model& model, std::span<const Mat4f> joint_matrices, const Mat4f& transform, Color tint = colors::white,
                      u32 id = 0);
    // Many copies of `mesh`, in one draw.
    void draw_instanced(const Mesh& mesh, std::span<const MeshInstance> instances, const Material3D& material = {});
    void draw_instanced(const Mesh& mesh, std::span<const Mat4f> transforms, const Material3D& material = {});
    // Room for `count` copies of `mesh`, for the caller to fill (from worker
    // threads, say). Valid until the next draw is added.
    std::span<MeshInstance> add_instances(const Mesh& mesh, std::size_t count, const Material3D& material = {});
    // Particles (P3c): a batch of sprites drawn after the scene's blended meshes, depth-tested and not written, in the
    // order of their depth (`look.sort`: the sprites within it back to front too). The sprites are copied (they need not
    // outlive the call); clear() takes them away. The overlay's are not drawn.
    void draw_particles(const ParticleLook3D& look, std::span<const ParticleSprite3D> sprites);
    // The system's live particles with its desc's look.
    void draw_particles(const ParticleSystem3D& system);
    void add_light(const PointLight& light) { lights.points.push_back(light); }
    void add_spot_light(const SpotLight& light) { lights.spots.push_back(light); }
    // Takes the draws and point and spot lights and particles away, the overlay's too (cameras, the
    // sun and the ambient light and the weather stay), keeping their room.
    void clear();

    struct Draw {
        Mesh mesh;
        Material3D material;
        u32 first = 0; // into instances()
        u32 count = 0;
        // draw_skinned(): the copies are skinned; copy i's joint matrices start at joint_matrices()[skin_offsets()[skin_first + i]].
        bool skinned = false;
        u32 skin_first = 0;
    };
    // A batch of particles (draw_particles): its look and sprites [first, first + count) of particle_sprites().
    struct ParticleDraw {
        ParticleLook3D look;
        u32 first = 0;
        u32 count = 0;
    };
    std::span<const ParticleDraw> particle_draws() const { return _particle_draws; }
    std::span<const ParticleSprite3D> particle_sprites() const { return _particle_sprites; }
    std::span<const Draw> draws() const { return _draws; }
    std::span<const MeshInstance> instances() const { return _instances; }
    // Skinned copies: the joint matrices of all of them, and where each copy's start (one for each copy of a skinned draw).
    std::span<const Mat4f> joint_matrices() const { return _joints; }
    std::span<const u32> skin_offsets() const { return _skin_offsets; }

private:
    std::span<MeshInstance> add_instances(const Mesh& mesh, std::size_t count, const Material3D& material, bool skinned, u32 skin_first);
    // One skinned copy whose matrices start at `first_joint` of _joints (already there).
    void add_skinned_copy(const Mesh& mesh, u32 first_joint, const Mat4f& transform, const Material3D& material, Color tint, u32 id);

    std::vector<Draw> _draws;
    std::vector<MeshInstance> _instances;
    std::vector<Mat4f> _joints;
    std::vector<u32> _skin_offsets;
    std::vector<ParticleDraw> _particle_draws;
    std::vector<ParticleSprite3D> _particle_sprites;
    bool _warned_skin = false;
    std::unique_ptr<Scene3D> _overlay;
};

} // namespace kin
