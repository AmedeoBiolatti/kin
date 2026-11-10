#pragma once
#include <kin/render3d/errors3d.hpp>

// Experimental 3D rendering (docs/3d_plan.md): 3D views drawn inside a 2D
// frame, on the SDL_GPU and experimental WebGL2 backends. The API may change
// in any release. Built with KIN_EXPERIMENTAL_3D (a CMake option).
//
//     kin::Renderer3D r3d{renderer};
//     kin::Mesh3D cube = r3d.create_mesh(kin::meshes::box({1, 1, 1})).value_or({});
//     kin::View3D view = r3d.create_view({640, 360}).value_or({});
//     ...each frame:
//     kin::Scene3D scene;
//     scene.camera = kin::Camera3D::orbit({}, angle, 30.0f, 4.0f);
//     scene.draw(cube, kin::Mat4f::identity(), {.color = kin::Color::rgb(200, 80, 60)});
//     r3d.render(view, scene);                       // after the 2D drawn so far
//     renderer.draw_texture(view.texture(), {0, 0, 640, 360});
//
// Without a 3D backend (SDL_Renderer, headless runs), meshes are empty and a view
// shows only its background.

#if !defined(KIN_EXPERIMENTAL_3D)
#error "kin's 3D rendering is experimental: configure kin with -DKIN_EXPERIMENTAL_3D=ON"
#endif

#include <filesystem>
#include <kin/render3d/backend3d.hpp>
#include <kin/render3d/camera3d.hpp>
#include <kin/render3d/material_graph3d.hpp>
#include <kin/render3d/mesh3d.hpp>
#include <kin/render3d/model3d.hpp>
#include <kin/render3d/scene3d.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class Renderer2D;
class Assets3D;
class BakeCache;

// A picture a 3D scene is drawn into (colour and depth), laid into the frame
// with Renderer2D::draw_texture(view.texture(), rect). Cheap to copy (copies
// share it). It keeps its picture until rendered again.
class View3D {
public:
    View3D() = default;
    explicit View3D(std::shared_ptr<IViewBackend3D> backend) : _backend(std::move(backend)) {}

    bool valid() const { return _backend != nullptr; }
    explicit operator bool() const { return valid(); }
    // Premultiplied; a new texture after resize().
    Texture texture() const { return _backend ? _backend->texture() : Texture{}; }
    Vec2i size() const { return _backend ? _backend->size() : Vec2i{}; }
    u32 samples() const { return _backend ? _backend->samples() : 1u; }
    f32 aspect() const {
        const Vec2i s = size();
        return s.y > 0 ? static_cast<f32>(s.x) / static_cast<f32>(s.y) : 1.0f;
    }
    IViewBackend3D* backend() const { return _backend.get(); }
    // Renders fill only `size` pixels of the picture, from its top-left corner (at most size(); none or {}: the
    // whole), where Renderer3D::can_render_part says they can: a picture whose size changes from frame to frame (a
    // mirror's: where it shows) drawn at its own size into a view kept larger, without the view resized (all its
    // targets made again). False where the backend cannot. Whoever draws texture() reads the picture from its UV
    // (0, 0) to render_size() / size(). A resize renders the whole again.
    bool set_render_size(Vec2i size) { return _backend && _backend->set_render_size(size); }
    Vec2i render_size() const { return _backend ? _backend->render_size() : Vec2i{}; }
    // Drops the history Scene3D::taa blends in, so that the next frame does
    // not show the one before: call it when the camera cuts to somewhere
    // else, or the scene is replaced. (A resize does it by itself.)
    void reset_history() {
        if (_backend) {
            _backend->reset_history();
        }
    }

private:
    std::shared_ptr<IViewBackend3D> _backend;
};

