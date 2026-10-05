#pragma once

#include <kin/core/affine.hpp>
#include <kin/core/types.hpp>
#include <kin/platform/window.hpp>
#include <kin/renderer/clip.hpp>
#include <kin/renderer/mask.hpp>
#include <kin/renderer/material.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/draw_trace.hpp>
#include <kin/renderer/gradient.hpp>
#include <kin/renderer/render_target.hpp>
#include <kin/renderer/shader.hpp>
#include <kin/renderer/shape.hpp>
#include <kin/renderer/sprite.hpp>
#include <kin/renderer/texture.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

// Selects the 2D backend for `window`. The SDL_GPU (Vulkan) backend is the default
// when `allow_gpu` is true; KIN_RENDER_BACKEND=sdl forces the SDL_Renderer backend
// (KIN_RENDER_BACKEND=gpu also forces a GPU attempt). `allow_gpu` must be false when
// there is no real GPU (e.g. headless/dummy video driver). Falls back to the
// SDL_Renderer backend if the GPU device can't be created. Construct a Renderer2D
// from the result.
std::unique_ptr<IRenderer2DBackend> make_render_backend(Window& window, bool vsync = false,
                                                        bool allow_gpu = true);

class PooledTarget; // defined below; a render target checked out of the pool

class Renderer2D {
public:
    class ViewportGuard {
    public:
        ViewportGuard() = default;
        ViewportGuard(Renderer2D& renderer, Rectf rect);
        ~ViewportGuard();

        ViewportGuard(const ViewportGuard&) = delete;
        ViewportGuard& operator=(const ViewportGuard&) = delete;

        ViewportGuard(ViewportGuard&& other) noexcept;
        ViewportGuard& operator=(ViewportGuard&& other) noexcept = delete;

    private:
        Renderer2D* _renderer = nullptr;
    };

    class NativeCoordinateGuard {
    public:
        NativeCoordinateGuard() = default;
        explicit NativeCoordinateGuard(Renderer2D& renderer);
        ~NativeCoordinateGuard();

        NativeCoordinateGuard(const NativeCoordinateGuard&) = delete;
        NativeCoordinateGuard& operator=(const NativeCoordinateGuard&) = delete;

        NativeCoordinateGuard(NativeCoordinateGuard&& other) noexcept;
        NativeCoordinateGuard& operator=(NativeCoordinateGuard&&) noexcept = delete;

    private:
        Renderer2D* _renderer = nullptr;
    };

    // Ends a gpu_scope() when it goes.
    class GpuScope {
    public:
        GpuScope() = default;
        explicit GpuScope(Renderer2D* renderer) : _renderer(renderer) {}
        ~GpuScope();
        GpuScope(const GpuScope&) = delete;
        GpuScope& operator=(const GpuScope&) = delete;
        GpuScope(GpuScope&& other) noexcept : _renderer(std::exchange(other._renderer, nullptr)) {}
        GpuScope& operator=(GpuScope&&) noexcept = delete;

    private:
        Renderer2D* _renderer = nullptr;
    };

    // Sets the blend mode for the guard's scope and restores the previous one.
    class BlendModeGuard {
    public:
        BlendModeGuard() = default;
        BlendModeGuard(Renderer2D& renderer, BlendMode mode);
        ~BlendModeGuard();

        BlendModeGuard(const BlendModeGuard&) = delete;
        BlendModeGuard& operator=(const BlendModeGuard&) = delete;

        BlendModeGuard(BlendModeGuard&& other) noexcept;
        BlendModeGuard& operator=(BlendModeGuard&&) noexcept = delete;

    private:
        Renderer2D* _renderer = nullptr;
        BlendMode _previous = BlendMode::Alpha;
    };

    // A layer (begin_layer): drawn into a pooled target, laid over when it goes.
    class LayerGuard {
    public:
        LayerGuard() = default;
        explicit LayerGuard(Renderer2D* renderer) : _renderer(renderer) {}
        ~LayerGuard();
        LayerGuard(const LayerGuard&) = delete;
        LayerGuard& operator=(const LayerGuard&) = delete;
        LayerGuard(LayerGuard&& other) noexcept : _renderer(std::exchange(other._renderer, nullptr)) {}
        LayerGuard& operator=(LayerGuard&&) noexcept = delete;

