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
#include <kin/renderer/render_target.hpp>
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
    // Whether Scene3D::texture_lod_bias moves the mip level here: the device's samplers take a bias (Vulkan's and
    // Direct3D 12's; not Metal's, and WebGL2 has none).
    bool texture_lod_bias = false;
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
    // Whether Scene3D::global_illumination can work: compute that writes 3D textures (half floats, 8-bit and four
    // half-float texels) and reads the meshes' buffers.
    bool global_illumination = false;
    // Whether spot lights, the shadows of lights that are not the sun and the fill light's shadow can work (the light
    // shaders and a depth format that can be sampled; P3b); where not, they are off and a line in the log says so the
    // first time one is asked for.
    bool local_shadows = false;
    // The most point and spot lights a frame can draw (Lights3D::points and ::spots: the strongest and nearest of those
    // that reach the view): the light grid's 256 of each where the light shaders are made (local_shadows), else 16.
    u32 max_point_lights = 16;
    u32 max_spot_lights = 16;
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
    // Standalone opaque/cutout geometry into caller-owned D32F + RG16F targets.
    bool geometry_pass = false;
    bool shared_preparation = false;
    bool surface_pass = false, sun_shadow_pass = false, local_shadow_pass = false;
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

// Single-sample data attachments. Depth is reverse Z (0 background/far, 1 near),
// with the scene camera's exact, unjittered projection. Normals are world-space
// unit vectors encoded octahedrally in signed RG16F [-1,1]; depth distinguishes
// background from a valid normal. No material normal maps or custom displacement.
struct GeometryTargets3D {
    Texture depth;   // TextureFormat::Depth32Float
    Texture normals; // TextureFormat::Rg16Float; same extent and device as depth
};

// A directional depth map and its projection metadata. The texture is mutable:
// keep matching projection/depth versions, or use distinct persistent targets.
struct SunShadow3D {
    Texture depth; // sampled D32F 2D array, 1..max_shadow_cascades layers
    ShadowCascades3D projection;
};
enum class LocalShadowKind3D { Spot, Point };
// Explicit selection, independent of the stock shadow-slot allocator. A point
// occupies six consecutive layers (+x,-x,+y,-y,+z,-z); a spot occupies one.
struct LocalShadowRequest3D {
    LocalShadowKind3D kind = LocalShadowKind3D::Spot;
    u32 light = 0; // index in the source scene's spots/points
    u32 first_layer = 0;
    u32 resolution = 0; // 0: full array side; otherwise a viewport at its top left
};
struct LocalShadowMap3D {
    LocalShadowRequest3D source;
    std::array<LocalShadowView3D, cube_faces> projection{};
    f32 bias = 1, normal_offset = 1.5f, softness = 1, fade_distance = 10;
};
// Mutable bindings, like SunShadow3D. Keep map/projection/light-index versions
// aligned, and use distinct persistent targets for historical snapshots.
struct LocalShadows3D {
    Texture depth; // sampled square D32F array, up to max_local_shadow_layers
    std::vector<LocalShadowMap3D> maps;
};
struct SurfaceTargets3D {
    RenderTarget colour; // current Renderer2D working colour format
    Texture depth;       // sampled D32F 2D, same extent; stores reverse-Z depth
};
struct SurfaceOptions3D {
    Color background = colors::transparent;
    const SunShadow3D* sun_shadow = nullptr;       // null: no sun shadow; never generates maps implicitly
    const LocalShadows3D* local_shadows = nullptr; // null: unshadowed point/spot lights
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

// The GI scene a render built (Scene3D::global_illumination; Renderer3D::read_gi_scene), for tests and tools:
// `size` voxels of `cell` units, voxel (0, 0, 0)'s low corner at `origin`; the arrays a value a voxel, x fastest, then
// y, then z.
struct GiScene3D {
    Vec3f origin{};
    f32 cell = 0.0f;
    std::array<u32, 3> size{};
    // From the voxel's centre to the nearest centre of a voxel a surface is in, in voxels (0 in one; large where the
    // scene has no surface at all).
    std::vector<f32> distance;
    // The surface's colour (linear, 8 bits a channel, red in the lowest byte); alpha 255 where a surface is in the
    // voxel, 0 elsewhere (and then the rest is 0 too).
    std::vector<u32> albedo;
    // Its normal (the face of the triangle that owns the voxel, on the side its vertices' normals are), each component
    // as (n + 1) / 2 in a byte; alpha 255 for a double-sided material, 128 for a single-sided one.
    std::vector<u32> normal;
    // Its emission (linear RGB, three floats a voxel).
    std::vector<f32> emission;
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

// Backend-owned frame preparation. Source lifetime/edits and frame boundaries
// invalidate it. valid() must be safe after source/renderer destruction.
class IPreparedSceneBackend3D {
public:
    virtual ~IPreparedSceneBackend3D() = default;
    virtual bool valid() const = 0;
};
struct PrepareOptions3D {
    u64 max_cpu_bytes = 32ull * 1024 * 1024; // payload admission limit per handle
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
    virtual bool render_geometry(const GeometryTargets3D&, const Scene3D&, RenderDebugStats3D&) { return false; }
    virtual std::shared_ptr<IPreparedSceneBackend3D> prepare(const Scene3D&, const PrepareOptions3D&,
                                                             RenderDebugStats3D&) {
        return {};
    }
    virtual bool render_prepared(IViewBackend3D&, IPreparedSceneBackend3D&, RenderDebugStats3D&) { return false; }
    virtual bool render_geometry_prepared(const GeometryTargets3D&, IPreparedSceneBackend3D&, RenderDebugStats3D&) {
        return false;
    }
    virtual Result3D<SunShadow3D> render_sun_shadow(const Texture&, IPreparedSceneBackend3D&, Vec2i,
                                                    RenderDebugStats3D&) {
        return {std::nullopt, {"sun shadow operation unavailable"}};
    }
    virtual Result3D<LocalShadows3D> render_local_shadows(const Texture&, IPreparedSceneBackend3D&,
                                                          std::span<const LocalShadowRequest3D>, Vec2i,
                                                          RenderDebugStats3D&) {
        return {std::nullopt, {"local shadow operation unavailable"}};
    }
    virtual bool render_surface(const SurfaceTargets3D&, IPreparedSceneBackend3D&, const SurfaceOptions3D&,
                                RenderDebugStats3D&) {
        return false;
    }
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
    // The GI scene the view holds (built by the last render with Scene3D::global_illumination), for tests and tools;
    // waits for the GPU. False when it has none.
    virtual bool read_gi_scene(IViewBackend3D&, GiScene3D&) { return false; }
};

} // namespace kin
