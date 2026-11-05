// Area lights' CPU half (engine/src/renderer/gpu/gpu_area_lights3d.hpp): the hierarchy over the copies' triangles and
// the emitters as the shaders read them. No GPU needed.
#include "gpu_area_lights3d.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace kin;
using namespace kin::gpu;

namespace {

int failures = 0;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c);                                 \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

struct Node {
    Vec3f lo, hi;
    u32 first, count; // count 0: inside, children first and first + 1
};

std::vector<Node> nodes_of(const std::vector<f32>& data, u32 count) {
    std::vector<Node> out;
    for (u32 i = 0; i < count; ++i) {
        const f32* n = data.data() + i * area_light_floats_node;
        out.push_back({{n[0], n[1], n[2]}, {n[4], n[5], n[6]}, std::bit_cast<u32>(n[3]), std::bit_cast<u32>(n[7])});
    }
    return out;
}

bool inside(Vec3f p, const Node& n, f32 slack = 1e-5f) {
    return p.x >= n.lo.x - slack && p.y >= n.lo.y - slack && p.z >= n.lo.z - slack && p.x <= n.hi.x + slack &&
           p.y <= n.hi.y + slack && p.z <= n.hi.z + slack;
}

// Every triangle in exactly one leaf, inside every box on the way down; children inside their parents.
void check_hierarchy(std::span<const AreaLightTriangle> tris, const AreaLightMesh& d) {
    CHECK(d.triangle_count == tris.size());
    CHECK(d.triangles.size() == tris.size() * area_light_floats_triangle);
    CHECK(d.depth <= area_light_max_depth);
    const std::vector<Node> nodes = nodes_of(d.nodes, d.node_count);
    std::vector<u32> seen(tris.size(), 0);
    struct Item {
        u32 node;
        const Node* parent;
    };
    std::vector<Item> stack{{0, nullptr}};
    while (!stack.empty()) {
        const Item it = stack.back();
        stack.pop_back();
        CHECK(it.node < nodes.size());
        if (it.node >= nodes.size()) {
            continue;
        }
        const Node& n = nodes[it.node];
        if (it.parent) {
            CHECK(inside(n.lo, *it.parent) && inside(n.hi, *it.parent));
        }
        if (n.count == 0) {
            CHECK(n.first + 1 < nodes.size());
            stack.push_back({n.first, &n});
            stack.push_back({n.first + 1, &n});
            continue;
        }
        CHECK(n.count <= 8);
        for (u32 t = n.first; t < n.first + n.count; ++t) {
            CHECK(t < tris.size());
            const f32* r = d.triangles.data() + t * area_light_floats_triangle;
            const Vec3f a{r[0], r[1], r[2]}, e0{r[4], r[5], r[6]}, e1{r[8], r[9], r[10]}, normal{r[12], r[13], r[14]};
            CHECK(inside(a, n) && inside(a + e0, n) && inside(a + e1, n));
            CHECK(length(normal - cross(e0, e1)) <= 1e-4f * (1.0f + length(normal)));
            CHECK(std::abs(r[3] - dot(a, normal)) <= 1e-4f * (1.0f + std::abs(r[3])));
            // Which input it is: the first not yet found with the same corners (some are alike).
            for (u32 i = 0; i < tris.size(); ++i) {
                if (seen[i] == 0 && length(tris[i].a - a) < 1e-6f && length(tris[i].b - (a + e0)) < 1e-5f &&
                    length(tris[i].c - (a + e1)) < 1e-5f) {
                    ++seen[i];
                    break;
                }
            }
        }
    }
    CHECK(std::all_of(seen.begin(), seen.end(), [](u32 s) { return s == 1; }));
}

struct Emitter {
    Vec3f p0, p1, p2, normal, radiance;
    f32 kind, gi, jacobian, two_sided;
};

std::vector<Emitter> emitters_of(const AreaLightEmitters& d) {
    std::vector<Emitter> out;
    for (u32 i = 0; i < d.count; ++i) {
        const f32* e = d.data.data() + i * area_light_floats_emitter;
        out.push_back({{e[0], e[1], e[2]},
                       {e[4], e[5], e[6]},
                       {e[8], e[9], e[10]},
                       {e[12], e[13], e[14]},
                       {e[16], e[17], e[18]},
                       e[3],
                       e[7],
                       e[11],
                       e[15]});
    }
    return out;
}

