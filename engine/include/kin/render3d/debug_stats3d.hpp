#pragma once

#include <array>
#include <kin/core/math3d.hpp>
#include <kin/render3d/mesh_lod3d.hpp>
#include <kin/render3d/shadow_cascades3d.hpp>
#include <kin/render3d/stats3d.hpp>

namespace kin {

// Detailed renderer instrumentation for tools, profilers and engine tests.
// Inherits the same summary returned by stats(); no second counter store.
// Implementation counters can change as the renderer changes. Reading this
// snapshot never triggers a GPU readback.
struct RenderDebugStats3D : RenderStats3D {
    // Explicit shared preparation: build counts belong to prepare(); reused
    // records and upload savings belong to a consuming operation.
    u64 preparation_records = 0, preparation_skin_bounds = 0, preparation_cpu_bytes = 0;
    u64 shared_records = 0, shared_instance_bytes = 0, shared_joint_bytes = 0;
    u64 joint_upload_bytes = 0, preparation_scratch_bytes = 0;
    u32 shared_uploads = 0, shared_joint_uploads = 0;
    bool graph_reused = false;
    u32 graph_passes = 0, graph_culled_passes = 0;
    f64 graph_compile_ms = 0, graph_record_ms = 0;  // CPU, never GPU timestamps
    bool geometry_graph_reused = false;             // stock prepass geometry subgraph cache
    u32 surface_colour_copies = 0;                  // explicit out-of-place transparency input copy
    u32 bloom_draws = 0, output_draws = 0;          // shared stock/public fullscreen recorders
    u32 geometry_draws = 0, geometry_instances = 0; // standalone geometry; stock prepass also keeps ao_* counters
    // Shared small-mesh GPU pages, excluding dedicated meshes and driver allocations. Retired ranges await GPU fences.
    u64 shared_mesh_allocated_bytes = 0, shared_mesh_live_bytes = 0, shared_mesh_retired_bytes = 0;
    u64 shared_mesh_range_reuses = 0;
    u32 shared_mesh_pages = 0;
    // Optional instance-cell culling. Tests count refined copies, not individual planes;
    // rejected local candidates count a copy once per wanted shadow view.
    // Live bounded GPU tables; per-render upload bytes (records exclude index lists).
    u64 retained_gpu_bytes = 0, retained_upload_bytes = 0, retained_index_bytes = 0, instance_upload_bytes = 0;
    u32 retained_instances = 0;
    // Tables given up this render for other batches (least recently drawn first), and draws that wanted one but found
    // no room.
    u32 retained_evictions = 0, retained_refused = 0;
    // Device-wide GPU waits (SDL_WaitForGPUIdle) inside this render(): 0 in an ordinary frame.
    u32 device_waits = 0;
    // CPU sphere storage: resident-table cache (<=4 MiB), compact transient allocation,
    // submitted copies using the cache, and actual cache rebuilds this render.
    u64 bounds_cache_bytes = 0, bounds_scratch_bytes = 0, bounds_cache_rebuilt = 0;
    u32 bounds_cache_instances = 0;
    // Draw/piece/task layout reuse, excluding visibility and instance data. Layout bytes
    // are existing vector payload capacities; cache bytes are the additional key/stamps.
    u32 prepared_reused = 0, prepared_draws = 0;
    u64 prepared_cache_bytes = 0, prepared_layout_bytes = 0;
    u32 spatial_groups = 0;
    u64 camera_instance_tests = 0, bounds_instance_tests = 0, local_instance_tests = 0;
    u64 camera_group_rejected = 0, local_group_rejected = 0;
    u64 camera_range_accepted = 0; // copies classified together from a camera-inside cell proof
    // Local gather after whole-cell rejection: candidates, fully accepted ranges,
    // potential plane tests before per-copy early outs, actual radius tests, byte scratch.
    u64 local_candidates = 0, local_group_accepted = 0, local_plane_budget = 0, local_radius_tests = 0,
        local_bucket_bytes = 0;
    // Local preparation vector payload capacities: gather tasks/counts, caster-union slot map,
    // and accepted/upload copy lists (including their simultaneous backing storage).
    u64 local_task_bytes = 0, local_slot_bytes = 0, local_copy_bytes = 0;
    // Skinned copies (Scene3D::draw_skinned): those the camera sees (drawn by the main pass), the joint matrices handed
    // to the GPU for all of the frame's skinned copies, and the main pass's draw calls for them (a run of copies of one
    // mesh is one). Also counted in draws, instances and triangles above. 0 for a frame without any.
    u32 skin_matrices = 0;
    // Main-pass calls using the specialized unlit shaders; textured calls are a subset.
    u32 unlit_draws = 0, unlit_textured_draws = 0;
    std::array<u32, max_shadow_cascades> cascade_casters{}; // meshes drawn into each cascade
    u32 ao_draws = 0; // draw calls into the depth and normal pre-pass (ambient occlusion or contact shadows; 0: neither
                      // this render)
    u32 ao_instances = 0;       // meshes drawn into it
    u32 ao_dispatches = 0;      // compute dispatches (the occlusion and its blur)
    u32 contact_dispatches = 0; // compute dispatches of the contact shadows (the march and its blur): 0 when off
    // With both on at one size (the occlusion not at half resolution) the two blurs are one dispatch a direction; these
    // count that dispatch in each of the two numbers above, and this says how many there were (2, else 0).
    u32 blur_merged = 0;
    u32 ao_chain_built = 0; // 1 when the occlusion read the linear depth chain this render (the chain's own dispatch is
                            // not in ao_dispatches), else 0
    u32 taa_dispatches =
        0; // compute dispatches of the temporal anti-aliasing (the resolve, and the sharpen): 0 when off
    bool taa_reset = false; // its history was not used this render: the first, after a resize or reset_history(), or
                            // the camera changed kind
    Vec2f taa_jitter{};     // this render's offset in pixels (0, 0 when off)
    // With temporal anti-aliasing and nothing else that needs a pre-pass, the main pass writes the velocity itself (no
    // geometry pass for it: ao_draws is 0); false with the pre-pass (ambient occlusion, contact shadows, occlusion
    // culling, an opaque mesh with a custom shader) or without temporal anti-aliasing.
    bool taa_main_velocity = false;
    // The main pass began from the pre-pass's depth (copied: a sample a pixel, no overlay), so that what the pre-pass's
    // surfaces hide is not shaded.
    bool main_depth_from_prepass = false;
    u32 taa_reactive_draws = 0; // transient coverage draws; no extra targets, zero without TAA/effects
    // The GI scene (Scene3D::global_illumination): its voxels (all its levels', 0 without one this render) and levels,
    // whether this render made any of it again (else it was kept: nothing it is made of changed and its windows
    // stayed), and, when it did, the blocks of 8 x 8 x 8 voxels it made, the copies and triangles it voxelized and its
    // compute dispatches.
    u32 gi_voxels = 0;
    u32 gi_levels = 0;
    bool gi_built = false;
    u32 gi_blocks = 0;
    u32 gi_copies = 0;
    u64 gi_triangles = 0;
    u32 gi_dispatches = 0;
    // Its probes (GiProbes3D): how many there are, how many this render updated (and the rays they cast), whether
    // their light was in the picture, and whether it was read once a texel on what the pre-pass drew (else each
    // fragment read the probes).
    u32 gi_probes = 0, gi_probes_updated = 0;
    u32 gi_rays = 0;
    bool gi_lit = false;
    bool gi_sun_map = false; // the GI's voxels took the sun's light through its own sun map (else their march)
    bool gi_resolved = false;
    // The video memory the view's opt-in targets hold now (ambient occlusion, contact shadows, the pre-pass, temporal
    // anti-aliasing and occlusion culling's pyramid and history; with occlusion culling the renderer's second gathered
    // buffer too), in bytes, and how many feature groups of them this render gave back: a feature that has not been
    // used for 120 renders (`View3D`'s own, so a view that is not drawn keeps them) releases its targets, and a feature
    // on again makes them again.
    u32 targets_released = 0;
    // Graphics pipelines this render had to make (the first use of a kind of draw: tens of milliseconds on a cold
    // driver cache). Renderer3D::warm_up makes the ones a scene will need beforehand, and the first render after it
    // makes none. Spot lights and the shadows of lights that are not the sun (P3b). `spots`: the spot lights used
    // (those that reach the view, at most 16). `spot_slot[i]` and `point_slot[i]`: the shadow-map slot of
    // Lights3D::spots[i] and ::points[i] (-1: none; a point light's is its cube's). `local_shadow_maps`: layers of the
    // local array in use this render (a spot's one, a point light's cube's faces that a visible surface can fall in),
    // `local_shadow_drawn` of them drawn now and `local_shadow_cached` kept from before (Lights3D::cache_shadows);
    // `local_shadow_draws` and `local_shadow_casters` the draw calls and meshes drawn into them (the moon's cascades
    // included). `moon_cascades`: the fill light's shadow cascades this render (0: none). `local_shadow_bytes`: the
    // video memory the local and moon arrays hold. `lights_variant`: the main pass used the light shaders.
    static constexpr std::size_t max_reported_lights = 64; // lights past this index are not reported
    std::array<i32, max_reported_lights> spot_slot{};
    std::array<i32, max_reported_lights> point_slot{};
    u32 local_shadow_maps = 0, local_shadow_drawn = 0, local_shadow_cached = 0, local_shadow_draws = 0,
        local_shadow_casters = 0;
    u32 moon_cascades = 0;
    u64 local_shadow_triangles =
        0; // submitted to the local and moon maps this render (a clustered copy's: its cut's, from the light)
    bool lights_variant = false;
    u32 environment_prefilters = 0; // environments filtered by this render (once each)
    // Sky, fog and the sky's image-based light (P3a): the sky pass ran; meshes were drawn with the fog's shader
    // variants (or custom kin_output); with Lights3D::sky_lights_scene the environment was made again this
    // render (the sky moved past Sky3D::light_threshold and light_interval renders had passed), how many times so far,
    // and the CPU time spent recording the last one.
    bool sky_drawn = false;
    bool fog_applied = false;
    bool fog_sun_shadowed = false; // Fog3D::sun_shadows: the share of the fog's air the sun reaches was walked
    bool sky_env_regenerated = false;
    u32 sky_env_generations = 0;
    f64 sky_env_cpu_ms = 0.0;
    // Particles and weather (P3c). `particles_submitted`: sprites handed in (the scene's batches and the weather's
    // layers), `particles_drawn`: those left after the view's cull and the frame's limit (written to the GPU: 64 bytes
    // each), `particle_batches`: batches drawn (one instanced draw each), `particle_soft`: batches that read the depth,
    // `weather_particles`: of the submitted, the weather's. `particles_dropped`: past the frame's limit.
    // `particle_cpu_ms`: the weather's making, the culling, the sorting and the writing.
    u32 particles_submitted = 0;
    u32 particle_batches = 0;
    u32 particle_soft = 0;
    u32 particle_shadow_batches = 0; // batches sampling shadow maps; excludes TAA coverage replays
    u32 particle_shadow_submitted = 0, particle_shadow_dropped = 0; // caster upload, independently capped
    u32 particle_shadow_draws = 0, particle_shadow_instances = 0;   // shadow passes; repeated per light/cascade
    u32 particle_shadow_map_dropped = 0;    // sum of visible casters beyond each map's 16,384 budget
    u64 particle_shadow_uploaded_bytes = 0; // packed caster bytes sent this frame; zero for cached maps
    u64 particle_shadow_bytes = 0;          // caster GPU buffer, released after the ordinary idle interval
    u32 weather_particles = 0;
    f64 particle_cpu_ms = 0.0;
    // With Scene3D::lod: the copies the camera sees, by the level of detail they are drawn at (all in 0 when it is
    // off).
    std::array<u32, max_mesh_levels> lod_copies_by_level{};
    // With Scene3D::lod and a mesh with clusters (MeshOptions3D::clusters): the copies cut through their hierarchy this
    // render (those the camera or a shadow map sees), the clusters the cuts looked at, the clusters of the camera's
    // cut that are in view (`triangles` counts what they draw), the bytes of the compacted indices uploaded (all
    // passes), and the CPU time of the cuts (choosing, compacting, handing over the upload).
    u32 cluster_copies = 0;
    u64 clusters_tested = 0;
    u32 transparent_draw_runs = 0;   // main colour runs after global mesh/sprite ordering
    u64 composition_order_bytes = 0; // retained CPU capacity of the compact composition commands
    f64 transparent_sort_ms = 0;     // CPU scheduling/sort/compaction, never GPU time
    u32 clusters_drawn = 0;
    u32 cluster_budget_fallbacks = 0;          // submitted copies conservatively using their complete level-zero mesh
    u64 cluster_budget_reserved_triangles = 0; // worst-case cut storage, including sun views; no readback
    u32 cluster_upload_bytes = 0;
    f64 cluster_ms = 0.0;
    f64 cluster_upload_ms = 0.0; // of which the compaction and handing over the upload (the rest is choosing)
    // When the cut ran in compute (Scene3D::debug.gpu_culling): `cluster_gpu` is true, `cluster_copies` the copies it
    // covers, `cluster_upload_bytes` the bytes handed over (its table of copies), `cluster_ms` the CPU time of setting
    // it up and recording it, `cluster_dispatches` its compute dispatches. The counts below need
    // Scene3D::debug.stats_readback (0 without): `clusters_tested` the clusters looked at (all of every copy's),
    // `clusters_drawn` and `triangles` the camera's cut, `cluster_overflow` the clusters dropped for lack of room in
    // Lod3D::max_cluster_triangles, in all passes, `cluster_triangles_all` the triangles gathered for all passes.
    bool cluster_gpu = false;
    u32 cluster_dispatches = 0;
    u32 cluster_overflow = 0;
    u64 cluster_triangles_all = 0;
    // With Lod3D::occlusion (and the GPU cut): `occlusion` is true when it ran this render; `occlusion_levels` the
    // depth pyramid's levels; `occlusion_dispatches` the compute dispatches that built it. With stats_readback: the
    // clusters that were in the cut and in view but behind this frame's depth (`clusters_occluded`,
    // `triangles_occluded`: not drawn), the clusters drawn in the first pass because they showed last frame and were
    // hidden after all
    // (`clusters_wasted`, `triangles_wasted`), and the clusters of the first pass (`clusters_first_pass`).
    bool occlusion = false;
    u32 occlusion_levels = 0;
    u32 occlusion_dispatches = 0;
    u32 clusters_occluded = 0, clusters_wasted = 0, clusters_first_pass = 0;
    u64 triangles_occluded = 0, triangles_wasted = 0;
};

} // namespace kin
