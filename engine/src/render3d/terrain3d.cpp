#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <kin/assets/image.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/terrain3d.hpp>
#include <kin/renderer/renderer2d.hpp>

extern "C" {
char* kin_zlib_inflate(const char* bytes, int size, int expected, int* out_size);
void kin_import_free(void* pixels);
unsigned char* kin_zlib_deflate(const unsigned char* bytes, int size, int* out_size);
void kin_deflate_free(void* bytes);
}

namespace kin {

namespace {

// A value noise in [0, 1] (the Noise brush's).
f32 hash01(i32 x, i32 z) {
    u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(z) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<f32>((h ^ (h >> 16)) & 0xffffffu) / 16777215.0f;
}
f32 value_noise(f32 x, f32 z) {
    const i32 ix = static_cast<i32>(std::floor(x)), iz = static_cast<i32>(std::floor(z));
    const f32 fx = x - static_cast<f32>(ix), fz = z - static_cast<f32>(iz);
    const f32 ux = fx * fx * (3.0f - 2.0f * fx), uz = fz * fz * (3.0f - 2.0f * fz);
    const f32 a = hash01(ix, iz), b = hash01(ix + 1, iz), c = hash01(ix, iz + 1), d = hash01(ix + 1, iz + 1);
    const f32 top = a + (b - a) * ux, bottom = c + (d - c) * ux;
    return top + (bottom - top) * uz;
}

// The undo's tiles: squares of samples saved as a stroke first touches them.
constexpr i32 undo_tile = 32;

// The surface's variants by name, in SurfaceVariant3D's order (as cmake/kin_embed.cmake names them).
constexpr std::array<const char*, surface_variant_count3d> terrain_variants{"plain",
                                                                            "fog",
                                                                            "lights",
                                                                            "lights_fog",
                                                                            "velocity",
                                                                            "velocity_fog",
                                                                            "velocity_lights",
                                                                            "velocity_lights_fog",
                                                                            "material",
                                                                            "material_fog",
                                                                            "material_lights",
                                                                            "material_lights_fog",
                                                                            "shadow_mask",
                                                                            "pick_mask",
                                                                            "prepass_mask",
                                                                            "prepass_velocity_mask",
                                                                            "visibility_mask",
                                                                            "vertex_mesh",
                                                                            "vertex_mesh_velocity",
                                                                            "vertex_mesh_indexed",
                                                                            "vertex_prepass",
                                                                            "vertex_prepass_indexed",
                                                                            "vertex_prepass_velocity",
                                                                            "vertex_shadow",
                                                                            "vertex_shadow_mask"};
static_assert(terrain_variants.size() == surface_variant_count3d);

// (The GI scene shapes its grid by the height map itself, in the material's colour: Terrain3D's mean colour.)
constexpr SurfaceTraits3D terrain_traits{.emission = false, .gi = SurfaceGi3D::Terrain};

f32 clamp01(f32 v) {
    return std::clamp(v, 0.0f, 1.0f);
}

// `image` resampled (bilinear) to `size`, RGBA8.
std::vector<u8> resample(const Image& image, Vec2i size) {
    std::vector<u8> out(static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * 4);
    for (i32 y = 0; y < size.y; ++y)
        for (i32 x = 0; x < size.x; ++x) {
            const f32 fx =
                (static_cast<f32>(x) + 0.5f) * static_cast<f32>(image.size.x) / static_cast<f32>(size.x) - 0.5f;
            const f32 fy =
                (static_cast<f32>(y) + 0.5f) * static_cast<f32>(image.size.y) / static_cast<f32>(size.y) - 0.5f;
            const i32 x0 = std::clamp(static_cast<i32>(std::floor(fx)), 0, image.size.x - 1);
            const i32 y0 = std::clamp(static_cast<i32>(std::floor(fy)), 0, image.size.y - 1);
            const i32 x1 = std::min(x0 + 1, image.size.x - 1), y1 = std::min(y0 + 1, image.size.y - 1);
            const f32 u = clamp01(fx - static_cast<f32>(x0)), v = clamp01(fy - static_cast<f32>(y0));
            for (int c = 0; c < 4; ++c) {
                const auto at = [&](i32 px, i32 py) {
                    return static_cast<f32>(
                        image.rgba[(static_cast<std::size_t>(py) * static_cast<std::size_t>(image.size.x) +
                                    static_cast<std::size_t>(px)) *
                                       4 +
                                   static_cast<std::size_t>(c)]);
                };
                const f32 value =
                    (at(x0, y0) * (1 - u) + at(x1, y0) * u) * (1 - v) + (at(x0, y1) * (1 - u) + at(x1, y1) * u) * v;
                out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(size.x) + static_cast<std::size_t>(x)) * 4 +
                    static_cast<std::size_t>(c)] = static_cast<u8>(std::lround(std::clamp(value, 0.0f, 255.0f)));
            }
        }
    return out;
}

