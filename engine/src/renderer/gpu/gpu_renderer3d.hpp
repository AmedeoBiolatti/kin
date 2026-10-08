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

class GpuView3D final : public IView3DBackend {
public:
    Texture texture() const override;
    Vec2i size() const override { return _size; }
    u32 samples() const override { return _samples; }

private:
    friend class GpuRenderer3D;
    Vec2i _size{};
    u32 _wanted_samples = 4; // as asked (View3DOptions::samples)
    u32 _samples = 1;        // as made: what the device has for the format
    Color _background = colors::transparent;
    SDL_GPUTextureFormat _format = SDL_GPU_TEXTUREFORMAT_INVALID; // the colour targets'
    GpuTexture _colour; // multisampled; empty with one sample (drawn into the resolved one)
    GpuTexture _depth;
    std::shared_ptr<GpuTextureBackend> _resolved; // what 2D draws sample
};

class GpuRenderer3D final : public IRenderer3DBackend {
public:
    GpuRenderer3D(GpuRenderer2DBackend& owner, GpuDevice& device);

    // Whether `device` can draw 3D views (it has a depth format).
    static bool supported(SDL_GPUDevice* device);

    Renderer3DCapabilities capabilities() const override { return _capabilities; }
    std::shared_ptr<IMeshBackend> create_mesh(const MeshData& data) override;
    std::shared_ptr<IView3DBackend> create_view(Vec2i size, const View3DOptions& options) override;
    bool resize_view(IView3DBackend& view, Vec2i size) override;
    bool render(IView3DBackend& view, const Scene3D& scene, Render3DStats& stats) override;

    // The format of views' depth buffers: D32_FLOAT, else D24_UNORM, else
    // D16_UNORM, as the device has them.
    SDL_GPUTextureFormat depth_format() const { return _depth_format; }

private:
    // (Re)makes `view`'s targets for its size and samples, in the colour
    // format the 2D backend draws in now.
    bool make_targets(GpuView3D& view);
    // The mesh pipeline for a pass of `format` and `samples`, culling back
    // faces or not.
    SDL_GPUGraphicsPipeline* pipeline(SDL_GPUTextureFormat format, u32 samples, bool cull) const;
    u32 usable_samples(u32 wanted, SDL_GPUTextureFormat format) const;

    GpuRenderer2DBackend* _owner = nullptr;
    GpuDevice* _device = nullptr;
    SDL_GPUTextureFormat _depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    Renderer3DCapabilities _capabilities;
    GpuShader _vertex_shader;   // mesh.vert
    GpuShader _fragment_shader; // mesh.frag
    std::shared_ptr<GpuBuffer> _instances; // grows; kept by the frame while it draws from it
    std::array<f32, 256> _srgb_to_linear{};

    // render()'s scratch, kept between frames.
    struct DrawRef {
        const Scene3D::Draw* draw = nullptr;
        const MeshInstance* instances = nullptr; // the draw's first
        const GpuMesh3D* mesh = nullptr;
        u32 scene = 0;            // 0 the scene, 1 its overlay
        const GpuFrustum* frustum = nullptr; // null: not culled
        Vec3f center{};           // the mesh's bounding sphere
        f32 radius = 0.0f;
        u32 out_first = 0;        // where its visible copies go in the instance buffer
        u32 out_count = 0;
    };
    struct Piece {
        u32 ref = 0;
        u32 begin = 0, end = 0; // its copies, within the draw
        u32 flat = 0;           // the first's place among all the frame's copies
        u32 out = 0;            // where its visible copies go
    };
    std::vector<DrawRef> _refs;
    std::vector<Piece> _pieces;
    std::vector<u32> _tasks;   // where each task's pieces start
    std::vector<u8> _visible;  // a byte a copy
    std::vector<u32> _order;   // draws sorted
};

} // namespace kin::gpu
