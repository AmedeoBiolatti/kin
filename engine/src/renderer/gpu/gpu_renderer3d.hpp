#pragma once
// The SDL_GPU backend's 3D part (IRendererBackend3D), made the first time a
// Renderer3D asks for it. It shares the 2D backend's device, pipeline cache
// and frame: a view is rendered in the frame's command buffer between the 2D
// draws around it. Experimental: see docs/3d_plan.md.
#include "gpu_buffer.hpp"
#include "gpu_mesh_pool.hpp"
#include "gpu_shader.hpp"
#include "gpu_texture.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <kin/render3d/backend3d.hpp>
#include <kin/render3d/local_lights3d.hpp>
#include <kin/render3d/shadow_cascades3d.hpp>
#include <kin/render3d/skeleton3d.hpp>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace kin {
class GpuRenderer2DBackend;
class JobSystem;
} // namespace kin

namespace kin::gpu {

class GpuDevice;
class GpuTextureBackend;

// A view's bounding planes (xyz the normal, inward, length 1; w the offset),
// from its view-projection matrix; an infinite far plane is left out.
struct GpuFrustum {
    std::array<Vec4f, 6> planes{};
    u32 count = 0;
};

// One mesh instance as mesh.vert reads it (std430): 128 bytes.
struct GpuInstance3D {
    f32 model[16];
    f32 normal[12]; // three columns of the normals' matrix, each padded to four
    f32 color[4];   // linear, straight alpha
};
static_assert(sizeof(GpuInstance3D) == 128);

// Meshes the GPU cuts (MeshOptions3D::clusters, on a device that can) share big buffers, so that one dispatch of the
// cut covers many meshes (a barrier for every buffer bound is what a dispatch a mesh costs: Sponza's hundred meshes
// took a millisecond of it): a chunk is an index buffer holding each of its meshes' indices (the leaves', then every
// other cluster's) and the table of their clusters. Meshes are put in the first chunk with room; a mesh too big for a
// chunk has one of its own. Ranges are not given back when a mesh goes; the chunk is, with its last mesh.
struct GpuClusterChunk {
    explicit GpuClusterChunk(SharedDevice device) : device(std::move(device)) {}
    ~GpuClusterChunk();
    GpuClusterChunk(const GpuClusterChunk&) = delete;
    GpuClusterChunk& operator=(const GpuClusterChunk&) = delete;

    SharedDevice device;
    SDL_GPUBuffer* indices = nullptr;       // INDEX, and storage the cut reads
    SDL_GPUBuffer* table = nullptr;         // GpuCluster each
    u32 index_capacity = 0, index_used = 0; // in indices
    u32 table_capacity = 0, table_used = 0; // in clusters
    u32 id = 0;                             // among the renderer's chunks, for ordering a render's copies
};

// A mesh's buffers. Games may keep meshes past the renderer (in scenes that
// outlive it): like textures they hold the device's shared handle, and skip
// the release once it is gone.
class GpuMesh3D final : public IMeshBackend3D {
public:
    explicit GpuMesh3D(SharedDevice device) : device(std::move(device)) {}
    ~GpuMesh3D() override;
    GpuMesh3D(const GpuMesh3D&) = delete;
    GpuMesh3D& operator=(const GpuMesh3D&) = delete;