f32 srgb_to_linear01(f32 c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
f32 linear_to_srgb01(f32 c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

} // namespace

Result3D<SurfaceShader3D> create_terrain_surface(Renderer3D& renderer,
                                                 const std::function<std::span<const u8>(std::string_view)>& spirv) {
    std::array<std::span<const u8>, surface_variant_count3d> variants{};
    Diagnostics3D errors;
    for (u32 i = 0; i < surface_variant_count3d; ++i) {
        const std::string name = std::string("terrain.") + terrain_variants[i] + ".spv";
        variants[i] = spirv ? spirv(name) : std::span<const u8>{};
        // (The cut-outs' are not built: the terrain is opaque.)
        const bool optional = i >= surface_mask_variant3d && i < surface_vertex_variant3d;
        if (variants[i].empty() && !optional)
            errors.push_back("terrain surface: no " + name);
    }
    if (!errors.empty())
        return result3d(SurfaceShader3D{}, std::move(errors));
    auto made = renderer.create_surface_shader(variants);
    if (!made)
        return result3d(SurfaceShader3D{}, std::move(made.diagnostics));
    return result3d(SurfaceShader3D{made->backend(), terrain_traits});
}

Result3D<SurfaceShader3D> compile_terrain_surface(Renderer3D& renderer) {
#ifdef KIN_SHADER_INCLUDE_DIR
    auto made = renderer.compile_surface_shader(std::filesystem::path{KIN_SHADER_INCLUDE_DIR} / "terrain.surface.glsl");
    if (!made)
        return result3d(SurfaceShader3D{}, std::move(made.diagnostics));
    return result3d(SurfaceShader3D{made->backend(), terrain_traits});
#else
    (void)renderer;
    return result3d(SurfaceShader3D{}, Diagnostics3D{"terrain surface: this build has no glslc (shipping, or none "
                                                     "found): build it with kin_terrain_surface"});
#endif
}

struct Terrain3D::State {
    Renderer3D* r3d = nullptr;
    Renderer2D* renderer = nullptr;
    TerrainDesc3D desc;
    TerrainLook3D look;
    std::vector<u16> heights;
    std::vector<u8> texels; // the height map's RGBA8: the height's high and low byte, the normal's x and z
    Texture height_map;
    // The layers' weights, max_terrain_layers3d a sample (0 to 255), and their four RGBA8 maps (layers 0-3 ... 12-15)
    // in the material's metallic-roughness, normal, occlusion and emissive slots; the layers' maps (their albedos,
    // then their normals with roughness in B) in its graph maps.
    std::vector<u8> weights;
    std::array<Texture, 4> weight_maps;
    std::vector<u8> weight_texels; // the four maps' texels, one after another
    u32 layer_count = 0;
    std::array<f32, max_terrain_layers3d> tiles{};
    Vec2i weights_min{0, 0}, weights_max{0, 0};
    std::vector<TerrainLayer3D> layers; // as set (kept for save)
    Vec2i map_size{512, 512};
    std::array<LinearRgb, max_terrain_layers3d> layer_colours{}; // each layer's mean albedo (linear, tinted)

    // The GI's colour for it (Material3D::color: the GI scene's voxels take it, the surface does not): the layers'
    // mean albedos by their shares of all the weights, else the stock look's colour.
    void gi_colour() {
        LinearRgb c = look.color;
        if (layer_count > 0) {
            std::array<f64, max_terrain_layers3d> sums{};
            f64 total = 0;
            const std::size_t samples = weights.size() / max_terrain_layers3d;
            for (std::size_t s = 0; s < samples; s += 7) // (every 7th sample is enough)
                for (u32 l = 0; l < layer_count; ++l) {
                    const f64 w = weights[s * max_terrain_layers3d + l];
                    sums[l] += w;
                    total += w;
                }
            if (total > 0) {
                c = {};
                for (u32 l = 0; l < layer_count; ++l) {
                    const f32 share = static_cast<f32>(sums[l] / total);
                    c = {c.r + layer_colours[l].r * share, c.g + layer_colours[l].g * share,
                         c.b + layer_colours[l].b * share};
                }
            }
        }
        const auto srgb = [](f32 v) { return static_cast<u8>(std::lround(linear_to_srgb01(clamp01(v)) * 255.0f)); };
        material.color = Color{srgb(c.r), srgb(c.g), srgb(c.b), 255};
    }

    // Undo: a stroke is the tiles it touched, each as it was before (undo swaps them back, keeping the after for redo).
    struct Tile {
        i32 x = 0, z = 0; // its first sample
        std::vector<u16> heights;
        std::vector<u8> weights;
    };
    struct Stroke {
        std::vector<Tile> tiles;
        Vec2i min{0, 0}, max{0, 0};
    };
    std::vector<Stroke> undo, redo;
    Stroke current;
    std::vector<bool> saved; // a tile a flag: kept in `current`
    bool in_stroke = false;
    u32 undo_limit = 64;

    Vec2i tiles_count() const {
        return {(desc.samples.x + undo_tile - 1) / undo_tile, (desc.samples.y + undo_tile - 1) / undo_tile};
    }
    // Swaps a tile's saved samples with the terrain's (both directions of undo).
    void swap_tile(Tile& tile) {
        const i32 w = std::min(undo_tile, desc.samples.x - tile.x), h = std::min(undo_tile, desc.samples.y - tile.z);
        for (i32 z = 0; z < h; ++z)
            for (i32 x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(tile.z + z) * static_cast<std::size_t>(desc.samples.x) +
                                      static_cast<std::size_t>(tile.x + x);
                const std::size_t j = static_cast<std::size_t>(z * w + x);
                std::swap(heights[i], tile.heights[j]);
                for (u32 l = 0; l < max_terrain_layers3d; ++l)
                    std::swap(weights[i * max_terrain_layers3d + l], tile.weights[j * max_terrain_layers3d + l]);
            }
    }
    void keep(i32 x0, i32 z0, i32 x1, i32 z1) {
        const Vec2i n = tiles_count();
        if (saved.size() != static_cast<std::size_t>(n.x * n.y))
            saved.assign(static_cast<std::size_t>(n.x * n.y), false);
        for (i32 tz = z0 / undo_tile; tz <= (z1 - 1) / undo_tile; ++tz)
            for (i32 tx = x0 / undo_tile; tx <= (x1 - 1) / undo_tile; ++tx) {
                const std::size_t k = static_cast<std::size_t>(tz * n.x + tx);
                if (saved[k])
                    continue;
                saved[k] = true;
                Tile tile{tx * undo_tile, tz * undo_tile, {}, {}};
                const i32 w = std::min(undo_tile, desc.samples.x - tile.x),
                          h = std::min(undo_tile, desc.samples.y - tile.z);
                tile.heights.resize(static_cast<std::size_t>(w * h));
                tile.weights.resize(static_cast<std::size_t>(w * h) * max_terrain_layers3d);
                for (i32 z = 0; z < h; ++z)
                    for (i32 x = 0; x < w; ++x) {
                        const std::size_t i =
                            static_cast<std::size_t>(tile.z + z) * static_cast<std::size_t>(desc.samples.x) +
                            static_cast<std::size_t>(tile.x + x);
                        tile.heights[static_cast<std::size_t>(z * w + x)] = heights[i];
                        std::copy_n(&weights[i * max_terrain_layers3d], max_terrain_layers3d,
                                    &tile.weights[static_cast<std::size_t>(z * w + x) * max_terrain_layers3d]);
                    }
                current.tiles.push_back(std::move(tile));
            }
        if (current.min.x >= current.max.x) {
            current.min = {x0, z0};
            current.max = {x1, z1};
        } else {
            current.min = {std::min(current.min.x, x0), std::min(current.min.y, z0)};
            current.max = {std::max(current.max.x, x1), std::max(current.max.y, z1)};
        }
    }
    // Everything derived from samples [x0, x1) x [z0, z1) made again (after an edit or an undo).
    void changed(i32 x0, i32 z0, i32 x1, i32 z1) {
        const i32 ax = std::max(0, x0 - 1), az = std::max(0, z0 - 1);
        const i32 bx = std::min(desc.samples.x, x1 + 1), bz = std::min(desc.samples.y, z1 + 1);
        pack(ax, az, bx, bz);
        refit(ax, az, bx, bz);
        safe_ranges();
        params();
        mark(ax, az, bx, bz);
        pack_weights(x0, z0, x1, z1);
        mark_weights(x0, z0, x1, z1);
    }
    Mesh3D grid;
    Material3D material;
    // The nodes' height ranges, a level at a time (level 0 the finest): `nodes[l]` x `nodes[l]` ... as counts.
    std::vector<Vec2i> counts;
    std::vector<std::vector<Vec2f>> ranges; // (min, max) world y a node
    u32 levels = 1;
    // The height map's rectangle not yet uploaded (min inclusive, max exclusive; empty when min >= max).
    Vec2i dirty_min{0, 0}, dirty_max{0, 0};
    std::vector<TerrainNode3D> selected;
    std::vector<MeshInstance3D> instances;
    std::vector<u64> keys;

    f32 step() const { return (desc.max_height - desc.min_height) / 65535.0f; }
    f32 world_y(u16 h) const { return desc.origin.y + desc.min_height + static_cast<f32>(h) * step(); }
    u16 at(i32 x, i32 z) const {
        x = std::clamp(x, 0, desc.samples.x - 1);
        z = std::clamp(z, 0, desc.samples.y - 1);
        return heights[static_cast<std::size_t>(z) * static_cast<std::size_t>(desc.samples.x) + static_cast<u32>(x)];
    }
    f32 leaf_size() const { return static_cast<f32>(desc.node_quads) * desc.spacing; }
    // The ranges and morph actually used. CDLOD draws a level-l node where it reaches into its level's range r, so its
    // far side can lie up to its diagonal d (with its height range) past r; there its coarser neighbour must not yet
    // morph (its morph starts at s 2r) and no neighbour two levels coarser may meet it (it is past 2r): s >= (1 + d/r)
    // / 2 and d/r < 1. The finest nodes' steepest d over their size bounds d/r for every level: the ranges widen until
    // it is at most a half, and the morph starts no earlier than (1 + d/r) / 2.
    f32 lod = 3.0f, morph = 0.75f;
    void safe_ranges() {
        f32 steep = 0.0f; // the finest nodes' greatest height range over their size
        for (const Vec2f r : ranges[0])
            steep = std::max(steep, (r.y - r.x) / leaf_size());
        const f32 diagonal = std::sqrt(2.0f + steep * steep);
        lod = std::max(desc.lod_distance, 2.0f * diagonal);
        morph = std::clamp(std::max(desc.morph_start, 0.5f * (1.0f + diagonal / lod)), 0.0f, 0.95f);
    }
    Vec2f extent() const {
        return {static_cast<f32>(desc.samples.x - 1) * desc.spacing,
                static_cast<f32>(desc.samples.y - 1) * desc.spacing};
    }

    // The texels of samples [x0, x1) x [z0, z1): heights and normals.
    void pack(i32 x0, i32 z0, i32 x1, i32 z1) {
        const f32 inv = 1.0f / (2.0f * desc.spacing);
        for (i32 z = z0; z < z1; ++z)
            for (i32 x = x0; x < x1; ++x) {
                const u16 h = at(x, z);
                const f32 dx = (world_y(at(x + 1, z)) - world_y(at(x - 1, z))) * inv;
                const f32 dz = (world_y(at(x, z + 1)) - world_y(at(x, z - 1))) * inv;
                const f32 len = std::sqrt(dx * dx + dz * dz + 1.0f);
                u8* t = &texels[(static_cast<std::size_t>(z) * static_cast<std::size_t>(desc.samples.x) +
                                 static_cast<std::size_t>(x)) *
                                4];
                t[0] = static_cast<u8>(h >> 8);
                t[1] = static_cast<u8>(h & 0xffu);
                t[2] = static_cast<u8>(std::lround(clamp01(-dx / len * 0.5f + 0.5f) * 255.0f));
                t[3] = static_cast<u8>(std::lround(clamp01(-dz / len * 0.5f + 0.5f) * 255.0f));
            }
    }

    // The finest nodes' height ranges over samples [x0, x1) x [z0, z1), then every level above them.
    void refit(i32 x0, i32 z0, i32 x1, i32 z1) {
        const i32 q = static_cast<i32>(desc.node_quads);
        const i32 nx0 = std::max(0, (x0 - 1) / q), nz0 = std::max(0, (z0 - 1) / q);
        const i32 nx1 = std::min(counts[0].x, (x1 + q - 1) / q + 1), nz1 = std::min(counts[0].y, (z1 + q - 1) / q + 1);
        for (i32 nz = nz0; nz < nz1; ++nz)
            for (i32 nx = nx0; nx < nx1; ++nx) {
                u16 lo = 65535, hi = 0;
                for (i32 z = nz * q; z <= std::min(nz * q + q, desc.samples.y - 1); ++z)
                    for (i32 x = nx * q; x <= std::min(nx * q + q, desc.samples.x - 1); ++x) {
                        lo = std::min(lo, at(x, z));
                        hi = std::max(hi, at(x, z));
                    }
                ranges[0][static_cast<std::size_t>(nz * counts[0].x + nx)] = {world_y(lo), world_y(hi)};
            }
        Vec2i a{nx0, nz0}, b{nx1, nz1};
        for (u32 l = 1; l < levels; ++l) {
            a = {a.x / 2, a.y / 2};
            b = {std::min(counts[l].x, (b.x + 1) / 2), std::min(counts[l].y, (b.y + 1) / 2)};
            for (i32 nz = a.y; nz < b.y; ++nz)
                for (i32 nx = a.x; nx < b.x; ++nx) {
                    Vec2f r{1e30f, -1e30f};
                    for (i32 c = 0; c < 4; ++c) {
                        const i32 cx = nx * 2 + (c & 1), cz = nz * 2 + (c >> 1);
                        if (cx >= counts[l - 1].x || cz >= counts[l - 1].y)
                            continue;
                        const Vec2f cr = ranges[l - 1][static_cast<std::size_t>(cz * counts[l - 1].x + cx)];
                        r = {std::min(r.x, cr.x), std::max(r.y, cr.y)};
                    }
                    ranges[l][static_cast<std::size_t>(nz * counts[l].x + nx)] = r;
                }
        }
    }

    void mark(i32 x0, i32 z0, i32 x1, i32 z1) {
        if (dirty_min.x >= dirty_max.x || dirty_min.y >= dirty_max.y) {
            dirty_min = {x0, z0};
            dirty_max = {x1, z1};
        } else {
            dirty_min = {std::min(dirty_min.x, x0), std::min(dirty_min.y, z0)};
            dirty_max = {std::max(dirty_max.x, x1), std::max(dirty_max.y, z1)};
        }
    }

    void params() {
        auto& p = material.shader_params;
        const auto put = [&](u32 vec, f32 a, f32 b, f32 c, f32 d) {
            p[vec * 4] = a, p[vec * 4 + 1] = b, p[vec * 4 + 2] = c, p[vec * 4 + 3] = d;
        };
        put(0, desc.origin.x, desc.origin.z, desc.spacing, static_cast<f32>(desc.samples.x));
        put(1, static_cast<f32>(desc.samples.y), desc.origin.y + desc.min_height, step(),
            static_cast<f32>(desc.node_quads));
        put(2, lod * leaf_size(), morph, leaf_size(), 0.0f);
        put(3, look.color.r, look.color.g, look.color.b, look.roughness);
        p[2 * 4 + 3] = clamp01(look.blend);
        for (u32 i = 0; i < max_terrain_layers3d; ++i)
            p[16 + i] = tiles[i] > 0.0f ? 1.0f / tiles[i] : 0.25f;
        put(8, static_cast<f32>(layer_count), 0.0f, 0.0f, 0.0f);
    }

    // The weight maps' texels of samples [x0, x1) x [z0, z1).
    void pack_weights(i32 x0, i32 z0, i32 x1, i32 z1) {
        const std::size_t plane =
            static_cast<std::size_t>(desc.samples.x) * static_cast<std::size_t>(desc.samples.y) * 4;
        for (i32 z = z0; z < z1; ++z)
            for (i32 x = x0; x < x1; ++x) {
                const std::size_t i = static_cast<std::size_t>(z) * static_cast<std::size_t>(desc.samples.x) +
                                      static_cast<std::size_t>(x);
                for (u32 l = 0; l < max_terrain_layers3d; ++l)
                    weight_texels[(l / 4) * plane + i * 4 + (l % 4)] = weights[i * max_terrain_layers3d + l];
            }
    }
    void mark_weights(i32 x0, i32 z0, i32 x1, i32 z1) {
        if (weights_min.x >= weights_max.x || weights_min.y >= weights_max.y) {
            weights_min = {x0, z0};
            weights_max = {x1, z1};
        } else {
            weights_min = {std::min(weights_min.x, x0), std::min(weights_min.y, z0)};
            weights_max = {std::max(weights_max.x, x1), std::max(weights_max.y, z1)};
        }
    }

    // A node's box (nullopt: none there, past the terrain's edge).
    std::optional<Bounds3D> box(u32 level, i32 nx, i32 nz) const {
        if (nx < 0 || nz < 0 || nx >= counts[level].x || nz >= counts[level].y)
            return std::nullopt;
        const f32 size = leaf_size() * static_cast<f32>(1u << level);
        const Vec2f e = extent();
        const f32 x0 = desc.origin.x + static_cast<f32>(nx) * size, z0 = desc.origin.z + static_cast<f32>(nz) * size;
        const Vec2f r = ranges[level][static_cast<std::size_t>(nz * counts[level].x + nx)];
        return Bounds3D{{x0, r.x, z0},
                        {std::min(x0 + size, desc.origin.x + e.x), r.y, std::min(z0 + size, desc.origin.z + e.y)}};
    }

    void emit(u32 level, i32 nx, i32 nz, const Bounds3D& b) {
        const f32 size = leaf_size() * static_cast<f32>(1u << level);
        selected.push_back({{b.min.x, b.min.z}, size, level, b.min.y, b.max.y});
        keys.push_back((static_cast<u64>(level) << 48) | (static_cast<u64>(static_cast<u32>(nz)) << 24) |
                       static_cast<u64>(static_cast<u32>(nx)));
    }

    // Strugar's CDLOD selection: false when the node lies past its level's range (its parent then draws its area).
    bool visit(Vec3f eye, u32 level, i32 nx, i32 nz) {
        const auto b = box(level, nx, nz);
        if (!b)
            return true; // (nothing there to draw)
        const auto within = [&](f32 range) {
            const f32 dx = std::max({b->min.x - eye.x, 0.0f, eye.x - b->max.x});
            const f32 dy = std::max({b->min.y - eye.y, 0.0f, eye.y - b->max.y});
            const f32 dz = std::max({b->min.z - eye.z, 0.0f, eye.z - b->max.z});
            return dx * dx + dy * dy + dz * dz <= range * range;
        };
        const f32 finest = lod * leaf_size();
        if (!within(finest * static_cast<f32>(1u << level)))
            return false;
        if (level == 0 || !within(finest * static_cast<f32>(1u << (level - 1)))) {
            emit(level, nx, nz, *b);
            return true;
        }
        for (i32 c = 0; c < 4; ++c) {
            const i32 cx = nx * 2 + (c & 1), cz = nz * 2 + (c >> 1);
            if (!visit(eye, level - 1, cx, cz))
                if (const auto cb = box(level - 1, cx, cz))
                    emit(level - 1, cx, cz, *cb); // (past its level's range: fully morphed, this level's grid)
        }
        return true;
    }
};

Terrain3D::Terrain3D() = default;
Terrain3D::~Terrain3D() = default;
Terrain3D::Terrain3D(Terrain3D&&) noexcept = default;
Terrain3D& Terrain3D::operator=(Terrain3D&&) noexcept = default;

Result3D<Terrain3D> Terrain3D::create(Renderer3D& renderer, SurfaceShader3D surface, const TerrainDesc3D& desc,
                                      std::span<const u16> heights) {
    const auto fail = [](std::string why) { return result3d(Terrain3D{}, Diagnostics3D{"Terrain3D::create: " + why}); };
    if (!surface.valid())
        return fail("no terrain surface (create_terrain_surface or compile_terrain_surface)");
    if (desc.samples.x < 2 || desc.samples.y < 2 || desc.samples.x > 8193 || desc.samples.y > 8193)
        return fail("samples must be 2 to 8193 a side");
    if (!(desc.spacing > 0.0f) || !std::isfinite(desc.spacing))
        return fail("the spacing must be above 0");
    if (!(desc.max_height > desc.min_height))
        return fail("max_height must be above min_height");
    if (desc.node_quads < 4 || desc.node_quads > 128 || (desc.node_quads & (desc.node_quads - 1)) != 0)
        return fail("node_quads must be a power of two from 4 to 128");
    if (!(desc.lod_distance >= 1.0f))
        return fail("lod_distance must be at least 1");
    const std::size_t count = static_cast<std::size_t>(desc.samples.x) * static_cast<std::size_t>(desc.samples.y);
    if (!heights.empty() && heights.size() != count)
        return fail("heights must be samples.x * samples.y");

    Terrain3D t;
    t._s = std::make_unique<State>();
    State& s = *t._s;
    s.r3d = &renderer;
    s.renderer = &renderer.renderer();
    s.desc = desc;
    s.heights.assign(count, 0);
    if (!heights.empty())
        std::copy(heights.begin(), heights.end(), s.heights.begin());
    s.texels.assign(count * 4, 0);
    s.pack(0, 0, desc.samples.x, desc.samples.y);

    // The quadtree: as many levels as halve the finest nodes' grid down to one node.
    const i32 q = static_cast<i32>(desc.node_quads);
    Vec2i n{std::max(1, (desc.samples.x - 1 + q - 1) / q), std::max(1, (desc.samples.y - 1 + q - 1) / q)};
    s.counts.push_back(n);
    while (n.x > 1 || n.y > 1) {
        n = {(n.x + 1) / 2, (n.y + 1) / 2};
        s.counts.push_back(n);
    }
    s.levels = static_cast<u32>(s.counts.size());
    for (const Vec2i c : s.counts)
        s.ranges.emplace_back(static_cast<std::size_t>(c.x) * static_cast<std::size_t>(c.y), Vec2f{});
    s.refit(0, 0, desc.samples.x, desc.samples.y);
    s.safe_ranges();

    // The grid: its quads over the unit square, vertices at y 0; two more, used by no triangle, at y 0 and 1 give it
    // a unit height, which each copy's scale makes its node's height range.
    MeshData3D grid;
    const u32 row = desc.node_quads + 1;
    for (u32 z = 0; z <= desc.node_quads; ++z)
        for (u32 x = 0; x <= desc.node_quads; ++x) {
            const f32 u = static_cast<f32>(x) / static_cast<f32>(desc.node_quads);
            const f32 v = static_cast<f32>(z) / static_cast<f32>(desc.node_quads);
            grid.vertices.push_back({{u, 0.0f, v}, {0.0f, 1.0f, 0.0f}, {u, v}});
        }
    for (u32 z = 0; z < desc.node_quads; ++z)
        for (u32 x = 0; x < desc.node_quads; ++x) {
            const u32 a = z * row + x, b = a + 1, c = a + row + 1, d = a + row;
            grid.indices.insert(grid.indices.end(), {a, d, c, a, c, b});
        }
    grid.vertices.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}});
    grid.vertices.push_back({{1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}});
    auto mesh = renderer.create_mesh(grid);
    if (!mesh)
        return fail("the grid mesh: " + diagnostics3d_text(mesh.diagnostics));
    s.grid = std::move(*mesh);

    s.height_map = s.renderer->create_texture(desc.samples, TextureFormat::Rgba8, s.texels.data());
    if (!s.height_map)
        return fail("the height map's texture could not be made");
    s.material.surface = std::move(surface);
    s.material.texture = s.height_map;
    s.material.texture_samplers[static_cast<u32>(MaterialMap3D::BaseColor)] = TextureSampler3D{
        Wrap3D::Clamp, Wrap3D::Clamp, TextureFilter3D::Nearest, TextureFilter3D::Nearest, MipFilter3D::None};
    // The weights: all the first layer's.
    s.weights.assign(count * max_terrain_layers3d, 0);
    for (std::size_t i = 0; i < count; ++i)
        s.weights[i * max_terrain_layers3d] = 255;
    s.weight_texels.assign(count * 4 * 4, 0);
    s.pack_weights(0, 0, desc.samples.x, desc.samples.y);
    const TextureSampler3D linear{Wrap3D::Clamp, Wrap3D::Clamp, TextureFilter3D::Linear, TextureFilter3D::Linear,
                                  MipFilter3D::None};
    constexpr std::array<MaterialMap3D, 4> slots{MaterialMap3D::MetallicRoughness, MaterialMap3D::Normal,
                                                 MaterialMap3D::Occlusion, MaterialMap3D::Emissive};
    for (u32 m = 0; m < 4; ++m) {
        s.weight_maps[m] =
            s.renderer->create_texture(desc.samples, TextureFormat::Rgba8, &s.weight_texels[m * count * 4]);
        if (!s.weight_maps[m])
            return fail("the weight maps' textures could not be made");
        s.material.texture_samplers[static_cast<u32>(slots[m])] = linear;
    }
    s.material.metallic_roughness = s.weight_maps[0];
    s.material.normal = s.weight_maps[1];
    s.material.occlusion = s.weight_maps[2];
    s.material.emissive_texture = s.weight_maps[3];
    s.material.emissive = 0.0f;
    s.material.roughness = s.look.roughness;
    s.tiles.fill(4.0f);
    s.params();
    s.gi_colour();
    return result3d(std::move(t));
}

