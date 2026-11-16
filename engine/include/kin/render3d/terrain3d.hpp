#pragma once

// Heightmap terrain (docs/rendering_3d.md, Terrain): heights on a regular grid of samples, drawn as CDLOD (a quadtree
// of nodes, the finest near the camera; each node one copy of a flat grid mesh, its vertices lifted to the heights and
// morphed toward the coarser level's grid by distance, so levels meet without cracks or popping) under kin's terrain
// surface (engine/shaders/terrain.surface.glsl).
//
//   kin::Terrain3D terrain = kin::Terrain3D::create(r3d, surface, {.samples = {1025, 1025}, .spacing = 1.0f,
//                                                                 .max_height = 200.0f}, heights).value_or({});
//   terrain.draw(scene); // each frame: the nodes for the scene's camera, one instanced draw
//
// The surface is built by the game (about a megabyte of variants, as kin's planar surfaces): kin_terrain_surface(
// <target>) in CMake embeds it (create_terrain_surface), or glslc compiles it now (compile_terrain_surface).

#include <array>
#include <filesystem>
#include <functional>
#include <kin/assets/image.hpp>
#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/render3d/errors3d.hpp>
#include <kin/render3d/mesh3d.hpp>
#include <kin/render3d/scene3d.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class Renderer3D;

// kin's terrain surface from its variants as kin_terrain_surface(<target>) (cmake/kin_embed.cmake) builds and embeds
// them: `spirv` gives a file's bytes by name ("terrain.plain.spv" ...; the embedded `terrain_surface` function).
Result3D<SurfaceShader3D> create_terrain_surface(Renderer3D& renderer,
                                                 const std::function<std::span<const u8>(std::string_view)>& spirv);
// Compiled now by glslc from kin's shader folder: development builds on a machine with glslc only.
Result3D<SurfaceShader3D> compile_terrain_surface(Renderer3D& renderer);

struct TerrainDesc3D {
    Vec2i samples{257, 257}; // the height map's samples along x and z (at least 2 each, at most 8193)
    f32 spacing = 1.0f;      // world units between neighbouring samples
    Vec3f origin{};          // where sample (0, 0) is; heights are above origin.y
    // The range the 16-bit heights span (above origin.y): 0 is min_height, 65535 max_height.
    f32 min_height = 0.0f, max_height = 100.0f;
    // Level of detail: a node's grid has `node_quads` quads a side (a power of two, 4 to 128), the finest nodes cover
    // as many samples; the finest level is drawn within `lod_distance` finest nodes of the camera, each coarser level
    // twice as far; a level's vertices morph toward the next one's over the last (1 - morph_start) of its range.
    // Both are raised where the terrain needs it to meet without cracks (its steepest finest node: for a flat one,
    // lod_distance at least 2.8 and morph_start 0.75).
    u32 node_quads = 32;
    f32 lod_distance = 3.0f;
    f32 morph_start = 0.7f;
};

// The most painted layers a terrain has.
inline constexpr u32 max_terrain_layers3d = 16;

// A painted layer (grass, rock, sand ...): its maps tile the ground every `tile` world units. The albedo is colour
// (sRGB), its alpha the layer's height for blending (where two layers meet, the higher shows: pebbles over sand;
// none: 0.5 everywhere); the normal map is tangent space (OpenGL's: green up); either may be empty (a plain `tint`, a
// flat surface).
struct TerrainLayer3D {
    Image albedo;
    Image normal;
    LinearRgb tint{1.0f, 1.0f, 1.0f}; // times the albedo (its colour where there is none)
    f32 roughness = 0.85f;
    f32 tile = 4.0f;
};

// The stock look (without layers): one colour, linear, and a roughness. `blend` is how sharply layers meet by their
// heights (0: by their weights alone; 1: the higher wins over a short blend).
struct TerrainLook3D {
    LinearRgb color{0.18f, 0.26f, 0.10f};
    f32 roughness = 0.9f;
    f32 blend = 0.5f;
};

// A selected node: its corner (x, z), size, the level its grid's vertices are spaced for, its heights' range.
struct TerrainNode3D {
    Vec2f corner{};
    f32 size = 0.0f;
    u32 level = 0;
    f32 min_y = 0.0f, max_y = 0.0f;
};

// Sculpting and painting (Terrain3D::brush): what a dab does inside its circle, weighted by the falloff (1 inside
// (1 - falloff) x radius, easing to 0 at the radius) times `strength` times the dab's amount.
enum class TerrainBrushMode3D : u8 {
    Raise,   // up by strength world units
    Lower,   // down by strength world units
    Smooth,  // toward the mean of its neighbours (strength: how far, 0 to 1)
    Flatten, // toward `height` (world y; strength: how far, 0 to 1)
    Noise,   // up and down by strength world units, by a value noise `scale` world units wide
    Paint,   // `layer`'s weight up by strength (0 to 1 of the whole), the others' down to make room
};
struct TerrainBrush3D {
    TerrainBrushMode3D mode = TerrainBrushMode3D::Raise;
    f32 radius = 8.0f;
    f32 strength = 0.25f;
    f32 falloff = 0.5f;
    f32 height = 0.0f;
    f32 scale = 4.0f;
    u32 layer = 0;
};

struct TerrainHit3D {
    Vec3f position{};
    Vec3f normal{0.0f, 1.0f, 0.0f};
    f32 distance = 0.0f;
};