void random_scene() {
    std::mt19937 rng(7);
    std::uniform_real_distribution<f32> u(-10.0f, 10.0f), s(-0.5f, 0.5f);
    std::vector<AreaLightTriangle> tris;
    for (int i = 0; i < 3000; ++i) {
        const Vec3f c{u(rng), u(rng), u(rng)};
        tris.push_back(
            {c + Vec3f{s(rng), s(rng), s(rng)}, c + Vec3f{s(rng), s(rng), s(rng)}, c + Vec3f{s(rng), s(rng), s(rng)}});
    }
    // And many alike (all centres equal): the builder must still end.
    for (int i = 0; i < 40; ++i) {
        tris.push_back({{1, 1, 1}, {2, 1, 1}, {1, 2, 1}});
    }
    check_hierarchy(tris, build_area_light_mesh(tris));
}

void cornell_like_panel() {
    // A ceiling panel of two triangles (a quad, wound to face down), one GI emitter; and the room's floor.
    const Vec3f a{2.13f, 5.483f, 2.27f}, b{3.43f, 5.483f, 2.27f}, c{3.43f, 5.483f, 3.32f}, d{2.13f, 5.483f, 3.32f};
    std::vector<AreaLightTriangle> tris{{a, b, c}, {a, c, d}, {{0, 0, 0}, {5, 0, 0}, {5, 0, 5}}};
    const Vec3f radiance{1.0f, 0.706f, 0.235f};
    std::vector<AreaLightEmitterTriangle> em{{a, b, c, radiance, 1, false, 9}, {a, c, d, radiance, 1, false, 9}};
    check_hierarchy(tris, build_area_light_mesh(tris));
    const AreaLightEmitters out = build_area_light_emitters(em, 64);
    const std::vector<Emitter> e = emitters_of(out);
    CHECK(e.size() == 1);
    CHECK(out.explicit_emitters == std::vector<u32>{1});
    if (e.size() == 1) {
        CHECK(e[0].kind == 0.0f);                              // a parallelogram
        CHECK(std::abs(e[0].jacobian - 1.3f * 1.05f) < 1e-4f); // its area
        CHECK(std::abs(length(e[0].p2) - 1.3f) < 1e-5f);       // lines along the longer side
        CHECK(std::abs(length(e[0].p1) - 1.05f) < 1e-5f);
        CHECK(std::abs(e[0].normal.y + 1.0f) < 1e-5f); // emitting down, as wound
        CHECK(e[0].gi == 1.0f && e[0].two_sided == 0.0f && e[0].radiance == radiance);
        // Its far corner is the quad's.
        const Vec3f far = e[0].p0 + e[0].p1 + e[0].p2;
        const bool corner =
            length(far - a) < 1e-5f || length(far - b) < 1e-5f || length(far - c) < 1e-5f || length(far - d) < 1e-5f;
        CHECK(corner);
    }
}

void triangles_and_limits() {
    std::vector<AreaLightTriangle> tris{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}};
    // A lone triangle: kind 1, jacobian twice its area, its lines along the longest side (P2).
    std::vector<AreaLightEmitterTriangle> em{{{0, 0, 0}, {4, 0, 0}, {0, 1, 0}, {1, 1, 1}, 2, true, 1}};
    AreaLightEmitters out = build_area_light_emitters(em, 64);
    std::vector<Emitter> e = emitters_of(out);
    CHECK(e.size() == 1);
    if (e.size() == 1) {
        CHECK(e[0].kind == 1.0f && std::abs(e[0].jacobian - 4.0f) < 1e-5f && e[0].two_sided == 1.0f);
        CHECK(std::abs(length(e[0].p2) - std::sqrt(17.0f)) < 1e-5f);
        CHECK(length(e[0].p0 - Vec3f{0, 0, 0}) < 1e-6f); // the apex opposite it
    }
    // Two triangles not forming a parallelogram (a kite): two emitters.
    em = {{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 1}, 3, false, 1},
          {{1, 0, 0}, {2, 2, 0}, {0, 1, 0}, {1, 1, 1}, 3, false, 1}};
    out = build_area_light_emitters(em, 64);
    CHECK(out.count == 2);
    // Over the limit, a GI emitter stays with the probes entirely; one without a number is never lit here.
    em.clear();
    for (int i = 0; i < 5; ++i) {
        const f32 x = static_cast<f32>(i) * 3.0f;
        em.push_back({{x, 0, 0}, {x + 1, 0, 0}, {x, 1, 0}, {1, 1, 1}, 4, false, static_cast<u64>(i)});
    }
    em.push_back({{0, 5, 0}, {1, 5, 0}, {0, 6, 0}, {1, 1, 1}, 5, false, 9});
    em.push_back({{0, 9, 0}, {1, 9, 0}, {0, 9, 1}, {1, 1, 1}, 0, false, 10});
    out = build_area_light_emitters(em, 3);
    CHECK(out.explicit_emitters == std::vector<u32>{5});
    CHECK(out.count == 1);
    out = build_area_light_emitters(em, 64);
    CHECK((out.explicit_emitters == std::vector<u32>{4, 5}));
    CHECK(out.count == 6);
}

