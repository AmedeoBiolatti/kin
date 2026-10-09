#pragma once
// The SDL_GPU backend's 3D part (IRenderer3DBackend), made the first time a
// Renderer3D asks for it. It shares the 2D backend's device, pipeline cache
// and frame: a view is rendered in the frame's command buffer between the 2D
// draws around it. Experimental: see docs/3d_plan.md.
#include <kin/render3d/backend3d.hpp>
#include <kin/render3d/shadow_cascades.hpp>

#include "gpu_buffer.hpp"
#include "gpu_shader.hpp"
#include "gpu_texture.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace kin {
class GpuRenderer2DBackend;
class JobSystem;
}

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

// Meshes the GPU cuts (MeshOptions::clusters, on a device that can) share big buffers, so that one dispatch of the cut
// covers many meshes (a barrier for every buffer bound is what a dispatch a mesh costs: Sponza's hundred meshes took a
// millisecond of it): a chunk is an index buffer holding each of its meshes' indices (the leaves', then every other
// cluster's) and the table of their clusters. Meshes are put in the first chunk with room; a mesh too big for a chunk
// has one of its own. Ranges are not given back when a mesh goes; the chunk is, with its last mesh.
struct GpuClusterChunk {
    explicit GpuClusterChunk(SharedDevice device) : device(std::move(device)) {}
    ~GpuClusterChunk();
    GpuClusterChunk(const GpuClusterChunk&) = delete;
    GpuClusterChunk& operator=(const GpuClusterChunk&) = delete;

    SharedDevice device;
    SDL_GPUBuffer* indices = nullptr; // INDEX, and storage the cut reads
    SDL_GPUBuffer* table = nullptr;   // GpuCluster each
    u32 index_capacity = 0, index_used = 0;  // in indices
    u32 table_capacity = 0, table_used = 0;  // in clusters
    u32 id = 0;                              // among the renderer's chunks, for ordering a render's copies
};

// A mesh's buffers. Games may keep meshes past the renderer (in scenes that
// outlive it): like textures they hold the device's shared handle, and skip
// the release once it is gone.
class GpuMesh3D final : public IMeshBackend {
public:
    explicit GpuMesh3D(SharedDevice device) : device(std::move(device)) {}
    ~GpuMesh3D() override;
    GpuMesh3D(const GpuMesh3D&) = delete;
    GpuMesh3D& operator=(const GpuMesh3D&) = delete;

    SharedDevice device;
    SDL_GPUBuffer* vertices = nullptr;
    SDL_GPUBuffer* indices = nullptr; // the chunk's, when `chunk` is set (then levels[].first_index counts from the mesh's range in it)
    u32 index_count = 0; // level 0's
    // The levels of detail (render3d/mesh_lod.hpp): each a run of the one index buffer, over the one vertex
    // buffer. At least one: level 0, the mesh itself.
    struct Level {
        u32 first_index = 0;
        u32 index_count = 0;
        f32 error = 0.0f;
    };
    std::vector<Level> levels;
    // With MeshOptions::clusters: the hierarchy (on the CPU), whose leaves are `levels[0]`.
    std::shared_ptr<const MeshClusters> clusters;
    // And, where the device can cut in compute (Renderer3DCapabilities::gpu_driven_clusters): the chunk holding its
    // indices (the leaves', then those of every other cluster, in cluster order, so that every cluster's triangles
    // are at its GpuCluster::first_index) and its clusters' table from `table_first`.
    std::shared_ptr<GpuClusterChunk> chunk;
    u32 table_first = 0;
    u64 serial = 0; // unique for the renderer's life: the shadow cache's name for the mesh (a pointer can be reused)
};

// A cluster as the GPU cut reads it (cluster_cull.comp's Cluster, std430): 80 bytes.
struct GpuCluster {
    f32 center[3], radius;
    f32 parent_center[3], parent_radius;
    f32 cull_center[3], cull_radius;
    f32 error, parent_error, unused0, unused1;
    u32 first_index, triangle_count, depth, parent_depth;
};
static_assert(sizeof(GpuCluster) == 80);