class Terrain3D {
public:
    Terrain3D();
    ~Terrain3D();
    Terrain3D(Terrain3D&&) noexcept;
    Terrain3D& operator=(Terrain3D&&) noexcept;

    // A terrain of `heights` (samples.x * samples.y, row by row from z 0; 0 to 65535 over the height range; empty:
    // flat at min_height). Its textures and grid mesh are made now.
    static Result3D<Terrain3D> create(Renderer3D& renderer, SurfaceShader3D surface, const TerrainDesc3D& desc,
                                      std::span<const u16> heights = {});

    bool valid() const;
    explicit operator bool() const { return valid(); }
    const TerrainDesc3D& desc() const;
    // Moves it: sample (0, 0) at `origin` (its heights above origin.y).
    void set_origin(Vec3f origin);
    // The world's x and z it covers, from origin to origin + size.
    Vec2f size() const;
    std::span<const u16> heights() const;

    // Its surface as triangles (every `stride`th sample a vertex, the last row and column always), in the world: for
    // a navigation mesh (build_navmesh3d), a bake or an export. terrain_mesh3d without the GPU.
    MeshData3D mesh(u32 stride = 1) const;

    // The height (world y) and normal at a place, between the samples round it; nullopt outside the terrain.
    std::optional<f32> height(Vec2f xz) const;
    std::optional<Vec3f> normal(Vec2f xz) const;
    // Where a ray first meets the surface (within `max_distance`).
    std::optional<TerrainHit3D> raycast(Vec3f from, Vec3f direction, f32 max_distance = 1e30f) const;

    // Heights changed in a rectangle of samples (`at`, `size`; `values` row by row): the normals, the nodes' height
    // ranges and the textures there follow (the textures at the next update() or draw()).
    bool set_heights(Vec2i at, Vec2i size, std::span<const u16> values);
    // Uploads what changed since (the height map's changed rectangle). False when the upload failed.
    bool update();

    // Its painted layers (up to max_terrain_layers3d; empty: the stock look), their maps resampled to `map_size`
    // (one texture array for all). The weights stay as they are (a new terrain's: all the first layer's).
    Status3D set_layers(std::span<const TerrainLayer3D> layers, Vec2i map_size = {512, 512});
    u32 layer_count() const;
    // A layer's weights over a rectangle of samples (0 to 255; `values` row by row). Each sample's weights are shared
    // out by their sum (none: the first layer). Uploaded with the heights.
    bool set_weights(u32 layer, Vec2i at, Vec2i size, std::span<const u8> values);
    // The weights of every layer at a sample (16, 0 to 255).
    std::array<u8, max_terrain_layers3d> weights(Vec2i sample) const;

    void set_look(const TerrainLook3D& look);
    const TerrainLook3D& look() const;

    // One dab of `brush` at `center` (world x, z), `amount` times its strength (a stroke's dabs: the time since the
    // last, for a rate). The samples it changed: their rectangle (empty: none). Within a stroke (begin_stroke ..
    // end_stroke) each sample's first state is kept for undo; outside one, each dab is a stroke of its own.
    struct Changed {
        Vec2i at{}, size{};
        bool empty() const { return size.x <= 0 || size.y <= 0; }
    };
    Changed brush(const TerrainBrush3D& brush, Vec2f center, f32 amount = 1.0f);
    void begin_stroke();
    // Ends the stroke: one undo step (none when it changed nothing). The rectangle the stroke changed.
    Changed end_stroke();
    // Puts the last stroke back (redo: does it again); false when there is none. The rectangle it changed.
    std::optional<Changed> undo();
    std::optional<Changed> redo();
    bool can_undo() const;
    bool can_redo() const;
    // At most this many strokes are kept (the oldest dropped).
    void set_undo_limit(u32 strokes);

    // A .kinterrain file: the description, look, heights, weights and layers (their pictures too), deflated.
    Status3D save(const std::filesystem::path& file) const;
    static Result3D<Terrain3D> load(Renderer3D& renderer, SurfaceShader3D surface, const std::filesystem::path& file);

    // The nodes drawn for a camera at `eye`: the whole terrain, the finest near it (the renderer culls each pass).
    std::span<const TerrainNode3D> select(Vec3f eye);
    // Selects for the scene's camera and adds one instanced draw (`id` for picking).
    Status3D draw(Scene3D& scene, u32 id = 0);
    // The material it draws with (its surface, height map and parameters).
    const Material3D& material() const;
    const Mesh3D& grid() const;

private:
    struct State;
    std::unique_ptr<State> _s;
};

// A terrain's surface as triangles from its description and heights alone (no renderer: a server's navigation mesh,
// a tool's export): every `stride`th sample a vertex (the last row and column always), facing up, in the world.
MeshData3D terrain_mesh3d(const TerrainDesc3D& desc, std::span<const u16> heights, u32 stride = 1);

// Heights from a file: a raw 16-bit little-endian height map (.r16, .raw: `samples` given, or square from its size),
// a 16-bit PNG (its first channel; not interlaced), or an 8-bit image (its red channel, scaled to 16 bits). nullopt
// (and `why`) when it cannot be read.
struct HeightMap3D {
    Vec2i samples{};
    std::vector<u16> heights;
};
std::optional<HeightMap3D> load_height_map(const std::filesystem::path& file, Vec2i samples = {},
                                           std::string* why = nullptr);

} // namespace kin
