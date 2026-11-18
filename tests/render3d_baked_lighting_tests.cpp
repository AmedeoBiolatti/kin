// Baked lighting's CPU half (kin/render3d/baked_lighting3d.hpp): the baker's rays, lightmap UVs, the light it bakes
// against what the stock shaders draw, light modes, probes, determinism and files. No GPU needed.
#include "bake_bvh3d.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <kin/core/jobs.hpp>
#include <kin/render3d/baked_lighting3d.hpp>
#include <kin/render3d/mesh_lod3d.hpp>
#include <random>
#include <stop_token>
#include <vector>

using namespace kin;

namespace {

int failures = 0;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c);                                 \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

bool near(f32 a, f32 b, f32 tolerance) {
    return std::fabs(a - b) <= tolerance;
}
bool near(LinearRgb a, LinearRgb b, f32 tolerance) {
    return near(a.r, b.r, tolerance) && near(a.g, b.g, tolerance) && near(a.b, b.b, tolerance);
}
void print(const char* what, LinearRgb c) {
    std::fprintf(stderr, "  %s: %.4f %.4f %.4f\n", what, c.r, c.g, c.b);
}

// A square in the xz plane facing +y (or turned), size x size, centred, its lightmap UVs filling [0, 1]^2 inset by
// a texel's worth.
MeshData3D quad(f32 size, Vec3f normal = {0.0f, 1.0f, 0.0f}) {
    MeshData3D m;
    const f32 h = size * 0.5f;
    Vec3f t{1.0f, 0.0f, 0.0f}, b{0.0f, 0.0f, -1.0f};
    if (std::fabs(normal.y) < 0.9f) {
        t = normalize(cross({0.0f, 1.0f, 0.0f}, normal));
        b = cross(normal, t);
    } else if (normal.y < 0.0f) {
        b = {0.0f, 0.0f, 1.0f};
    }
    const Vec3f corners[4] = {(t * -h) + (b * -h), (t * h) + (b * -h), (t * h) + (b * h), (t * -h) + (b * h)};
    const Vec2f uvs[4] = {{0.02f, 0.02f}, {0.98f, 0.02f}, {0.98f, 0.98f}, {0.02f, 0.98f}};
    for (u32 i = 0; i < 4; ++i) {
        MeshVertex3D v;
        v.position = corners[i];
        v.normal = normal;
        v.uv = uvs[i];
        m.vertices.push_back(v);
        m.lightmap_uv.push_back(uvs[i]);
    }
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

// A closed unit cube (single-sided, faces outward), without lightmap UVs.
MeshData3D cube(f32 size) {
    MeshData3D m;
    const f32 h = size * 0.5f;
    const Vec3f n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3f& normal : n) {
        Vec3f t = std::fabs(normal.y) < 0.9f ? normalize(cross({0.0f, 1.0f, 0.0f}, normal)) : Vec3f{1.0f, 0.0f, 0.0f};
        Vec3f b = cross(normal, t);
        const u32 base = static_cast<u32>(m.vertices.size());
        const Vec3f c = normal * h;
        const Vec3f corners[4] = {c - t * h - b * h, c + t * h - b * h, c + t * h + b * h, c - t * h + b * h};
        for (const Vec3f& p : corners) {
            MeshVertex3D v;
            v.position = p;
            v.normal = normal;
            m.vertices.push_back(v);
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return m;
}

LinearRgb lightmap_at(const BakedLightingData3D& baked, u32 surface, Vec2f lightmap_uv) {
    const LightmapPlacement3D& p = baked.placements[surface];
    const LightmapPage3D& page = baked.pages[p.page];
    const f32 u = lightmap_uv.x * p.scale_offset.x + p.scale_offset.z;
    const f32 v = lightmap_uv.y * p.scale_offset.y + p.scale_offset.w;
    return page.at(
        {static_cast<i32>(u * static_cast<f32>(page.size.x)), static_cast<i32>(v * static_cast<f32>(page.size.y))});
}

Lights3D dark() {
    Lights3D l;
    l.sun.color = {};
    l.fill.color = {};
    l.ambient = {};
    return l;
}

BakeSky3D uniform_sky(LinearRgb c) {
    BakeSky3D s;
    s.above = c;
    s.below = c;
    return s;
}

LightBakeSettings3D quick() {
    LightBakeSettings3D s;
    s.texels_per_unit = 8.0f;
    s.samples = 64;
    s.bounces = 2;
    s.probe_samples = 128;
    s.probe_spacing = 2.0f;
    return s;
}

// --- Rays -----------------------------------------------------------------------------------------------------------

void bvh_matches_brute_force() {
    std::mt19937 rng(7);
    std::uniform_real_distribution<f32> u(-5.0f, 5.0f), small(-0.6f, 0.6f);
    std::vector<Vec3f> corners;
    for (u32 i = 0; i < 2000; ++i) {
        const Vec3f c{u(rng), u(rng), u(rng)};
        corners.push_back(c + Vec3f{small(rng), small(rng), small(rng)});
        corners.push_back(c + Vec3f{small(rng), small(rng), small(rng)});
        corners.push_back(c + Vec3f{small(rng), small(rng), small(rng)});
    }
    bake::TriangleBvh bvh;
    bvh.build(corners);
    CHECK(bvh.triangle_count() == 2000);
    u32 hits = 0;
    for (u32 r = 0; r < 2000; ++r) {
        const Vec3f o{u(rng), u(rng), u(rng)};
        const Vec3f d = normalize(Vec3f{u(rng), u(rng), u(rng)});
        const f32 t_max = r % 3 == 0 ? 2.0f : 1e30f;
        // Brute force (double precision).
        f64 best = t_max;
        i64 best_tri = -1;
        for (u32 t = 0; t < 2000; ++t) {
            const Vec3f a = corners[3 * t], e1 = corners[3 * t + 1] - a, e2 = corners[3 * t + 2] - a;
            const Vec3f p = cross(d, e2);
            const f64 det = dot(e1, p);
            if (std::fabs(det) < 1e-12)
                continue;
            const Vec3f s = o - a;
            const f64 uu = dot(s, p) / det;
            const Vec3f q = cross(s, e1);
            const f64 vv = dot(d, q) / det;
            const f64 tt = dot(e2, q) / det;
            if (uu >= 0 && vv >= 0 && uu + vv <= 1 && tt > 0 && tt < best) {
                best = tt;
                best_tri = t;
            }
        }
        bake::RayHit hit;
        const bool found = bvh.nearest(o, d, t_max, hit);
        CHECK(found == (best_tri >= 0));
        CHECK(bvh.any(o, d, t_max) == (best_tri >= 0));
        if (found && best_tri >= 0) {
            ++hits;
            CHECK(hit.triangle == static_cast<u32>(best_tri) || std::fabs(hit.t - best) < 1e-4);
            CHECK(std::fabs(hit.t - best) < 1e-3 * std::max(1.0, best));
        }
    }
    CHECK(hits > 200);
    // Back faces are told apart: a ray from above meets a +y-facing triangle's front.
    const std::vector<Vec3f> one = {{-1, 0, 1}, {1, 0, 1}, {0, 0, -1}};
    bvh.build(one);
    bake::RayHit hit;
    CHECK(bvh.nearest({0, 1, 0}, {0, -1, 0}, 10.0f, hit) && !hit.back && near(hit.t, 1.0f, 1e-5f));
    CHECK(bvh.nearest({0, -1, 0}, {0, 1, 0}, 10.0f, hit) && hit.back);
}

// --- Lightmap UVs ---------------------------------------------------------------------------------------------------

void lightmap_uvs_from_xatlas() {
    MeshData3D m = cube(1.0f);
    m.lightmap_uv.clear();
    const std::size_t triangles = m.triangle_count();
    const Status3D made = generate_lightmap_uv(m, {.resolution = 64, .padding = 2});
    CHECK(made.ok());
    CHECK(m.lightmapped());
    CHECK(m.triangle_count() == triangles);
    CHECK(m.validate().ok());
    // Every triangle has room of its own: sample a grid and count how many triangles cover each point.
    f32 area = 0.0f;
    constexpr u32 grid = 128;
    std::vector<u8> covered(grid * grid, 0);
    bool overlap = false;
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        const Vec2f a = m.lightmap_uv[m.indices[3 * t]], b = m.lightmap_uv[m.indices[3 * t + 1]],
                    c = m.lightmap_uv[m.indices[3 * t + 2]];
        const f32 signed_area = 0.5f * ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
        area += std::fabs(signed_area);
        for (u32 y = 0; y < grid; ++y) {
            for (u32 x = 0; x < grid; ++x) {
                const Vec2f p{(static_cast<f32>(x) + 0.5f) / grid, (static_cast<f32>(y) + 0.5f) / grid};
                const auto e = [](Vec2f p0, Vec2f p1, Vec2f q) {
                    return (p1.x - p0.x) * (q.y - p0.y) - (p1.y - p0.y) * (q.x - p0.x);
                };
                const f32 w0 = e(b, c, p), w1 = e(c, a, p), w2 = e(a, b, p);
                const bool inside =
                    (w0 > 1e-6f && w1 > 1e-6f && w2 > 1e-6f) || (w0 < -1e-6f && w1 < -1e-6f && w2 < -1e-6f);
                if (inside) {
                    overlap = overlap || covered[y * grid + x];
                    covered[y * grid + x] = 1;
                }
            }
        }
    }
    CHECK(!overlap);
    CHECK(area > 0.2f && area <= 1.0f);
    // The faces' own attributes stay with them.
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        const Vec3f n = m.vertices[m.indices[3 * t]].normal;
        CHECK(m.vertices[m.indices[3 * t + 1]].normal == n && m.vertices[m.indices[3 * t + 2]].normal == n);
    }
    // Refused for a skinned mesh; validate refuses mismatched lightmap UVs.
    MeshData3D skinned = cube(1.0f);
    skinned.skin.resize(skinned.vertices.size(), MeshSkinVertex3D{{0, 0, 0, 0}, {1, 0, 0, 0}});
    CHECK(!generate_lightmap_uv(skinned).ok());
    MeshData3D bad = quad(1.0f);
    bad.lightmap_uv.pop_back();
    CHECK(!bad.validate().ok());
    bad = quad(1.0f);
    bad.lightmap_uv[0] = {1.5f, 0.0f};
    CHECK(!bad.validate().ok());
    // append() drops them.
    MeshData3D joined = quad(1.0f);
    joined.append(quad(1.0f), Mat4f::translation({2.0f, 0.0f, 0.0f}));
    CHECK(!joined.lightmapped() && joined.validate().ok());
}

