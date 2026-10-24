#pragma once

#include <kin/render3d/backend3d.hpp>
#include <memory>

namespace kin {
class WebGL2Renderer2DBackend;

// Core WebGL2 mesh path: RGBA8/depth views and vertex-attribute instancing.
// No SDL_GPU resources or storage/compute shaders enter this implementation.
class WebGLRenderer3D final : public IRendererBackend3D {
public:
    explicit WebGLRenderer3D(WebGL2Renderer2DBackend& renderer);
    ~WebGLRenderer3D() override;
    RendererCapabilities3D capabilities() const override;
    std::shared_ptr<IMeshBackend3D> create_mesh(const MeshData3D& data, const MeshLodChain3D* chain,
                                                std::shared_ptr<const MeshClusters3D> clusters) override;
    std::shared_ptr<IEnvironmentBackend3D> create_environment(Vec2i, std::span<const f32>) override { return {}; }
    std::shared_ptr<IMaterialShaderBackend3D> create_material_shader(std::span<const u8>) override { return {}; }
    std::shared_ptr<IViewBackend3D> create_view(Vec2i size, const ViewOptions3D& options) override;
    bool resize_view(IViewBackend3D& view, Vec2i size) override;
    bool render(IViewBackend3D& view, const Scene3D& scene, RenderDebugStats3D& stats) override;
    Result3D<PickResult3D> pick(IViewBackend3D&, Vec2i) override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace kin