// Frame-scoped preparation, borrowing its scene. Edits to mesh commands, batch
// records or poses, moving/destroying the source, and submission/readback/present
// invalidate it. Camera, light and quality settings remain operation-specific.
class PreparedScene3D {
public:
    PreparedScene3D() = default;
    explicit PreparedScene3D(std::shared_ptr<IPreparedSceneBackend3D> backend) : _backend(std::move(backend)) {}
    bool valid() const { return _backend && _backend->valid(); }
    explicit operator bool() const { return valid(); }
    IPreparedSceneBackend3D* backend() const { return _backend.get(); }

private:
    std::shared_ptr<IPreparedSceneBackend3D> _backend;
};

// A surface shader's GLSL (docs/rendering_3d.md, Surface shaders): kin's mesh fragment shader with `surface`, the text
// of a file defining `void kin_surface(inout KinSurface s)` (no #version), between its two halves. Each variant is
// this compiled as a fragment shader with surface_variant_defines(variant) (glslc's -D, after #version) and kin's
// shader folder on the include path (KIN_SHADER_INCLUDE_DIR; cmake's kin_compile_surface_shader does both).
std::string surface_shader_source(std::string_view surface, SurfaceVariant3D variant = SurfaceVariant3D::Plain);
// Whether a surface's cut-out variants are made: it defines `float kin_surface_alpha(vec2 uv, vec3 position, float
// vertex_alpha)` (under KIN_SURFACE_MASK: kin_cutout.inc.glsl), as material graphs whose alpha the cut-out passes can
// compute do.
bool surface_has_alpha(std::string_view surface);
// Whether a surface (its source) moves its vertices: a material graph's offset (kin_surface_offset).
bool surface_has_offset(std::string_view surface);
// The defines of a surface shader's variant: NAME or NAME=VALUE.
std::span<const char* const> surface_variant_defines(SurfaceVariant3D variant);

class Renderer3D {
public:
    // Draws through `renderer`, which must outlive it.
    explicit Renderer3D(Renderer2D& renderer);

    // Whether the renderer's backend draws 3D (SDL_GPU and WebGL2 do, SDL_Renderer
    // doesn't).
    bool available() const { return _backend != nullptr; }
    // What views can have; all defaults where 3D isn't available.
    RendererCapabilities3D capabilities() const;
    Renderer2D& renderer() const { return *_renderer; }