// Levels of detail of a mesh with lightmap UVs: simpler levels, every one indexing the mesh's own vertices (which
// carry the lightmap UVs: no vertices of the chain's own), the UVs' charts minded.
void lightmapped_levels_of_detail() {
    MeshData3D ball = meshes::sphere(1.0f, 48, 24);
    CHECK(generate_lightmap_uv(ball, {.resolution = 256}).ok());
    const MeshLodChain3D chain = build_mesh_levels(ball, {.levels = 4});
    CHECK(chain.count() >= 3);
    CHECK(chain.extra_vertices.empty());
    for (const MeshLodLevel3D& level : chain.levels) {
        for (const u32 i : level.indices)
            CHECK(i < ball.vertices.size());
    }
    CHECK(chain.levels.back().triangles < ball.triangle_count() / 3);
    // Unindexed, the same: corners alike but on two charts are not taken as one.
    MeshData3D soup;
    for (const u32 i : ball.indices) {
        soup.vertices.push_back(ball.vertices[i]);
        soup.lightmap_uv.push_back(ball.lightmap_uv[i]);
    }
    const MeshLodChain3D soup_chain = build_mesh_levels(soup, {.levels = 4});
    CHECK(soup_chain.extra_vertices.empty());
    for (const MeshLodLevel3D& level : soup_chain.levels) {
        for (const u32 i : level.indices) {
            CHECK(i < soup.vertices.size());
        }
    }
}