bool Terrain3D::valid() const {
    return _s && _s->material.surface.valid();
}
const TerrainDesc3D& Terrain3D::desc() const {
    static const TerrainDesc3D none{};
    return _s ? _s->desc : none;
}
void Terrain3D::set_origin(Vec3f origin) {
    if (!_s || origin == _s->desc.origin)
        return;
    State& s = *_s;
    s.desc.origin = origin;
    s.refit(0, 0, s.desc.samples.x, s.desc.samples.y);
    s.safe_ranges();
    s.params();
}

Vec2f Terrain3D::size() const {
    return _s ? _s->extent() : Vec2f{};
}
std::span<const u16> Terrain3D::heights() const {
    return _s ? std::span<const u16>{_s->heights} : std::span<const u16>{};
}

std::optional<f32> Terrain3D::height(Vec2f xz) const {
    if (!_s)
        return std::nullopt;
    const State& s = *_s;
    const f32 fx = (xz.x - s.desc.origin.x) / s.desc.spacing, fz = (xz.y - s.desc.origin.z) / s.desc.spacing;
    if (!(fx >= 0.0f && fz >= 0.0f && fx <= static_cast<f32>(s.desc.samples.x - 1) &&
          fz <= static_cast<f32>(s.desc.samples.y - 1)))
        return std::nullopt;
    const i32 x = std::min(static_cast<i32>(fx), s.desc.samples.x - 2);
    const i32 z = std::min(static_cast<i32>(fz), s.desc.samples.y - 2);
    const f32 u = fx - static_cast<f32>(x), v = fz - static_cast<f32>(z);
    const f32 a = s.world_y(s.at(x, z)), b = s.world_y(s.at(x + 1, z));
    const f32 c = s.world_y(s.at(x, z + 1)), d = s.world_y(s.at(x + 1, z + 1));
    return (a * (1.0f - u) + b * u) * (1.0f - v) + (c * (1.0f - u) + d * u) * v;
}