    // `data` kept on the GPU. Empty when 3D isn't available or the data is
    // not valid (MeshData3D::validate); the result carries the failure diagnostics.
    Result3D<Mesh3D> create_mesh(const MeshData3D& data);
    // The same with levels of detail (MeshOptions3D::levels): simpler versions of
    // the mesh that Scene3D::lod draws copies far from the camera with. Level 0
    // is `data` itself, so the picture without Scene3D::lod is the same. Takes
    // time to build (about a second for a million triangles).
    Result3D<Mesh3D> create_mesh(const MeshData3D& data, const MeshOptions3D& options);
    // Morph targets (MeshData3D::morph_targets; docs/rendering_3d.md, Morph targets): the weights `mesh` shows from the
    // next render on, one for each target (Mesh3D::morph_target_count), each within [-1, 1]. Blended on the CPU and
    // uploaded (skipped when they are what it shows); a skinned mesh is blended, then skinned. Every copy drawn of the
    // mesh shows the same weights: copy_mesh() makes one that can show others.
    Status3D set_morph_weights(const Mesh3D& mesh, std::span<const f32> weights);
    // A mesh with morph targets again, its own weights starting at `mesh`'s (the same targets, a GPU copy of its own).
    Result3D<Mesh3D> copy_mesh(const Mesh3D& mesh);
    // Where create_mesh keeps what it builds for levels of detail and clusters (kin/assets/bake_cache.hpp), and
    // reads it back on a later run instead of building again: the same bytes, so the same mesh and pictures. Keyed by
    // the mesh's bytes and the options the build reads (mesh_clusters_bake_key, mesh_levels_bake_key); meshes too
    // small to build anything for are not kept. Null (the default): nothing is kept. Not owned: it must outlive
    // the meshes made while it is set (Assets3D and load_model make theirs through create_mesh, and use it too).
    void set_bake_cache(BakeCache* cache) { _bake_cache = cache; }
    BakeCache* bake_cache() const { return _bake_cache; }
    // A view `size` pixels (at least 1 x 1). Where 3D isn't available, a
    // render target that render() fills with the background.
    Result3D<View3D> create_view(Vec2i size, const ViewOptions3D& options = {});
    // A glTF 2.0 model (.glb or .gltf, read through the content folder or
    // pack): its meshes, materials and textures (mipmapped). A failed result explains why
    // when it can't be read or 3D isn't available.
    Result3D<Model3D> load_model(const std::filesystem::path& path);
    // The same, each of its meshes made with `options` (levels of detail).
    Result3D<Model3D> load_model(const std::filesystem::path& path, const MeshOptions3D& options);
    // An environment (render3d/environment3d.hpp) from a picture: `size` pixels
    // of linear RGB floats, rows top to bottom, 2:1 equirectangular. Filtered
    // on the GPU (with compute) the first time a render uses it; empty where
    // 3D or compute isn't available.
    Result3D<Environment3D> create_environment(Vec2i size, std::span<const f32> rgb);
    // One from a Radiance .hdr file, read through the content folder or pack.
    // A failed result explains why it could not be read.
    Result3D<Environment3D> load_environment(const std::filesystem::path& path);
    // A reflection probe's picture (render3d/reflection_probes3d.hpp), `side` pixels a face of its six (16 to 1024),
    // empty until captured; empty where 3D or compute isn't available.
    Result3D<ReflectionCapture3D> create_reflection_capture(u32 side = 128);
    // Renders `scene` all round from `position` into `capture` (six views, as render() draws them: before the 2D drawn
    // after, its sky, lights, fog and GI too, without temporal anti-aliasing or the overlay; `scene.camera` is put back
    // after). The renders that use it then filter it, once a capture. Cost: six renders at the capture's side.
    Status3D capture_reflection(ReflectionCapture3D& capture, Scene3D& scene, Vec3f position,
                                const ReflectionCaptureOptions3D& options = {});
    // A capture baked: its panorama written to `file` (a Radiance .hdr picture, 2:1, linear: what load_environment and
    // load_reflection_capture read), and one loaded from such a picture through the content folder or pack (a baked
    // probe, or any panorama: a room painted or rendered elsewhere). A loaded capture can be captured again.
    Status3D save_reflection_capture(const ReflectionCapture3D& capture, const std::filesystem::path& file);
    // For tools (write_scene_snapshot, kin/render3d/snapshot3d.hpp): a mesh's level 0 read back from the GPU as it was
    // made (vertices, indices, skin), and an RGBA16F render attachment's texels as floats (four a texel, rows from the
    // top). Each waits for the GPU (what is recorded so far is submitted first).
    Result3D<MeshData3D> read_mesh(const Mesh3D& mesh);
    Result3D<std::vector<f32>> read_linear_texture(const Texture& texture);
    // Whether `view`'s global illumination has light yet (what capture_reflection's global_illumination borrows).
    bool global_illumination_lit(const View3D& view) const;
    Result3D<ReflectionCapture3D> load_reflection_capture(const std::filesystem::path& path);
    // A fragment shader for materials (Material3D::shader), from SPIR-V
    // compiled against kin_material.inc.glsl (docs/rendering_3d.md, Custom
    // shaders). Empty where 3D isn't available or the shader is refused.
    Result3D<MaterialShader3D> create_material_shader(std::span<const u8> spirv);
    // The same from GLSL source, compiled now by glslc (development: see
    // kin/renderer/shader_compiler.hpp; shipped games load SPIR-V). Empty,
    // with glslc's messages in the result when it doesn't compile.
    Result3D<MaterialShader3D> compile_material_shader(const std::filesystem::path& source);
    // A surface shader (Material3D::surface; docs/rendering_3d.md, Surface shaders) from its variants' SPIR-V, in
    // SurfaceVariant3D's order, each the surface's source compiled as surface_shader_source() writes it with that
    // variant's defines (surface_variant_defines). Empty where 3D isn't available, the backend has no surface shaders,
    // or a variant is refused.
    Result3D<SurfaceShader3D>
    create_surface_shader(std::span<const std::span<const u8>, surface_variant_count3d> spirv);
    // The same from a surface source file (its `void kin_surface(inout KinSurface s)`), every variant compiled now by
    // glslc (development, as compile_material_shader). Empty, with glslc's messages, when one doesn't compile.
    Result3D<SurfaceShader3D> compile_surface_shader(const std::filesystem::path& source);
    // A material graph (kin.material-graph/1: kin/render3d/material_graph3d.hpp) as a surface shader, compiled now
    // (development, as compile_surface_shader). Empty, with the graph's errors (each naming its node) or glslc's.
    Result3D<MaterialGraphShader3D> compile_material_graph(const std::filesystem::path& graph);
    // A cooked graph (unpack_cooked_material_graph3d) made into its surface shader: what a shipped game does.
    Result3D<MaterialGraphShader3D> create_material_graph(const CookedMaterialGraph3D& cooked);
    // A material graph's maps past its five slots (Material3D::graph_maps): a 2D array, a picture a layer (RGBA8, all
    // `size`, tightly packed; sRGB ones as they are: the graph decodes them), mipmapped on the GPU. None where surface
    // shaders aren't supported, or for no layers or a wrong size.
    Texture create_map_array(Vec2i size, std::span<const u8* const> layers);
    // Shader-specific immutable extra resource bindings. Sampled textures occupy
    // fragment set 2 slots 12..15; read-only storage follows the shader's sampler
    // count. Shapes/types/counts/ownership are validated once here. GPU contents
    // can change; graph resize or shader reload requires a new binding set.
    Result3D<MaterialInputs3D> create_material_inputs(const MaterialShader3D& shader,
                                                      std::span<const Texture> textures = {},
                                                      std::span<const DataBuffer> buffers = {});
    // Gives `view` a new size (its texture changes); false when it can't.
    Status3D resize(View3D& view, Vec2i size);
    // Whether a render of `scene` into `view` can fill only part of it (View3D::set_render_size): the GPU backend's,
    // for a view that is not HDR and a scene without temporal anti-aliasing, global illumination, the fog's sun shafts,
    // occlusion culling, contact shadows or an overlay; and a view without picking. Otherwise renders fill the whole.
    bool can_render_part(const View3D& view, const Scene3D& scene) const;
    // Draws `scene` into `view`: after the 2D drawn so far in this frame, and
    // before the 2D drawn after (which may draw view.texture()).
    Status3D render(View3D& view, const Scene3D& scene);
    // Clears and writes caller-owned depth/normals, without a stock view or any
    // lighting, shadows, particles, postprocessing or temporal history. Opaque
    // and masked world meshes use stock culling, skinning, retained batches and
    // LOD selection. Blended/wireframe meshes and overlays are excluded. No
    // occlusion history or jitter. Suitable as a graph callback; targets may be
    // GraphResourceKind::RenderAttachment resources. Unsupported backends fail.
    Status3D render_geometry(const GeometryTargets3D& targets, const Scene3D& scene);
    // Builds world bounds and packed instance data once for multiple passes in
    // this GPU frame. Culling and LOD remain per camera/shadow view. No GPU wait,
    // history advance or implicit retention after the handle is released.
    Result3D<PreparedScene3D> prepare(const Scene3D& scene, const PrepareOptions3D& options = {});
    Status3D render_prepared(View3D& view, const PreparedScene3D& prepared);
    Status3D render_geometry_prepared(const GeometryTargets3D& targets, const PreparedScene3D& prepared);
    // Exact, unjittered source-camera projection. Reuses preparation, culling, LOD, skinning and alpha cutouts.
    // No stock colour, lights, GI, AO, shadows, TAA, particles or readback; does not advance any View3D history.
    Status3D render_visibility(const VisibilityTargets3D&, const PreparedScene3D&, const VisibilityOptions3D& = {});
    // Directional shadow rasterization into graph-owned D32F array layers. The
    // receiver extent controls cascade fitting; no colour or history is rendered.
    Result3D<SunShadow3D> render_sun_shadow(const Texture& depth_array, const PreparedScene3D&, Vec2i receiver_size);
    // Explicit point/spot mesh casters into caller-owned D32F array layers.
    // Writes all six faces of requested points and clears every unused layer.
    // At most eight spots/two points; no implicit selection, particles or cache.
    Result3D<LocalShadows3D> render_local_shadows(const Texture& depth_array, const PreparedScene3D&,
                                                  std::span<const LocalShadowRequest3D> requests, Vec2i receiver_size);
    // Single-sample material shading into explicit colour/depth attachments.
    // Meshes/overlays, environment, point/spot lights and optional supplied sun/local
    // shadows. Default All preserves combined mesh-only rendering. Opaque clears;
    // Transparent loads existing attachments or copies colour_input first. Opt-in
    // particles/weather share the transparent mesh order; soft particles need a
    // separate sampled soft_depth. No implicit maps, sky/fog, AO/GI/TAA or postproc.
    Status3D render_surface(const SurfaceTargets3D&, const PreparedScene3D&, const SurfaceOptions3D& = {});
    // Stock bloom extraction and two separable blur rounds into caller-owned
    // attachments. Source is linear premultiplied RGBA16F; no view/history or
    // implicit resource allocation. Final bloom is in targets.result.
    Status3D render_bloom(const BloomTargets3D&, const Texture& linear_colour, const BloomOptions3D& = {});
    // Stock bloom composition, exposure/ACES and output encoding, replacing the
    // whole current-working-format target. No blend, history or implicit bloom.
    Status3D render_output(const RenderTarget&, const Texture& linear_colour, const OutputOptions3D& = {});
    // Makes, without drawing, what rendering `scene` into `view` would make on its first render: the graphics pipelines
    // of its draws and of the passes its settings switch on (shadow maps and cascades, the pre-pass, ambient occlusion,
    // contact shadows, temporal anti-aliasing, occlusion culling), and those features' targets for the view. For a
    // loading screen, or the moment a game turns a feature on, so that the first frame with it does not hitch (pipeline
    // creation is tens of milliseconds a pipeline on a cold driver cache; `stats().pipelines_made` is 0 for a render
    // after it). Nothing is made at start-up: call it for the scenes and settings you will use. False where there is
    // nothing to make (no 3D). Cost: the same as the first render's, in memory too, and the targets are given back
    // after 120 unused renders as ever.
    Status3D warm_up(View3D& view, const Scene3D& scene);
    // What `pixel` (the view's own pixels, from its top left) showed when
    // `view` was last rendered: the copy there and the point of it. Needs a
    // view made with ViewOptions3D::pick. It waits for the GPU to finish
    // what is recorded so far (submitting it): ask when something calls for
    // it (a click, the pointer moving), not every frame.
    Result3D<PickResult3D> pick(View3D& view, Vec2i pixel);
    // The exposure auto exposure has adapted `view` to (ViewOptions3D::auto_exposure: the multiplier it puts on
    // ViewOptions3D::exposure; 2^ev), as it came back from the GPU: a frame or two behind the pictures, read without
    // waiting (the view copies it out each render; it arrives once that frame's work is done). nullopt before the
    // first value arrives, or for a view without auto exposure.
    std::optional<f32> exposure(const View3D& view);
    // The clusters the last render's GPU cut chose (Scene3D::lod, Scene3D::debug.gpu_culling), for tests and tools;
    // waits for the GPU like pick(). False when that render cut on the CPU or drew no clusters. With Lod3D::occlusion
    // the second pass's clusters (view 0, the camera's) follow the first pass's.
    Result3D<bool> read_cluster_picks(View3D& view, std::vector<ClusterPick3D>& out);
    // The indirect draws that cut made, a (view, copy) each (for tests); waits for the GPU. With Lod3D::occlusion the
    // second pass's (the camera's view only) follow.
    Result3D<bool> read_cluster_draws(View3D& view, std::vector<ClusterDraw3D>& out);
    // The GI scene the view holds (Scene3D::global_illumination, built by the last render with it on), for tests and
    // tools; waits for the GPU like pick(). False when the view has none.
    Result3D<bool> read_gi_scene(View3D& view, GiScene3D& out);
    // Compact, nonblocking summary of the last render().
    const RenderStats3D& stats() const { return _stats; }
    // Detailed implementation counters for profilers/tools. Reading this never
    // waits; GPU-only counts require Scene3D::debug.stats_readback during render.
    const RenderDebugStats3D& debug_stats() const { return _stats; }
    // Last runtime operation's failure. Success clears it; capability/stat
    // queries do not.
    const Diagnostics3D& diagnostics() const { return _errors; }

private:
    // Internal callers must validate every primitive before uploading any of them.
    Mesh3D upload_validated_mesh(const MeshData3D& data, const MeshOptions3D& options);
    Mesh3D create_mesh_operation(const MeshData3D& data, const MeshOptions3D& options);
    Mesh3D create_mesh_operation(const MeshData3D& data);
    View3D create_view_operation(Vec2i size, const ViewOptions3D& options = {});
    Model3D load_model_operation(const std::filesystem::path& path, const MeshOptions3D& options);
    Model3D load_model_operation(const std::filesystem::path& path);
    Environment3D create_environment_operation(Vec2i size, std::span<const f32> rgb);
    Environment3D load_environment_operation(const std::filesystem::path& path);
    MaterialShader3D create_material_shader_operation(std::span<const u8> spirv);
    MaterialShader3D compile_material_shader_operation(const std::filesystem::path& source);
    SurfaceShader3D
    create_surface_shader_operation(std::span<const std::span<const u8>, surface_variant_count3d> spirv);
    SurfaceShader3D compile_surface_shader_operation(const std::filesystem::path& source);
    SurfaceShader3D compile_surface_text_operation(const std::string& surface, const std::string& label);
    bool resize_operation(View3D& view, Vec2i size);
    bool render_operation(View3D& view, const Scene3D& scene);
    bool warm_up_operation(View3D& view, const Scene3D& scene);
    PickResult3D pick_operation(View3D& view, Vec2i pixel);
    bool read_cluster_picks_operation(View3D& view, std::vector<ClusterPick3D>& out);
    bool read_cluster_draws_operation(View3D& view, std::vector<ClusterDraw3D>& out);
    bool read_gi_scene_operation(View3D& view, GiScene3D& out);
    friend class Assets3D;
    Model3D load_model_shared(const std::filesystem::path& path, const MeshOptions3D& options, std::string* error,
                              Assets3D* assets);
    void warn_refused_bake(bool restored);
    // A mesh with morph targets from `source` (shared), at weights 0, then at `weights`.
    Mesh3D upload_morph_mesh(std::shared_ptr<const MeshData3D> source, std::span<const f32> weights);
    bool apply_morph_weights(const Mesh3D& mesh, std::span<const f32> weights);
    Renderer2D* _renderer = nullptr;
    IRendererBackend3D* _backend = nullptr;
    RenderDebugStats3D _stats{};
    Diagnostics3D _errors;
    BakeCache* _bake_cache = nullptr;
    bool _warned = false;
    bool _bake_warned = false;
};

} // namespace kin