// --- Light ----------------------------------------------------------------------------------------------------------

// An open plane under a sky of one colour: every ray sees the sky, so every texel holds it exactly.
void open_plane_sees_the_sky() {
    const MeshData3D plane = quad(4.0f);
    const BakeSurface3D surfaces[] = {{.mesh = &plane}};
    const LinearRgb sky{0.3f, 0.5f, 0.7f};
    auto baked = bake_lighting(surfaces, dark(), uniform_sky(sky), quick());
    CHECK(baked.ok());
    if (!baked.ok())
        return;
    CHECK(baked->pages.size() == 1);
    CHECK(baked->placements.size() == 1 && baked->placements[0].placed());
    CHECK(baked->stats.texels > 500 && baked->stats.inside_texels == 0);
    for (const Vec2f uv : {Vec2f{0.5f, 0.5f}, Vec2f{0.1f, 0.1f}, Vec2f{0.9f, 0.3f}}) {
        const LinearRgb c = lightmap_at(*baked, 0, uv);
        CHECK(near(c, sky, 1e-4f));
    }
    // A hemisphere sky: the ground's colour below the horizon is not seen by a face looking up; a wall facing +x
    // sees half of each (the stock shaders' mix(ground, ambient, n.y / 2 + 1 / 2), exactly).
    const MeshData3D wall = quad(2.0f, {1.0f, 0.0f, 0.0f});
    const BakeSurface3D walls[] = {{.mesh = &wall}, {.mesh = &plane, .transform = Mat4f::translation({0, -50, 0})}};
    BakeSky3D hemisphere;
    hemisphere.above = {1.0f, 1.0f, 1.0f};
    hemisphere.below = {0.0f, 0.0f, 0.0f};
    LightBakeSettings3D settings = quick();
    settings.samples = 512;
    settings.bounces = 0;
    auto lit = bake_lighting(walls, dark(), hemisphere, settings);
    CHECK(lit.ok());
    if (lit.ok()) {
        const LinearRgb c = lightmap_at(*lit, 0, {0.5f, 0.5f});
        print("wall", c);
        CHECK(near(c, {0.5f, 0.5f, 0.5f}, 0.03f));
    }
}