std::optional<Vec3f> Terrain3D::normal(Vec2f xz) const {
    if (!height(xz))
        return std::nullopt;
    const f32 e = _s->desc.spacing;
    const auto h = [&](f32 dx, f32 dz) { return height({xz.x + dx, xz.y + dz}).value_or(*height(xz)); };
    const f32 gx = (h(e, 0.0f) - h(-e, 0.0f)) / (2.0f * e), gz = (h(0.0f, e) - h(0.0f, -e)) / (2.0f * e);
    return normalize(Vec3f{-gx, 1.0f, -gz});
}

std::optional<TerrainHit3D> Terrain3D::raycast(Vec3f from, Vec3f direction, f32 max_distance) const {
    if (!_s)
        return std::nullopt;
    const State& s = *_s;
    const f32 len = length(direction);
    if (!(len > 0.0f))
        return std::nullopt;
    const Vec3f d = direction / len;
    // Into the terrain's box first.
    const Vec2f e = s.extent();
    const Vec2f r = s.ranges.back().front();
    const Vec3f lo{s.desc.origin.x, r.x, s.desc.origin.z}, hi{s.desc.origin.x + e.x, r.y, s.desc.origin.z + e.y};
    f32 t0 = 0.0f, t1 = max_distance;
    for (int a = 0; a < 3; ++a) {
        const f32 o = a == 0 ? from.x : a == 1 ? from.y : from.z, v = a == 0 ? d.x : a == 1 ? d.y : d.z;
        const f32 l = a == 0 ? lo.x : a == 1 ? lo.y : lo.z, h = a == 0 ? hi.x : a == 1 ? hi.y : hi.z;
        if (std::abs(v) < 1e-12f) {
            if (o < l || o > h)
                return std::nullopt;
            continue;
        }
        f32 ta = (l - o) / v, tb = (h - o) / v;
        if (ta > tb)
            std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1)
            return std::nullopt;
    }
    const f32 step = s.desc.spacing * 0.5f;
    const auto above = [&](f32 t) {
        const Vec3f p = from + d * t;
        const auto h = height({p.x, p.z});
        return !h || p.y >= *h;
    };
    f32 prev = t0;
    if (!above(prev))
        return TerrainHit3D{from + d * prev,
                            normal({(from + d * prev).x, (from + d * prev).z}).value_or(Vec3f{0, 1, 0}), prev};
    for (f32 t = t0 + step;; t += step) {
        const f32 at = std::min(t, t1);
        if (!above(at)) {
            f32 a = prev, b = at;
            for (int i = 0; i < 24; ++i) {
                const f32 m = 0.5f * (a + b);
                (above(m) ? a : b) = m;
            }
            const Vec3f p = from + d * b;
            return TerrainHit3D{p, normal({p.x, p.z}).value_or(Vec3f{0, 1, 0}), b};
        }
        if (at >= t1)
            return std::nullopt;
        prev = at;
    }
}

