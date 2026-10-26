#pragma once

#include <kin/ecs/render3d.hpp>
#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/backend3d.hpp>
#include <kin/render3d/material_asset3d.hpp>

namespace kin {

// Camera pose comes from Transform3D, including parents. A zero far distance
// in authored data means an infinite far plane. Higher priority wins; ties
// choose the lowest entity ID. Disabled components leave host settings alone.
struct SceneCamera3D {
    Camera3D camera{.z_far = 1000.0f};
    bool enabled = true;
    i32 priority = 0;
};

// Authored scene-wide settings. Local light vectors and GPU environment maps
// stay runtime-only; use the procedural sky for authored environment lighting.
struct SceneEnvironment3D {
    Lights3D lights;
    Sky3D sky;
    Fog3D fog;
    AmbientOcclusion3D ambient_occlusion;
    ContactShadows3D contact_shadows;
    TemporalAA3D taa;
    u32 anisotropy = 1;          // Scene3D::anisotropy (1 to 16)
    f32 texture_lod_bias = 0.0f; // Scene3D::texture_lod_bias (-2 to 2)
    Lod3D lod;
    GlobalIllumination3D global_illumination; // Scene3D::global_illumination
    // The picture (samples, background, hdr, exposure, bloom). `pick` is not
    // authored and never applied: picking is the game's (see scene3d_view_options).
    ViewOptions3D view;
    bool enabled = true;
    i32 priority = 0;
};

// The view options a scene is rendered with: the environment's picture settings
// with the game's `pick` (a pick pass is the game's choice, whatever the
// environment holds). The one policy of apply_scene3d_settings, SceneFrame3D
// and SceneSession3D.
inline ViewOptions3D scene3d_view_options(ViewOptions3D environment, const ViewOptions3D& game) {
    environment.pick = game.pick;
    return environment;
}

// Call after propagate_transforms(), before collecting local lights/draws.
// Applies each selected camera/environment atomically after validation.
// Optional view options are for create_view; this does no GPU work. The
// environment replaces them except `pick`, which stays as passed
// (scene3d_view_options).
// With `only`, validate/apply that entity even when disabled (editor preflight).
Status3D apply_scene3d_settings(EcsWorld& world, Scene3D& scene, ViewOptions3D* view = nullptr, EcsEntity only = {},
                                const WorldRenderState3D::Presentation3D& presentation = {});

struct AnimatorConfiguration3D {
    std::shared_ptr<const AnimationStateMachine> machine;
    std::unordered_map<std::string, Animator3D::BlendSpace> blend_spaces;
    std::vector<Animator3D::LayerConfig> layers;
    std::vector<Animator3D::Fade> fades;
    std::vector<IKConstraint3D> ik;
    std::shared_ptr<const AnimationEventTracks3D> events;
    std::shared_ptr<const CharacterActions3D> actions;
    // Exact sources for explicit reload identity; shared once per loaded config.
    std::string source, machine_source;
    std::filesystem::path machine_asset;
};

struct AnimatorAsset3D {
    ComponentAssetRef asset;
    std::shared_ptr<const AnimatorConfiguration3D> configuration; // runtime-only
};

// CPU-only configuration binding. Paths (including .kinanim references) are
// relative to content_root. Atomic per entity; invalid replacements retain the
// last good configuration. Unchanged content keeps animation progress intact.
// Called by resolve_render3d_assets after models bind, or separately by tools.
Status3D resolve_animator3d_assets(EcsWorld& world, const std::filesystem::path& content_root,
                                   std::span<const EcsId> entities = {});
// Atomic per entity, renderer thread. Called by resolve_render3d_assets.
// Invalid replacements retain the last good binding; empty refs clear it.
Status3D resolve_materials3d_assets(EcsWorld& world, Assets3D& assets, std::span<const EcsId> entities = {});

// Extend the SAME component registry used by .kinscene/.kinprefab, inspection,
// patches and Lua. No renderer needed. Idempotent; the standard
// register_builtin_editor_components() calls this in experimental 3D builds.
void register_render3d_components(EcsWorld& world);
void register_physics3d_components(EcsWorld& world);

// Authored capacity budgets, including disabled emitters. Validation does no
// I/O/allocation of particles. Call after edits and before simulation/collection.
// This bounds configured capacity, not textures or custom C++ particle draws.
struct ParticleLimits3D {
    std::size_t emitters = 128;
    u32 per_emitter = 20000;
    u64 total_particles = 65536;
};
Status3D validate_particles3d(EcsWorld& world, ParticleLimits3D limits = {}, std::span<const EcsId> entities = {});
// Renderer-thread texture binding; uses the existing Assets3D weak texture cache.
// Validation precedes binding; failed texture loads retain the previous texture.
Status3D resolve_particles3d_assets(EcsWorld& world, Assets3D& assets, ParticleLimits3D limits = {},
                                    std::span<const EcsId> entities = {});

// Renderer-thread-only binding after normal scene/prefab instantiation or an
// asset-reference edit. Uses Assets3D handles, never serializes GPU objects.
// Empty references leave programmatic models alone. Failed replacements keep
// the previous model and return contextual diagnostics; call explicitly again
// after fixing content. No hidden per-frame I/O, owning cache, or entity map.
// Clear Assets3D first to force a same-path content refresh. Bind before
// animation/physics setup; rebuild existing ragdolls when changing skeletons.
// An empty entity span means the whole world; a nonempty span binds only those
// entities. Use whole-world particle validation separately for aggregate budgets.
Status3D resolve_render3d_assets(EcsWorld& world, Assets3D& assets, ParticleLimits3D particles = {},
                                 std::span<const EcsId> entities = {});

} // namespace kin
