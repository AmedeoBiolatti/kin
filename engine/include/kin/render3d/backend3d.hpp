#pragma once

// The seam between Renderer3D and a renderer backend that can draw 3D views
// (IRenderer2DBackend::renderer_3d(); SDL_GPU only). Experimental: see
// docs/3d_plan.md.

#include <kin/core/types.hpp>
#include <kin/render3d/mesh.hpp>
#include <kin/render3d/mesh_clusters.hpp>
#include <kin/render3d/mesh_lod.hpp>
#include <kin/render3d/scene3d.hpp>
#include <kin/render3d/shadow_cascades.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/texture.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace kin {

// What 3D views can have on this device.
struct Renderer3DCapabilities {
    // The depth buffer views draw with: 32-bit float where the device has it,
    // else 24 or 16-bit fixed point (less precise far away).
    u32 depth_bits = 0;
    bool depth_float = false;
    // The most samples a pixel of a view can take (MSAA; 1: none), with 8-bit
    // colour and with HDR (16-bit float) colour.
    u32 max_samples = 1;
    u32 max_samples_hdr = 1;
    // Whether Scene3D::ambient_occlusion, ::contact_shadows and ::taa can work
    // here (the formats and compute they need); where not, they are off and a
    // line in the log says so the first time one is asked for.
    bool ambient_occlusion = false;
    bool contact_shadows = false;
    bool temporal_aa = false;
    // Whether Scene3D::lod's cluster cut can run in compute (Lod3D::gpu_culling): compute with storage buffers read and
    // written, buffers that are index and indirect-argument buffers a compute pass writes. Else the CPU cuts.
    bool gpu_driven_clusters = false;
    // Whether Lod3D::occlusion can work: the GPU cut, plus a sampled depth format and R32F storage textures.
    bool occlusion_culling = false;
};

struct View3DOptions {
    // Samples a pixel (MSAA: smooth edges), 1, 2, 4 or 8; fewer where the
    // device has fewer.
    u32 samples = 4;
    // What shows where nothing is drawn; transparent lets what is under the
    // view show through.
    Color background = colors::transparent;
    // Light kept past white (a float picture), exposed and tonemapped (ACES:
    // brights roll off, the darkest tones darken a little) on its way to the
    // 2D frame; left linear for an HDR 2D pipeline, which tonemaps itself.
    // On with bloom.
    bool hdr = false;
    f32 exposure = 1.0f;
    // A glow round what is brighter than `bloom_threshold` (linear), this
    // strong (0: none). Emissive materials past 1 glow.
    f32 bloom = 0.0f;
    f32 bloom_threshold = 1.0f;
    // What is under each pixel kept too (Renderer3D::pick): one more pass
    // over the scene's opaque meshes.
    bool pick = false;
};

// What a pixel of a view shows (Renderer3D::pick): the mesh copy nearest the
// camera there (its MeshInstance::id) and the point of it the pixel shows.
struct PickResult {
    bool hit = false; // false: nothing but background there
    u32 id = 0;
    Vec3f position{};
};

// One cluster the GPU's cut chose, for tests and tools (IRenderer3DBackend::read_cluster_picks).
struct ClusterPick {
    u32 scene = 0; // 0 the scene, 1 its overlay
    u32 draw = 0;  // index in Scene3D::draws()
    u32 copy = 0;  // the copy within the draw
    u32 view = 0;  // 0 the camera's passes; 1 the shadow map, or 1 + k the cascade k's
    u32 cluster = 0;
};

// One indirect draw of the GPU cut, as the GPU holds it (IRenderer3DBackend::read_cluster_draws).
struct ClusterDraw {
    u32 view = 0;        // as ClusterPick::view
    u32 job = 0;         // the copy's place among those cut this render
    u32 index_count = 0; // 0: nothing is drawn
    u32 first_index = 0;
    u32 first_instance = 0; // the copy's slot in the instance buffer
};

