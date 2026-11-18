#pragma once

// Baked lighting (docs/rendering_3d.md, "Baked lighting"): light worked out ahead of time, on the CPU, by a path
// tracer, and drawn for almost nothing.
//
// - Lightmaps: for each copy of a static mesh (one with lightmap UVs, MeshData3D::lightmap_uv), a rectangle of a
//   page of texels, each holding the diffuse light that reaches the surface there: the sky's and the environment's
//   past whatever hides them, the bounced light of every light that is not Realtime, and all the light (direct too)
//   of Baked lights. Drawn, it takes the place of the ambient, environment, reflection-probe and GI diffuse light.
// - Probes: a grid of points over an area, each holding the light arriving there from every way (the same light, as
//   linear spherical harmonics), for what moves: a copy without a lightmap inside the grid takes its diffuse light
//   from the probes round each of its points, in place of the ambient or environment's.
//
// Lights choose how they take part (LightBake3D, on each light). The bake is deterministic: the same surfaces,
// lights and settings give the same texels, to the bit, on any number of threads.

#include <array>
#include <filesystem>
#include <functional>
#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/render3d/errors3d.hpp>
#include <kin/render3d/mesh3d.hpp>
#include <kin/render3d/scene3d.hpp>
#include <kin/renderer/color.hpp>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace kin {

class JobSystem;

// "realtime", "mixed" or "baked" (authored scenes), and back (nothing for another word).
std::string_view light_bake_name(LightBake3D mode);
std::optional<LightBake3D> light_bake_from_name(std::string_view name);

// --- Lightmap UVs ---------------------------------------------------------------------------------------------------

struct LightmapUvOptions3D {
    // The square's side in texels the charts are laid out for: the room left between charts is `padding` texels of
    // it. Lay a mesh out for about the texels a copy will get (texels_per_unit times its size): far fewer leave
    // charts closer than a texel, and their light bleeds.
    u32 resolution = 256;
    u32 padding = 2;
};

// Lays out lightmap UVs (MeshData3D::lightmap_uv) for `mesh` with xatlas: cut into charts, flattened and packed into
// [0, 1] x [0, 1]. Vertices on a chart's seam are split (the mesh gains vertices; its triangles and their order stay,
// and every vertex keeps its other attributes). Refused for a skinned or morphed mesh.
Status3D generate_lightmap_uv(MeshData3D& mesh, const LightmapUvOptions3D& options = {});

// --- What the bake takes --------------------------------------------------------------------------------------------

// A material's picture: RGBA8 rows from the top (v = 0), read at the surface's own UVs (MeshVertex3D::uv) times
// `uv_scale` plus `uv_offset` (Material3D's), bilinear, wrapping (or clamped).
struct BakeTexture3D {
    std::span<const u8> rgba;
    Vec2i size{};
    bool srgb = true;
    bool repeat = true;
    Vec2f uv_scale{1.0f, 1.0f};
    Vec2f uv_offset{};
    bool empty() const { return rgba.empty() || size.x <= 0 || size.y <= 0; }
};

// One copy of a mesh in the bake.
struct BakeSurface3D {
    const MeshData3D* mesh = nullptr;
    Mat4f transform = Mat4f::identity();
    // Its diffuse colour (linear): the base colour, times the vertex colour and `albedo_texture` where they are.
    // A metal reflects no diffuse light: pass its base colour times (1 - metallic).
    LinearRgb albedo{0.8f, 0.8f, 0.8f};
    BakeTexture3D albedo_texture{};
    // The light it gives off, as drawn (Material3D's emission times its strength), times `emission_texture` where it
    // has one: it lights the rest by its bounce.
    LinearRgb emission{};
    BakeTexture3D emission_texture{};
    // With a lightmap (its mesh needs lightmap UVs); else it only hides and bounces light (a prop that will move,
    // a mesh without lightmap UVs), with what the probes (or, near it, the lightmaps) would give it.
    bool lightmapped = true;
    // Times LightBakeSettings3D::texels_per_unit for this copy (a floor seen up close: more; a distant roof: less).
    f32 texel_scale = 1.0f;
    // Light reaches both of its sides (a leaf, a sheet), as Material3D::double_sided; else its back hides and is dark.
    bool double_sided = false;
};

// Where the rays that leave the scene end: the sky. The hemisphere of Lights3D (ambient above, ground_ambient below:
// the light the stock shaders give a surface from it, exactly, where nothing hides the sky), or a panorama in place
// of it (equirectangular, 2:1, linear RGB floats, rows from the top, as Renderer3D::read_linear_texture reads an
// environment's; its colours times `intensity`, turned `rotation` degrees about +y as Lights3D::environment_rotation).
struct BakeSky3D {
    LinearRgb above{0.25f, 0.25f, 0.28f};
    LinearRgb below{0.25f, 0.25f, 0.28f};
    std::span<const f32> panorama;
    Vec2i panorama_size{};
    f32 intensity = 1.0f;
    f32 rotation = 0.0f;
};
// The sky of `lights` without an environment picture: `ambient` above and `ground_ambient` (if enabled) below.
BakeSky3D bake_sky(const Lights3D& lights);