// The copies' hierarchy: every copy in one leaf, inside its boxes; each copy's rows take the world back to its mesh.
void copies() {
    std::mt19937 rng(3);
    std::uniform_real_distribution<f32> u(-20.0f, 20.0f), a(0.0f, 360.0f), s(0.5f, 2.0f);
    std::vector<AreaLightCopy> in;
    for (u32 i = 0; i < 300; ++i) {
        const Vec3f at{u(rng), u(rng), u(rng)};
        const Mat4f to_world =
            Mat4f::translation(at) * Mat4f::rotation_y(a(rng)) * Mat4f::scaling({s(rng), s(rng), s(rng)});
        in.push_back({to_world, at - Vec3f{1, 1, 1}, at + Vec3f{1, 1, 1}, i * 10, i * 7});
    }
    const AreaLightCopies out = build_area_light_copies(in);
    CHECK(out.copy_count == in.size());
    CHECK(out.data.size() == out.node_count * area_light_floats_node + in.size() * area_light_floats_copy);
    const std::vector<Node> nodes = nodes_of(out.data, out.node_count);
    std::vector<u32> seen(in.size(), 0);
    std::vector<u32> stack{0};
    while (!stack.empty()) {
        const Node n = nodes[stack.back()];
        stack.pop_back();
        if (n.count == 0) {
            stack.push_back(n.first);
            stack.push_back(n.first + 1);
            continue;
        }
        CHECK(n.count <= 4);
        for (u32 c = n.first; c < n.first + n.count; ++c) {
            const f32* r = out.data.data() + out.node_count * area_light_floats_node + c * area_light_floats_copy;
            const u32 first_node = std::bit_cast<u32>(r[24]), first_triangle = std::bit_cast<u32>(r[25]);
            CHECK(first_node % 10 == 0 && first_node / 10 < in.size() && first_triangle == first_node / 10 * 7);
            if (first_node / 10 >= in.size()) {
                continue;
            }
            const AreaLightCopy& copy = in[first_node / 10];
            ++seen[first_node / 10];
            CHECK(inside(copy.lo, n) && inside(copy.hi, n));
            // A point there and back; a normal's plane kept (dot(column_i, n) is its turn into the mesh).
            const Vec3f p{0.3f, -0.7f, 1.1f}, w = copy.to_world.apply(p);
            const Vec3f back{r[0] * w.x + r[1] * w.y + r[2] * w.z + r[3], r[4] * w.x + r[5] * w.y + r[6] * w.z + r[7],
                             r[8] * w.x + r[9] * w.y + r[10] * w.z + r[11]};
            CHECK(length(back - p) < 1e-3f);
            const Vec3f col0{r[12], r[13], r[14]};
            CHECK(length(col0 - copy.to_world.apply_vector({1, 0, 0})) < 1e-4f);
        }
    }
    CHECK(std::all_of(seen.begin(), seen.end(), [](u32 v) { return v == 1; }));
}

} // namespace

// A cube's 12 triangles, wound outward, as a soup (each corner repeated, as GpuMesh3D::gi_triangles holds them).
std::vector<Vec3f> cube_soup() {
    const Vec3f v[8] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const int faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 4, 7, 3}};
    std::vector<Vec3f> out;
    for (const auto& f : faces) {
        for (const int i : {f[0], f[1], f[2], f[0], f[2], f[3]}) {
            out.push_back(v[i]);
        }
    }
    return out;
}