// What the last render() drew.
struct Render3DStats {
    u32 draws = 0;     // draw calls
    u32 instances = 0; // meshes drawn
    u32 culled = 0;    // meshes skipped, outside the view
    u32 shadow_draws = 0; // draw calls into the shadow map
    u32 shadow_casters = 0; // meshes drawn into it (those seen, and those casting into view; all cascades)
    u32 shadow_cascades = 0; // maps drawn (0: no shadows)
    bool shadow_cached = false; // the maps are those of the render before (Lights3D::cache_shadows): nothing was drawn into them, so the shadow numbers above are 0
    std::array<u32, max_shadow_cascades> cascade_casters{}; // meshes drawn into each cascade
    u32 ao_draws = 0;      // draw calls into the depth and normal pre-pass (ambient occlusion or contact shadows; 0: neither this render)
    u32 ao_instances = 0;  // meshes drawn into it
    u32 ao_dispatches = 0; // compute dispatches (the occlusion and its blur)
    u32 contact_dispatches = 0; // compute dispatches of the contact shadows (the march and its blur): 0 when off
    // With both on at one size (the occlusion not at half resolution) the two blurs are one dispatch a direction; these
    // count that dispatch in each of the two numbers above, and this says how many there were (2, else 0).
    u32 blur_merged = 0;
    u32 taa_dispatches = 0; // compute dispatches of the temporal anti-aliasing (the resolve, and the sharpen): 0 when off
    bool taa_reset = false; // its history was not used this render: the first, after a resize or reset_history(), or the camera changed kind
    Vec2f taa_jitter{};     // this render's offset in pixels (0, 0 when off)
    // With temporal anti-aliasing and nothing else that needs a pre-pass, the main pass writes the velocity itself (no
    // geometry pass for it: ao_draws is 0); false with the pre-pass (ambient occlusion, contact shadows, occlusion culling, an opaque
    // mesh with a custom shader) or without temporal anti-aliasing.
    bool taa_main_velocity = false;
    // The video memory the view's opt-in targets hold now (ambient occlusion, contact shadows, the pre-pass, temporal anti-aliasing and
    // occlusion culling's pyramid and history; with occlusion culling the renderer's second gathered buffer too), in bytes, and how many
    // feature groups of them this render gave back: a feature that has not been used for 120 renders (`View3D`'s own, so a view that is
    // not drawn keeps them) releases its targets, and a feature on again makes them again.
    u64 feature_bytes = 0;
    u32 targets_released = 0;
    // Graphics pipelines this render had to make (the first use of a kind of draw: tens of milliseconds on a cold driver cache). Renderer3D::warm_up
    // makes the ones a scene will need beforehand, and the first render after it makes none.
    u32 pipelines_made = 0;
    u32 samples = 0;        // samples a pixel of the scene took: the view's, or 1 with temporal anti-aliasing
    u32 lights = 0;    // point lights used
    u32 environment_prefilters = 0; // environments filtered by this render (once each)
    u64 triangles = 0; // drawn by the main pass: at the level each copy is drawn at (Scene3D::lod)
    // With Scene3D::lod: the copies the camera sees, by the level of detail they are drawn at (all in 0 when it is off).
    std::array<u32, max_mesh_levels> lod_copies_by_level{};
    // With Scene3D::lod and a mesh with clusters (MeshOptions::clusters): the copies cut through their hierarchy this
    // render (those the camera or a shadow map sees), the clusters the cuts looked at, the clusters of the camera's
    // cut that are in view (`triangles` counts what they draw), the bytes of the compacted indices uploaded (all
    // passes), and the CPU time of the cuts (choosing, compacting, handing over the upload).
    u32 cluster_copies = 0;
    u64 clusters_tested = 0;
    u32 clusters_drawn = 0;
    u32 cluster_upload_bytes = 0;
    f64 cluster_ms = 0.0;
    f64 cluster_upload_ms = 0.0; // of which the compaction and handing over the upload (the rest is choosing)
    // When the cut ran in compute (Lod3D::gpu_culling): `cluster_gpu` is true, `cluster_copies` the copies it covers,
    // `cluster_upload_bytes` the bytes handed over (its table of copies), `cluster_ms` the CPU time of setting it up
    // and recording it, `cluster_dispatches` its compute dispatches. The counts below need Lod3D::stats_readback (0
    // without): `clusters_tested` the clusters looked at (all of every copy's), `clusters_drawn` and `triangles` the
    // camera's cut, `cluster_overflow` the clusters dropped for lack of room in Lod3D::max_cluster_triangles, in all
    // passes, `cluster_triangles_all` the triangles gathered for all passes.
    bool cluster_gpu = false;
    u32 cluster_dispatches = 0;
    u32 cluster_overflow = 0;
    u64 cluster_triangles_all = 0;
    // With Lod3D::occlusion (and the GPU cut): `occlusion` is true when it ran this render; `occlusion_levels` the depth
    // pyramid's levels; `occlusion_dispatches` the compute dispatches that built it. With stats_readback: the clusters
    // that were in the cut and in view but behind this frame's depth (`clusters_occluded`, `triangles_occluded`: not
    // drawn), the clusters drawn in the first pass because they showed last frame and were hidden after all
    // (`clusters_wasted`, `triangles_wasted`), and the clusters of the first pass (`clusters_first_pass`).
    bool occlusion = false;
    u32 occlusion_levels = 0;
    u32 occlusion_dispatches = 0;
    u32 clusters_occluded = 0, clusters_wasted = 0, clusters_first_pass = 0;
    u64 triangles_occluded = 0, triangles_wasted = 0;
    f64 cpu_ms = 0.0;  // render()'s own time on the CPU
};