bool Terrain3D::set_heights(Vec2i at, Vec2i size, std::span<const u16> values) {
    if (!_s || size.x <= 0 || size.y <= 0 || at.x < 0 || at.y < 0 || at.x + size.x > _s->desc.samples.x ||
        at.y + size.y > _s->desc.samples.y ||
        values.size() != static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y))
        return false;
    State& s = *_s;
    for (i32 z = 0; z < size.y; ++z)
        std::memcpy(&s.heights[static_cast<std::size_t>(at.y + z) * static_cast<std::size_t>(s.desc.samples.x) +
                               static_cast<std::size_t>(at.x)],
                    &values[static_cast<std::size_t>(z) * static_cast<std::size_t>(size.x)],
                    static_cast<std::size_t>(size.x) * sizeof(u16));
    // (A sample's normal reads its neighbours: one more round.)
    const i32 x0 = std::max(0, at.x - 1), z0 = std::max(0, at.y - 1);
    const i32 x1 = std::min(s.desc.samples.x, at.x + size.x + 1), z1 = std::min(s.desc.samples.y, at.y + size.y + 1);
    s.pack(x0, z0, x1, z1);
    s.refit(x0, z0, x1, z1);
    s.safe_ranges();
    s.params();
    s.mark(x0, z0, x1, z1);
    return true;
}

bool Terrain3D::update() {
    if (!_s)
        return false;
    State& s = *_s;
    bool ok = true;
    if (s.dirty_min.x < s.dirty_max.x && s.dirty_min.y < s.dirty_max.y) {
        const Vec2i size{s.dirty_max.x - s.dirty_min.x, s.dirty_max.y - s.dirty_min.y};
        std::vector<u8> rect(static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * 4);
        for (i32 z = 0; z < size.y; ++z)
            std::memcpy(
                &rect[static_cast<std::size_t>(z) * static_cast<std::size_t>(size.x) * 4],
                &s.texels[(static_cast<std::size_t>(s.dirty_min.y + z) * static_cast<std::size_t>(s.desc.samples.x) +
                           static_cast<std::size_t>(s.dirty_min.x)) *
                          4],
                static_cast<std::size_t>(size.x) * 4);
        ok = s.renderer->update_texture(s.height_map, s.dirty_min, size, rect.data());
        if (ok)
            s.dirty_min = s.dirty_max = {0, 0};
    }
    if (s.weights_min.x < s.weights_max.x && s.weights_min.y < s.weights_max.y) {
        const Vec2i wsize{s.weights_max.x - s.weights_min.x, s.weights_max.y - s.weights_min.y};
        const std::size_t plane =
            static_cast<std::size_t>(s.desc.samples.x) * static_cast<std::size_t>(s.desc.samples.y) * 4;
        std::vector<u8> part(static_cast<std::size_t>(wsize.x) * static_cast<std::size_t>(wsize.y) * 4);
        bool all = true;
        for (u32 m = 0; m < 4; ++m) {
            for (i32 z = 0; z < wsize.y; ++z)
                std::memcpy(&part[static_cast<std::size_t>(z) * static_cast<std::size_t>(wsize.x) * 4],
                            &s.weight_texels[m * plane + (static_cast<std::size_t>(s.weights_min.y + z) *
                                                              static_cast<std::size_t>(s.desc.samples.x) +
                                                          static_cast<std::size_t>(s.weights_min.x)) *
                                                             4],
                            static_cast<std::size_t>(wsize.x) * 4);
            all = s.renderer->update_texture(s.weight_maps[m], s.weights_min, wsize, part.data()) && all;
        }
        if (all)
            s.weights_min = s.weights_max = {0, 0};
        ok = ok && all;
    }
    return ok;
}

Status3D Terrain3D::set_layers(std::span<const TerrainLayer3D> layers, Vec2i map_size) {
    if (!_s)
        return status3d(false, {"Terrain3D::set_layers: an empty terrain"});
    State& s = *_s;
    if (layers.size() > max_terrain_layers3d)
        return status3d(false, {"Terrain3D::set_layers: at most " + std::to_string(max_terrain_layers3d) + " layers"});
    if (map_size.x < 1 || map_size.y < 1 || map_size.x > 4096 || map_size.y > 4096)
        return status3d(false, {"Terrain3D::set_layers: map_size must be 1 to 4096 a side"});
    if (layers.empty()) {
        s.layer_count = 0;
        s.layers.clear();
        s.material.graph_maps = {};
        s.gi_colour();
        s.params();
        return status3d(true);
    }
    // The albedos (times the tint, sRGB; the height in alpha), then the normals (xy in RG, roughness in B).
    const std::size_t texels = static_cast<std::size_t>(map_size.x) * static_cast<std::size_t>(map_size.y);
    std::vector<std::vector<u8>> pictures;
    for (const TerrainLayer3D& l : layers) {
        std::vector<u8> albedo = l.albedo.valid() ? resample(l.albedo, map_size) : std::vector<u8>(texels * 4, 255);
        if (!l.albedo.valid())
            for (std::size_t i = 0; i < texels; ++i)
                albedo[i * 4 + 3] = 128;
        const std::array<f32, 3> tint{l.tint.r, l.tint.g, l.tint.b};
        for (std::size_t i = 0; i < texels; ++i)
            for (int c = 0; c < 3; ++c) {
                const f32 lin =
                    srgb_to_linear01(static_cast<f32>(albedo[i * 4 + static_cast<std::size_t>(c)]) / 255.0f) *
                    std::max(tint[static_cast<std::size_t>(c)], 0.0f);
                albedo[i * 4 + static_cast<std::size_t>(c)] =
                    static_cast<u8>(std::lround(linear_to_srgb01(clamp01(lin)) * 255.0f));
            }
        LinearRgb mean{};
        for (std::size_t i = 0; i < texels; ++i) {
            mean.r += srgb_to_linear01(static_cast<f32>(albedo[i * 4]) / 255.0f);
            mean.g += srgb_to_linear01(static_cast<f32>(albedo[i * 4 + 1]) / 255.0f);
            mean.b += srgb_to_linear01(static_cast<f32>(albedo[i * 4 + 2]) / 255.0f);
        }
        s.layer_colours[pictures.size()] = {mean.r / static_cast<f32>(texels), mean.g / static_cast<f32>(texels),
                                            mean.b / static_cast<f32>(texels)};
        pictures.push_back(std::move(albedo));
    }
    for (const TerrainLayer3D& l : layers) {
        std::vector<u8> normal = l.normal.valid() ? resample(l.normal, map_size) : std::vector<u8>(texels * 4, 128);
        const u8 rough = static_cast<u8>(std::lround(std::clamp(l.roughness, 0.045f, 1.0f) * 255.0f));
        for (std::size_t i = 0; i < texels; ++i) {
            normal[i * 4 + 2] = rough;
            normal[i * 4 + 3] = 255;
        }
        pictures.push_back(std::move(normal));
    }
    std::vector<const u8*> pointers;
    for (const auto& p : pictures)
        pointers.push_back(p.data());
    Texture maps = s.r3d->create_map_array(map_size, pointers);
    if (!maps)
        return status3d(false, {"Terrain3D::set_layers: the layers' texture array could not be made"});
    s.material.graph_maps = maps;
    s.layer_count = static_cast<u32>(layers.size());
    s.layers.assign(layers.begin(), layers.end());
    s.map_size = map_size;
    s.gi_colour();
    s.tiles.fill(4.0f);
    for (std::size_t i = 0; i < layers.size(); ++i)
        s.tiles[i] = layers[i].tile > 0.0f ? layers[i].tile : 4.0f;
    s.params();
    return status3d(true);
}

u32 Terrain3D::layer_count() const {
    return _s ? _s->layer_count : 0;
}

bool Terrain3D::set_weights(u32 layer, Vec2i at, Vec2i size, std::span<const u8> values) {
    if (!_s || layer >= max_terrain_layers3d || size.x <= 0 || size.y <= 0 || at.x < 0 || at.y < 0 ||
        at.x + size.x > _s->desc.samples.x || at.y + size.y > _s->desc.samples.y ||
        values.size() != static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y))
        return false;
    State& s = *_s;
    for (i32 z = 0; z < size.y; ++z)
        for (i32 x = 0; x < size.x; ++x)
            s.weights[(static_cast<std::size_t>(at.y + z) * static_cast<std::size_t>(s.desc.samples.x) +
                       static_cast<std::size_t>(at.x + x)) *
                          max_terrain_layers3d +
                      layer] =
                values[static_cast<std::size_t>(z) * static_cast<std::size_t>(size.x) + static_cast<std::size_t>(x)];
    s.pack_weights(at.x, at.y, at.x + size.x, at.y + size.y);
    s.mark_weights(at.x, at.y, at.x + size.x, at.y + size.y);
    s.gi_colour();
    return true;
}

std::array<u8, max_terrain_layers3d> Terrain3D::weights(Vec2i sample) const {
    std::array<u8, max_terrain_layers3d> out{};
    if (!_s || sample.x < 0 || sample.y < 0 || sample.x >= _s->desc.samples.x || sample.y >= _s->desc.samples.y)
        return out;
    const std::size_t i = static_cast<std::size_t>(sample.y) * static_cast<std::size_t>(_s->desc.samples.x) +
                          static_cast<std::size_t>(sample.x);
    std::copy_n(&_s->weights[i * max_terrain_layers3d], max_terrain_layers3d, out.begin());
    return out;
}