// A baked point light over a plane: the texel under it holds what the stock shaders give a surface there (the
// light's colour times the windowed inverse square), its edge less by n.l and the distance.
void baked_point_light_matches_the_shader() {
    const MeshData3D plane = quad(4.0f);
    const BakeSurface3D surfaces[] = {{.mesh = &plane}};
    Lights3D lights = dark();
    PointLightSource3D lamp;
    lamp.position = {0.0f, 1.0f, 0.0f};
    lamp.color = {2.0f, 1.0f, 0.5f};
    lamp.radius = 10.0f;
    lamp.size = 0.01f;
    lamp.bake = LightBake3D::Baked;
    lights.points.push_back(lamp);
    LightBakeSettings3D settings = quick();
    settings.texels_per_unit = 16.0f;
    auto baked = bake_lighting(surfaces, lights, uniform_sky({}), settings);
    CHECK(baked.ok());
    if (!baked.ok())
        return;
    const auto expected = [&](Vec3f p) {
        const Vec3f to = lamp.position - p;
        const f32 d2 = dot(to, to), r2 = d2 / (lamp.radius * lamp.radius);
        const f32 window = std::clamp(1.0f - r2 * r2, 0.0f, 1.0f);
        const f32 k = window * window / d2 * (to.y / std::sqrt(d2));
        return lamp.color * k;
    };
    // The quad's lightmap UV (0.5, 0.5) is its middle; (0.98, 0.5) its +x edge at x = 2.
    const LinearRgb middle = lightmap_at(*baked, 0, {0.5f, 0.5f});
    print("under the lamp", middle);
    print("expected", expected({0, 0, 0}));
    CHECK(near(middle, expected({0, 0, 0}), 0.02f * 2.0f));
    const LinearRgb edge = lightmap_at(*baked, 0, {0.9f, 0.5f});
    const LinearRgb edge_expected = expected({4.0f * (0.9f - 0.5f) / 0.96f, 0, 0});
    print("toward the edge", edge);
    print("expected", edge_expected);
    CHECK(near(edge, edge_expected, 0.05f * 2.0f));
    // As Mixed, its direct light is drawn each frame: the lightmap keeps only its bounce (none: nothing else here).
    lights.points[0].bake = LightBake3D::Mixed;
    auto mixed = bake_lighting(surfaces, lights, uniform_sky({}), settings);
    CHECK(mixed.ok() && near(lightmap_at(*mixed, 0, {0.5f, 0.5f}), {}, 1e-6f));
    // As Realtime it is left out altogether.
    lights.points[0].bake = LightBake3D::Realtime;
    auto realtime = bake_lighting(surfaces, lights, uniform_sky({}), settings);
    CHECK(realtime.ok() && near(lightmap_at(*realtime, 0, {0.5f, 0.5f}), {}, 1e-6f));
}