    SharedDevice device;
    SDL_GPUBuffer* vertices = nullptr;
    u32 vertex_offset = 0, index_offset = 0; // byte offsets for small static meshes in shared pages
    std::shared_ptr<GpuMeshBufferPool> buffer_pool;
    GpuMeshBufferAllocation allocation;
    // With a skin (MeshData3D::skin; never together with levels or clusters): the second vertex buffer (GpuSkinVertex),
    // and the sphere each joint's vertices stay within, for the culling of posed copies (render3d/skeleton3d.hpp).
    SDL_GPUBuffer* skin = nullptr;
    std::vector<SkinBound3D> skin_bounds;
    SDL_GPUBuffer* indices =
        nullptr; // the chunk's, when `chunk` is set (then levels[].first_index counts from the mesh's range in it)
    u32 index_count = 0; // level 0's
    // The levels of detail (render3d/mesh_lod3d.hpp): each a run of the one index buffer, over the one vertex
    // buffer. At least one: level 0, the mesh itself.
    struct Level {
        u32 first_index = 0;
        u32 index_count = 0;
        f32 error = 0.0f;
    };
    std::vector<Level> levels;
    // With MeshOptions3D::clusters: the hierarchy (on the CPU), whose leaves are `levels[0]`.
    std::shared_ptr<const MeshClusters3D> clusters;
    // And, where the device can cut in compute (RendererCapabilities3D::gpu_driven_clusters): the chunk holding its
    // indices (the leaves', then those of every other cluster, in cluster order, so that every cluster's triangles
    // are at its GpuCluster::first_index) and its clusters' table from `table_first`.
    std::shared_ptr<GpuClusterChunk> chunk;
    u32 table_first = 0;
    u64 serial = 0; // unique for the renderer's life: the shadow cache's name for the mesh (a pointer can be reused)
    // Where global illumination can be had: the box of each run of gi_run_triangles of level 0's triangles (the mesh's
    // space, at rest), so that the GI scene's voxelizer takes only the runs that reach where it changed.
    std::vector<Bounds3D> gi_runs;
};
constexpr u32 gi_run_triangles = 64;

// A cluster as the GPU cut reads it (cluster_cull.comp's Cluster, std430): 80 bytes.
struct GpuCluster {
    f32 center[3], radius;
    f32 parent_center[3], parent_radius;
    f32 cull_center[3], cull_radius;
    f32 error, parent_error, unused0, unused1;
    u32 first_index, triangle_count, depth, parent_depth;
};
static_assert(sizeof(GpuCluster) == 80);

// An environment (render3d/environment3d.hpp): the picture with its mips, and
// the copies filtered from it by the first render that uses it.
class GpuEnvironment3D final : public IEnvironmentBackend3D {
public:
    GpuTexture source;     // the picture, mipmapped
    GpuTexture specular;   // equirectangular, a level a roughness (0 to 1)
    GpuTexture irradiance; // equirectangular, small: what a diffuse surface facing each way receives
    u32 levels = 1;
    bool filtered = false;
};

// A material's own fragment shader (Material3D::shader).
class GpuMaterialShader3D final : public IMaterialShaderBackend3D {
public:
    GpuShader shader;
};

// Where last frame's copies were, for the velocity of this frame's (Scene3D::taa):
// the matrices of every copy of the scene, kept by group (a mesh in a scene), and
// those with a MeshInstance3D::id by that id.
struct GpuCopyHistory {
    struct Span {
        u64 key;
        u32 first, count;       // a group's first matrix and how many
        u32 effect_first = ~0u; // transparent appearances only
    };
    std::vector<Span> groups;         // by key
    std::unordered_map<u64, u32> ids; // a mesh's copy by id: its matrix
    std::vector<Mat4f> matrices;
    std::vector<u64> effects; // transparent appearance, by the same matched copy
    // Skinned copies: the frame's joint matrices (the scene's, then the overlay's) and, by the same place as
    // `matrices`, where each copy's start in them (~0 for a copy that is not skinned). Empty in a frame with no skinned
    // copy.
    std::vector<Mat4f> joints;
    std::vector<u32> joint_first;
};

class GpuView3D final : public IViewBackend3D {
public:
    Texture texture() const override;
    Vec2i size() const override { return _size; }
    u32 samples() const override { return _samples; }
    void reset_history() override {
        _taa_reset_asked = true;
        _occ_reset = true;
    }

private:
    friend class GpuRenderer3D;
    Vec2i _size{};
    u32 _samples = 1; // as made: what the device has for the format
    ViewOptions3D _options{};
    bool _hdr = false;                                                   // drawn in float, then out through view_output
    SDL_GPUTextureFormat _format = SDL_GPU_TEXTUREFORMAT_INVALID;        // the scene's colour (float when HDR)
    SDL_GPUTextureFormat _output_format = SDL_GPU_TEXTUREFORMAT_INVALID; // the 2D pipeline's, followed
    GpuTexture _colour; // multisampled; empty with one sample (drawn into the resolved one)
    GpuTexture _depth;
    GpuTexture _scene;             // HDR: the resolved float picture
    GpuTexture _bloom_a, _bloom_b; // HDR with bloom: half size
    GpuTexture _pick, _pick_depth; // with ViewOptions3D::pick: position and id a pixel
    bool _pick_valid =
        false; // a render has written _pick since it was made (a new target holds whatever the memory did)
    // With Scene3D::ambient_occlusion, made by the first render that asks:
    // the pre-pass's depth and normals (one sample, whatever the view's), and
    // the occlusion (raw, blurred across, blurred down: R8, the view's size or half).
    GpuTexture _ao_chain; // the linear depth chain of the occlusion (R32F, 5 levels), rebuilt each render that uses it;
                          // goes with _ao_depth
    GpuTexture _ao_depth, _ao_normals, _ao_raw,
        _ao_across; // the occlusion's result is in _ao_raw (the blur ends back in it)
    // With Scene3D::contact_shadows, the same way (the pre-pass's depth and
    // normals are shared): the march, blurred across, blurred down; R8, the view's size.
    GpuTexture _contact_raw, _contact_across; // and the contact shadows' in _contact_raw
    // With Scene3D::taa, made by the first render that asks and dropped by the
    // first that does not: one-sample depth for the main pass (the view's own
    // may be multisampled), the 8-bit view's float picture before the resolve
    // (an HDR view draws into _scene), the history (this frame's resolve and the
    // one before, swapped each frame) and the sharpened result.
    GpuTexture _taa_depth, _taa_in, _taa_sharp;
    GpuTexture _taa_motion; // the last frame's pre-pass normals and velocity (swapped with _ao_normals each frame)
    std::array<GpuTexture, 2> _taa_history;
    // The velocities the resolve took (the nearest surface's round each pixel; two halves in R32_UINT, their lowest
    // bits holding how recently the depth bent there, for the sharpening), this frame's and the last, read and written
    // as the history is: the next frame compares its own with them.
    std::array<GpuTexture, 2> _taa_dilated;
    u32 _taa_read = 0;       // which of the two holds the last frame
    u32 _taa_frame = 0;      // frames drawn with it, for the jitter
    u32 _taa_held = 0;       // frames the history holds since it was last reset (a still pixel's weight)
    bool _taa_valid = false; // the history holds a picture to blend
    u64 _taa_coverage_key = 0;
    u64 _taa_shadow_revision = 0;
    bool _taa_reactive = false;        // the last motion buffer includes transient coverage
    bool _taa_reset_asked = false;     // View3D::reset_history()
    bool _taa_ortho = false;           // the camera's kind when it was made
    std::array<Mat4f, 2> _taa_prior{}; // the scene's and the overlay's view-projections last frame (no jitter)
    GpuCopyHistory _taa_last, _taa_now;
    struct ParticleHistory {
        u64 source = 0, id = 0, look = 0;
        ParticleQuad3D quad;
        Vec3f centre;
        LinearRgb colour;
        f32 alpha = 0, frame = 0;
    };
    std::vector<ParticleHistory> _taa_particles;
    std::shared_ptr<GpuTextureBackend> _resolved; // what 2D draws sample
    // With Lod3D::occlusion (made by the first render that asks): the depth pyramid (level 0 a quarter of the view's
    // size, each next half of the one before; `_hiz_levels` of them), the depth it is built from when no pre-pass makes
    // one, and the history of which clusters showed last frame (a word each, in two buffers swapped each render; the
    // draws' places in it are `_occ_layout`).
    static constexpr u32 max_hiz_levels = 12;
    std::array<GpuTexture, max_hiz_levels> _hiz;
    u32 _hiz_levels = 0;
    GpuTexture _occ_depth;
    struct OccRegion {
        u32 scene = 0, draw = 0;
        u64 mesh = 0; // GpuMesh3D::serial: not the address, which a mesh made after another was freed may share
        u32 copies = 0, per_copy = 0;
        bool operator==(const OccRegion&) const = default;
    };
    std::array<std::shared_ptr<GpuBuffer>, 2> _occ_history;
    u32 _occ_read = 0;
    u32 _occ_words = 0; // clusters the history covers
    std::vector<OccRegion> _occ_layout;
    bool _occ_reset = true; // View3D::reset_history(), a resize: the history is dropped
    // With Scene3D::global_illumination (G5; gpu_gi3d.cpp), made by the first render that asks. The GI scene: its
    // voxels' distances (R16F), colours and normals (RGBA8), emission (RGBA16F) and light (the lit scene: RGBA16F, each
    // side's), each level's window stacked along z and stored wrapped round; the build's two words a voxel (the owners
    // and the faces' ways, then the flood's seeds); a window's size, the levels, the first level's voxel side, each
    // window's first voxel, the key of the settings a whole build was made for, and the last render's copies (a hash
    // and a box each, by hash), from which what changed is found. The blocks made this render (the lit scene lights
    // them at once; each a stored x, y and z in a byte each, the level in the top byte) and the boxes whose distances
    // it wrote (a level, then a box of window blocks: the probes near them are placed again); the renders so far (the
    // lit scene's turn). The probes (G5b): their atlases (irradiance RGBA16F, 8 x 8 a probe; moments RG16F, 16 x 16;
    // data RGBA16F, one a probe: offset and state), stacked by level; the rays of a batch and the lists the passes
    // read; a window's counts, the spacing in voxels, the levels, each window's first probe, those waiting to be traced
    // first, the next in turn, the updates so far (the rays' turn) and whether their light is shown.
    struct Gi {
        GpuTexture distance, albedo, normal, emission, radiance, radiance_back;
        std::shared_ptr<GpuBuffer> owners, seeds;
        std::array<u32, 3> size{};
        u32 levels = 0;
        f32 cell = 0.0f;
        std::array<std::array<i32, 3>, max_gi_levels> origin{};
        u64 key = 0;
        bool valid = false; // the textures hold a built scene
        std::vector<std::pair<u64, Bounds3D>> copies;
        std::vector<u32> made;
        std::vector<std::array<i32, 7>> rewritten;
        std::array<std::vector<Bounds3D>, max_gi_levels> waiting_changes; // what changed, each level's, not yet made
        std::vector<Bounds3D> last_changes; // what changed in the last render something did, and its tick
        u32 last_change_tick = 0;
        u32 ticks = 0; // renders that had the GI scene
        u64 renders = 0;
        GpuTexture irradiance, moments, probe_data;
        // The probes' light on the view (gi_resolve.comp's; RGBA16F, half its size), and the render before's, which it
        // keeps from (the pairs take turns), with that render's view.
        GpuTexture light, slope, last_light, last_slope;
        Mat4f last_view_projection{};
        Vec3f last_eye{}, last_forward{};
        bool has_last = false;
        std::shared_ptr<GpuBuffer> rays, place_list, batch_list, light_list;
        std::array<u32, 3> probe_counts{};
        u32 spacing = 0, probe_levels = 0;
        std::array<std::array<i32, 3>, max_gi_levels> probe_origin{};
        std::vector<u32> pending; // probes to trace before the others (a stored index, the level in the top 8 bits)
        std::vector<u8> waiting;  // a probe each (level by level): 1 while it is in `pending`
        bool probes_valid = false;
        u32 next = 0;
        u64 updates = 0;
        bool lit = false;
    } _gi;
    // Renders since each feature's targets were last used; at GpuRenderer3D::feature_release_frames (120) they are
    // given back (age_targets). Zero while the feature runs, so a feature toggled every other render never lets go.
    u32 _idle_ao = 0, _idle_contact = 0, _idle_depth = 0, _idle_normals = 0, _idle_taa = 0, _idle_occlusion = 0,
        _idle_gi = 0;
};

class GpuRenderer3D final : public IRendererBackend3D {
public:
    GpuRenderer3D(GpuRenderer2DBackend& owner, GpuDevice& device);
    ~GpuRenderer3D() override;
    // Called after a frame is submitted: covers pending retired ranges without starting the GPU frame timer early.
    void on_frame_submitted();

    // Whether `device` can draw 3D views (it has a depth format).
    static bool supported(SDL_GPUDevice* device);

    RendererCapabilities3D capabilities() const override { return _capabilities; }
    std::shared_ptr<IMeshBackend3D> create_mesh(const MeshData3D& data, const MeshLodChain3D* chain,
                                                std::shared_ptr<const MeshClusters3D> clusters) override;
    std::shared_ptr<IEnvironmentBackend3D> create_environment(Vec2i size, std::span<const f32> rgb) override;
    std::shared_ptr<IMaterialShaderBackend3D> create_material_shader(std::span<const u8> spirv) override;
    std::shared_ptr<IViewBackend3D> create_view(Vec2i size, const ViewOptions3D& options) override;
    bool resize_view(IViewBackend3D& view, Vec2i size) override;
    bool render(IViewBackend3D& view, const Scene3D& scene, RenderDebugStats3D& stats) override;
    Result3D<PickResult3D> pick(IViewBackend3D& view, Vec2i pixel) override;
    bool warm_up(IViewBackend3D& view, const Scene3D& scene) override;