    private:
        Renderer2D* _renderer = nullptr;
    };

    // A clip or mask (scoped_clip, scoped_mask), popped when the guard goes.
    class ClipGuard {
    public:
        ClipGuard() = default;
        explicit ClipGuard(Renderer2D* renderer) : _renderer(renderer) {}
        ~ClipGuard();
        ClipGuard(const ClipGuard&) = delete;
        ClipGuard& operator=(const ClipGuard&) = delete;
        ClipGuard(ClipGuard&& other) noexcept : _renderer(std::exchange(other._renderer, nullptr)) {}
        ClipGuard& operator=(ClipGuard&&) noexcept = delete;

    private:
        Renderer2D* _renderer = nullptr;
    };

    // Binds a render target for the guard's scope (push in ctor, pop in dtor).
    // Distinct from the pool below: this manages which target is *bound*, not
    // target lifetime. Mirrors ViewportGuard.
    class RenderTargetGuard {
    public:
        RenderTargetGuard() = default;
        RenderTargetGuard(Renderer2D& renderer, const RenderTarget& target);
        ~RenderTargetGuard();

        RenderTargetGuard(const RenderTargetGuard&) = delete;
        RenderTargetGuard& operator=(const RenderTargetGuard&) = delete;

        RenderTargetGuard(RenderTargetGuard&& other) noexcept;
        RenderTargetGuard& operator=(RenderTargetGuard&&) noexcept = delete;

    private:
        Renderer2D* _renderer = nullptr;
    };

    // Pops a push_transform() when it goes.
    class TransformGuard {
    public:
        TransformGuard() = default;
        explicit TransformGuard(Renderer2D* renderer) : _renderer(renderer) {}
        ~TransformGuard();
        TransformGuard(const TransformGuard&) = delete;
        TransformGuard& operator=(const TransformGuard&) = delete;
        TransformGuard(TransformGuard&& other) noexcept : _renderer(std::exchange(other._renderer, nullptr)) {}
        TransformGuard& operator=(TransformGuard&&) noexcept = delete;

    private:
        Renderer2D* _renderer = nullptr;
    };

    explicit Renderer2D(Window& window, bool vsync = false);
    explicit Renderer2D(std::unique_ptr<IRenderer2DBackend> backend);
    ~Renderer2D();

    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;

    Renderer2D(Renderer2D&&) noexcept;
    Renderer2D& operator=(Renderer2D&&) noexcept;

    // Unique for the life of the process and never reused, unlike the renderer's
    // address. Caches of renderer-owned resources (such as font glyph atlases) key
    // on it so a new renderer never picks up a dead one's textures.
    u64 id() const { return _id; }
    std::string_view backend_name() const;
    RendererBackendCapabilities capabilities() const;
    RendererBackendStats backend_stats() const;
    void reset_backend_stats();
    // GPU time per frame in backend_stats().last_gpu_frame_ms (SDL_GPU only; off
    // by default, as it costs a fence per frame and a waiting thread).
    void set_gpu_timing_enabled(bool enabled);
    // Times the GPU work drawn until the returned guard goes, as `name`, while
    // GPU timing is on (run_scene_app reports it as gpu.<name> under --profile
    // and in the debug overlay); otherwise does nothing. SDL_GPU has no GPU
    // timestamps, so the scope's edges split the frame's submission and are
    // timed with fences: about 0.1 ms resolution, and a little overhead from
    // the split. Scopes do not nest (an inner one is ignored) and end at present.
    [[nodiscard]] GpuScope gpu_scope(std::string_view name);
    // The scopes that finished on the GPU since the last call (a frame or two
    // after they were drawn).
    std::vector<GpuScopeTiming> take_gpu_scope_timings();
    void set_texture_batching_enabled(bool enabled);
    // A layer: what is drawn until the guard goes lands in a pooled render
    // target, in the same coordinates (clips and viewport too), and is then laid
    // over the current target once, at `opacity`, with `blend`:
    //
    //     { auto shadows = renderer.begin_layer({.opacity = 0.6f, .resolution = 0.5f});
    //       draw_shadows(renderer); }   // overlapping shadows darken once
    //
    // Only what the draws covered is laid over (and counted as overdraw), not
    // the whole target; `resolution` below 1 draws soft content (shadows, glow,
    // light) at a fraction of the pixels. Layers nest. SDL_GPU and SDL's
    // renderer; elsewhere the draws go straight to the current target (opacity
    // not applied).
    struct LayerOptions {
        f32 opacity = 1.0f;
        f32 resolution = 1.0f; // of the current target's pixels, each way
        BlendMode blend = BlendMode::Alpha;
    };
    [[nodiscard]] LayerGuard begin_layer(LayerOptions options);
    [[nodiscard]] LayerGuard begin_layer() { return begin_layer(LayerOptions{}); }