struct LightBakeSettings3D {
    // Lightmap texels a unit of surface, along each side (times each copy's texel_scale), within
    // [min_texels, max_texels] a copy's square side.
    f32 texels_per_unit = 4.0f;
    u32 min_texels = 8;
    u32 max_texels = 512;
    // Each page's side (a power of two, 64 to 4096); copies are packed into as many pages as they need.
    u32 page_size = 1024;
    // Empty texels left round each copy's rectangle on its page, so that filtering never reads a neighbour's.
    u32 padding = 2;
    // Rays a texel and a bounce: more is smoother and slower.
    u32 samples = 128;
    // Bounces of light between surfaces (0: the sky's light and direct light only; at most 8).
    u32 bounces = 3;
    // Smooths what the samples left noisy, among texels of the same surface (edge-aware: normals and positions).
    bool denoise = true;
    // A texel whose rays mostly hit the backs of single-sided faces is inside something (under a wall standing on
    // the floor): it takes its neighbours' light rather than none. The share of back hits that marks it so.
    f32 backface_limit = 0.25f;

    // Probes for what moves.
    bool probes = true;
    // Where: empty, round every lightmapped copy, grown by a spacing.
    Bounds3D probe_area{};
    f32 probe_spacing = 1.0f;  // units; grown where more than max_probes would be needed
    u32 max_probes = 1u << 15; // at most
    u32 probe_samples = 512;   // rays a probe

    u64 seed = 1;
};

// How the bake is run: on `jobs` (none: the calling thread only), stopped early by `stop` (the result is then an
// error), with `progress` called on the calling thread with a share from 0 to 1 (as each stage ends).
struct LightBakeControl3D {
    JobSystem* jobs = nullptr;
    std::stop_token stop{};
    std::function<void(f32 done, std::string_view stage)> progress;
};

// --- What the bake gives --------------------------------------------------------------------------------------------

// A page of lightmap texels: for each, linear RGB, the diffuse light arriving there in the units of Lights3D::ambient
// (a white surface lit by it shows that colour), rows from v = 0.
struct LightmapPage3D {
    Vec2i size{};
    std::vector<f32> rgb;
    LinearRgb at(Vec2i texel) const;
};

// Where a copy's lightmap is: its lightmap UVs times xy plus zw, on page `page` (in the page's own 0-1 square).
struct LightmapPlacement3D {
    u32 page = 0;
    Vec4f scale_offset{0.0f, 0.0f, 0.0f, 0.0f};
    bool placed() const { return scale_offset.x > 0.0f; }
};

// The probes: count[0] x count[1] x count[2] points on a grid filling `area` (corners included), x fastest, then y,
// then z; each 12 floats,
// for red, green then blue the light from all round as a + b.n facing n (linear spherical harmonics, in Lights3D
// ambient's units): a, bx, by, bz.
// `visibility`, 12 floats a probe: for each of six ways (+x, -x, +y, -y, +z, -z), the mean and the mean square of the
// distance its rays went that way before they met a surface: what keeps a probe's light from leaking through a wall
// to a point it cannot see (a probe inside something: 0, it sees nothing).
struct BakedProbes3D {
    Bounds3D area{};
    std::array<u32, 3> count{};
    std::vector<f32> sh;
    std::vector<f32> visibility;
    bool empty() const { return sh.empty(); }
    u32 size() const { return static_cast<u32>(sh.size() / 12); }
    // The light a surface facing `normal` at `position` takes from them, as the shaders read it: from the eight round
    // the point moved a quarter of a spacing along the normal, each weighed trilinearly, by whether it is in front of
    // the surface and by whether it can see the point (Chebyshev's inequality on its distances that way); the
    // position held inside the grid. And 0 to 1 how far it is inside the grid (1 inside, to 0 half a spacing out).
    LinearRgb light(Vec3f position, Vec3f normal) const;
    f32 cover(Vec3f position) const;
};

struct LightBakeStats3D {
    u32 surfaces = 0;
    u32 lightmapped = 0;
    u32 triangles = 0;
    u32 pages = 0;
    u64 texels = 0;        // covered by a surface
    u64 inside_texels = 0; // of those, inside something (filled from their neighbours)
    u32 probes = 0;
    u32 inside_probes = 0;
    u64 rays = 0;
    f64 seconds = 0.0;
};

struct BakedLightingData3D {
    std::vector<LightmapPage3D> pages;
    // One for each BakeSurface3D, in their order: where its lightmap is (not placed() for one without a lightmap).
    std::vector<LightmapPlacement3D> placements;
    BakedProbes3D probes;
    LightBakeStats3D stats;
    bool empty() const { return pages.empty() && probes.empty(); }
};

// Bakes the light of `lights` (each by its LightBake3D: Realtime ones are left out) and of `sky` over `surfaces`.
// Refused for a lightmapped surface whose mesh has no lightmap UVs, invalid meshes or settings, or a stop request.
Result3D<BakedLightingData3D> bake_lighting(std::span<const BakeSurface3D> surfaces, const Lights3D& lights,
                                            const BakeSky3D& sky, const LightBakeSettings3D& settings = {},
                                            const LightBakeControl3D& control = {});

// Writes `baked` into `folder` (made if needed): `lighting.json` (the placements and the probe grid), a Radiance
// picture a page (`lightmap_<n>.hdr`) and `probes.bin` (the probes' floats).
Status3D save_baked_lighting(const BakedLightingData3D& baked, const std::filesystem::path& folder);
// Reads what save_baked_lighting wrote, from `folder`: through `read` (a path to its bytes, or nothing: missing),
// read_content_file by default, so a packed game reads it from its pack.
using BakeFileReader3D = std::function<std::optional<std::string>(const std::filesystem::path&)>;
Result3D<BakedLightingData3D> load_baked_lighting(const std::filesystem::path& folder,
                                                  const BakeFileReader3D& read = {});

// The lights of `lights` as drawn with baked lighting: Baked ones taken out (their light is in the bake), the sun's
// and the fill's colour set to black. The renderer does this itself for a scene with Scene3D::baked_lighting.
Lights3D drawn_lights(const Lights3D& lights);

} // namespace kin