// An environment (render3d/environment.hpp): the picture with its mips, and
// the copies filtered from it by the first render that uses it.
class GpuEnvironment3D final : public IEnvironmentBackend {
public:
    GpuTexture source;     // the picture, mipmapped
    GpuTexture specular;   // equirectangular, a level a roughness (0 to 1)
    GpuTexture irradiance; // equirectangular, small: what a diffuse surface facing each way receives
    u32 levels = 1;
    bool filtered = false;
};

// A material's own fragment shader (Material3D::shader).
class GpuMaterialShader3D final : public IMaterialShaderBackend {
public:
    GpuShader shader;
};

// Where last frame's copies were, for the velocity of this frame's (Scene3D::taa):
// the matrices of every copy of the scene, kept by group (a mesh in a scene), and
// those with a MeshInstance::id by that id.
struct GpuCopyHistory {
    struct Span {
        u64 key;
        u32 first, count; // a group's first matrix and how many
    };
    std::vector<Span> groups; // by key
    std::unordered_map<u64, u32> ids;                    // a mesh's copy by id: its matrix
    std::vector<Mat4f> matrices;
};

class GpuView3D final : public IView3DBackend {
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
    View3DOptions _options{};
    bool _hdr = false; // drawn in float, then out through view_output
    SDL_GPUTextureFormat _format = SDL_GPU_TEXTUREFORMAT_INVALID;        // the scene's colour (float when HDR)
    SDL_GPUTextureFormat _output_format = SDL_GPU_TEXTUREFORMAT_INVALID; // the 2D pipeline's, followed
    GpuTexture _colour; // multisampled; empty with one sample (drawn into the resolved one)
    GpuTexture _depth;
    GpuTexture _scene;             // HDR: the resolved float picture
    GpuTexture _bloom_a, _bloom_b; // HDR with bloom: half size
    GpuTexture _pick, _pick_depth; // with View3DOptions::pick: position and id a pixel
    // With Scene3D::ambient_occlusion, made by the first render that asks:
    // the pre-pass's depth and normals (one sample, whatever the view's), and
    // the occlusion (raw, blurred across, blurred down: R8, the view's size or half).
    GpuTexture _ao_depth, _ao_normals, _ao_raw, _ao_across; // the occlusion's result is in _ao_raw (the blur ends back in it)
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
    u32 _taa_read = 0;               // which of the two holds the last frame
    u32 _taa_frame = 0;              // frames drawn with it, for the jitter
    bool _taa_valid = false;         // the history holds a picture to blend
    bool _taa_reset_asked = false;   // View3D::reset_history()
    bool _taa_ortho = false;         // the camera's kind when it was made
    std::array<Mat4f, 2> _taa_prior{}; // the scene's and the overlay's view-projections last frame (no jitter)
    GpuCopyHistory _taa_last, _taa_now;
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
        const void* mesh = nullptr;
        u32 copies = 0, per_copy = 0;
        bool operator==(const OccRegion&) const = default;
    };
    std::array<std::shared_ptr<GpuBuffer>, 2> _occ_history;
    u32 _occ_read = 0;
    u32 _occ_words = 0; // clusters the history covers
    std::vector<OccRegion> _occ_layout;
    bool _occ_reset = true; // View3D::reset_history(), a resize: the history is dropped
    // Renders since each feature's targets were last used; at GpuRenderer3D::feature_release_frames (120) they are given back
    // (age_targets). Zero while the feature runs, so a feature toggled every other render never lets go.
    u32 _idle_ao = 0, _idle_contact = 0, _idle_depth = 0, _idle_normals = 0, _idle_taa = 0, _idle_occlusion = 0;
};

class GpuRenderer3D final : public IRenderer3DBackend {
public:
    GpuRenderer3D(GpuRenderer2DBackend& owner, GpuDevice& device);
    ~GpuRenderer3D() override;

    // Whether `device` can draw 3D views (it has a depth format).
    static bool supported(SDL_GPUDevice* device);

    Renderer3DCapabilities capabilities() const override { return _capabilities; }
    std::shared_ptr<IMeshBackend> create_mesh(const MeshData& data, const MeshLodChain* chain, std::shared_ptr<const MeshClusters> clusters) override;
    std::shared_ptr<IEnvironmentBackend> create_environment(Vec2i size, std::span<const f32> rgb) override;
    std::shared_ptr<IMaterialShaderBackend> create_material_shader(std::span<const u8> spirv) override;
    std::shared_ptr<IView3DBackend> create_view(Vec2i size, const View3DOptions& options) override;
    bool resize_view(IView3DBackend& view, Vec2i size) override;
    bool render(IView3DBackend& view, const Scene3D& scene, Render3DStats& stats) override;
    PickResult pick(IView3DBackend& view, Vec2i pixel) override;
    bool warm_up(IView3DBackend& view, const Scene3D& scene) override;