void closed_meshes() {
    std::vector<Vec3f> cube = cube_soup();
    CHECK(area_light_closed(cube));
    // Every face outward: N points away from the centre.
    for (std::size_t i = 0; i < cube.size(); i += 3) {
        const Vec3f n = cross(cube[i + 1] - cube[i], cube[i + 2] - cube[i]);
        CHECK(dot(n, (cube[i] + cube[i + 1] + cube[i + 2]) * (1.0f / 3.0f) - Vec3f{0.5f, 0.5f, 0.5f}) > 0.0f);
    }
    std::vector<Vec3f> open(cube.begin(), cube.end() - 3); // a triangle short
    CHECK(!area_light_closed(open));
    std::vector<Vec3f> flipped = cube; // one triangle wound the other way
    std::swap(flipped[1], flipped[2]);
    CHECK(!area_light_closed(flipped));
    std::vector<Vec3f> twice = cube; // two cubes sharing nothing: still closed
    for (const Vec3f& c : cube) {
        twice.push_back(c + Vec3f{3.0f, 0.0f, 0.0f});
    }
    CHECK(area_light_closed(twice));
    std::vector<Vec3f> doubled = cube; // a face twice the same way: an edge three times
    doubled.insert(doubled.end(), cube.begin(), cube.begin() + 3);
    CHECK(!area_light_closed(doubled));
    std::vector<Vec3f> seam = cube; // a seam's copies of a corner a few bits apart, and -0 for 0: still closed
    seam[0] = {-0.0f, 1e-9f, -0.0f};
    seam[4].x = std::nextafter(seam[4].x, 2.0f);
    CHECK(area_light_closed(seam));
    std::vector<Vec3f> crack = cube; // a corner of one triangle moved by a thousandth: open
    crack[0] = {0.001f, 0.0f, 0.0f};
    CHECK(!area_light_closed(crack));
    const std::vector<Vec3f> quad = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    CHECK(!area_light_closed(quad));
    // The flag reaches the shaders: N's w.
    std::vector<AreaLightTriangle> tris;
    for (std::size_t i = 0; i < cube.size(); i += 3) {
        tris.push_back({cube[i], cube[i + 1], cube[i + 2], true});
    }
    tris.push_back({{5, 0, 0}, {6, 0, 0}, {5, 1, 0}});
    const AreaLightMesh mesh = build_area_light_mesh(tris);
    u32 closed = 0;
    for (u32 i = 0; i < mesh.triangle_count; ++i) {
        closed += mesh.triangles[i * area_light_floats_triangle + 15] > 0.5f ? 1 : 0;
    }
    CHECK(closed == 12);
}

// A UV sphere as a soup, its seam's corners computed apart (sin(2 pi) is not sin(0)), as a modeller's export has them.
std::vector<Vec3f> sphere_soup(int around, int down) {
    const auto at = [&](int i, int j) {
        const float u = 6.2831853f * static_cast<float>(i) / static_cast<float>(around);
        const float v = 3.1415927f * static_cast<float>(j) / static_cast<float>(down);
        return Vec3f{std::sin(v) * std::cos(u), std::cos(v), std::sin(v) * std::sin(u)};
    };
    std::vector<Vec3f> out;
    for (int j = 0; j < down; ++j) {
        for (int i = 0; i < around; ++i) {
            const Vec3f a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
            if (j > 0) {
                out.insert(out.end(), {a, b, d});
            }
            if (j + 1 < down) {
                out.insert(out.end(), {b, c, d});
            }
        }
    }
    return out;
}

void coarse_occluders() {
    const std::vector<Vec3f> sphere = sphere_soup(48, 24);
    const std::size_t triangles = sphere.size() / 3;
    CHECK(area_light_closed(sphere));
    // A quarter of its triangles: about that many, still closed, still the sphere's size.
    const std::vector<Vec3f> coarse = simplify_area_light_occluder(sphere, triangles / 4);
    CHECK(coarse.size() % 3 == 0);
    CHECK(coarse.size() / 3 <= triangles / 4 + 8 && coarse.size() / 3 >= triangles / 8);
    CHECK(area_light_closed(coarse));
    float lo = 2.0f, hi = 0.0f;
    for (const Vec3f& c : coarse) {
        lo = std::min(lo, length(c));
        hi = std::max(hi, length(c));
    }
    CHECK(lo > 0.85f && hi < 1.05f);
    // As many as it has, or more: the same corners.
    CHECK(simplify_area_light_occluder(sphere, triangles) == sphere);
    // An open quad's outline stays where it is.
    const std::vector<Vec3f> quad = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    CHECK(simplify_area_light_occluder(quad, 1).size() == 6);
}

void largest_pieces() {
    // A unit square's two triangles pair into one piece of area 1; a lone triangle of the same two edges is half.
    const std::vector<Vec3f> square = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const AreaLightPieces both = area_light_pieces(square);
    CHECK(both.count == 1 && std::abs(both.largest - 1.0f) < 1e-5f);
    const AreaLightPieces lone = area_light_pieces(std::span(square).first(3));
    CHECK(lone.count == 1 && std::abs(lone.largest - 0.5f) < 1e-5f);
    CHECK(area_light_pieces({}).count == 0);
}

int main() {
    closed_meshes();
    largest_pieces();
    coarse_occluders();
    random_scene();
    cornell_like_panel();
    triangles_and_limits();
    copies();
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::puts("area lights: ok");
    return 0;
}