    // Colour (kin/renderer/color_grading.hpp). The colour space colours blend
    // in: Linear decodes colour textures and colours from sRGB to linear light,
    // blends there, and encodes for the display; `hdr` keeps light above white
    // in a float scene until set_color_output() tonemaps it. Set it at
    // start-up, before making textures: those made before keep the encoding
    // they were made with (create_texture()'s data textures are never
    // decoded). Clears the render target pool. SDL_GPU
    // (capabilities().linear_color); elsewhere false, and nothing changes.
    bool set_color_space(ColorSpace space, bool hdr = false);
    ColorSpace color_space() const { return _color_space; }
    bool hdr() const { return _hdr; }
    // The image on its way to the display, from the next present: exposure and
    // tonemapping (Linear), grading through LUTs, dithering. SDL_GPU
    // (capabilities().color_output); elsewhere kept but not applied.
    void set_color_output(ColorOutput output);
    const ColorOutput& color_output() const { return _color_output; }

    // Overdraw view (SDL_GPU; the debug overlay's toggle): instead of itself,
    // every draw adds one layer where it covers, and the screen shows the counts
    // as colours: black none, blue 1, green 2, yellow 4, red 8, white 16 or more.
    // Only the screen's own draws show (a render target's composite counts once).
    void set_overdraw_view(bool enabled) { _backend->set_overdraw_view(enabled); }
    bool texture_batching_enabled() const;

    void clear(Color color = colors::black);
    void clear(u8 r, u8 g, u8 b, u8 a = 255);
    void present();

    // Saves the current render target to a PNG file. Returns false if the
    // backend cannot read back pixels.
    bool save_png(const std::string& path);
    bool read_rgba(Rectf logical_region, std::vector<u8>& out, Vec2i& out_size);

    void set_logical_size(i32 width, i32 height);
    void set_logical_size(Vec2i size);
    void set_integer_logical_size(i32 width, i32 height);
    void set_integer_logical_size(Vec2i size);
    Vec2i output_size() const;
    Vec2f window_to_logical(Vec2f window_px) const;
    Vec2f logical_to_window(Vec2f logical) const;