    // The format of views' depth buffers: D32_FLOAT, else D24_UNORM, else
    // D16_UNORM, as the device has them.
    SDL_GPUTextureFormat depth_format() const { return _depth_format; }

private:
    // In the order a scene's draws are made (shadow, pick and the occlusion's pre-pass are passes of their own).
    enum class Pass : u8 { Opaque, Wire, Alpha, Additive, Shadow, Pick, Prepass, Occluder, Reactive, ReactiveWire };
    enum class Unlit : u8 { Off, Colour, Textured };
    // (Re)makes `view`'s targets for its size, samples and HDR, in the colour
    // format the 2D backend draws in now.
    bool make_targets(GpuView3D& view);
    // `mask`: for a Blend3D::Mask material, in the shadow, pick and pre-passes
    // and the unlit main pass (the generic main pass discards in mesh.frag itself).
    // `velocity`: the main pass of a frame with temporal anti-aliasing and no pre-pass: mesh_velocity's shaders and a
    // second colour target, written by opaque meshes and masked for the rest.
    // `skinned`: for a skinned copy's draws (every pass): the skinning vertex shaders and the layout with the second
    // vertex buffer. `fog`: the main pass's stock shaders with the fog applied (mesh_fog.frag / mesh_velocity_fog.frag;
    // a custom shader is not fogged). `lights`: the stock shaders with spot lights and the shadows of lights that are
    // not the sun (P3b: mesh_light.frag and its fog and velocity variants; a custom shader has neither).
    SDL_GPUGraphicsPipeline* pipeline(SDL_GPUTextureFormat format, u32 samples, bool cull, Pass pass, bool mask = false,
                                      SDL_GPUShader* custom = nullptr, bool velocity = false, bool skinned = false,
                                      bool fog = false, bool lights = false, Unlit unlit = Unlit::Off,
                                      bool indexed = false) const;
    // Pure requests shared by actual draws and conservative warm-up variants.
    struct MaterialPipeline {
        const GpuMaterialShader3D* custom;
        Unlit unlit;
        bool cull, mask;
        Pass pass;
    };
    struct MeshPipelineRequest {
        SDL_GPUTextureFormat format;
        u32 samples;
        MaterialPipeline material;
        bool velocity, skinned;
        bool fog = false, lights = false, indexed = false;
    };
    MaterialPipeline material_pipeline(const Material3D& material, bool skinned, bool taa, bool velocity,
                                       bool fog) const;
    SDL_GPUGraphicsPipeline* pipeline(const MeshPipelineRequest& request) const;
    // The sky pass (sky.frag): a full-screen triangle at the far plane in the main pass's colour target (`velocity`:
    // the second target too, as the main pass with temporal anti-aliasing and no pre-pass has), only where the depth is
    // far.
    SDL_GPUGraphicsPipeline* sky_pipeline(SDL_GPUTextureFormat format, u32 samples, bool velocity) const;
    // The particle pass (P3c): the sprites' pipeline for a blend mode, in the main pass's colour target (`velocity`:
    // the second target too, masked), with the soft-particle fragment shader or not. Null where the shaders were not
    // made.
    SDL_GPUGraphicsPipeline* particle_pipeline(SDL_GPUTextureFormat format, u32 samples, ParticleBlend3D blend,
                                               bool soft, bool velocity, bool reactive = false, bool local = false,
                                               bool shadows = false) const;
    // The camera's turn into the pre-pass's velocity target, over the pre-pass's depth.
    SDL_GPUGraphicsPipeline* sky_velocity_pipeline(SDL_GPUTextureFormat depth) const;
    SDL_GPUGraphicsPipeline* fullscreen(SDL_GPUShader* fragment, SDL_GPUTextureFormat format) const;
    // The pre-pass with velocity, into a target of the two-float normal's size
    // and four floats, over a depth of `depth`.
    SDL_GPUGraphicsPipeline* velocity_pipeline(SDL_GPUTextureFormat depth, bool cull, bool mask,
                                               bool skinned = false) const;
    u32 usable_samples(u32 wanted, SDL_GPUTextureFormat format) const;
    // The shadow map at `resolution` with up to `layers` layers (one a cascade),
    // made or remade: how many it has (fewer if the device could not make
    // them all, said once in the log), 0 without a map.
    u32 ensure_shadow_map(u32 resolution, u32 layers);
    // Records the compute passes that make `environment`'s filtered copies.
    void filter_environment(SDL_GPUCommandBuffer* commands, GpuEnvironment3D& environment);
    // An environment's textures at `w` x `h` (its picture's size), empty: for create_environment and the sky's own.
    std::shared_ptr<GpuEnvironment3D> allocate_environment(u32 w, u32 h);
    // Makes `view`'s pre-pass targets for its size, or reports they can't be made:
    // the depth and normals always; the occlusion's three targets (half the size
    // when `half`) with `want_ao`, the contact shadows' with `want_contact`.
    // With `velocity` the pre-pass's normals are four floats, the last two the velocity.
    bool ensure_ao_targets(GpuView3D& view, bool half, bool want_ao, bool want_contact, bool velocity);
    // The velocity target alone (_ao_normals as four floats), for the main pass that writes the velocity itself.
    bool ensure_velocity_target(GpuView3D& view);
    // Records the compute passes that turn the view's pre-pass into its occlusion.
    // `projection` is the one the main pass draws with (jittered, with temporal anti-aliasing).
    // With `blur_too` false only the first dispatch (the blur is then someone else's: blur_screen_space_both).
    bool build_ao_chain(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Mat4f& projection, f32 radius);
    void compute_occlusion(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Camera3D& camera,
                           const Mat4f& projection, const AmbientOcclusion3D& ao, RenderDebugStats3D& stats,
                           bool blur_too = true);
    // The blur of both, in one dispatch a direction (the two results at one size).
    void blur_screen_space_both(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Mat4f& projection,
                                RenderDebugStats3D& stats);
    // Records the compute passes that march the view's pre-pass toward the sun
    // (`sun`: toward it in world space) and blur the result.
    void compute_contact_shadows(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Camera3D& camera,
                                 const Mat4f& projection, const ContactShadows3D& contact, const Vec3f& sun,
                                 RenderDebugStats3D& stats, bool blur_too = true);
    // The view's passes after the picture is drawn: bloom (HDR), then out to what 2D draws.
    // `source` is the picture (the view's float scene, or the temporal resolve's);
    // `plain` is an 8-bit view's resolved picture (temporal anti-aliasing), which only needs encoding.
    // `dither`: the output shader with a dither of an 8-bit step (a view with a sky; falls back to the plain one
    // without it).
    void finish_view(SDL_GPUCommandBuffer* commands, GpuView3D& view, const GpuTexture& source, bool plain,
                     bool dither = false);
    // Temporal anti-aliasing's targets for the view, made or kept; false if they can't be.
    bool ensure_taa_targets(GpuView3D& view, bool sharpen);
    void release_taa(GpuView3D& view);
    // Occlusion culling's depth pyramid for a depth of w x h (the levels' sizes and count come back), and the depth it
    // is built from when no pre-pass makes one.
    bool ensure_pyramid(GpuView3D& view, u32 w, u32 h, std::array<std::pair<u32, u32>, GpuView3D::max_hiz_levels>& size,
                        u32& levels);
    void ensure_occluder_depth(GpuView3D& view);
    // A frame without temporal anti-aliasing: the history is no good any more (a later frame starts afresh, as after
    // release_taa), but the targets stay until age_targets lets them go.
    void suspend_taa(GpuView3D& view);
    u64 feature_bytes(const GpuView3D& view) const;
    static constexpr u32 _release_after = 120; // unused feature targets expire after 120 renders
    u32 _idle_occlusion_renders = 0; // renders since any view used occlusion culling: the renderer's second gathered
                                     // buffer goes at _release_after
    // Where each copy of `_refs` was last frame, into _taa_prev (a matrix a copy, flat as _visible is) and the
    // view's record for the next.
    // With `skinned` (some copy is) it also settles, for each copy, where its joint matrices of last frame start in the
    // view's last joints (_taa_prev_joint, ~0: none, so it is posed as now).
    void match_copies(GpuView3D& view, bool history_usable, JobSystem* jobs, bool skinned);
    // Resolves `input` into the history and returns the picture to show.
    const GpuTexture* resolve_taa(SDL_GPUCommandBuffer* commands, GpuView3D& view, const GpuTexture& input,
                                  const TemporalAA3D& taa, bool reset, Vec2f jitter, bool reactive, bool reject,
                                  bool fixed_jitter, const GpuTexture* depth, bool overlaid, RenderDebugStats3D& stats);