// Light bounces: a floor in a mixed sun's light lights a ceiling the sun cannot reach (its bounce is baked, its
// direct light not), and a baked sun's bounce is the same.
void bounced_light_from_mixed_and_baked_lights() {
    const MeshData3D floor = quad(4.0f);
    const MeshData3D ceiling = quad(4.0f, {0.0f, -1.0f, 0.0f});
    const BakeSurface3D surfaces[] = {{.mesh = &floor, .albedo = {0.5f, 0.5f, 0.5f}},
                                      {.mesh = &ceiling, .transform = Mat4f::translation({0, 1, 0})}};
    for (const LightBake3D mode : {LightBake3D::Mixed, LightBake3D::Baked}) {
        Lights3D lights = dark();
        lights.sun.direction = {0.0f, 1.0f, 0.0f};
        lights.sun.color = {1.0f, 1.0f, 1.0f};
        lights.sun.bake = mode;
        // The sun comes from above: the ceiling's back faces it and is single-sided, but rays toward the sun from
        // the floor are blocked by the ceiling; put the sun at an angle under the ceiling's edge instead.
        lights.sun.direction = normalize(Vec3f{1.0f, 0.15f, 0.0f});
        auto baked = bake_lighting(surfaces, lights, uniform_sky({}), quick());
        CHECK(baked.ok());
        if (!baked.ok())
            continue;
        const LinearRgb up = lightmap_at(*baked, 1, {0.5f, 0.5f});
        const LinearRgb down = lightmap_at(*baked, 0, {0.5f, 0.5f});
        print(mode == LightBake3D::Mixed ? "mixed: ceiling" : "baked: ceiling", up);
        print(mode == LightBake3D::Mixed ? "mixed: floor" : "baked: floor", down);
        CHECK(up.r > 0.02f);
        // The floor's own direct light is in its lightmap only when baked.
        if (mode == LightBake3D::Baked) {
            CHECK(down.r > 0.1f);
        } else {
            CHECK(down.r < up.r);
        }
    }
}

// Texels hidden inside a closed box standing on a floor see only its backs: they take their neighbours' light, and
// the floor round it is darkened by it (the sky partly hidden).
void inside_texels_and_occlusion() {
    const MeshData3D floor = quad(6.0f);
    const MeshData3D box = cube(1.0f);
    const BakeSurface3D surfaces[] = {
        {.mesh = &floor}, {.mesh = &box, .transform = Mat4f::translation({0, 0.5f, 0}), .lightmapped = false}};
    LightBakeSettings3D settings = quick();
    settings.bounces = 0;
    auto baked = bake_lighting(surfaces, dark(), uniform_sky({1, 1, 1}), settings);
    CHECK(baked.ok());
    if (!baked.ok())
        return;
    CHECK(baked->stats.inside_texels > 0);
    CHECK(!baked->placements[1].placed());
    // Next to the box (x just past 0.5) the floor sees less sky than far from it.
    const LinearRgb near_box = lightmap_at(*baked, 0, {0.5f + 0.6f / 6.0f, 0.5f});
    const LinearRgb far = lightmap_at(*baked, 0, {0.95f, 0.95f});
    print("by the box", near_box);
    print("far", far);
    CHECK(near_box.r < far.r - 0.1f);
    CHECK(far.r > 0.9f);
    // Under the box the value is filled from round it: not black.
    const LinearRgb under = lightmap_at(*baked, 0, {0.5f, 0.5f});
    CHECK(under.r > 0.05f);
}