    Texture create_texture_from_rgba(const u8* pixels, i32 width, i32 height);
    Texture create_texture_from_rgba(const u8* pixels, Vec2i size);
    // A texture of any TextureFormat, filled from `pixels` (size.x * size.y texels
    // of texture_format_bytes(format) each, rows top to bottom) or with zeros when
    // null. The data formats need capabilities().data_textures: without it this
    // logs and returns an invalid texture. Data textures are only for shaders;
    // draw_texture() refuses them.
    Texture create_texture(Vec2i size, TextureFormat format, const void* pixels = nullptr);
    // Replace part of a texture in place: the `size` texels at `at` from `pixels`,
    // tightly packed in the texture's format (texture_format_bytes each). Much cheaper
    // than making the texture again when only part of it changes. False when the
    // backend cannot (the software backend) or the region is outside the texture;
    // the texture is then unchanged, and the caller makes a new one.
    bool update_texture(const Texture& texture, Vec2i at, Vec2i size, const u8* pixels);
    // As update_texture, but `fill` writes the region's texels (row by row, in
    // the texture's format) into the span it is given, which on SDL_GPU is the
    // upload memory itself: no copy, for texels made each frame. That memory is
    // uncached, so write it in order, once, and never read it. `fill` must not
    // use the renderer. Elsewhere the span is a plain buffer.
    bool write_texture(const Texture& texture, Vec2i at, Vec2i size, const std::function<void(std::span<u8>)>& fill);
    void draw_texture(const Texture& texture, Rectf dest);
    void draw_texture(const Texture& texture, Rectf source, Rectf dest);
    void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint);
    void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint, f32 rotation, Vec2f pivot);
    // Mirrored within `dest` (Flip::X left-right, Flip::Y upside down), then turned.
    // An empty `source` is the whole texture.
    void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint, f32 rotation, Vec2f pivot, Flip flip);
    // Many quads from one texture, in order: the same pixels as a draw_texture()
    // per sprite, but backends with instancing (SDL_GPU) submit them as one batch.
    void draw_sprites(const Texture& texture, std::span<const SpriteInstance> sprites);
    // Lets the backend spread large batches over these workers (null: none);
    // returns the job system it replaces. The caller keeps it alive while set.
    JobSystem* set_job_system(JobSystem* jobs);
    JobSystem* job_system() const { return _jobs; }
    void draw_texture(const Texture& texture, Vec2f pos, Vec2f size);
    void draw_texture(const Texture& texture, Vec2f pos);
    void draw_sprite(const Sprite& sprite, Rectf dest);
    void draw_sprite(const Sprite& sprite, Rectf dest, Color tint, f32 rotation, Vec2f pivot);
    void draw_sprite(const Sprite& sprite, Rectf dest, Color tint, f32 rotation, Vec2f pivot, Flip flip);
    void draw_sprite(const Sprite& sprite, Vec2f pos, Vec2f size);
    void draw_sprite(const Sprite& sprite, Vec2f pos);

    void fill_rect(Rectf rect, Color color);
    void fill_rect(Vec2f pos, Vec2f size, Color color);
    void fill_rect(Vec2f pos, Vec2f size, u8 r, u8 g, u8 b, u8 a = 255);
    void draw_rect(Rectf rect, Color color);
    void draw_rect(Vec2f pos, Vec2f size, Color color);
    void draw_rect(Vec2f pos, Vec2f size, u8 r, u8 g, u8 b, u8 a = 255);
    void fill_rounded_rect(Rectf rect, f32 radius, Color color);
    void draw_rounded_rect(Rectf rect, f32 radius, Color color, f32 width = 1.0f);
    void fill_gradient_rect(Rectf rect, const Gradient& gradient);

    // Shapes (kin/renderer/shape.hpp), anti-aliased: one pixel soft at any
    // scale on SDL_GPU, a soft edge as wide as the mesh's fringe elsewhere.
    // draw_shape draws a mesh made once, placed by `transform` and coloured by
    // `tint`; the rest tessellate as they are called (for the odd circle or
    // line; a mesh is cheaper for anything drawn often). Widths are in drawing
    // units, so they grow with the transform; arcs' angles are degrees, 0 at +x,
    // clockwise.
    void draw_shape(const ShapeMesh& mesh, const Affine2& transform = {}, Color tint = colors::white);
    void fill_circle(Vec2f center, f32 radius, Color color);
    void draw_circle(Vec2f center, f32 radius, Color color, f32 width = 1.0f);
    void fill_ellipse(Vec2f center, Vec2f radii, Color color);
    void draw_ellipse(Vec2f center, Vec2f radii, Color color, f32 width = 1.0f);
    void fill_polygon(std::span<const Vec2f> points, Color color);
    void draw_polygon(std::span<const Vec2f> points, Color color, StrokeStyle style = {});
    void draw_polyline(std::span<const Vec2f> points, Color color, StrokeStyle style = {});
    // A line `width` wide (the other draw_line is one unit wide and pixel-aligned).
    void draw_line(Vec2f a, Vec2f b, Color color, f32 width, LineCap cap = LineCap::Butt);
    void draw_arc(Vec2f center, f32 radius, f32 start, f32 end, Color color, StrokeStyle style = {});
    void fill_pie(Vec2f center, f32 radius, f32 start, f32 end, Color color);
    void fill_path(const Path& path, Color color, FillRule rule = FillRule::NonZero);
    // Quads of a distance-field texture (alpha 0.5 on the outline, see
    // DistanceFieldStyle), tinted, their edges one pixel soft at any size,
    // turn or zoom, with an optional outline: scalable text and icons. False
    // (nothing drawn) where the backend cannot (capabilities().distance_fields).
    bool draw_distance_field(const Texture& texture, std::span<const SpriteInstance> quads,
                             const DistanceFieldStyle& style = {});
    void stroke_path(const Path& path, Color color, StrokeStyle style = {});
    // Reads the SPIR-V's layout (kin/renderer/shader_reflect.hpp): the sampler,
    // storage buffer and uniform block counts come from it, so a ShaderDesc's
    // counts may be left as they are (a mismatch is logged and the shader's
    // own used).
    ShaderHandle create_shader(const ShaderDesc& desc);
    // The pipelines (shader, blend mode, target, vertex layout) this run made, as
    // text; a later run hands it to prewarm_pipelines, which makes them at once
    // (the engine's own) or as their shader is created, instead of at their
    // first draw: up to ~20 ms each on a cold driver cache. run_scene_app keeps
    // it in the game's user data (windowed runs).
    std::string pipeline_record() const { return _backend->pipeline_record(); }
    void prewarm_pipelines(std::string_view record) { _backend->prewarm_pipelines(record); }
    // Replaces a shader with a new version, keeping its handle (hot reload: see
    // kin/renderer/shader_compiler.hpp). Draws already queued use the old one.
    bool reload_shader(ShaderHandle shader, const ShaderDesc& desc);
    // The layout read from a shader's SPIR-V (null when there was none).
    std::shared_ptr<const ShaderLayout> shader_layout(ShaderHandle shader) const;
    // Params sized for the shader's uniform block, settable by member name.
    ShaderParams shader_params(ShaderHandle shader) const;
    void draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params);
    void draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params,
                             const Texture& source);
    void draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params,
                             const Texture& source0, const Texture& source1);
    // `sources[i]` binds at fragment sampler i. More than MaxShaderSamplers sources
    // is an error: it is logged and nothing is drawn.
    void draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params,
                             std::span<const Texture> sources);
    // Triangles drawn with a material shader: only the pixels they cover run it.
    // `indices` index `vertices` three at a time; empty, the vertices themselves
    // are the triangles. Sources and params as for draw_shader_surface; the
    // current blend mode applies. Needs capabilities().shader_geometry (SDL_GPU);
    // elsewhere nothing is drawn.
    void draw_shader_geometry(std::span<const ShaderVertex> vertices, std::span<const u32> indices,
                              ShaderHandle shader, const ShaderParams& params,
                              std::span<const Texture> sources = {}, std::span<const DataBuffer> buffers = {});
    // A shader surface that also reads storage buffers (`buffers[i]` after the
    // shader's textures in set 2; kin/renderer/data_buffer.hpp).
    void draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params,
                             std::span<const Texture> sources, std::span<const DataBuffer> buffers);

    // A shader surface computed at `resolution` (0.5: half as many pixels each
    // way, a quarter of the work) into a pooled render target, then stretched
    // over `rect` with linear filtering: for smooth, costly effects (fog, glow,
    // soft light). The shader must work from its UV, not gl_FragCoord. At 1 or
    // more, or without render targets, it is a plain draw_shader_surface.
    void draw_shader_surface_scaled(f32 resolution, Rectf rect, ShaderHandle shader, const ShaderParams& params,
                                    std::span<const Texture> sources = {}, std::span<const DataBuffer> buffers = {});

    // Compute shaders (capabilities().compute; elsewhere a null handle): made
    // from SPIR-V, whose layout and workgroup size are read from it. A storage
    // texture is one they can write (and other shaders sample, or draws draw).
    // dispatch_compute runs enough workgroups to cover `size` threads (x, y),
    // recorded in the frame in order with the draws around it; false when the
    // bindings fall short of what the shader declares (logged).
    ComputeShaderHandle create_compute_shader(ShaderBlob spirv);
    Texture create_storage_texture(Vec2i size, TextureFormat format = TextureFormat::Rgba8);
    bool dispatch_compute(ComputeShaderHandle shader, Vec2i size, const ComputeBindings& bindings);

    // Storage buffers for shaders (capabilities().data_buffers; elsewhere an
    // invalid buffer). Filled from `data`, or zeros. Updates go to the GPU with
    // the frame's texture uploads; write_data_buffer's `fill` writes straight
    // into the upload memory (in order, once; it must not use the renderer).
    DataBuffer create_data_buffer(std::size_t bytes, const void* data = nullptr);
    bool update_data_buffer(const DataBuffer& buffer, std::size_t offset, std::size_t bytes, const void* data);
    bool write_data_buffer(const DataBuffer& buffer, std::size_t offset, std::size_t bytes,
                           const std::function<void(std::span<u8>)>& fill);
    void draw_line(Vec2f a, Vec2f b, Color color);
    void draw_line(Vec2f a, Vec2f b, u8 r, u8 g, u8 b_color, u8 a_color = 255);

    // How later draws combine with what is under them, until set again:
    //   Alpha     straight alpha over (the default)
    //   Additive  dst + src * src_alpha: lights, glows
    //   Multiply  dst * src (alpha ignored): light maps, tinting the scene
    //   Replace   src, no blending
    //   Max       max(dst, src) per channel, alpha too (src not weighted by its
    //             alpha): overlapping shadows, fog of war, coverage, heat maps
    //   Min       min(dst, src) per channel, alpha too
    // A render target's premultiplied texture keeps its own blend under Alpha.
    // Backends without blend modes (capabilities().blend_modes false) ignore it;
    // those without Max and Min (capabilities().min_max_blend false: SDL's
    // software renderer) draw them as Alpha and log a warning once.
    void set_blend_mode(BlendMode mode);
    BlendMode blend_mode() const { return _blend_mode; }
    BlendModeGuard scoped_blend_mode(BlendMode mode);

    void set_viewport(Rectf rect);
    void reset_viewport();
    void push_viewport(Rectf rect);
    void pop_viewport();
    // Clips and masks: one stack, each popped by pop_clip() (or its guard).
    //
    //   push_clip(rect)                 the scissor: a rectangle in untransformed
    //                                   coordinates, to whole pixels; free
    //   push_clip(path, rule, edge)     inside a path, under the transform,
    //                                   anti-aliased (or Hard: see ClipEdge)
    //   push_mask(draw, options)        where `draw` draws: anything, under the
    //                                   transform, read as `options` say
    //   push_mask(texture, dest, opts)  a texture stretched over `dest`
    //   push_clip(clip)                 any of these, as a kin::ClipRegion value
    //
    //     { auto clip = renderer.scoped_clip(kin::Path::circle(eye, 40.0f));
    //       renderer.draw_texture(iris, iris_rect); }
    //     { auto lit = renderer.scoped_mask([&](kin::Renderer2D& r) { r.draw_texture(light, light_rect); },
    //                                       {.source = kin::MaskSource::Luminance});
    //       draw_level(renderer); }
    //
    // Each nests in those open: a pixel shows as far as all of them let it.
    // A path or mask draws through two pooled layers (the mask, then what it
    // masks) and lays the second over through the first: a render pass each and
    // the pixels they cover, so clip to rectangles where they will do (a
    // rectangular path, unturned, clips as one). capabilities().masks: SDL_GPU,
    // and SDL's renderer, which lays them over on the CPU (slow). Elsewhere a
    // path clips to its bounds and a mask lets everything through.
    void push_clip(Rectf rect);
    void push_clip(const Path& path, FillRule rule = FillRule::NonZero, ClipEdge edge = ClipEdge::Smooth);
    void push_clip(const ClipRegion& clip);
    void push_mask(const std::function<void(Renderer2D&)>& draw_mask, MaskOptions options = {});
    void push_mask(const Texture& mask, Rectf dest, MaskOptions options = {});
    void pop_clip();
    [[nodiscard]] ClipGuard scoped_clip(Rectf rect);
    [[nodiscard]] ClipGuard scoped_clip(const Path& path, FillRule rule = FillRule::NonZero,
                                        ClipEdge edge = ClipEdge::Smooth);
    [[nodiscard]] ClipGuard scoped_clip(const ClipRegion& clip);
    [[nodiscard]] ClipGuard scoped_mask(const std::function<void(Renderer2D&)>& draw_mask, MaskOptions options = {});
    [[nodiscard]] ClipGuard scoped_mask(const Texture& mask, Rectf dest, MaskOptions options = {});
    ViewportGuard scoped_viewport(Rectf rect);
    NativeCoordinateGuard scoped_native_coordinates();

    // Transforms. push_transform(m) maps everything drawn after it by m, then
    // by the transforms pushed before it (m is the more local one), until
    // pop_transform(). Shapes are transformed as if drawn and then moved,
    // turned and scaled: a line or outline one unit wide grows with the scale.
    // Viewports, clips, capture_backdrop and read_rgba stay in untransformed
    // coordinates. A render target starts untransformed and the transform
    // returns when it is popped; layers keep the transform. Applied to the
    // vertices on the CPU, so draws batch across transform changes.
    //
    // With a camera:  auto view = renderer.scoped_transform(camera.view_transform());
    void push_transform(const Affine2& transform);
    void pop_transform();
    TransformGuard scoped_transform(const Affine2& transform);
    // Replaces the current transform (what the stack's top maps by).
    void set_transform(const Affine2& transform);
    const Affine2& transform() const { return _transform; }

    // Render targets (A1). create_render_target makes a target you own outright;
    // acquire_render_target borrows one from the engine pool (RAII-returned via
    // PooledTarget). On a backend without render_targets these yield empty
    // targets and callers degrade on !valid().
    RenderTarget create_render_target(Vec2i size, ScaleMode mode = ScaleMode::Linear);
    void push_render_target(const RenderTarget& target);
    void pop_render_target();
    void set_scale_mode(const Texture& texture, ScaleMode mode);
    RenderTargetGuard scoped_render_target(const RenderTarget& target);

    PooledTarget acquire_render_target(Vec2i size, ScaleMode mode = ScaleMode::Linear);

    // Capture a logical-space region of the current target into a pooled, Linear,
    // sampleable render target (for glass/refraction). Uses a GPU-side blit when the
    // backend supports it (no CPU round-trip), else falls back to read_rgba + re-upload.
    // Returns an invalid PooledTarget when capture is unsupported/empty.
    PooledTarget capture_backdrop(Rectf region);

    void trim_render_target_pool();
    std::size_t render_target_pool_size() const; // entry count (free + in use); diagnostics/tests

    // Full-scene post-processing (GPU backend). The chain persists until set again;
    // clear_post_process() removes it. No-op on backends without material support.
    void set_post_process(std::span<const PostProcessPass> passes);
    void clear_post_process();

    // Lazily create (and cache) an engine-shipped fragment shader from KIN_GPU_SHADER_DIR.
    // Returns a null handle when materials are unsupported / the .spv is missing — callers
    // degrade. Cached per id so repeat calls are cheap.
    ShaderHandle builtin_shader(BuiltinShader id);