    // render()'s stages, in the order they run, over a FrameCtx (defined in the .cpp)
    // that holds what they share. Each records into the frame's command buffer; any
    // pass in the middle of the frame is followed by resume_2d().
    struct FrameCtx;
    struct DrawRef;
    void setup_frame(FrameCtx& ctx);     // cameras, temporal anti-aliasing and its jitter
    void fit_sun_shadow(FrameCtx& ctx);  // the shadow map and its cascades or box
    void cull_and_upload(FrameCtx& ctx); // what is seen, and the instance buffers
    void order_draws(FrameCtx& ctx);
    void choose_point_lights(FrameCtx& ctx);
    // P3b: the spot lights that reach the view; which spot and point lights have a shadow map (importance, hysteresis);
    // the moon's cascades; the views all those maps are drawn from. `build_local_shadows` then culls the casters for
    // each view on the CPU, decides which maps are kept from last render (the cache) and uploads what the others draw;
    // `local_shadow_pass` records them.
    void choose_local_lights(FrameCtx& ctx);
    void build_local_shadows(FrameCtx& ctx);
    void local_shadow_pass(FrameCtx& ctx);
    void push_local_lights(const FrameCtx& ctx, u32 s, u32 slot);
    // The local array made or kept (`side` x `side`, a layer for each of `spot_layers` spots then six for each of
    // `point_slots` point lights: only ever more, until it has been idle for _release_after renders; false where it
    // could not be made), and the moon's (its own side, `layers` cascades; how many it has, 0 where one could not be
    // made).
    bool ensure_local_map(u32 side, u32 spot_layers, u32 point_slots);
    u32 ensure_moon_map(u32 side, u32 layers);
    void upload_joints(FrameCtx& ctx);
    void begin_commands(FrameCtx& ctx); // flushes 2D; environment and empty-shadow set-up
    void shadow_pass(FrameCtx& ctx);
    // End of a render: counts the renders each feature's targets went unused and releases those unused for
    // `_release_after`; fills RenderDebugStats3D::feature_bytes.
    void age_targets(FrameCtx& ctx);
    void prepass(FrameCtx& ctx); // depth and normals (or velocity), then the occlusion and contact shadows
    void draw_velocity(FrameCtx& ctx, SDL_GPURenderPass* pass, u32 which, SDL_GPUTextureFormat depth_format);
    void overlay_velocity(FrameCtx& ctx);
    bool main_pass(FrameCtx& ctx);       // false: the pass could not begin (2D is resumed already)
    void resolve_history(FrameCtx& ctx); // the temporal resolve
    void pick_pass(FrameCtx& ctx);
    void push_frame_uniforms(FrameCtx& ctx, u32 s, u32 slot = 0, bool record_history = true);
    // Lights3D::sky_lights_scene: the sky drawn into a small panorama and filtered, when it has moved enough.
    void sky_environment(FrameCtx& ctx);
    // G5, global illumination (gpu_gi3d.cpp). make_gi: the GI scene's pipelines, with the renderer (where the device
    // cannot, absent and RendererCapabilities3D::global_illumination false). gi_scene: the view's GI scene for `scene`,
    // built (compute passes into `commands`) when it has none or what it is made of changed; true when the view has one
    // for this render. draw_gi_view: RenderDebugOptions3D::gi_view over the main pass (`pass`: its colour `format`,
    // `samples`, its velocity target's format or INVALID), traced with `view_projection` (the one the pass draws with).
    void make_gi();
    void release_gi_pipelines();
    bool gi_scene(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Scene3D& scene, RenderDebugStats3D& stats);
    // G5b: the lit scene and the probes' update, after gi_scene (compute passes into `commands`): the sky the probes'
    // rays meet is `environment` (null: the hemisphere of Lights3D::ambient and ground_ambient) turned by
    // `to_environment`. True when the probes' light is in the picture this render.
    bool gi_probes(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Scene3D& scene,
                   const GpuEnvironment3D* environment, const Mat4f& to_environment, RenderDebugStats3D& stats);
    // What the light variants bind for global illumination (samplers 12 and 13: the probes' light on the view, when
    // `resolved`), dummies without; and the probes' three atlases as the compute and the debug view read them.
    std::array<SDL_GPUTextureSamplerBinding, 2> gi_bindings(const GpuView3D& view, bool resolved) const;
    std::array<SDL_GPUTextureSamplerBinding, 3> gi_atlases(const GpuView3D& view, bool lit) const;
    // G5c: the probes' light on what the pre-pass drew, at half the view's size (gi_resolve.comp), after it and its
    // compute (`projection`: the one it drew with; `jittered`: temporal anti-aliasing's, which each texel's keeping
    // from the render before evens out); true when it was read.
    bool gi_resolve(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Scene3D& scene, const Mat4f& projection,
                    bool jittered, RenderDebugStats3D& stats);
    // The view's GI scene and probes as the shaders read them (kin_gi.inc.glsl's KinGiScene, 5 vec4s, into `scene`;
    // KinGiProbes, 6, into `probes`; either may be null).
    void gi_uniforms(const GpuView3D& view, f32 intensity, bool lit, f32* scene, f32* probes) const;
    void draw_gi_view(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const GpuView3D& view,
                      const Scene3D& scene, const Mat4f& view_projection, SDL_GPUTextureFormat format, u32 samples,
                      SDL_GPUTextureFormat velocity_format, bool encode);
    bool read_gi_scene(IViewBackend3D& view, GiScene3D& out) override;
    void release_gi(GpuView3D& view);
    bool _gi_available = false;
    bool _gi_warned = false;
    SDL_GPUComputePipeline* _gi_clear = nullptr;      // gi_clear.comp
    SDL_GPUComputePipeline* _gi_voxelize = nullptr;   // gi_voxelize.comp
    SDL_GPUComputePipeline* _gi_attributes = nullptr; // gi_voxelize_attributes.comp
    SDL_GPUComputePipeline* _gi_flood = nullptr;      // gi_flood.comp
    SDL_GPUComputePipeline* _gi_light = nullptr;      // gi_light.comp
    SDL_GPUComputePipeline* _gi_place = nullptr;      // gi_probe_place.comp
    SDL_GPUComputePipeline* _gi_trace = nullptr;      // gi_probe_trace.comp
    SDL_GPUComputePipeline* _gi_update = nullptr;     // gi_probe_update.comp
    SDL_GPUComputePipeline* _gi_resolve = nullptr;    // gi_resolve.comp
    SDL_GPUSampler* _gi_linear = nullptr;             // filtered, clamped on all three axes
    GpuTexture _gi_none;                              // 1 x 1 x 1, zero: the probes' bindings without probes
    GpuShader _gi_view_shader;                        // gi_view.frag
    SDL_GPUSampler* _gi_sampler = nullptr;            // texel by texel, clamped (the shaders fetch)
    // A build's copies (each draw's in a run: placing, normals, colour, first joint) and their joint matrices, a small
    // buffer bound where a draw has no skin, the lights, the blocks to make (a list, and a bit each); grow.
    std::shared_ptr<GpuBuffer> _gi_copies, _gi_joints, _gi_empty, _gi_lights, _gi_blocks, _gi_dirty;
    // The sky's triangle into a render pass (mode 0: colour, 1: colour and velocity, 2: velocity only; see the .cpp).
    void draw_sky(FrameCtx& ctx, SDL_GPURenderPass* pass, u32 mode);
    // P3c: the frame's particle batches (the scene's draw_particles, then the weather's), culled, sorted and written to
    // the instance buffer; draw_particles() then records them into the main pass.
    struct ParticleSource {
        const ParticleLook3D* look;
        const ParticleSprite3D* sprites;
        u32 count;
        bool weather;
        u64 history;
    };
    void prepare_weather(FrameCtx& ctx);
    std::vector<ParticleSource> particle_sources(const Scene3D& scene) const;
    void prepare_particle_casts(FrameCtx& ctx, std::span<const ParticleSource> sources);
    u32 select_particles(FrameCtx& ctx, std::span<const ParticleSource> sources);
    void match_particles(FrameCtx& ctx, u32 total);
    void upload_particles(FrameCtx& ctx, u32 total);
    void order_particle_batches();
    void prepare_particles(FrameCtx& ctx);
    void draw_particles(FrameCtx& ctx, SDL_GPURenderPass* pass, bool reactive = false);
    bool reactive_pass(FrameCtx& ctx);
    SDL_GPUGraphicsPipeline* particle_cast_pipeline() const;
    struct ParticleCastView {
        Mat4f matrix;
        struct Range {
            u32 batch, first, count;
        };
        std::vector<Range> ranges;
        u64 key = 0;
        bool unstable = false, packed = false;
    };
    const ParticleCastView* particle_cast_view(const Mat4f&) const;
    const ParticleCastView& build_particle_cast_view(FrameCtx&, const Mat4f&, bool pack);
    void upload_particle_casts(FrameCtx&);
    std::vector<ParticleCastView> _particle_cast_views;
    bool draw_particle_shadows(FrameCtx& ctx, SDL_GPURenderPass* pass, const Mat4f& light_view, bool local,
                               u32 cascade = 0);
    void resume_2d();
    // `buffer` as a storage buffer of at least `bytes` (grown to twice its size, and not under `minimum`).
    void ensure_buffer(std::shared_ptr<GpuBuffer>& buffer, u32 bytes, u32 minimum,
                       SDL_GPUBufferUsageFlags usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
    static Pass pass_of(const Material3D& material);
    // A material's texture as a binding: its own, or a white one without.
    SDL_GPUTextureSamplerBinding texture_binding(const Texture& texture, Wrap3D wrap_u, Wrap3D wrap_v);
    void bind_cutout(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Material3D& material);

    GpuRenderer2DBackend* _owner = nullptr;
    GpuDevice* _device = nullptr;
    SDL_GPUTextureFormat _depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTextureFormat _shadow_format = SDL_GPU_TEXTUREFORMAT_INVALID; // depth that can be sampled
    RendererCapabilities3D _capabilities;
    GpuShader _indexed_vertex, _indexed_unlit_vertex, _indexed_unlit_textured_vertex, _indexed_prepass_vertex;
    GpuShader _vertex_shader;   // mesh.vert
    GpuShader _fragment_shader; // mesh.frag
    // Optional unlit main-pass variants. Fog, skinning, custom shaders and TAA use the general shaders.
    bool _unlit_available = false;
    GpuShader _unlit_vertex_shader, _unlit_textured_vertex_shader;
    GpuShader _unlit_shader, _unlit_textured_shader, _unlit_mask_shader, _unlit_textured_mask_shader;
    GpuShader _shadow_shader;                  // shadow.frag
    GpuShader _shadow_vertex_shader;           // shadow.vert
    GpuShader _fullscreen_shader;              // fullscreen.vert
    GpuShader _bloom_shader;                   // bloom.frag
    GpuShader _output_shader;                  // view_output.frag
    GpuShader _pick_shader;                    // pick.frag
    GpuShader _shadow_mask_shader;             // shadow_mask.frag
    GpuShader _shadow_mask_vertex_shader;      // shadow_mask.vert
    GpuShader _pick_mask_shader;               // pick_mask.frag
    GpuShader _prepass_vertex_shader;          // prepass.vert
    GpuShader _prepass_shader;                 // prepass.frag
    GpuShader _prepass_mask_shader;            // prepass_mask.frag
    GpuShader _prepass_velocity_vertex_shader; // prepass_velocity.vert
    GpuShader _prepass_velocity_shader;        // prepass_velocity.frag
    GpuShader _prepass_velocity_mask_shader;   // prepass_velocity_mask.frag
    GpuShader _mesh_velocity_vertex_shader;    // mesh_velocity.vert
    GpuShader _mesh_velocity_shader;           // mesh_velocity.frag
    // P3a: fog variants of the two stock mesh fragment shaders, and the sky's. Absent, and the features off (said
    // once), if they cannot be made.
    GpuShader _mesh_fog_shader;          // mesh_fog.frag
    GpuShader _mesh_velocity_fog_shader; // mesh_velocity_fog.frag
    // P3b: the light variants (spot lights, local and moon shadows), each with and without fog and velocity. Absent,
    // and the features off (said once), if they cannot be made.
    GpuShader _mesh_light_shader;              // mesh_light.frag
    GpuShader _mesh_light_fog_shader;          // mesh_light_fog.frag
    GpuShader _mesh_velocity_light_shader;     // mesh_velocity_light.frag
    GpuShader _mesh_velocity_light_fog_shader; // mesh_velocity_light_fog.frag
    bool _lights_available = false;
    bool _lights_warned = false;
    GpuShader _sky_shader;               // sky.frag
    GpuShader _sky_velocity_shader;      // sky_velocity.frag
    GpuShader _sky_velocity_only_shader; // sky_velocity_only.frag
    GpuShader _sky_env_shader;           // sky_env.frag
    GpuShader _output_dither_shader; // view_output_dither.frag: view_output.frag with a dither, for a view with a sky
    // P3c: particles (the billboards' vertex shader, the fragment shader and its soft-particle variant).
    GpuShader _particle_vertex_shader; // particle.vert
    GpuShader _particle_shader;        // particle.frag
    GpuShader _particle_soft_shader;   // particle_soft.frag
    GpuShader _particle_temporal_vertex;
    GpuShader _reactive_shader, _particle_reactive_shader, _particle_soft_reactive_shader;
    GpuShader _particle_light_vertex, _particle_light_shader, _particle_soft_light_shader;
    GpuShader _particle_reactive_light_shader, _particle_soft_reactive_light_shader;
    GpuShader _particle_shadow_vertex, _particle_shadow_shader, _particle_soft_shadow_shader;
    GpuShader _particle_reactive_shadow_shader, _particle_soft_reactive_shadow_shader;
    GpuShader _particle_cast_shader;
    bool _particles_available = false;
    bool _soft_particles_available = false;
    bool _particles_warned = false;
    bool _particle_shadows_warned = false;
    bool _particles_overflow_warned = false;
    std::shared_ptr<GpuBuffer> _particle_buffer; // the frame's sprites, batch by batch (GpuParticle3D); grows
    struct ParticleBatch {
        ParticleLook3D look;
        Texture texture; // the look's, kept for the draw
        u32 first = 0;   // in the instance buffer, after culling and sorting
        u32 count = 0;
        f32 depth = 0.0f;  // the batch's mean depth along the camera's forward (batches are drawn far to near)
        bool soft = false; // reads the depth
        u64 source = 0;
        u64 look_key = 0;
        bool unstable = false;
    };
    std::vector<ParticleBatch> _particle_batches;
    struct ParticleCastBatch {
        ParticleLook3D look;
        u32 first = 0, count = 0;
        Vec3f centre{};
        f32 radius = 0;
    };
    std::vector<ParticleCastBatch> _particle_cast_batches;
    std::vector<ParticleSprite3D> _particle_cast_sprites;
    std::vector<u32> _particle_cast_order;     // compact per-map source indices
    std::vector<u32> _particle_history_lookup; // bounded previous-history lookup scratch
    std::shared_ptr<GpuBuffer> _particle_cast_buffer;
    std::shared_ptr<GpuBuffer> _particle_previous;
    std::vector<GpuView3D::ParticleHistory> _particle_history_now;
    u32 _particle_cast_idle = 0, _particle_taa_idle = 0;
    std::vector<ParticleSprite3D> _weather_sprites; // the weather's, made each render
    std::vector<WeatherBatch3D> _weather_batches;
    std::vector<f32> _weather_key, _weather_key_now; // what _weather_sprites were made from (and this render's)
    std::vector<std::pair<f32, u32>> _particle_keys,
        _particle_scratch;            // render()'s scratch: a sprite's depth and its index (and the sort's)
    std::vector<u32> _particle_order; // the sprites to write, batch after batch in draw order
    std::vector<const ParticleSprite3D*>
        _particle_source; // and where each batch's sprites are (a scene's or the weather's)
    bool _fog_available = false;
    bool _sky_available = false;
    bool _sky_env_available = false;
    bool _fog_warned = false;
    bool _sky_warned = false;
    // The environment made from the sky (Lights3D::sky_lights_scene), shared by the renderer's views, the sky
    // parameters it was made from, the renders since it was, and how many times it has been made.
    std::shared_ptr<GpuEnvironment3D> _sky_env;
    std::array<f32, 16> _sky_key{};
    // The fog's table of the sky's light (Fog3D::colour_from_sky), and what it was made for.
    sky_model::FogTable3D _fog_table;
    u32 _sky_env_age = 0;
    u32 _sky_env_generations = 0;
    // Skinned copies (Scene3D::draw_skinned): the vertex shaders that pose the vertex by its joints, with the same
    // varyings. Absent (and skinned copies not drawn, said once) if they cannot be made.
    GpuShader _mesh_skinned_vertex_shader;             // mesh_skinned.vert
    GpuShader _mesh_skinned_velocity_vertex_shader;    // mesh_skinned_velocity.vert
    GpuShader _shadow_skinned_vertex_shader;           // shadow_skinned.vert
    GpuShader _shadow_mask_skinned_vertex_shader;      // shadow_mask_skinned.vert
    GpuShader _prepass_skinned_vertex_shader;          // prepass_skinned.vert
    GpuShader _prepass_velocity_skinned_vertex_shader; // prepass_velocity_skinned.vert
    bool _skinning_available = false;
    bool _skin_warned = false;          // skinned copies cannot be drawn
    std::shared_ptr<GpuBuffer> _joints; // the frame's joint matrices (scene, then overlay); grows
    std::shared_ptr<GpuBuffer>
        _joint_offsets;             // each copy's first matrix, a u32 in the places _instances has its copies; grows
    std::vector<u32> _skin_ordered; // render()'s scratch: the offsets in the instances' order
    // Temporal anti-aliasing with skinned copies: last frame's joint matrices (the view's record, uploaded whole) and
    // where each copy's own start there, in the places _instances has the copies (~0: none).
    std::shared_ptr<GpuBuffer> _previous_joints, _previous_joint_offsets;
    std::vector<u32> _taa_prev_joint;    // match_copies' answer a copy (flat, as _taa_prev)
    std::vector<u32> _taa_past;          // and the copy's place in last frame's record (~0: none)
    std::vector<u32> _skin_prev_ordered; // render()'s scratch: the same in the instances' order
    // Temporal anti-aliasing needs the pre-pass and four-float colour targets,
    // and a float storage texture; else it is off (said once, when asked for).
    bool _taa_available = false;
    // The main pass's one-sample depth (_taa_depth) can be sampled: kept for the resolve, which takes the velocity of
    // the nearest surface at an edge (the overlay's in front, as its slice of the depth range puts it).
    bool _taa_depth_sampled = false;
    // The main pass writes velocity when temporal anti-aliasing has no other use for a pre-pass.
    bool _taa_warned = false;
    bool _taa_prepass_warned = false;
    SDL_GPUComputePipeline* _taa_pipeline = nullptr; // taa.comp
    std::shared_ptr<GpuBuffer> _previous;            // last frame's matrix of each copy in _instances; grows
    std::vector<Mat4f> _taa_prev;                    // render()'s scratch: a matrix per copy of the scene
    std::vector<Mat4f> _taa_ordered;                 // and per copy in _instances
    struct CopyRef {
        u32 slot = 0;                        // a draw's first copy among its group's in the view's record,
        u32 past = 0;                        // and where its place was last frame (~0: no same-sized group then)
        u32 effect = ~0u, past_effect = ~0u; // compact transparent-only records
    };
    std::vector<u32> _taa_matched;               // previous history slot per current grouped copy (~0 when new)
    std::vector<CopyRef> _taa_copy_refs;         // match_copies' scratch, kept between frames:
    std::vector<std::pair<u64, u32>> _taa_keyed; // draws by group key,
    std::vector<u32> _taa_group_of;              // their groups,
    std::vector<u8> _taa_slow;                   // and a byte a copy: not settled by the fast path
    // Ambient occlusion needs these formats and two compute pipelines; where
    // the device lacks one it is off (a line in the log the first time it is asked for).
    SDL_GPUTextureFormat _ao_depth_format = SDL_GPU_TEXTUREFORMAT_INVALID; // depth that can be sampled
    bool _ao_available = false;
    bool _ao_warned = false;
    bool _contact_warned = false;
    SDL_GPUComputePipeline* _contact_pipeline = nullptr;            // contact.comp
    SDL_GPUComputePipeline* _gtao_pipeline = nullptr;               // gtao.comp
    SDL_GPUComputePipeline* _gtao_chain_pipeline = nullptr;         // gtao_chain.comp (optional: gtao.comp without)
    SDL_GPUComputePipeline* _ao_chain_pipeline = nullptr;           // ao_depth_chain.comp
    SDL_GPUComputePipeline* _ao_blur_chain_pipeline = nullptr;      // ao_blur_chain.comp (optional)
    SDL_GPUComputePipeline* _ao_blur_both_chain_pipeline = nullptr; // ao_blur_both_chain.comp (optional)
    SDL_GPUComputePipeline* _ao_blur_pipeline = nullptr;            // ao_blur.comp
    SDL_GPUComputePipeline* _ao_blur_both_pipeline =
        nullptr; // ao_blur_both.comp: both features' blur in one dispatch (optional)
    // The shadow map's cache (Lights3D::cache_shadows): what the map was last drawn from, so that the pass is skipped
    // when this render's would come out the same (shadow_unchanged / shadow_drawn). `key` is every number the pass is a
    // function of except the casters' copies (camera, sun, area, cascade fit, frusta, levels of detail, each caster's
    // mesh and material); `copies` is a snapshot of the casters' MeshInstances, taken only once two renders in a row
    // agree on the key and a sample of the copies (a scene that moves never copies anything). `valid`: `copies` is
    // exactly what the map holds.
    struct ShadowCache {
        std::vector<u8> key, prev_key, scratch;
        u64 sample = 0, prev_sample = 0, now = 0;
        std::vector<u8> copies;
        bool valid = false;
        bool eligible = false; // this render's pass can be cached on
        u32 backoff = 0;       // renders to wait before copying again after a snapshot that did not hold
    } _shadow_cache;
    u64 _shadow_revision = 0;
    u64 _shadow_generation = 0; // changes whenever _shadow_map is made again
    u64 _next_mesh_serial = 1;
    template <class Consume> bool visit_shadow_copy_bytes(Consume&& consume) const;
    bool shadow_unchanged(FrameCtx& ctx);
    void shadow_drawn(FrameCtx& ctx, bool complete);
    bool _chain_ready = false; // the occlusion built the linear depth chain this render: the blurs read it
    SDL_GPUSampler* _point_sampler = nullptr;           // texel by texel, for compute
    SDL_GPUSampler* _compare_sampler = nullptr;         // the shadow map's
    SDL_GPUSampler* _environment_sampler = nullptr;     // wraps round, clamps at the poles, mipmapped
    SDL_GPUComputePipeline* _filter_pipeline = nullptr; // env_filter.comp
    GpuTexture _shadow_map;
    u32 _shadow_size = 0;
    u32 _shadow_layers = 0;
    u32 _shadow_layers_limit = max_shadow_cascades; // lowered when the device would not make more
    GpuTexture _no_shadow;                          // 1 x 1, all far: every comparison lit
    // P3b. The local array: a layer a spot light's map and six a point light's, `_local_side` texels a side (a light
    // draws into a corner of it); the moon's cascades are an array of their own. `_*_generation` changes when one is
    // made again (nothing cached is in it).
    GpuTexture _local_map;
    u32 _local_side = 0, _local_layers = 0;
    u32 _local_spot_layers = 0, _local_point_slots = 0; // its layout: the spots' layers, then the point lights' cubes
    u64 _local_generation = 0;
    u32 _local_idle = 0;
    u32 _local_smaller = 0; // renders in a row the lights with maps needed a smaller side than the array's
    GpuTexture _moon_map;
    u32 _moon_side = 0, _moon_layers = 0;
    u64 _moon_generation = 0;
    u32 _moon_idle = 0;
    // Which light holds which slot, across renders.
    ShadowSlots3D _spot_slots, _point_slots;
    // What each layer (0..19 the local array, 20..23 the moon's) was last drawn from: the exact bytes of everything the
    // draw is a function of (the view, the casters' meshes and materials, their copies and poses), so a render whose
    // bytes are the same does not draw it again.
    struct LocalCache {
        std::vector<u8> payload;
        u64 revision = 0;
        bool fingerprinted = false;
        bool valid = false;
    };
    std::array<LocalCache, max_local_shadow_layers + max_shadow_cascades> _local_cache;
    // The local views' casters of this render: the union of their copies as GpuInstance3D, each view's lists of them
    // (u32), and the joint offsets of the posed ones.
    std::shared_ptr<GpuBuffer> _local_instances, _local_lists, _local_joint_offsets;
    struct LocalEntry {
        u32 ref = 0, level = 0;
        u32 list_first = 0, count = 0;
        // A clustered mesh's copies: each has its own run of the compacted indices (the cut as the light's map sees
        // it), in `_local_runs` from `run_first`, one for each copy in the list.
        bool clustered = false;
        u32 run_first = 0;
    };
    struct LocalView {
        Mat4f view_projection{};
        GpuFrustum frustum{};
        GpuFrustum slice{}; // the moon's: the camera's view of this cascade's depth range
        SDL_GPUTexture* texture = nullptr;
        u32 layer = 0; // in its array
        u32 cache = 0; // its _local_cache index
        u32 size = 0;  // the viewport's side
        bool moon = false;
        bool single_map = false; // a moon's lone map over the whole area (no slice test)
        Vec3f down{};            // the moon's light's way
        Vec3f centre{};          // a sphere that holds all that can cast into it (a spot's or point light's reach)
        f32 radius = 0.0f;
        bool bounded = false;
        // What a clustered mesh's cut is judged from (the light's eye and axis, the pixels a unit spans, the near
        // plane).
        Vec3f eye{}, axis{0.0f, 0.0f, -1.0f};
        f32 per_unit = 1.0f, near_plane = 0.1f;
        u32 kind = 0;       // 0 a spot, 1 a face of a point light's cube, 2 the moon
        u32 owner = 0;      // the light's index in the scene
        bool wanted = true; // a face of a cube no visible surface falls in is not drawn
        bool cached = false;
        bool drawn = false;
        u32 entry_first = 0, entry_count = 0;
    };
    std::vector<LocalView> _local_views;
    std::vector<LocalEntry> _local_entries;
    std::vector<u32> _local_list;                  // the lists, as uploaded
    std::vector<u32> _local_batch_list;            // reused while joining compatible depth draws
    std::vector<u32> _local_batch_refs;            // each ref's compatible depth-draw representative
    std::vector<std::array<u32, 2>> _local_copies; // the union's (ref, copy), packed directly into upload memory
    std::vector<u32> _local_joint_list;
    std::vector<std::array<u32, 2>> _local_runs; // a clustered copy's (first index, count) in _local_cluster_indices
    std::vector<u32> _local_cluster_indices;     // the compacted indices of the clusters each local view's cut chose
    std::shared_ptr<GpuBuffer> _local_index_buffer;
    std::vector<Vec4f>
        _world_spheres;             // uncached local casters only; DrawRef::sphere_first addresses this compact scratch
    std::vector<u32> _local_slot;   // each copy's place in the union (~0: none yet)
    std::vector<u8> _local_payload; // scratch
    bool _no_shadow_cleared = false;
    // Samplers for materials' textures by filter, wrap, anisotropy and mip bias (made on first use). The mipmapped ones
    // read with _anisotropy and _lod_bias: this render's Scene3D::anisotropy and ::texture_lod_bias (in sixteenths of
    // a level), clamped.
    SDL_GPUSampler* sampler_for(const GpuTextureBackend& texture, Wrap3D u, Wrap3D v);
    std::unordered_map<u32, SDL_GPUSampler*> _samplers;
    u32 _anisotropy = 1;
    i32 _lod_bias = 0;
    std::vector<std::shared_ptr<GpuMaterialShader3D>> _material_shaders;
    std::shared_ptr<GpuBuffer> _instances;    // grows
    std::shared_ptr<GpuBuffer> _caster_lists; // the cascades' lists of copies (indices into _instances); grows
    std::array<f32, 256> _srgb_to_linear{};

    // render()'s scratch, kept between frames.
    struct DrawRef {
        const Scene3D::Draw* draw = nullptr;
        const MeshInstance3D* instances = nullptr; // the draw's first
        const GpuMesh3D* mesh = nullptr;
        u32 scene = 0;                       // 0 the scene, 1 its overlay
        const GpuFrustum* frustum = nullptr; // null: not culled
        bool caster = false;                 // drawn into the shadow map
        Vec3f center{};                      // the mesh's bounding sphere
        f32 radius = 0.0f;
        u32 out_first = 0;                                    // where its copies go in the instance buffer:
        u32 out_count = 0;                                    // those the camera sees,
        u32 out_extra = 0;                                    // then those only the sun sees (casting into view)
        std::array<u32, max_shadow_cascades> cascade_first{}; // with cascades: each one's list of copies,
        std::array<u32, max_shadow_cascades> cascade_count{}; // in _caster_list
        // With levels of detail (Scene3D::lod, a mesh with levels): the copies are grouped by the level each is
        // drawn at, and within a level the seen ones come first, then the sun's. `levels` is 1 without, and then
        // all of this is the run above (level_first[0] == out_first, level_seen[0] == out_count, ...).
        u32 levels = 1;
        std::array<u32, max_mesh_levels> level_first{}, level_seen{}, level_extra{};
        // And each cascade's list is in runs by level.
        std::array<std::array<u32, max_mesh_levels>, max_shadow_cascades> cascade_level_first{}, cascade_level_count{};
        // A mesh with clusters, cut by Scene3D::lod this render: its copies are drawn one by one from the frame's
        // compacted indices (`levels` is 1: the copies keep to the seen-then-sun order).
        bool clustered = false;
        u32 job_first = 0, job_count = 0; // the GPU cut's copies of it: a run of the job table
        u32 hist_first = 0; // with Lod3D::occlusion: where its copies' clusters start in the view's history
        // Skinned copies (Scene3D::draw_skinned): each copy's first matrix in the scene's joints (indexed as
        // `instances`), the scene's matrices, and where the scene's start in the frame's joint buffer. `center` and
        // `radius` are then only a start: a copy's own bound comes from its pose.
        bool skinned = false;
        const u32* skin_first = nullptr;
        const Mat4f* joints = nullptr;
        u32 joint_base = 0;
        u32 group_first = 0, group_count = 0;  // in _cull_groups, preserved through draw merging
        u32 retained = ~0u;                    // bounded retained-table cache entry, or immediate path
        const Vec4f* cached_spheres = nullptr; // stable CPU bounds from a resident table, one level only
        u32 sphere_first = 0;                  // uncached local casters' start in _world_spheres
    };
    // One layout only, reusing the existing draw/piece/task arrays. The key stores no
    // owning scene handles; check it before dereferencing any previous draw pointer.
    struct PreparedKey {
        const Scene3D* scene = nullptr;
        const Scene3D::Draw* draws = nullptr;
        const MeshInstance3D* instances = nullptr;
        std::size_t count = 0;
        u64 identity = 0, revision = 0;
        bool operator==(const PreparedKey&) const = default;
    };
    // Exact structural validation across clear()/resubmission. Weak mesh owners
    // prevent pointer-reuse mistakes without keeping removed assets resident.
    struct PreparedDraw {
        std::weak_ptr<IMeshBackend3D> mesh;
        Bounds3D bounds;
        u64 batch = 0;
        u32 first = 0, count = 0, skin_first = 0, group_first = 0, group_count = 0;
        bool skinned = false;
    };
    static_assert(sizeof(PreparedDraw) <= 96);
    std::vector<PreparedDraw> _prepared_draws;            // <=8192 descriptors, never instance records
    std::vector<Scene3D::InstanceGroup> _prepared_groups; // <=4096 streamed groups
    struct PreparedBatch {
        u32 ref;
        u64 revision;
    };
    std::array<PreparedKey, 2> _prepared_keys{};
    std::vector<PreparedBatch> _prepared_batches; // <=128 entries, not per copy
    bool _prepared_valid = false;
    u32 _prepared_total = 0;
    // A table per batch and material colour (two views or two draws of a batch in different colours keep one each).
    struct RetainedTable {
        u64 id = 0, revision = 0, claimed = 0, upload_frame = ~0ull;
        u64 frame = 0, used = 0; // the _retained_frames it was last claimed in, and the claim's place in
                                 // _retained_claims (eviction's order)
        u64 serial = ~0ull,
            layout = 0; // the GPU frame (submission) it was last claimed in; the batch layout_revision it holds
        Color color{};
        std::weak_ptr<const void> lifetime;
        std::shared_ptr<GpuBuffer> buffer; // null: a free entry (evicted), dropped at the next collection
        u64 bounds_revision = 0;
        std::vector<Vec4f>
            bounds; // one of a batch's tables holds them; at most 1/8 of the live GPU-table budget (4 MiB total)
    };
    std::vector<RetainedTable> _retained;
    u64 _retained_epoch = 0, _retained_gc_frame = ~0ull, _retained_claims = 0;
    // Frame serials with a 3D render (counted at the collection): what "unused for a whole frame" counts in. A
    // read-back with no render between does not age the tables; a pick() or read-back between two renders of one app
    // frame does (each render then sees a new serial), which can cost an early eviction and re-upload under a full
    // budget, never a wrong picture.
    u64 _retained_frames = 0;
    u32 _retained_bytes = 0;
    // The largest batch refused a table this GPU frame, and the room the next collection evicted for it (kept for it
    // that frame).
    struct RetainedWant {
        u64 id = 0, bytes = 0;
        std::weak_ptr<const void> lifetime;
    };
    RetainedWant _retained_wanted, _retained_reserved;
    std::vector<u32> _retained_victims;                          // scratch
    std::vector<InstanceBatch3D::ChangedRange> _retained_ranges; // an edited table's changed copies (scratch)
    std::vector<std::array<u32, 2>> _retained_spans;             // and their bytes in the table
    std::vector<u32> _retained_starts;                           // each range's first among the changed copies
    void drop_table(RetainedTable& table);
    bool make_room(u64 id, u64 bytes, bool active_too, RenderDebugStats3D& stats);
    bool _retained_enabled = false;
    bool _bounds_cache_grew = false;
    bool _gpu_markers = false; // external GPU profiler labels, no fence scopes
    u32 _bounds_cache_bytes = 0;
    std::vector<u32> _retained_visible;
    std::shared_ptr<GpuBuffer> _retained_indices;
    GpuInstance3D pack_instance(const MeshInstance3D& instance, Color material) const;
    void retain_instances(DrawRef& ref, FrameCtx& ctx);
    void cache_instance_bounds(DrawRef& ref, RenderDebugStats3D& stats);
    struct CullGroup {
        u32 first = 0, end = 0; // relative to the draw
        u8 camera = 1;          // 0 outside, 1 partial, 2 inside
        u8 sun_mask = 0, sun_inside = 0;
        std::array<u8, max_shadow_cascades> sun_reach_planes{}, moon_reach_planes{};
        u32 local_mask = 0, local_radius_inside = 0;
        std::array<u8, max_local_shadow_layers + max_shadow_cascades> local_planes{};
    };
    struct Piece {
        u32 ref = 0;
        u32 begin = 0, end = 0; // its copies, within the draw
        u32 flat = 0;           // the first's place among all the frame's copies
        u32 group = ~0u;        // no group for unbounded gaps
        u32 camera_tests = 0, bounds_tests = 0;
        // By level of detail (index 0 alone without): the copies seen, and those only the sun sees; where they go.
        std::array<u32, max_mesh_levels> seen{}, extra{};
        std::array<u32, max_mesh_levels> out{}, out_extra{};
        // Copies casting into each cascade, by level, and where they go in its list.
        std::array<std::array<u32, max_mesh_levels>, max_shadow_cascades> cascade{}, cascade_out{};
    };
    // The cut through each cluster mesh's hierarchy, per copy (render3d/mesh_clusters3d.hpp): the clusters chosen, as
    // runs in one index buffer made for the frame. A copy has a run for the camera's passes (main, pre-pass, velocity,
    // pick: the clusters in the camera's view) and one for each shadow map (a cascade, or the one map): those in its
    // view; a run equal to an earlier one is that run.
    static constexpr u32 cluster_views = 1 + max_shadow_cascades;
    struct ClusterRun {
        u32 first = 0, count = 0; // in indices
    };
    struct ClusterJob {
        u32 ref = 0, copy = 0; // the draw, and the copy within it
        u32 slot = 0;          // its place in the instance buffer
        u8 visible = 0;        // _visible's bits
        u32 tested = 0;
        u32 scene_draw = 0; // the draw's index in its scene's draws()
        std::vector<SelectedCluster3D> picks;
        std::array<ClusterRun, cluster_views> runs{};
        std::array<u8, cluster_views> same_as{}; // 1 + the view this one repeats, 0 if none
    };
    std::vector<ClusterJob> _cluster_jobs;
    // The camera's view of `ref`'s clustered copies, from the passes `phase_parts` says (all, but in the pre-pass of
    // Lod3D::occlusion); `stats`, when given, counts them as the pre-pass's.
    template <typename Binder, typename Uniforms>
    void draw_cluster_copies(const DrawRef& ref, SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass,
                             Binder& binder, Uniforms& u, RenderDebugStats3D* stats) const;
    // Lod3D::occlusion splits the pre-pass in two: `First` draws the opaque meshes and the first pass's clusters, from
    // which the pyramid is built; `Second` the second pass's. A mesh with a shader of its own (which may discard what
    // the pyramid would count as covered) is no occluder: it waits for `Second`, all of its clusters. `All` is every
    // other draw.
    enum class Phase : u8 { All, First, Second };
    Phase _phase = Phase::All;
    bool phase_skips(const DrawRef& ref) const;
    u32 phase_parts(const DrawRef& ref) const;
    std::vector<std::array<ClusterRun, cluster_views>> _cluster_runs; // by instance slot (the clustered copies')
    std::shared_ptr<GpuBuffer> _cluster_indices; // the frame's compacted indices (an index buffer); grows
    // Cuts every clustered copy this render sees and uploads the compacted indices (after cull_and_upload has placed
    // the copies). With the GPU cut (ctx.gpu_clusters) it only lists the copies and uploads the list; cluster_pass_gpu
    // records the compute.
    void cut_clusters(FrameCtx& ctx, JobSystem* jobs);
    void cut_clusters_gpu(FrameCtx& ctx, std::size_t count);
    // The GPU cut's compute passes (before the passes that draw): cull, scan, arguments, gather.
    // `phase` 0: the whole cut; with occlusion 1: the first pass (set 0), 2: the second (set 1, after the pyramid is
    // built).
    void cluster_pass_gpu(FrameCtx& ctx, u32 phase = 0);
    // Lod3D::occlusion: the depth pyramid from `depth` (a pass's single-sample depth), and the pass that draws the
    // frame's opaque meshes into a depth of their own when no pre-pass does. Both record into the frame; false when
    // the pyramid could not be built (the second pass then tests nothing).
    bool build_depth_pyramid(FrameCtx& ctx, const GpuTexture& depth);
    void occluder_pass(FrameCtx& ctx);
    // Which clustered draws a pass draws: the first pass's and the second's (set 0's camera view and set 1's).
    enum : u32 { cluster_first = 1, cluster_second = 2, cluster_both = 3 };
    // The index buffer clustered meshes draw from this render: the GPU cut's, or the CPU cut's.
    SDL_GPUBuffer* cluster_index_buffer(u32 set = 0) const;
    // Draws `ref`'s copies in view `view` (0 the camera's, 1 + k the cascade k's, 1 the single map) with indirect
    // draws.
    template <typename Uniforms>
    void draw_cluster_indirect(const DrawRef& ref, SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, Uniforms& u,
                               u32 view, u32 set = 0) const;
    // Waits for the GPU and reads the cut's statistics (Lod3D::stats_readback), or, for `picks`, which clusters it
    // chose.
    void read_cluster_stats(FrameCtx& ctx);
    bool read_cluster_picks(IViewBackend3D& view, std::vector<ClusterPick3D>& out) override;
    bool read_cluster_draws(IViewBackend3D& view, std::vector<ClusterDraw3D>& out) override;
    // The GPU cut. `_gc_*` are its compute pipelines and buffers; `_gc` what this render set up (valid while `active`).
    bool _gpu_clusters = false;        // the device can (capabilities)
    bool _occlusion_available = false; // and can build a depth pyramid (RendererCapabilities3D::occlusion_culling)
    bool _occlusion_warned = false;
    bool _occlusion_history_warned = false;
    SDL_GPUComputePipeline* _gc_cull_occ = nullptr;         // cluster_cull_occ.comp
    SDL_GPUComputePipeline* _hiz_depth_pipeline = nullptr;  // hiz_depth.comp
    SDL_GPUComputePipeline* _hiz_reduce_pipeline = nullptr; // hiz_reduce.comp
    std::array<GpuTexture, 4> _hiz_spare;       // 1 x 1, written by no one: bound where a pass has fewer levels to make
    SDL_GPUComputePipeline* _gc_cull = nullptr; // cluster_cull.comp
    SDL_GPUComputePipeline* _gc_scan = nullptr; // cluster_scan.comp
    SDL_GPUComputePipeline* _gc_args = nullptr; // cluster_args.comp
    SDL_GPUComputePipeline* _gc_gather = nullptr; // cluster_gather.comp
    std::shared_ptr<GpuBuffer> _gc_jobs;          // a copy each: slot, scene, bits, scale, first cluster
    // The cut's outputs. Set 0 is the cut's (all views); with Lod3D::occlusion, set 1 holds the second pass's clusters
    // (the camera's view alone) and set 0's camera view the first pass's.
    struct GcBuffers {
        std::shared_ptr<GpuBuffer> scan;    // the counts, then their scan: a row of all clusters a view
        std::shared_ptr<GpuBuffer> blocks;  // the scan's block sums
        std::shared_ptr<GpuBuffer> args;    // indirect draws, a (view, copy) each
        std::shared_ptr<GpuBuffer> list;    // the chosen elements, a queue a mesh
        std::shared_ptr<GpuBuffer> counts;  // statistics, then each queue's length (then, in set 1, the occlusion's)
        std::shared_ptr<GpuBuffer> indices; // the gathered indices
    };
    std::array<GcBuffers, 2> _gcb;
    std::shared_ptr<GpuBuffer> _gc_job_history; // a word a copy: where its clusters start in the view's history
    struct GcDispatch {
        const GpuClusterChunk* chunk = nullptr;
        u32 first_job = 0, jobs = 0;
        u32 threads = 0;       // the clusters of its copies, all
        u32 first_cluster = 0; // its first copy's first cluster among all
        u32 queue = 0;         // where its queue starts in _gc_list
        u32 queue_size = 0;    // and how many (view, cluster)s it has room for
    };
    std::vector<std::shared_ptr<GpuClusterChunk>> _chunks;
    u32 _next_chunk_id = 0;
    // Room in a chunk for a mesh of `indices` indices and `clusters` clusters (a chunk made if none has): the chunk,
    // and where its ranges start.
    std::shared_ptr<GpuClusterChunk> allocate_cluster_range(u32 indices, u32 clusters, u32& index_first,
                                                            u32& table_first);
    std::shared_ptr<GpuMeshBufferPool> _mesh_buffers;
    void collect_mesh_buffers();
    struct GcFrame {
        bool active = false;
        u32 jobs = 0, views = 0, clusters = 0; // copies, passes' views, clusters in all copies
        u32 room = 0;                          // triangles the gathered buffer holds
        u32 table_bytes = 0;
        std::vector<GcDispatch> dispatches;
        std::vector<GcDispatch>
            dispatches_second;  // with occlusion: the same copies' queues for the second pass (one view)
        bool occlusion = false; // both passes are recorded
        u32 room_second = 0;
        std::vector<u32> job_history;
        std::vector<u8> job_data; // the job table, uploaded
        std::vector<u32> bases;   // each copy's first cluster among all
        std::vector<u8> zeros;
        // The cull's uniform block (cluster_cull.comp's Params), but for the range it is dispatched over.
        std::vector<f32> params;
        bool overflow_warned = false;
    } _gc;
    bool _gc_overflow_warned = false;
    std::vector<DrawRef> _refs;
    std::vector<CullGroup> _cull_groups;
    std::vector<Piece> _pieces;
    std::vector<u32> _tasks;  // where each task's pieces start
    std::vector<u8> _visible; // a copy: the bits of camera_bit, sun_bit and cascade_bit(k) (gpu_renderer3d.cpp)
    std::vector<u8>
        _level_of; // a copy: its level of detail (only filled with Scene3D::lod, for the meshes that have levels)
    std::vector<u32> _caster_list;            // the cascades' lists, uploaded
    std::vector<u32> _order;                  // draws sorted
    std::vector<std::pair<f32, u32>> _sorted; // an alpha draw's copies, furthest first
};

} // namespace kin::gpu