// Probes: under a sky of one colour every probe holds it from every way; a baked sun adds its light facing it
// (a quarter of it from all round plus half toward it, linear spherical harmonics) and nothing facing away.
void probes_hold_the_light_from_all_round() {
    const MeshData3D plane = quad(1.0f);
    // The plane far below the probes' area, black: it hides almost none of the sky.
    const BakeSurface3D surfaces[] = {
        {.mesh = &plane, .transform = Mat4f::translation({0, -100, 0}), .albedo = {0, 0, 0}, .lightmapped = false}};
    LightBakeSettings3D settings = quick();
    settings.probe_area = {{-2, -2, -2}, {2, 2, 2}};
    settings.probe_spacing = 2.0f;
    settings.probe_samples = 1024;
    const LinearRgb sky{0.2f, 0.4f, 0.6f};
    auto baked = bake_lighting(surfaces, dark(), uniform_sky(sky), settings);
    CHECK(baked.ok());
    if (!baked.ok())
        return;
    CHECK(baked->probes.count[0] == 3 && baked->probes.count[1] == 3 && baked->probes.count[2] == 3);
    CHECK(baked->stats.probes == 27);
    for (const Vec3f n : {Vec3f{0, 1, 0}, Vec3f{1, 0, 0}, Vec3f{0, 0, -1}}) {
        const LinearRgb c = baked->probes.light({0.3f, 0.1f, -0.7f}, n);
        CHECK(near(c, sky, 0.02f));
    }
    CHECK(near(baked->probes.cover({0, 0, 0}), 1.0f, 1e-6f));
    CHECK(baked->probes.cover({0, 10, 0}) == 0.0f);
    Lights3D lights = dark();
    lights.sun.direction = {0.0f, 1.0f, 0.0f};
    lights.sun.color = {1.0f, 1.0f, 1.0f};
    lights.sun.bake = LightBake3D::Baked;
    auto sunny = bake_lighting(surfaces, lights, uniform_sky({}), settings);
    CHECK(sunny.ok());
    if (sunny.ok()) {
        CHECK(near(sunny->probes.light({0, 0, 0}, {0, 1, 0}), {0.75f, 0.75f, 0.75f}, 1e-3f));
        CHECK(near(sunny->probes.light({0, 0, 0}, {0, -1, 0}), {0.0f, 0.0f, 0.0f}, 1e-3f));
        CHECK(near(sunny->probes.light({0, 0, 0}, {1, 0, 0}), {0.25f, 0.25f, 0.25f}, 1e-3f));
    }
}

// A closed dark room under a white sky, the probe grid straddling its walls: a point just inside a wall takes no light
// from the probes outside it (they cannot see it); without the probes' visibility it would (light through the wall).
void probes_do_not_leak_through_walls() {
    std::vector<BakeSurface3D> surfaces;
    const Vec3f normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<MeshData3D> walls;
    for (const Vec3f& n : normals)
        walls.push_back(quad(4.0f, -n)); // facing in
    for (std::size_t i = 0; i < 6; ++i)
        surfaces.push_back({.mesh = &walls[i],
                            .transform = Mat4f::translation(normals[i] * 2.0f),
                            .albedo = {0, 0, 0},
                            .lightmapped = false,
                            .double_sided = true});
    LightBakeSettings3D settings = quick();
    settings.probe_area = {{-3.5f, -3.5f, -3.5f}, {3.5f, 3.5f, 3.5f}};
    settings.probe_spacing = 1.0f;
    settings.probe_samples = 512;
    auto baked = bake_lighting(surfaces, dark(), uniform_sky({1, 1, 1}), settings);
    CHECK(baked.ok());
    if (!baked.ok())
        return;
    const BakedProbes3D& probes = baked->probes;
    CHECK(probes.count[0] == 8 && probes.visibility.size() == probes.sh.size());
    const Vec3f up{0, 1, 0};
    const LinearRgb inside = probes.light({-1.7f, 0.3f, 0.3f}, up);
    const LinearRgb middle = probes.light({0.0f, 0.3f, 0.3f}, up);
    const LinearRgb outside = probes.light({-3.0f, 0.3f, 0.3f}, up);
    BakedProbes3D blind = probes;
    blind.visibility.clear();
    const LinearRgb leaked = blind.light({-1.7f, 0.3f, 0.3f}, up);
    print("inside, by the wall", inside);
    print("inside, without visibility", leaked);
    print("outside", outside);
    CHECK(inside.r < 0.03f && middle.r < 0.01f);
    CHECK(leaked.r > 0.1f);
    CHECK(outside.r > 0.4f);
}