void Terrain3D::set_look(const TerrainLook3D& look) {
    if (!_s)
        return;
    _s->look = look;
    _s->params();
    _s->gi_colour();
}
const TerrainLook3D& Terrain3D::look() const {
    static const TerrainLook3D none{};
    return _s ? _s->look : none;
}

std::span<const TerrainNode3D> Terrain3D::select(Vec3f eye) {
    if (!_s)
        return {};
    State& s = *_s;
    s.selected.clear();
    s.keys.clear();
    const u32 top = s.levels - 1;
    if (!s.visit(eye, top, 0, 0))
        if (const auto b = s.box(top, 0, 0))
            s.emit(top, 0, 0, *b); // (farther than every range: the whole terrain at its coarsest)
    return s.selected;
}

Status3D Terrain3D::draw(Scene3D& scene, u32 id) {
    if (!_s)
        return status3d(false, {"Terrain3D::draw: an empty terrain"});
    State& s = *_s;
    if (!update())
        return status3d(false, {"Terrain3D::draw: the height map's upload failed"});
    (void)select(scene.camera.position);
    s.instances.clear();
    for (std::size_t i = 0; i < s.selected.size(); ++i) {
        const TerrainNode3D& n = s.selected[i];
        const f32 rise = std::max(n.max_y - n.min_y, 1e-3f);
        s.instances.push_back({.transform = Mat4f::translation({n.corner.x, n.min_y, n.corner.y}) *
                                            Mat4f::scaling({n.size, rise, n.size}),
                               .id = id,
                               .history_id = s.keys[i] + 1});
    }
    return scene.draw_instanced(s.grid, s.instances, s.material);
}

const Material3D& Terrain3D::material() const {
    static const Material3D none{};
    return _s ? _s->material : none;
}
const Mesh3D& Terrain3D::grid() const {
    static const Mesh3D none{};
    return _s ? _s->grid : none;
}

Terrain3D::Changed Terrain3D::brush(const TerrainBrush3D& b, Vec2f center, f32 amount) {
    if (!_s || !(b.radius > 0.0f) || !std::isfinite(center.x) || !std::isfinite(center.y))
        return {};
    State& s = *_s;
    const TerrainDesc3D& d = s.desc;
    const i32 x0 = std::max(0, static_cast<i32>(std::floor((center.x - b.radius - d.origin.x) / d.spacing)));
    const i32 z0 = std::max(0, static_cast<i32>(std::floor((center.y - b.radius - d.origin.z) / d.spacing)));
    const i32 x1 =
        std::min(d.samples.x, static_cast<i32>(std::ceil((center.x + b.radius - d.origin.x) / d.spacing)) + 1);
    const i32 z1 =
        std::min(d.samples.y, static_cast<i32>(std::ceil((center.y + b.radius - d.origin.z) / d.spacing)) + 1);
    if (x0 >= x1 || z0 >= z1)
        return {};
    const bool own = !s.in_stroke;
    if (own)
        begin_stroke();
    s.keep(x0, z0, x1, z1);
    const f32 inner = 1.0f - std::clamp(b.falloff, 0.0f, 1.0f);
    const auto weight = [&](i32 x, i32 z) {
        const f32 wx = d.origin.x + static_cast<f32>(x) * d.spacing - center.x;
        const f32 wz = d.origin.z + static_cast<f32>(z) * d.spacing - center.y;
        const f32 r = std::sqrt(wx * wx + wz * wz) / b.radius;
        if (r >= 1.0f)
            return 0.0f;
        if (r <= inner)
            return 1.0f;
        const f32 t = (r - inner) / std::max(1.0f - inner, 1e-6f);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    };
    const f32 k = b.strength * amount;
    const std::size_t row = static_cast<std::size_t>(d.samples.x);
    if (b.mode == TerrainBrushMode3D::Paint) {
        const u32 layer = std::min(b.layer, max_terrain_layers3d - 1);
        for (i32 z = z0; z < z1; ++z)
            for (i32 x = x0; x < x1; ++x) {
                const f32 f = weight(x, z) * k;
                if (f <= 0.0f)
                    continue;
                u8* w = &s.weights[(static_cast<std::size_t>(z) * row + static_cast<std::size_t>(x)) *
                                   max_terrain_layers3d];
                // As shares of the whole: this layer's up by f, the rest scaled to fill what is left.
                f32 sum = 0.0f;
                for (u32 l = 0; l < max_terrain_layers3d; ++l)
                    sum += w[l];
                std::array<f32, max_terrain_layers3d> share{};
                for (u32 l = 0; l < max_terrain_layers3d; ++l)
                    share[l] = sum > 0.0f ? w[l] / sum : (l == 0 ? 1.0f : 0.0f);
                const f32 mine = std::min(1.0f, share[layer] + f), rest = 1.0f - share[layer];
                for (u32 l = 0; l < max_terrain_layers3d; ++l)
                    share[l] = l == layer ? mine : (rest > 1e-6f ? share[l] * (1.0f - mine) / rest : 0.0f);
                for (u32 l = 0; l < max_terrain_layers3d; ++l)
                    w[l] = static_cast<u8>(std::lround(share[l] * 255.0f));
            }
    } else {
        const f32 step = s.step();
        const auto to_u16 = [](f32 v) { return static_cast<u16>(std::lround(std::clamp(v, 0.0f, 65535.0f))); };
        // (Smooth reads the samples as they were before this dab.)
        std::vector<u16> before;
        if (b.mode == TerrainBrushMode3D::Smooth) {
            before.reserve(static_cast<std::size_t>((x1 - x0 + 2) * (z1 - z0 + 2)));
            for (i32 z = z0 - 1; z <= z1; ++z)
                for (i32 x = x0 - 1; x <= x1; ++x)
                    before.push_back(s.at(x, z));
        }
        const i32 bw = x1 - x0 + 2;
        const f32 target = (b.height - d.origin.y - d.min_height) / step;
        for (i32 z = z0; z < z1; ++z)
            for (i32 x = x0; x < x1; ++x) {
                const f32 f = weight(x, z);
                if (f <= 0.0f)
                    continue;
                u16& h = s.heights[static_cast<std::size_t>(z) * row + static_cast<std::size_t>(x)];
                const f32 v = static_cast<f32>(h);
                switch (b.mode) {
                case TerrainBrushMode3D::Raise:
                    h = to_u16(v + f * k / step);
                    break;
                case TerrainBrushMode3D::Lower:
                    h = to_u16(v - f * k / step);
                    break;
                case TerrainBrushMode3D::Smooth: {
                    f32 mean = 0.0f;
                    for (i32 dz = -1; dz <= 1; ++dz)
                        for (i32 dx = -1; dx <= 1; ++dx)
                            mean += before[static_cast<std::size_t>((z - z0 + 1 + dz) * bw + (x - x0 + 1 + dx))];
                    h = to_u16(v + (mean / 9.0f - v) * std::clamp(f * k, 0.0f, 1.0f));
                    break;
                }
                case TerrainBrushMode3D::Flatten:
                    h = to_u16(v + (target - v) * std::clamp(f * k, 0.0f, 1.0f));
                    break;
                case TerrainBrushMode3D::Noise: {
                    const f32 wx = (d.origin.x + static_cast<f32>(x) * d.spacing) / std::max(b.scale, 1e-3f);
                    const f32 wz = (d.origin.z + static_cast<f32>(z) * d.spacing) / std::max(b.scale, 1e-3f);
                    h = to_u16(v + (value_noise(wx, wz) * 2.0f - 1.0f) * f * k / step);
                    break;
                }
                case TerrainBrushMode3D::Paint:
                    break;
                }
            }
    }
    s.changed(x0, z0, x1, z1);
    if (own)
        (void)end_stroke();
    return {{x0, z0}, {x1 - x0, z1 - z0}};
}

void Terrain3D::begin_stroke() {
    if (!_s || _s->in_stroke)
        return;
    _s->in_stroke = true;
    _s->current = {};
    _s->saved.assign(static_cast<std::size_t>(_s->tiles_count().x * _s->tiles_count().y), false);
}

Terrain3D::Changed Terrain3D::end_stroke() {
    if (!_s || !_s->in_stroke)
        return {};
    State& s = *_s;
    s.in_stroke = false;
    if (s.current.tiles.empty())
        return {};
    const Changed out{s.current.min, {s.current.max.x - s.current.min.x, s.current.max.y - s.current.min.y}};
    s.undo.push_back(std::move(s.current));
    s.current = {};
    s.redo.clear();
    while (s.undo.size() > s.undo_limit)
        s.undo.erase(s.undo.begin());
    return out;
}

