#pragma once
// The SDL_GPU backend's 3D part (IRenderer3DBackend), made the first time a
// Renderer3D asks for it. It shares the 2D backend's device, pipeline cache
// and frame: a view is rendered in the frame's command buffer between the 2D
// draws around it. Experimental: see docs/3d_plan.md.
#include <kin/render3d/backend3d.hpp>

#include "gpu_buffer.hpp"
#include "gpu_shader.hpp"
#include "gpu_texture.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace kin {
class GpuRenderer2DBackend;
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
    SDL_GPUBuffer* indices = nullptr;
    u32 index_count = 0;
};

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

class GpuView3D final : public IView3DBackend {
public:
    Texture texture() const override;
    Vec2i size() const override { return _size; }
    u32 samples() const override { return _samples; }

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
    GpuTexture _ao_depth, _ao_normals, _ao_raw, _ao_across, _ao;
    // With Scene3D::contact_shadows, the same way (the pre-pass's depth and
    // normals are shared): the march, blurred across, blurred down; R8, the view's size.
    GpuTexture _contact_raw, _contact_across, _contact;
    std::shared_ptr<GpuTextureBackend> _resolved; // what 2D draws sample
};

class GpuRenderer3D final : public IRenderer3DBackend {
public:
    GpuRenderer3D(GpuRenderer2DBackend& owner, GpuDevice& device);
    ~GpuRenderer3D() override;

    // Whether `device` can draw 3D views (it has a depth format).
    static bool supported(SDL_GPUDevice* device);

    Renderer3DCapabilities capabilities() const override { return _capabilities; }
    std::shared_ptr<IMeshBackend> create_mesh(const MeshData& data) override;
    std::shared_ptr<IEnvironmentBackend> create_environment(Vec2i size, std::span<const f32> rgb) override;
    std::shared_ptr<IMaterialShaderBackend> create_material_shader(std::span<const u8> spirv) override;
    std::shared_ptr<IView3DBackend> create_view(Vec2i size, const View3DOptions& options) override;
    bool resize_view(IView3DBackend& view, Vec2i size) override;
    bool render(IView3DBackend& view, const Scene3D& scene, Render3DStats& stats) override;
    PickResult pick(IView3DBackend& view, Vec2i pixel) override;

    // The format of views' depth buffers: D32_FLOAT, else D24_UNORM, else
    // D16_UNORM, as the device has them.
    SDL_GPUTextureFormat depth_format() const { return _depth_format; }

private:
    // In the order a scene's draws are made (shadow and pick are passes of their own).
    enum class Pass : u8 { Opaque, Wire, Alpha, Additive, Shadow, Pick, Prepass };
    // (Re)makes `view`'s targets for its size, samples and HDR, in the colour
    // format the 2D backend draws in now.
    bool make_targets(GpuView3D& view);
    // `mask`: for a Blend3D::Mask material, in the shadow, pick and pre-passes
    // (the others discard in mesh.frag itself).
    SDL_GPUGraphicsPipeline* pipeline(SDL_GPUTextureFormat format, u32 samples, bool cull, Pass pass, bool mask = false,
                                         SDL_GPUShader* custom = nullptr) const;
    SDL_GPUGraphicsPipeline* fullscreen(SDL_GPUShader* fragment, SDL_GPUTextureFormat format) const;
    u32 usable_samples(u32 wanted, SDL_GPUTextureFormat format) const;
    // The shadow map at `resolution` with up to `layers` layers (one a cascade),
    // made or remade: how many it has (fewer if the device could not make
    // them all, said once in the log), 0 without a map.
    u32 ensure_shadow_map(u32 resolution, u32 layers);
    // Records the compute passes that make `environment`'s filtered copies.
    void filter_environment(SDL_GPUCommandBuffer* commands, GpuEnvironment3D& environment);
    // Makes `view`'s ambient occlusion targets for its size (half of it when
    // `half`), or reports they can't be made.
    // The pre-pass's depth and normals always; the occlusion's three targets
    // with `want_ao`, the contact shadows' with `want_contact`.
    bool ensure_ao_targets(GpuView3D& view, bool half, bool want_ao, bool want_contact);
    // Records the compute passes that turn the view's pre-pass into its occlusion.
    void compute_occlusion(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Camera3D& camera, f32 aspect,
                           const AmbientOcclusion3D& ao, Render3DStats& stats);
    // Records the compute passes that march the view's pre-pass toward the sun
    // (`sun`: toward it in world space) and blur the result.
    void compute_contact_shadows(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Camera3D& camera, f32 aspect,
                                 const ContactShadows3D& contact, const Vec3f& sun, Render3DStats& stats);
    // The view's HDR passes: bloom, then out to what 2D draws.
    void finish_hdr(SDL_GPUCommandBuffer* commands, GpuView3D& view);

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
    // Ambient occlusion needs these formats and two compute pipelines; where
    // the device lacks one it is off (a line in the log the first time it is asked for).
    SDL_GPUTextureFormat _ao_depth_format = SDL_GPU_TEXTUREFORMAT_INVALID; // depth that can be sampled
    bool _ao_available = false;
    bool _ao_warned = false;
    bool _contact_warned = false;
    SDL_GPUComputePipeline* _contact_pipeline = nullptr; // contact.comp
    SDL_GPUComputePipeline* _gtao_pipeline = nullptr;    // gtao.comp
    SDL_GPUComputePipeline* _ao_blur_pipeline = nullptr; // ao_blur.comp
    SDL_GPUSampler* _point_sampler = nullptr;            // texel by texel, for compute
    SDL_GPUSampler* _compare_sampler = nullptr; // the shadow map's
    SDL_GPUSampler* _environment_sampler = nullptr; // wraps round, clamps at the poles, mipmapped
    SDL_GPUComputePipeline* _filter_pipeline = nullptr; // env_filter.comp
    GpuTexture _shadow_map;
    u32 _shadow_size = 0;
    u32 _shadow_layers = 0;
    u32 _shadow_layers_limit = 4; // lowered when the device would not make more
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
        std::array<u32, 4> cascade_first{}; // with cascades: each one's list of copies,
        std::array<u32, 4> cascade_count{}; // in _caster_list
    };
    struct Piece {
        u32 ref = 0;
        u32 begin = 0, end = 0; // its copies, within the draw
        u32 flat = 0;           // the first's place among all the frame's copies
        u32 seen = 0, extra = 0;
        u32 out = 0, out_extra = 0;
        std::array<u32, 4> cascade{};     // copies casting into each cascade
        std::array<u32, 4> cascade_out{}; // where they go in its list
    };
    std::vector<DrawRef> _refs;
    std::vector<Piece> _pieces;
    std::vector<u32> _tasks;   // where each task's pieces start
    std::vector<u8> _visible;  // a copy: bit 0 the camera sees it, bit 1 only the sun does, bit 2 + k it casts into cascade k
    std::vector<u32> _caster_list; // the cascades' lists, uploaded
    std::vector<u32> _order;   // draws sorted
    std::vector<std::pair<f32, u32>> _sorted; // an alpha draw's copies, furthest first
};

} // namespace kin::gpu