void deterministic_on_any_number_of_threads() {
    const MeshData3D floor = quad(4.0f);
    const MeshData3D box = cube(1.0f);
    const BakeSurface3D surfaces[] = {
        {.mesh = &floor}, {.mesh = &box, .transform = Mat4f::translation({0.5f, 0.5f, 0}), .lightmapped = false}};
    Lights3D lights = dark();
    lights.sun.color = {1, 1, 1};
    lights.sun.bake = LightBake3D::Baked;
    PointLightSource3D lamp;
    lamp.position = {-1, 1, 0};
    lamp.bake = LightBake3D::Mixed;
    lights.points.push_back(lamp);
    const auto run = [&](JobSystem* jobs) {
        LightBakeControl3D control;
        control.jobs = jobs;
        return bake_lighting(surfaces, lights, uniform_sky({0.2f, 0.2f, 0.3f}), quick(), control);
    };
    auto serial = run(nullptr);
    JobSystem four({.workers = 4});
    auto threaded = run(&four);
    CHECK(serial.ok() && threaded.ok());
    if (!serial.ok() || !threaded.ok())
        return;
    CHECK(serial->pages.size() == threaded->pages.size());
    for (std::size_t p = 0; p < serial->pages.size(); ++p) {
        CHECK(serial->pages[p].rgb.size() == threaded->pages[p].rgb.size() &&
              std::memcmp(serial->pages[p].rgb.data(), threaded->pages[p].rgb.data(),
                          serial->pages[p].rgb.size() * sizeof(f32)) == 0);
    }
    CHECK(serial->probes.sh.size() == threaded->probes.sh.size() &&
          std::memcmp(serial->probes.sh.data(), threaded->probes.sh.data(), serial->probes.sh.size() * sizeof(f32)) ==
              0);
    CHECK(serial->stats.rays == threaded->stats.rays && serial->stats.rays > 0);
    // A stop request ends it with an error.
    std::stop_source stop;
    stop.request_stop();
    LightBakeControl3D control;
    control.jobs = &four;
    control.stop = stop.get_token();
    CHECK(!bake_lighting(surfaces, lights, uniform_sky({}), quick(), control).ok());
}

void refusals_and_light_lists() {
    MeshData3D plain = cube(1.0f);
    const BakeSurface3D lightmapped[] = {{.mesh = &plain}};
    CHECK(!bake_lighting(lightmapped, dark(), uniform_sky({}), quick()).ok());
    const BakeSurface3D none[] = {{.mesh = nullptr}};
    CHECK(!bake_lighting(none, dark(), uniform_sky({}), quick()).ok());
    LightBakeSettings3D odd = quick();
    odd.page_size = 1000;
    const MeshData3D plane = quad(1.0f);
    const BakeSurface3D ok[] = {{.mesh = &plane}};
    CHECK(!bake_lighting(ok, dark(), uniform_sky({}), odd).ok());
    // Drawn with baked lighting, Baked lights are gone, the others stay.
    Lights3D lights;
    lights.sun.bake = LightBake3D::Baked;
    lights.fill.color = {0.1f, 0.1f, 0.1f};
    lights.fill.bake = LightBake3D::Mixed;
    PointLightSource3D a, b;
    a.bake = LightBake3D::Baked;
    b.bake = LightBake3D::Mixed;
    lights.points = {a, b, a};
    const Lights3D drawn = drawn_lights(lights);
    CHECK(near(drawn.sun.color, {}, 0.0f));
    CHECK(near(drawn.fill.color, {0.1f, 0.1f, 0.1f}, 0.0f));
    CHECK(drawn.points.size() == 1 && drawn.points[0].bake == LightBake3D::Mixed);
}