    // The format of views' depth buffers: D32_FLOAT, else D24_UNORM, else
    // D16_UNORM, as the device has them.
    SDL_GPUTextureFormat depth_format() const { return _depth_format; }

private:
    // In the order a scene's draws are made (shadow, pick and the occlusion's pre-pass are passes of their own).
    enum class Pass : u8 { Opaque, Wire, Alpha, Additive, Shadow, Pick, Prepass, Occluder };
    // (Re)makes `view`'s targets for its size, samples and HDR, in the colour
    // format the 2D backend draws in now.
    bool make_targets(GpuView3D& view);
    // `mask`: for a Blend3D::Mask material, in the shadow, pick and pre-passes
    // (the others discard in mesh.frag itself).
    // `velocity`: the main pass of a frame with temporal anti-aliasing and no pre-pass: mesh_velocity's shaders and a
    // second colour target, written by opaque meshes and masked for the rest.
    SDL_GPUGraphicsPipeline* pipeline(SDL_GPUTextureFormat format, u32 samples, bool cull, Pass pass, bool mask = false,
                                         SDL_GPUShader* custom = nullptr, bool velocity = false) const;
    SDL_GPUGraphicsPipeline* fullscreen(SDL_GPUShader* fragment, SDL_GPUTextureFormat format) const;
    // The pre-pass with velocity, into a target of the two-float normal's size
    // and four floats, over a depth of `depth`.
    SDL_GPUGraphicsPipeline* velocity_pipeline(SDL_GPUTextureFormat depth, bool cull, bool mask) const;
    u32 usable_samples(u32 wanted, SDL_GPUTextureFormat format) const;
    // The shadow map at `resolution` with up to `layers` layers (one a cascade),
    // made or remade: how many it has (fewer if the device could not make
    // them all, said once in the log), 0 without a map.
    u32 ensure_shadow_map(u32 resolution, u32 layers);
    // Records the compute passes that make `environment`'s filtered copies.
    void filter_environment(SDL_GPUCommandBuffer* commands, GpuEnvironment3D& environment);
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
    void compute_occlusion(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Camera3D& camera, const Mat4f& projection,
                           const AmbientOcclusion3D& ao, Render3DStats& stats, bool blur_too = true);
    // The blur of both, in one dispatch a direction (the two results at one size).
    void blur_screen_space_both(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Mat4f& projection, Render3DStats& stats);
    // Records the compute passes that march the view's pre-pass toward the sun
    // (`sun`: toward it in world space) and blur the result.
    void compute_contact_shadows(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Camera3D& camera, const Mat4f& projection,
                                 const ContactShadows3D& contact, const Vec3f& sun, Render3DStats& stats, bool blur_too = true);
    // The view's passes after the picture is drawn: bloom (HDR), then out to what 2D draws.
    // `source` is the picture (the view's float scene, or the temporal resolve's);
    // `plain` is an 8-bit view's resolved picture (temporal anti-aliasing), which only needs encoding.
    void finish_view(SDL_GPUCommandBuffer* commands, GpuView3D& view, const GpuTexture& source, bool plain);
    // Temporal anti-aliasing's targets for the view, made or kept; false if they can't be.
    bool ensure_taa_targets(GpuView3D& view, bool sharpen);
    void release_taa(GpuView3D& view);
    // Reads the KIN_* switches (they are read on each render and warm_up, so that tests can change them).
    void read_switches();
    // Occlusion culling's depth pyramid for a depth of w x h (the levels' sizes and count come back), and the depth it is built from when no
    // pre-pass makes one.
    bool ensure_pyramid(GpuView3D& view, u32 w, u32 h, std::array<std::pair<u32, u32>, GpuView3D::max_hiz_levels>& size, u32& levels);
    void ensure_occluder_depth(GpuView3D& view);
    // A frame without temporal anti-aliasing: the history is no good any more (a later frame starts afresh, as after release_taa),
    // but the targets stay until age_targets lets them go.
    void suspend_taa(GpuView3D& view);
    u64 feature_bytes(const GpuView3D& view) const;
    u32 _release_after = 120; // KIN_RELEASE_FRAMES=n changes it (read each render: tests); 0 never
    u32 _idle_occlusion_renders = 0; // renders since any view used occlusion culling: the renderer's second gathered buffer goes at _release_after
    // Where each copy of `_refs` was last frame, into _taa_prev (a matrix a copy, flat as _visible is) and the
    // view's record for the next.
    void match_copies(GpuView3D& view, bool history_usable, JobSystem* jobs);
    // Resolves `input` into the history and returns the picture to show.
    const GpuTexture* resolve_taa(SDL_GPUCommandBuffer* commands, GpuView3D& view, const GpuTexture& input, const TemporalAA3D& taa,
                                  bool reset, Vec2f jitter, Render3DStats& stats);