std::optional<Terrain3D::Changed> Terrain3D::undo() {
    if (!_s || _s->in_stroke || _s->undo.empty())
        return std::nullopt;
    State& s = *_s;
    State::Stroke stroke = std::move(s.undo.back());
    s.undo.pop_back();
    for (auto& tile : stroke.tiles)
        s.swap_tile(tile);
    s.changed(stroke.min.x, stroke.min.y, stroke.max.x, stroke.max.y);
    const Changed out{stroke.min, {stroke.max.x - stroke.min.x, stroke.max.y - stroke.min.y}};
    s.redo.push_back(std::move(stroke));
    return out;
}

std::optional<Terrain3D::Changed> Terrain3D::redo() {
    if (!_s || _s->in_stroke || _s->redo.empty())
        return std::nullopt;
    State& s = *_s;
    State::Stroke stroke = std::move(s.redo.back());
    s.redo.pop_back();
    for (auto& tile : stroke.tiles)
        s.swap_tile(tile);
    s.changed(stroke.min.x, stroke.min.y, stroke.max.x, stroke.max.y);
    const Changed out{stroke.min, {stroke.max.x - stroke.min.x, stroke.max.y - stroke.min.y}};
    s.undo.push_back(std::move(stroke));
    return out;
}

bool Terrain3D::can_undo() const {
    return _s && !_s->undo.empty();
}
bool Terrain3D::can_redo() const {
    return _s && !_s->redo.empty();
}
void Terrain3D::set_undo_limit(u32 strokes) {
    if (!_s)
        return;
    _s->undo_limit = std::max(strokes, 1u);
    while (_s->undo.size() > _s->undo_limit)
        _s->undo.erase(_s->undo.begin());
}