void files_round_trip() {
    const MeshData3D floor = quad(4.0f);
    const BakeSurface3D surfaces[] = {{.mesh = &floor}, {.mesh = &floor, .transform = Mat4f::translation({5, 0, 0})}};
    Lights3D lights = dark();
    lights.sun.color = {1, 0.9f, 0.8f};
    lights.sun.bake = LightBake3D::Baked;
    auto baked = bake_lighting(surfaces, lights, uniform_sky({0.1f, 0.1f, 0.2f}), quick());
    CHECK(baked.ok());
    if (!baked.ok())
        return;
    const char* tmp = std::getenv("TMPDIR");
    const std::filesystem::path folder =
        std::filesystem::path(tmp ? tmp : std::filesystem::temp_directory_path().string()) / "kin_baked_lighting_test";
    std::filesystem::remove_all(folder);
    CHECK(save_baked_lighting(*baked, folder).ok());
    CHECK(std::filesystem::exists(folder / "lighting.json") && std::filesystem::exists(folder / "lightmap_0.hdr") &&
          std::filesystem::exists(folder / "probes.bin"));
    const auto read = [](const std::filesystem::path& p) -> std::optional<std::string> {
        std::FILE* f = std::fopen(p.string().c_str(), "rb");
        if (!f)
            return std::nullopt;
        std::string bytes;
        char buffer[65536];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0)
            bytes.append(buffer, n);
        std::fclose(f);
        return bytes;
    };
    auto loaded = load_baked_lighting(folder, read);
    CHECK(loaded.ok());
    if (loaded.ok()) {
        CHECK(loaded->pages.size() == baked->pages.size());
        CHECK(loaded->placements.size() == 2);
        for (std::size_t i = 0; i < 2; ++i) {
            CHECK(loaded->placements[i].page == baked->placements[i].page &&
                  loaded->placements[i].scale_offset == baked->placements[i].scale_offset);
        }
        // The pages as Radiance pictures: within RGBE's precision (8 bits of mantissa).
        for (std::size_t p = 0; p < baked->pages.size(); ++p) {
            const auto& a = baked->pages[p].rgb;
            const auto& b = loaded->pages[p].rgb;
            CHECK(a.size() == b.size());
            for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
                if (std::fabs(a[i] - b[i]) > 0.01f * std::max(1.0f, std::fabs(a[i]))) {
                    CHECK(false);
                    break;
                }
            }
        }
        CHECK(loaded->probes.count == baked->probes.count && loaded->probes.sh == baked->probes.sh &&
              loaded->probes.visibility == baked->probes.visibility);
        CHECK(loaded->probes.area.min == baked->probes.area.min && loaded->probes.area.max == baked->probes.area.max);
    }
    CHECK(!load_baked_lighting(folder / "missing", read).ok());
    std::filesystem::remove_all(folder);
}

} // namespace

int main() {
    bvh_matches_brute_force();
    lightmap_uvs_from_xatlas();
    lightmapped_levels_of_detail();
    open_plane_sees_the_sky();
    baked_point_light_matches_the_shader();
    bounced_light_from_mixed_and_baked_lights();
    inside_texels_and_occlusion();
    probes_hold_the_light_from_all_round();
    probes_do_not_leak_through_walls();
    deterministic_on_any_number_of_threads();
    refusals_and_light_lists();
    files_round_trip();
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::puts("baked lighting: ok");
    return 0;
}