    // render()'s stages, in the order they run, over a FrameCtx (defined in the .cpp)
    // that holds what they share. Each records into the frame's command buffer; any
    // pass in the middle of the frame is followed by resume_2d().
    struct FrameCtx;
    void setup_frame(FrameCtx& ctx);          // cameras, temporal anti-aliasing and its jitter
    void fit_sun_shadow(FrameCtx& ctx);       // the shadow map and its cascades or box
    void cull_and_upload(FrameCtx& ctx);      // what is seen, and the instance buffers
    void order_draws(FrameCtx& ctx);
    void choose_point_lights(FrameCtx& ctx);
    void begin_commands(FrameCtx& ctx);       // flushes 2D; environment and empty-shadow set-up
    void shadow_pass(FrameCtx& ctx);
    // End of a render: counts the renders each feature's targets went unused and releases those unused for `_release_after`;
    // fills Render3DStats::feature_bytes.
    void age_targets(FrameCtx& ctx);
    void prepass(FrameCtx& ctx);              // depth and normals (or velocity), then the occlusion and contact shadows
    void draw_velocity(FrameCtx& ctx, SDL_GPURenderPass* pass, u32 which, SDL_GPUTextureFormat depth_format);
    void overlay_velocity(FrameCtx& ctx);
    bool main_pass(FrameCtx& ctx);            // false: the pass could not begin (2D is resumed already)
    void resolve_history(FrameCtx& ctx);      // the temporal resolve
    void pick_pass(FrameCtx& ctx);
    void push_frame_uniforms(const FrameCtx& ctx, u32 s);
    void resume_2d();
    // `buffer` as a storage buffer of at least `bytes` (grown to twice its size, and not under `minimum`).
    void ensure_buffer(std::shared_ptr<GpuBuffer>& buffer, u32 bytes, u32 minimum, SDL_GPUBufferUsageFlags usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
    static Pass pass_of(const Material3D& material);
    // A material's texture as a binding: its own, or a white one without.
    SDL_GPUTextureSamplerBinding texture_binding(const Texture& texture, Wrap3D wrap_u, Wrap3D wrap_v);
    void bind_cutout(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Material3D& material);

    GpuRenderer2DBackend* _owner = nullptr;
    GpuDevice* _device = nullptr;
    SDL_GPUTextureFormat _depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTextureFormat _shadow_format = SDL_GPU_TEXTUREFORMAT_INVALID; // depth that can be sampled
    Renderer3DCapabilities _capabilities;
    GpuShader _vertex_shader;     // mesh.vert
    GpuShader _fragment_shader;   // mesh.frag
    GpuShader _shadow_shader;     // shadow.frag
    GpuShader _shadow_vertex_shader; // shadow.vert
    GpuShader _fullscreen_shader; // fullscreen.vert
    GpuShader _bloom_shader;      // bloom.frag
    GpuShader _output_shader;     // view_output.frag
    GpuShader _pick_shader;       // pick.frag
    GpuShader _shadow_mask_shader;        // shadow_mask.frag
    GpuShader _shadow_mask_vertex_shader; // shadow_mask.vert
    GpuShader _pick_mask_shader;          // pick_mask.frag
    GpuShader _prepass_vertex_shader;     // prepass.vert
    GpuShader _prepass_shader;            // prepass.frag
    GpuShader _prepass_mask_shader;       // prepass_mask.frag
    GpuShader _prepass_velocity_vertex_shader; // prepass_velocity.vert
    GpuShader _prepass_velocity_shader;        // prepass_velocity.frag
    GpuShader _prepass_velocity_mask_shader;   // prepass_velocity_mask.frag
    GpuShader _mesh_velocity_vertex_shader;    // mesh_velocity.vert
    GpuShader _mesh_velocity_shader;           // mesh_velocity.frag
    // Temporal anti-aliasing needs the pre-pass and four-float colour targets,
    // and a float storage texture; else it is off (said once, when asked for).
    bool _taa_available = false;
    // The main pass writes the velocity when temporal anti-aliasing has no other use for a pre-pass; KIN_TAA_PREPASS=1
    // brings the pre-pass back (to compare, and to test it).
    bool _main_velocity_allowed = true;
    bool _taa_warned = false;
    bool _taa_prepass_warned = false;
    SDL_GPUComputePipeline* _taa_pipeline = nullptr; // taa.comp
    std::shared_ptr<GpuBuffer> _previous;            // last frame's matrix of each copy in _instances; grows
    std::vector<Mat4f> _taa_prev;                    // render()'s scratch: a matrix per copy of the scene
    std::vector<Mat4f> _taa_ordered;                 // and per copy in _instances
    struct CopyRef {
        u32 slot = 0; // a draw's first copy among its group's in the view's record,
        u32 past = 0; // and where its place was last frame (~0: no same-sized group then)
    };
    std::vector<CopyRef> _taa_copy_refs;             // match_copies' scratch, kept between frames:
    std::vector<std::pair<u64, u32>> _taa_keyed;     // draws by group key,
    std::vector<u32> _taa_group_of;                  // their groups,
    std::vector<u8> _taa_slow;                       // and a byte a copy: not settled by the fast path
    // Ambient occlusion needs these formats and two compute pipelines; where
    // the device lacks one it is off (a line in the log the first time it is asked for).
    SDL_GPUTextureFormat _ao_depth_format = SDL_GPU_TEXTUREFORMAT_INVALID; // depth that can be sampled
    bool _ao_available = false;
    bool _ao_warned = false;
    bool _contact_warned = false;
    SDL_GPUComputePipeline* _contact_pipeline = nullptr; // contact.comp
    SDL_GPUComputePipeline* _gtao_pipeline = nullptr;    // gtao.comp
    SDL_GPUComputePipeline* _ao_blur_pipeline = nullptr; // ao_blur.comp
    SDL_GPUComputePipeline* _ao_blur_both_pipeline = nullptr; // ao_blur_both.comp: both features' blur in one dispatch (optional)
    // The shadow map's cache (Lights3D::cache_shadows): what the map was last drawn from, so that the pass is skipped when this
    // render's would come out the same (shadow_unchanged / shadow_drawn). `key` is every number the pass is a function of except
    // the casters' copies (camera, sun, area, cascade fit, frusta, levels of detail, each caster's mesh and material);
    // `copies` is a snapshot of the casters' MeshInstances, taken only once two renders in a row agree on the key and a sample
    // of the copies (a scene that moves never copies anything). `valid`: `copies` is exactly what the map holds.
    struct ShadowCache {
        std::vector<u8> key, prev_key, scratch;
        u64 sample = 0, prev_sample = 0, now = 0;
        std::vector<u8> copies;
        bool valid = false;
        bool eligible = false;  // this render's pass can be cached on
        u32 backoff = 0;        // renders to wait before copying again after a snapshot that did not hold
    } _shadow_cache;
    u64 _shadow_generation = 0; // changes whenever _shadow_map is made again
    u64 _next_mesh_serial = 1;
    bool _shadow_cache_allowed = true; // KIN_SHADOW_CACHE=0 turns it off; read each render
    bool shadow_unchanged(FrameCtx& ctx);
    void shadow_drawn(FrameCtx& ctx, bool complete);
    bool _merge_blurs = true; // KIN_BLUR_SEPARATE=1 turns it off (to compare, and to test the separate path); read each render
    SDL_GPUSampler* _point_sampler = nullptr;            // texel by texel, for compute
    SDL_GPUSampler* _compare_sampler = nullptr; // the shadow map's
    SDL_GPUSampler* _environment_sampler = nullptr; // wraps round, clamps at the poles, mipmapped
    SDL_GPUComputePipeline* _filter_pipeline = nullptr; // env_filter.comp
    GpuTexture _shadow_map;
    u32 _shadow_size = 0;
    u32 _shadow_layers = 0;
    u32 _shadow_layers_limit = max_shadow_cascades; // lowered when the device would not make more
    GpuTexture _no_shadow; // 1 x 1, all far: every comparison lit
    bool _no_shadow_cleared = false;
    // Samplers for materials' textures by filter and wrap (made on first use).
    SDL_GPUSampler* sampler_for(const GpuTextureBackend& texture, Wrap3D u, Wrap3D v);
    std::array<SDL_GPUSampler*, 27> _samplers{};
    std::vector<std::shared_ptr<GpuMaterialShader3D>> _material_shaders;
    std::shared_ptr<GpuBuffer> _instances; // grows
    std::shared_ptr<GpuBuffer> _caster_lists; // the cascades' lists of copies (indices into _instances); grows
    std::array<f32, 256> _srgb_to_linear{};

    // render()'s scratch, kept between frames.
    struct DrawRef {
        const Scene3D::Draw* draw = nullptr;
        const MeshInstance* instances = nullptr; // the draw's first
        const GpuMesh3D* mesh = nullptr;
        u32 scene = 0;                       // 0 the scene, 1 its overlay
        const GpuFrustum* frustum = nullptr; // null: not culled
        bool caster = false;                 // drawn into the shadow map
        Vec3f center{};                      // the mesh's bounding sphere
        f32 radius = 0.0f;
        u32 out_first = 0; // where its copies go in the instance buffer:
        u32 out_count = 0; // those the camera sees,
        u32 out_extra = 0; // then those only the sun sees (casting into view)
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
        u32 hist_first = 0;               // with Lod3D::occlusion: where its copies' clusters start in the view's history
    };
    struct Piece {
        u32 ref = 0;
        u32 begin = 0, end = 0; // its copies, within the draw
        u32 flat = 0;           // the first's place among all the frame's copies
        // By level of detail (index 0 alone without): the copies seen, and those only the sun sees; where they go.
        std::array<u32, max_mesh_levels> seen{}, extra{};
        std::array<u32, max_mesh_levels> out{}, out_extra{};
        // Copies casting into each cascade, by level, and where they go in its list.
        std::array<std::array<u32, max_mesh_levels>, max_shadow_cascades> cascade{}, cascade_out{};
    };
    // The cut through each cluster mesh's hierarchy, per copy (render3d/mesh_clusters.hpp): the clusters chosen, as
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
        std::vector<SelectedCluster> picks;
        std::array<ClusterRun, cluster_views> runs{};
        std::array<u8, cluster_views> same_as{}; // 1 + the view this one repeats, 0 if none
    };
    std::vector<ClusterJob> _cluster_jobs;
    // The camera's view of `ref`'s clustered copies, from the passes `phase_parts` says (all, but in the pre-pass of Lod3D::occlusion);
    // `stats`, when given, counts them as the pre-pass's.
    template <typename Binder, typename Uniforms>
    void draw_cluster_copies(const DrawRef& ref, SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, Binder& binder, Uniforms& u,
                             Render3DStats* stats) const;
    // Lod3D::occlusion splits the pre-pass in two: `First` draws the opaque meshes and the first pass's clusters, from which the
    // pyramid is built; `Second` the second pass's. A mesh with a shader of its own (which may discard what the pyramid
    // would count as covered) is no occluder: it waits for `Second`, all of its clusters. `All` is every other draw.
    enum class Phase : u8 { All, First, Second };
    Phase _phase = Phase::All;
    bool phase_skips(const DrawRef& ref) const;
    u32 phase_parts(const DrawRef& ref) const;
    std::vector<std::array<ClusterRun, cluster_views>> _cluster_runs; // by instance slot (the clustered copies')
    std::shared_ptr<GpuBuffer> _cluster_indices;                      // the frame's compacted indices (an index buffer); grows
    // Cuts every clustered copy this render sees and uploads the compacted indices (after cull_and_upload has placed the copies).
    // With the GPU cut (ctx.gpu_clusters) it only lists the copies and uploads the list; cluster_pass_gpu records the compute.
    void cut_clusters(FrameCtx& ctx, JobSystem* jobs);
    void cut_clusters_gpu(FrameCtx& ctx, std::size_t count);
    // The GPU cut's compute passes (before the passes that draw): cull, scan, arguments, gather.
    // `phase` 0: the whole cut; with occlusion 1: the first pass (set 0), 2: the second (set 1, after the pyramid is built).
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
    // Draws `ref`'s copies in view `view` (0 the camera's, 1 + k the cascade k's, 1 the single map) with indirect draws.
    template <typename Uniforms>
    void draw_cluster_indirect(const DrawRef& ref, SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, Uniforms& u, u32 view, u32 set = 0) const;
    // Waits for the GPU and reads the cut's statistics (Lod3D::stats_readback), or, for `picks`, which clusters it chose.
    void read_cluster_stats(FrameCtx& ctx);
    bool read_cluster_picks(IView3DBackend& view, std::vector<ClusterPick>& out) override;
    bool read_cluster_draws(IView3DBackend& view, std::vector<ClusterDraw>& out) override;
    // The GPU cut. `_gc_*` are its compute pipelines and buffers; `_gc` what this render set up (valid while `active`).
    bool _gpu_clusters = false; // the device can (capabilities)
    bool _occlusion_available = false; // and can build a depth pyramid (Renderer3DCapabilities::occlusion_culling)
    bool _occlusion_warned = false;
    SDL_GPUComputePipeline* _gc_cull_occ = nullptr;      // cluster_cull_occ.comp
    SDL_GPUComputePipeline* _hiz_depth_pipeline = nullptr;  // hiz_depth.comp
    SDL_GPUComputePipeline* _hiz_reduce_pipeline = nullptr; // hiz_reduce.comp
    std::array<GpuTexture, 4> _hiz_spare;                // 1 x 1, written by no one: bound where a pass has fewer levels to make
    SDL_GPUComputePipeline* _gc_cull = nullptr;    // cluster_cull.comp
    SDL_GPUComputePipeline* _gc_scan = nullptr;    // cluster_scan.comp
    SDL_GPUComputePipeline* _gc_args = nullptr;    // cluster_args.comp
    SDL_GPUComputePipeline* _gc_gather = nullptr;  // cluster_gather.comp
    std::shared_ptr<GpuBuffer> _gc_jobs;    // a copy each: slot, scene, bits, scale, first cluster
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
    // Room in a chunk for a mesh of `indices` indices and `clusters` clusters (a chunk made if none has): the chunk, and
    // where its ranges start.
    std::shared_ptr<GpuClusterChunk> allocate_cluster_range(u32 indices, u32 clusters, u32& index_first, u32& table_first);
    struct GcFrame {
        bool active = false;
        u32 jobs = 0, views = 0, clusters = 0; // copies, passes' views, clusters in all copies
        u32 room = 0;                          // triangles the gathered buffer holds
        u32 table_bytes = 0;
        std::vector<GcDispatch> dispatches;
        std::vector<GcDispatch> dispatches_second; // with occlusion: the same copies' queues for the second pass (one view)
        bool occlusion = false;                    // both passes are recorded
        u32 room_second = 0;
        std::vector<u32> job_history;
        std::vector<u8> job_data;              // the job table, uploaded
        std::vector<u32> bases;                // each copy's first cluster among all
        std::vector<u8> zeros;
        // The cull's uniform block (cluster_cull.comp's Params), but for the range it is dispatched over.
        std::vector<f32> params;
        bool overflow_warned = false;
    } _gc;
    bool _gc_overflow_warned = false;
    std::vector<DrawRef> _refs;
    std::vector<Piece> _pieces;
    std::vector<u32> _tasks;   // where each task's pieces start
    std::vector<u8> _visible;  // a copy: the bits of camera_bit, sun_bit and cascade_bit(k) (gpu_renderer3d.cpp)
    std::vector<u8> _level_of; // a copy: its level of detail (only filled with Scene3D::lod, for the meshes that have levels)
    std::vector<u32> _caster_list; // the cascades' lists, uploaded
    std::vector<u32> _order;   // draws sorted
    std::vector<std::pair<f32, u32>> _sorted; // an alpha draw's copies, furthest first
};

} // namespace kin::gpu