// A view's targets on the backend.
class IView3DBackend {
public:
    virtual ~IView3DBackend() = default;
    // The picture, premultiplied, to draw with Renderer2D::draw_texture.
    virtual Texture texture() const = 0;
    virtual Vec2i size() const = 0;
    virtual u32 samples() const = 0;
    // Forgets the temporal anti-aliasing's history, so the next frame starts
    // afresh (a camera cut, a scene change).
    virtual void reset_history() {}
};

class IRenderer3DBackend {
public:
    virtual ~IRenderer3DBackend() = default;

    virtual Renderer3DCapabilities capabilities() const = 0;
    // Null when the data can't be uploaded (validated already). `chain`, when
    // not null, holds the levels of detail to keep with it (level 0 of it is `data`).
    // `clusters`, when not null, is the mesh's cluster hierarchy (MeshOptions::clusters): the mesh is then
    // kept as its welded vertices and leaf clusters, and `data` is not used, nor `chain` (null then).
    virtual std::shared_ptr<IMeshBackend> create_mesh(const MeshData& data, const MeshLodChain* chain,
                                                      std::shared_ptr<const MeshClusters> clusters) = 0;
    // An equirectangular picture of `size` pixels, three floats (linear RGB)
    // each, rows top to bottom; its filtered copies are made on the first
    // render that uses it. Null when it can't be made (no compute).
    virtual std::shared_ptr<IEnvironmentBackend> create_environment(Vec2i size, std::span<const f32> rgb) = 0;
    // Null when the SPIR-V can't be made into a fragment shader.
    virtual std::shared_ptr<IMaterialShaderBackend> create_material_shader(std::span<const u8> spirv) = 0;
    virtual std::shared_ptr<IView3DBackend> create_view(Vec2i size, const View3DOptions& options) = 0;
    virtual bool resize_view(IView3DBackend& view, Vec2i size) = 0;
    // Records `scene` into `view`, after the 2D draws so far and before those
    // to come.
    virtual bool render(IView3DBackend& view, const Scene3D& scene, Render3DStats& stats) = 0;
    // What `pixel` of the view's last render shows; waits for the GPU.
    virtual PickResult pick(IView3DBackend& view, Vec2i pixel) = 0;
    // Makes what rendering `scene` into `view` would make on first use (pipelines, targets), without drawing. False where there is nothing to make.
    virtual bool warm_up(IView3DBackend& view, const Scene3D& scene) { static_cast<void>(view); static_cast<void>(scene); return false; }
    // The clusters the last render's GPU cut chose, in every pass, for tests and tools; waits for the GPU. False when
    // that render cut on the CPU (or not at all).
    virtual bool read_cluster_picks(IView3DBackend&, std::vector<ClusterPick>&) { return false; }
    // The indirect draws of the last render's GPU cut, a (view, copy) each; waits for the GPU.
    virtual bool read_cluster_draws(IView3DBackend&, std::vector<ClusterDraw>&) { return false; }
};

} // namespace kin
