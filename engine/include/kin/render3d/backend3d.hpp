#pragma once

// The seam between Renderer3D and a renderer backend that can draw 3D views
// (IRenderer2DBackend::renderer_3d(); SDL_GPU and WebGL2). Experimental: see
// docs/3d_plan.md.

#include <array>
#include <kin/core/types.hpp>
#include <kin/render3d/debug_stats3d.hpp>
#include <kin/render3d/local_lights3d.hpp>
#include <kin/render3d/mesh3d.hpp>
#include <kin/render3d/mesh_clusters3d.hpp>
#include <kin/render3d/mesh_lod3d.hpp>
#include <kin/render3d/scene3d.hpp>
#include <kin/render3d/shadow_cascades3d.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/texture.hpp>
#include <memory>
#include <span>
#include <vector>

namespace kin {

// What 3D views can have on this device.
struct RendererCapabilities3D {
    // The depth buffer views draw with: 32-bit float where the device has it,
    // else 24 or 16-bit fixed point (less precise far away).
    u32 depth_bits = 0;
    bool depth_float = false;
    // The most samples a pixel of a view can take (MSAA; 1: none), with 8-bit
    // colour and with HDR (16-bit float) colour.
    u32 max_samples = 1;
    u32 max_samples_hdr = 1;
    // The most Scene3D::anisotropy can be (1: no anisotropic filtering).
    u32 max_anisotropy = 1;
    // Whether Scene3D::ambient_occlusion, ::contact_shadows and ::taa can work
    // here (the formats and compute they need); where not, they are off and a
    // line in the log says so the first time one is asked for.
    bool ambient_occlusion = false;
    bool contact_shadows = false;
    bool temporal_aa = false;
    // Whether Scene3D::lod's cluster cut can run in compute (Scene3D::debug.gpu_culling): compute with storage buffers
    // read and written, buffers that are index and indirect-argument buffers a compute pass writes. Else the CPU cuts.
    bool gpu_driven_clusters = false;
    // Whether Lod3D::occlusion can work: the GPU cut, plus a sampled depth format and R32F storage textures.
    bool occlusion_culling = false;
    // Whether spot lights, the shadows of lights that are not the sun and the fill light's shadow can work (the light
    // shaders and a depth format that can be sampled; P3b); where not, they are off and a line in the log says so the
    // first time one is asked for.
    bool local_shadows = false;
    // Whether particles and weather can be drawn (P3c: the billboard shaders could be made), and whether soft particles
    // can (the depth-reading variant and the pre-pass's targets); where not, they are not drawn and a line in the log
    // says so once.
    bool particles = false;
    bool soft_particles = false;
    bool particle_shadows = false; // optional particle shadow-map casting and reception
    // Whether Scene3D::draw_skinned can draw (the skinning vertex shaders could be made); else skinned copies are not
    // drawn.
    bool skinning = false;
    // The device is a software rasteriser (Mesa's llvmpipe/lavapipe, SwiftShader, Direct3D's WARP): every picture is
    // right, no timing means anything. Tests compare milliseconds only where this is false.
    bool software = false;
};

struct ViewOptions3D {
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
// camera there (its MeshInstance3D::id) and the point of it the pixel shows.
struct PickResult3D {
    bool hit = false; // false: nothing but background there
    u32 id = 0;
    Vec3f position{};
};

// One cluster the GPU's cut chose, for tests and tools (IRendererBackend3D::read_cluster_picks).
struct ClusterPick3D {
    u32 scene = 0; // 0 the scene, 1 its overlay
    u32 draw = 0;  // index in Scene3D::draws()
    u32 copy = 0;  // the copy within the draw
    u32 view = 0;  // 0 the camera's passes; 1 the shadow map, or 1 + k the cascade k's
    u32 cluster = 0;
};

// One indirect draw of the GPU cut, as the GPU holds it (IRendererBackend3D::read_cluster_draws).
struct ClusterDraw3D {
    u32 view = 0;        // as ClusterPick3D::view
    u32 job = 0;         // the copy's place among those cut this render
    u32 index_count = 0; // 0: nothing is drawn
    u32 first_index = 0;
    u32 first_instance = 0; // the copy's slot in the instance buffer
};

// A view's targets on the backend.
class IViewBackend3D {
public:
    virtual ~IViewBackend3D() = default;
    // The picture, premultiplied, to draw with Renderer2D::draw_texture.
    virtual Texture texture() const = 0;
    virtual Vec2i size() const = 0;
    virtual u32 samples() const = 0;
    // Forgets the temporal anti-aliasing's history, so the next frame starts
    // afresh (a camera cut, a scene change).
    virtual void reset_history() {}
};

class IRendererBackend3D {
public:
    virtual ~IRendererBackend3D() = default;

    virtual RendererCapabilities3D capabilities() const = 0;
    // Null when the data can't be uploaded (validated already). `chain`, when
    // not null, holds the levels of detail to keep with it (level 0 of it is `data`).
    // `clusters`, when not null, is the mesh's cluster hierarchy (MeshOptions3D::clusters), to keep for the cut: `data`
    // is then its welded vertices and its leaves' triangles (MeshClusterBuild3D::vertices,
    // MeshClusters3D::leaf_indices), which the hierarchy does not hold, and `chain` is null.
    virtual std::shared_ptr<IMeshBackend3D> create_mesh(const MeshData3D& data, const MeshLodChain3D* chain,
                                                        std::shared_ptr<const MeshClusters3D> clusters) = 0;
    // An equirectangular picture of `size` pixels, three floats (linear RGB)
    // each, rows top to bottom; its filtered copies are made on the first
    // render that uses it. Null when it can't be made (no compute).
    virtual std::shared_ptr<IEnvironmentBackend3D> create_environment(Vec2i size, std::span<const f32> rgb) = 0;
    // Null when the SPIR-V can't be made into a fragment shader.
    virtual std::shared_ptr<IMaterialShaderBackend3D> create_material_shader(std::span<const u8> spirv) = 0;
    virtual std::shared_ptr<IViewBackend3D> create_view(Vec2i size, const ViewOptions3D& options) = 0;
    virtual bool resize_view(IViewBackend3D& view, Vec2i size) = 0;
    // Records `scene` into `view`, after the 2D draws so far and before those
    // to come.
    virtual bool render(IViewBackend3D& view, const Scene3D& scene, RenderDebugStats3D& stats) = 0;
    // What `pixel` of the view's last render shows; waits for the GPU.
    virtual Result3D<PickResult3D> pick(IViewBackend3D& view, Vec2i pixel) = 0;
    // Makes what rendering `scene` into `view` would make on first use (pipelines, targets), without drawing. False
    // where there is nothing to make.
    virtual bool warm_up(IViewBackend3D& view, const Scene3D& scene) {
        static_cast<void>(view);
        static_cast<void>(scene);
        return false;
    }
    // The clusters the last render's GPU cut chose, in every pass, for tests and tools; waits for the GPU. False when
    // that render cut on the CPU (or not at all).
    virtual bool read_cluster_picks(IViewBackend3D&, std::vector<ClusterPick3D>&) { return false; }
    // The indirect draws of the last render's GPU cut, a (view, copy) each; waits for the GPU.
    virtual bool read_cluster_draws(IViewBackend3D&, std::vector<ClusterDraw3D>&) { return false; }
};

} // namespace kin