private:
    JobSystem* _jobs = nullptr;
    friend class ViewportGuard;
    friend class PooledTarget;

    struct RenderTargetPoolEntry {
        RenderTarget target;
        Vec2i size;
        ScaleMode mode = ScaleMode::Linear;
        bool in_use = false;
    };

    // Returns a checked-out pooled target to the free list (matched by texture
    // identity). Called by PooledTarget's destructor.
    void release_render_target_(const RenderTarget& target);

#ifdef KIN_ENABLE_RENDER_PROBE
    // Draw tracing (see draw_trace.hpp): records a draw in the active trace, in
    // window pixels, unless it goes to a render target.
    void trace_draw(DrawKind kind, Rectf dest, Color color, const Texture* texture = nullptr, Rectf region = {},
                    f32 rotation = 0.0f, Vec2f pivot = {0.5f, 0.5f}, u32 source = 0);
    std::optional<Rectf> _trace_viewport;              // drawing is relative to its origin
    std::vector<std::optional<Rectf>> _trace_viewports; // pushed ones
    i32 _trace_targets = 0;                             // render targets pushed
    i32 _trace_native = 0;                              // native coordinate scopes
#endif

    u64 _id = 0;
    std::unique_ptr<IRenderer2DBackend> _backend;
    std::vector<RenderTargetPoolEntry> _rt_pool;
    std::unordered_map<int, ShaderHandle> _builtin_shaders; // BuiltinShader -> cached handle
    std::unordered_map<u64, std::shared_ptr<const ShaderLayout>> _shader_layouts; // read from each shader's SPIR-V
    std::unordered_map<u64, ShaderLayout> _compute_layouts;
    // Open layers, innermost last (an invalid target: drawn straight through).
    struct OpenLayer;
    std::vector<std::unique_ptr<OpenLayer>> _layers;
    void end_layer();
    // Open clips and masks, innermost last.
    struct OpenClip;
    std::vector<std::unique_ptr<OpenClip>> _clips;
    void end_mask(OpenClip& clip);
    BlendMode _blend_mode = BlendMode::Alpha;
    ColorSpace _color_space = ColorSpace::Gamma;
    bool _hdr = false;
    ColorOutput _color_output;
    Texture _lut_texture;    // _color_output.lut's, uploaded
    Texture _lut_to_texture; // and lut_to's
    // The shapes drawn as they are called: reused buffers, and how finely to
    // tessellate under the current transform.
    ShapeMesh _shape_scratch;
    std::vector<PathContour> _contour_scratch;
    std::vector<Vec2f> _stencil_scratch; // a hard clip's triangles
    struct ShapeDetail {
        f32 tolerance = 0.25f;
        f32 fringe = 1.0f;
    };
    ShapeDetail shape_detail() const;
    void fill_contours(FillRule rule, Color color);
    void stroke_contours(const StrokeStyle& style, Color color);
    void draw_shape_triangles(const ShapeMesh& mesh, const ShapeMesh::Run& run, Color tint);
    // Whole where the backend can, else tessellated.
    void draw_primitives(std::span<const ShapePrimitive> primitives, Color tint);
    ShapeMesh _primitive_scratch;
    Affine2 _transform{};                 // what draws are mapped by now
    std::vector<Affine2> _transform_stack; // push_transform's saved transforms
    std::vector<Affine2> _target_transforms; // the transform outside each pushed render target
    void apply_transform(const Affine2& transform);
};

// RAII checkout from the renderer's render-target pool. Move-only; returns its
// target to the pool on destruction. Drawing uses target().texture().
class PooledTarget {
public:
    PooledTarget() = default;
    ~PooledTarget();

    PooledTarget(const PooledTarget&) = delete;
    PooledTarget& operator=(const PooledTarget&) = delete;

    PooledTarget(PooledTarget&& other) noexcept;
    PooledTarget& operator=(PooledTarget&& other) noexcept;

    bool valid() const { return _rt.valid(); }
    explicit operator bool() const { return valid(); }
    const RenderTarget& target() const { return _rt; }
    const Texture& texture() const { return _rt.texture(); }
    Vec2i size() const { return _rt.size(); }

private:
    friend class Renderer2D;

    Renderer2D* _pool = nullptr;
    RenderTarget _rt;
};

} // namespace kin