namespace {
// The .kinterrain file's little-endian fields.
struct Writer {
    std::vector<u8> bytes;
    void u32v(u32 v) {
        for (int i = 0; i < 4; ++i)
            bytes.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void f32v(f32 v) {
        u32 b = 0;
        std::memcpy(&b, &v, 4);
        u32v(b);
    }
    void raw(const void* p, std::size_t n) {
        const auto* c = static_cast<const u8*>(p);
        bytes.insert(bytes.end(), c, c + n);
    }
    void image(const Image& i) {
        u32v(static_cast<u32>(i.valid() ? i.size.x : 0));
        u32v(static_cast<u32>(i.valid() ? i.size.y : 0));
        if (i.valid())
            raw(i.rgba.data(), i.rgba.size());
    }
};
struct Reader {
    std::span<const u8> bytes;
    std::size_t at = 0;
    bool ok = true;
    u32 u32v() {
        if (at + 4 > bytes.size()) {
            ok = false;
            return 0;
        }
        u32 v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<u32>(bytes[at + static_cast<std::size_t>(i)]) << (8 * i);
        at += 4;
        return v;
    }
    f32 f32v() {
        const u32 b = u32v();
        f32 v = 0;
        std::memcpy(&v, &b, 4);
        return v;
    }
    bool raw(void* p, std::size_t n) {
        if (at + n > bytes.size())
            return ok = false;
        std::memcpy(p, bytes.data() + at, n);
        at += n;
        return true;
    }
    Image image() {
        Image i;
        const u32 w = u32v(), h = u32v();
        if (!ok || w == 0 || h == 0)
            return i;
        if (w > 16384 || h > 16384) {
            ok = false;
            return i;
        }
        i.size = {static_cast<i32>(w), static_cast<i32>(h)};
        i.rgba.resize(static_cast<std::size_t>(w) * h * 4);
        if (!raw(i.rgba.data(), i.rgba.size()))
            return {};
        return i;
    }
};
constexpr u32 kinterrain_version = 1;
} // namespace

Status3D Terrain3D::save(const std::filesystem::path& file) const {
    if (!_s)
        return status3d(false, {"Terrain3D::save: an empty terrain"});
    const State& s = *_s;
    Writer w;
    const TerrainDesc3D& d = s.desc;
    w.u32v(static_cast<u32>(d.samples.x));
    w.u32v(static_cast<u32>(d.samples.y));
    w.f32v(d.spacing);
    w.f32v(d.origin.x);
    w.f32v(d.origin.y);
    w.f32v(d.origin.z);
    w.f32v(d.min_height);
    w.f32v(d.max_height);
    w.u32v(d.node_quads);
    w.f32v(d.lod_distance);
    w.f32v(d.morph_start);
    w.f32v(s.look.color.r);
    w.f32v(s.look.color.g);
    w.f32v(s.look.color.b);
    w.f32v(s.look.roughness);
    w.f32v(s.look.blend);
    w.raw(s.heights.data(), s.heights.size() * sizeof(u16));
    w.raw(s.weights.data(), s.weights.size());
    w.u32v(static_cast<u32>(s.layers.size()));
    w.u32v(static_cast<u32>(s.map_size.x));
    w.u32v(static_cast<u32>(s.map_size.y));
    for (const TerrainLayer3D& l : s.layers) {
        w.f32v(l.tint.r);
        w.f32v(l.tint.g);
        w.f32v(l.tint.b);
        w.f32v(l.roughness);
        w.f32v(l.tile);
        w.image(l.albedo);
        w.image(l.normal);
    }
    int size = 0;
    unsigned char* deflated = kin_zlib_deflate(w.bytes.data(), static_cast<int>(w.bytes.size()), &size);
    if (!deflated)
        return status3d(false, {"Terrain3D::save: out of memory"});
    std::ofstream out(file, std::ios::binary);
    Writer head;
    head.raw("KINTERRN", 8);
    head.u32v(kinterrain_version);
    head.u32v(static_cast<u32>(w.bytes.size()));
    out.write(reinterpret_cast<const char*>(head.bytes.data()), static_cast<std::streamsize>(head.bytes.size()));
    out.write(reinterpret_cast<const char*>(deflated), size);
    kin_deflate_free(deflated);
    if (!out)
        return status3d(false, {"Terrain3D::save: " + file.string() + " could not be written"});
    return status3d(true);
}

Result3D<Terrain3D> Terrain3D::load(Renderer3D& renderer, SurfaceShader3D surface, const std::filesystem::path& file) {
    const auto fail = [&](std::string why) {
        return result3d(Terrain3D{}, Diagnostics3D{"Terrain3D::load(" + file.string() + "): " + why});
    };
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return fail("cannot be read");
    const std::vector<char> bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    if (bytes.size() < 16 || std::memcmp(bytes.data(), "KINTERRN", 8) != 0)
        return fail("not a .kinterrain file");
    Reader head{{reinterpret_cast<const u8*>(bytes.data()), bytes.size()}, 8};
    const u32 version = head.u32v(), expected = head.u32v();
    if (version != kinterrain_version)
        return fail("of another version (" + std::to_string(version) + ")");
    if (expected > 2u * 1024 * 1024 * 1024 - 1)
        return fail("too large");
    int size = 0;
    char* inflated =
        kin_zlib_inflate(bytes.data() + 16, static_cast<int>(bytes.size() - 16), static_cast<int>(expected), &size);
    if (!inflated || static_cast<u32>(size) != expected) {
        if (inflated)
            kin_import_free(inflated);
        return fail("cut short (it does not inflate)");
    }
    std::vector<u8> payload(reinterpret_cast<u8*>(inflated), reinterpret_cast<u8*>(inflated) + size);
    kin_import_free(inflated);
    Reader r{payload};
    TerrainDesc3D d;
    d.samples = {static_cast<i32>(r.u32v()), static_cast<i32>(r.u32v())};
    d.spacing = r.f32v();
    d.origin = {r.f32v(), r.f32v(), r.f32v()};
    d.min_height = r.f32v();
    d.max_height = r.f32v();
    d.node_quads = r.u32v();
    d.lod_distance = r.f32v();
    d.morph_start = r.f32v();
    TerrainLook3D look;
    look.color = {r.f32v(), r.f32v(), r.f32v()};
    look.roughness = r.f32v();
    look.blend = r.f32v();
    if (!r.ok || d.samples.x < 2 || d.samples.y < 2 || d.samples.x > 8193 || d.samples.y > 8193)
        return fail("its description is not valid");
    const std::size_t count = static_cast<std::size_t>(d.samples.x) * static_cast<std::size_t>(d.samples.y);
    std::vector<u16> heights(count);
    std::vector<u8> weights(count * max_terrain_layers3d);
    if (!r.raw(heights.data(), count * sizeof(u16)) || !r.raw(weights.data(), weights.size()))
        return fail("cut short");
    const u32 layer_count = r.u32v();
    const Vec2i map_size{static_cast<i32>(r.u32v()), static_cast<i32>(r.u32v())};
    if (!r.ok || layer_count > max_terrain_layers3d)
        return fail("its layers are not valid");
    std::vector<TerrainLayer3D> layers(layer_count);
    for (TerrainLayer3D& l : layers) {
        l.tint = {r.f32v(), r.f32v(), r.f32v()};
        l.roughness = r.f32v();
        l.tile = r.f32v();
        l.albedo = r.image();
        l.normal = r.image();
    }
    if (!r.ok)
        return fail("its layers are cut short");
    auto made = create(renderer, std::move(surface), d, heights);
    if (!made)
        return made;
    Terrain3D t = std::move(*made);
    State& s = *t._s;
    s.weights = std::move(weights);
    s.pack_weights(0, 0, d.samples.x, d.samples.y);
    s.mark_weights(0, 0, d.samples.x, d.samples.y);
    t.set_look(look);
    if (!layers.empty())
        if (auto status = t.set_layers(layers, map_size); !status)
            return result3d(Terrain3D{}, std::move(status.diagnostics));
    return result3d(std::move(t));
}

MeshData3D terrain_mesh3d(const TerrainDesc3D& d, std::span<const u16> heights, u32 stride) {
    MeshData3D m;
    if (d.samples.x < 2 || d.samples.y < 2 ||
        heights.size() != static_cast<std::size_t>(d.samples.x) * static_cast<std::size_t>(d.samples.y))
        return m;
    stride = std::max(stride, 1u);
    // The samples kept along each axis: every stride-th, and the last.
    const auto picks = [&](i32 n) {
        std::vector<i32> out;
        for (i32 i = 0; i < n - 1; i += static_cast<i32>(stride))
            out.push_back(i);
        out.push_back(n - 1);
        return out;
    };
    const std::vector<i32> xs = picks(d.samples.x), zs = picks(d.samples.y);
    const f32 step = (d.max_height - d.min_height) / 65535.0f;
    const f32 u_scale = 1.0f / static_cast<f32>(d.samples.x - 1), v_scale = 1.0f / static_cast<f32>(d.samples.y - 1);
    m.vertices.reserve(xs.size() * zs.size());
    for (const i32 z : zs)
        for (const i32 x : xs) {
            const u16 h = heights[static_cast<std::size_t>(z) * static_cast<std::size_t>(d.samples.x) +
                                  static_cast<std::size_t>(x)];
            m.vertices.push_back(
                {{d.origin.x + static_cast<f32>(x) * d.spacing, d.origin.y + d.min_height + static_cast<f32>(h) * step,
                  d.origin.z + static_cast<f32>(z) * d.spacing},
                 {0.0f, 1.0f, 0.0f},
                 {static_cast<f32>(x) * u_scale, static_cast<f32>(z) * v_scale}});
        }
    const u32 row = static_cast<u32>(xs.size());
    m.indices.reserve((xs.size() - 1) * (zs.size() - 1) * 6);
    for (u32 z = 0; z + 1 < zs.size(); ++z)
        for (u32 x = 0; x + 1 < row; ++x) {
            const u32 a = z * row + x, b = a + 1, c = a + row + 1, dd = a + row;
            m.indices.insert(m.indices.end(), {a, dd, c, a, c, b});
        }
    // The normals: each vertex's from its neighbours.
    for (u32 z = 0; z < zs.size(); ++z)
        for (u32 x = 0; x < row; ++x) {
            const auto at = [&](u32 ix, u32 iz) { return m.vertices[iz * row + ix].position; };
            const Vec3f dx = at(std::min(x + 1, row - 1), z) - at(x > 0 ? x - 1 : 0, z);
            const Vec3f dz = at(x, std::min(z + 1, static_cast<u32>(zs.size()) - 1)) - at(x, z > 0 ? z - 1 : 0);
            m.vertices[z * row + x].normal = normalize(cross(dz, dx));
        }
    return m;
}

MeshData3D Terrain3D::mesh(u32 stride) const {
    return _s ? terrain_mesh3d(_s->desc, _s->heights, stride) : MeshData3D{};
}

namespace {
// A 16-bit PNG's first channel (grey, grey and alpha, RGB, RGBA; not interlaced): what height maps are made as.
// nullopt and `why` when it is not one (an 8-bit PNG: load_image reads it).
std::optional<HeightMap3D> png16(std::span<const u8> bytes, std::string& why) {
    static constexpr u8 signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 33 || std::memcmp(bytes.data(), signature, 8) != 0)
        return std::nullopt;
    const auto be32 = [&](std::size_t at) {
        return static_cast<u32>(bytes[at]) << 24 | static_cast<u32>(bytes[at + 1]) << 16 |
               static_cast<u32>(bytes[at + 2]) << 8 | static_cast<u32>(bytes[at + 3]);
    };
    u32 width = 0, height = 0;
    u8 depth = 0, colour = 0, interlace = 0;
    std::vector<char> compressed;
    for (std::size_t at = 8; at + 12 <= bytes.size();) {
        const u32 length = be32(at);
        if (at + 12 + length > bytes.size())
            break;
        const char* type = reinterpret_cast<const char*>(&bytes[at + 4]);
        const u8* data = &bytes[at + 8];
        if (std::memcmp(type, "IHDR", 4) == 0 && length >= 13) {
            width = be32(at + 8);
            height = be32(at + 12);
            depth = data[8];
            colour = data[9];
            interlace = data[12];
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            compressed.insert(compressed.end(), reinterpret_cast<const char*>(data),
                              reinterpret_cast<const char*>(data) + length);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        at += 12 + length;
    }
    if (depth != 16)
        return std::nullopt;
    const u32 channels = colour == 0 ? 1 : colour == 4 ? 2 : colour == 2 ? 3 : colour == 6 ? 4 : 0;
    if (channels == 0 || interlace != 0 || width == 0 || height == 0 || width > 16385 || height > 16385) {
        why = "a 16-bit PNG kin does not read (interlaced, or of another kind)";
        return std::nullopt;
    }
    const std::size_t bpp = channels * 2, row = width * bpp;
    const std::size_t expected = (row + 1) * height;
    int size = 0;
    char* raw =
        kin_zlib_inflate(compressed.data(), static_cast<int>(compressed.size()), static_cast<int>(expected), &size);
    if (!raw || static_cast<std::size_t>(size) < expected) {
        if (raw)
            kin_import_free(raw);
        why = "a 16-bit PNG cut short";
        return std::nullopt;
    }
    std::vector<u8> pixels(row * height), above(row, 0);
    const auto* in = reinterpret_cast<const u8*>(raw);
    for (u32 y = 0; y < height; ++y) {
        const u8 filter = in[y * (row + 1)];
        const u8* line = in + y * (row + 1) + 1;
        u8* out = &pixels[y * row];
        for (std::size_t i = 0; i < row; ++i) {
            const int a = i >= bpp ? out[i - bpp] : 0, b = above[i], c = i >= bpp ? above[i - bpp] : 0;
            int v = line[i];
            switch (filter) {
            case 1:
                v += a;
                break;
            case 2:
                v += b;
                break;
            case 3:
                v += (a + b) / 2;
                break;
            case 4: {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                break;
            }
            default:
                break;
            }
            out[i] = static_cast<u8>(v);
        }
        std::copy_n(out, row, above.begin());
    }
    kin_import_free(raw);
    HeightMap3D map{{static_cast<i32>(width), static_cast<i32>(height)},
                    std::vector<u16>(static_cast<std::size_t>(width) * height)};
    for (std::size_t i = 0; i < map.heights.size(); ++i)
        map.heights[i] = static_cast<u16>(pixels[i * bpp] << 8 | pixels[i * bpp + 1]);
    return map;
}
} // namespace

std::optional<HeightMap3D> load_height_map(const std::filesystem::path& file, Vec2i samples, std::string* why) {
    const auto fail = [&](std::string w) -> std::optional<HeightMap3D> {
        if (why)
            *why = std::move(w);
        return std::nullopt;
    };
    std::string ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".r16" || ext == ".raw") {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return fail(file.string() + ": cannot be read");
        const std::vector<char> bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        const std::size_t count = bytes.size() / 2;
        if (samples.x <= 0 || samples.y <= 0) {
            const auto side = static_cast<i32>(std::lround(std::sqrt(static_cast<double>(count))));
            samples = {side, side};
        }
        if (bytes.size() % 2 != 0 || count != static_cast<std::size_t>(samples.x) * static_cast<std::size_t>(samples.y))
            return fail(file.string() + ": not " + std::to_string(samples.x) + " x " + std::to_string(samples.y) +
                        " 16-bit samples");
        HeightMap3D map{samples, std::vector<u16>(count)};
        for (std::size_t i = 0; i < count; ++i)
            map.heights[i] = static_cast<u16>(static_cast<u8>(bytes[2 * i]) | (static_cast<u8>(bytes[2 * i + 1]) << 8));
        return map;
    }
    if (ext == ".png") {
        std::ifstream in(file, std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        std::string w;
        if (auto map = png16({reinterpret_cast<const u8*>(bytes.data()), bytes.size()}, w))
            return map;
        if (!w.empty())
            return fail(file.string() + ": " + w);
    }
    const Image image = load_image(file);
    if (!image.valid())
        return fail(file.string() + ": not an image kin reads");
    HeightMap3D map{image.size,
                    std::vector<u16>(static_cast<std::size_t>(image.size.x) * static_cast<std::size_t>(image.size.y))};
    for (std::size_t i = 0; i < map.heights.size(); ++i)
        map.heights[i] = static_cast<u16>(image.rgba[i * 4] * 257u);
    return map;
}

} // namespace kin
