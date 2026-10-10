#include <kin/render3d/mesh.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace kin {

namespace {

constexpr f32 pi = 3.14159265358979323846f;

bool finite(Vec3f v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// The cofactor matrix of `m`'s linear part: turns normals as `m` turns the
// shape (the inverse transpose times the determinant, here made positive so
// a mirror doesn't turn them in; normals are normalized after).
Mat4f normal_matrix(const Mat4f& m) {
    const Vec3f c0{m.at(0, 0), m.at(1, 0), m.at(2, 0)};
    const Vec3f c1{m.at(0, 1), m.at(1, 1), m.at(2, 1)};
    const Vec3f c2{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    const f32 sign = dot(c0, cross(c1, c2)) < 0.0f ? -1.0f : 1.0f;
    const Vec3f n0 = cross(c1, c2) * sign, n1 = cross(c2, c0) * sign, n2 = cross(c0, c1) * sign;
    Mat4f r;
    for (int row = 0; row < 3; ++row) {
        r.at(row, 0) = (&n0.x)[row];
        r.at(row, 1) = (&n1.x)[row];
        r.at(row, 2) = (&n2.x)[row];
    }
    return r;
}

// A flat quad (two triangles) a, b, c, d counter-clockwise from the front.
void quad(MeshData& mesh, Vec3f a, Vec3f b, Vec3f c, Vec3f d) {
    const Vec3f n = normalize(cross(b - a, c - a));
    const u32 base = static_cast<u32>(mesh.vertices.size());
    mesh.vertices.push_back({a, n, {0.0f, 1.0f}});
    mesh.vertices.push_back({b, n, {1.0f, 1.0f}});
    mesh.vertices.push_back({c, n, {1.0f, 0.0f}});
    mesh.vertices.push_back({d, n, {0.0f, 0.0f}});
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

// A flat triangle, counter-clockwise from the front.
void triangle(MeshData& mesh, Vec3f a, Vec3f b, Vec3f c) {
    const Vec3f n = normalize(cross(b - a, c - a));
    const u32 base = static_cast<u32>(mesh.vertices.size());
    mesh.vertices.push_back({a, n, {0.0f, 0.0f}});
    mesh.vertices.push_back({b, n, {1.0f, 0.0f}});
    mesh.vertices.push_back({c, n, {0.5f, 1.0f}});
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
}

// A flat polygon round the Y axis at height `y` (`up`: facing +Y), from
// `points` (anticlockwise seen from +Y), fanned from its centre.
void cap(MeshData& mesh, const std::vector<Vec2f>& points, f32 y, bool up) {
    const Vec3f n{0.0f, up ? 1.0f : -1.0f, 0.0f};
    const u32 centre = static_cast<u32>(mesh.vertices.size());
    mesh.vertices.push_back({{0.0f, y, 0.0f}, n, {0.5f, 0.5f}});
    for (const Vec2f p : points) {
        mesh.vertices.push_back({{p.x, y, p.y}, n, {0.5f + p.x, 0.5f + p.y}});
    }
    const u32 count = static_cast<u32>(points.size());
    for (u32 i = 0; i < count; ++i) {
        const u32 a = centre + 1 + i, b = centre + 1 + (i + 1) % count;
        // Anticlockwise seen from +Y: a face up as they come, down reversed.
        if (up) {
            mesh.indices.insert(mesh.indices.end(), {centre, a, b});
        } else {
            mesh.indices.insert(mesh.indices.end(), {centre, b, a});
        }
    }
}

// `count` points on a circle of `radius` in the XZ plane, anticlockwise seen
// from +Y (from +X toward -Z).
std::vector<Vec2f> circle(f32 radius, i32 count) {
    std::vector<Vec2f> points;
    points.reserve(static_cast<std::size_t>(count));
    for (i32 i = 0; i < count; ++i) {
        const f32 a = 2.0f * pi * static_cast<f32>(i) / static_cast<f32>(count);
        points.push_back({radius * std::cos(a), -radius * std::sin(a)});
    }
    return points;
}

} // namespace

bool MeshData::validate(std::string* error) const {
    const auto fail = [&](std::string why) {
        if (error) {
            *error = std::move(why);
        }
        return false;
    };
    if (vertices.empty()) {
        return fail("no vertices");
    }
    if (indices.empty() ? vertices.size() % 3 != 0 : indices.size() % 3 != 0) {
        return fail(indices.empty() ? "the vertex count is not a multiple of three (no indices)"
                                    : "the index count is not a multiple of three");
    }
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= vertices.size()) {
            return fail("index " + std::to_string(i) + " is " + std::to_string(indices[i]) + ", past the " +
                        std::to_string(vertices.size()) + " vertices");
        }
    }
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (!finite(vertices[i].position) || !finite(vertices[i].normal)) {
            return fail("vertex " + std::to_string(i) + " is not finite");
        }
    }
    if (!skin.empty()) {
        if (skin.size() != vertices.size()) {
            return fail("the skin has " + std::to_string(skin.size()) + " entries for " + std::to_string(vertices.size()) + " vertices");
        }
        for (std::size_t i = 0; i < skin.size(); ++i) {
            f32 sum = 0.0f;
            for (const f32 w : skin[i].weights) {
                if (!std::isfinite(w) || w < 0.0f) {
                    return fail("vertex " + std::to_string(i) + " has a weight that is negative or not finite");
                }
                sum += w;
            }
            if (!(sum > 0.0f)) {
                return fail("vertex " + std::to_string(i) + " has weights that sum to zero");
            }
        }
    }
    return true;
}

u32 MeshData::skin_joint_count() const {
    if (skin.empty()) {
        return 0;
    }
    u32 count = 1;
    for (const MeshSkinVertex& v : skin) {
        for (int k = 0; k < 4; ++k) {
            if (v.weights[static_cast<std::size_t>(k)] > 0.0f) {
                count = std::max(count, static_cast<u32>(v.joints[static_cast<std::size_t>(k)]) + 1u);
            }
        }
    }
    return count;
}

std::array<u8, 4> quantise_skin_weights(const std::array<f32, 4>& weights) {
    f32 sum = 0.0f;
    for (const f32 w : weights) {
        sum += w > 0.0f ? w : 0.0f;
    }
    if (!(sum > 0.0f)) {
        return {255, 0, 0, 0};
    }
    std::array<u8, 4> out{};
    int total = 0, largest = 0;
    for (std::size_t k = 0; k < 4; ++k) {
        const f32 w = weights[k] > 0.0f ? weights[k] / sum : 0.0f;
        out[k] = static_cast<u8>(std::clamp(static_cast<int>(std::lround(w * 255.0f)), 0, 255));
        total += out[k];
        if (weights[k] > weights[static_cast<std::size_t>(largest)]) {
            largest = static_cast<int>(k);
        }
    }
    // The largest takes what rounding left over, so the four always sum to 255.
    out[static_cast<std::size_t>(largest)] = static_cast<u8>(std::clamp(static_cast<int>(out[static_cast<std::size_t>(largest)]) + 255 - total, 0, 255));
    return out;
}

Bounds3 MeshData::bounds() const {
    if (vertices.empty()) {
        return {};
    }
    Bounds3 b{vertices.front().position, vertices.front().position};
    for (const MeshVertex& v : vertices) {
        b.min = {std::min(b.min.x, v.position.x), std::min(b.min.y, v.position.y), std::min(b.min.z, v.position.z)};
        b.max = {std::max(b.max.x, v.position.x), std::max(b.max.y, v.position.y), std::max(b.max.z, v.position.z)};
    }
    return b;
}

void MeshData::append(const MeshData& other, const Mat4f& transform) {
    // The result is indexed: unindexed vertices so far get indices first.
    if (indices.empty() && !vertices.empty()) {
        for (u32 i = 0; i < static_cast<u32>(vertices.size()); ++i) {
            indices.push_back(i);
        }
    }
    const u32 base = static_cast<u32>(vertices.size());
    if (!skin.empty() || !other.skin.empty()) {
        // One entry for each vertex: those of a mesh without a skin are bound to joint 0.
        skin.resize(vertices.size());
        if (other.skin.empty()) {
            skin.resize(vertices.size() + other.vertices.size());
        } else {
            skin.insert(skin.end(), other.skin.begin(), other.skin.end());
        }
    }
    const Mat4f normals = normal_matrix(transform);
    // A mirroring transform turns the triangles over: keep them counter-clockwise.
    const bool mirrored = transform.determinant() < 0.0f;
    for (MeshVertex v : other.vertices) {
        v.position = transform.apply(v.position);
        v.normal = normalize(normals.apply_vector(v.normal));
        vertices.push_back(v);
    }
    const auto add = [&](u32 a, u32 b, u32 c) {
        if (mirrored) {
            std::swap(b, c);
        }
        indices.insert(indices.end(), {base + a, base + b, base + c});
    };
    if (!other.indices.empty()) {
        for (std::size_t i = 0; i + 2 < other.indices.size(); i += 3) {
            add(other.indices[i], other.indices[i + 1], other.indices[i + 2]);
        }
    } else {
        for (u32 i = 0; i + 2 < static_cast<u32>(other.vertices.size()); i += 3) {
            add(i, i + 1, i + 2);
        }
    }
}

namespace meshes {

MeshData box(Vec3f size) {
    const Vec3f h = size * 0.5f;
    MeshData m;
    m.vertices.reserve(24);
    m.indices.reserve(36);
    quad(m, {-h.x, -h.y, h.z}, {h.x, -h.y, h.z}, {h.x, h.y, h.z}, {-h.x, h.y, h.z});     // +Z
    quad(m, {h.x, -h.y, -h.z}, {-h.x, -h.y, -h.z}, {-h.x, h.y, -h.z}, {h.x, h.y, -h.z}); // -Z
    quad(m, {h.x, -h.y, h.z}, {h.x, -h.y, -h.z}, {h.x, h.y, -h.z}, {h.x, h.y, h.z});     // +X
    quad(m, {-h.x, -h.y, -h.z}, {-h.x, -h.y, h.z}, {-h.x, h.y, h.z}, {-h.x, h.y, -h.z}); // -X
    quad(m, {-h.x, h.y, h.z}, {h.x, h.y, h.z}, {h.x, h.y, -h.z}, {-h.x, h.y, -h.z});     // +Y
    quad(m, {-h.x, -h.y, -h.z}, {h.x, -h.y, -h.z}, {h.x, -h.y, h.z}, {-h.x, -h.y, h.z}); // -Y
    return m;
}

MeshData plane(Vec2f size, i32 cells) {
    cells = std::max(1, cells);
    MeshData m;
    const u32 row = static_cast<u32>(cells) + 1;
    for (i32 z = 0; z <= cells; ++z) {
        for (i32 x = 0; x <= cells; ++x) {
            const f32 u = static_cast<f32>(x) / static_cast<f32>(cells);
            const f32 v = static_cast<f32>(z) / static_cast<f32>(cells);
            m.vertices.push_back({{(u - 0.5f) * size.x, 0.0f, (v - 0.5f) * size.y}, {0.0f, 1.0f, 0.0f}, {u, v}});
        }
    }
    for (u32 z = 0; z < static_cast<u32>(cells); ++z) {
        for (u32 x = 0; x < static_cast<u32>(cells); ++x) {
            const u32 a = z * row + x, b = a + 1, c = a + row + 1, d = a + row;
            // From +Y, counter-clockwise: a (near-left) -> d (far... toward +Z) is
            // toward the viewer, so a, d, c then a, c, b.
            m.indices.insert(m.indices.end(), {a, d, c, a, c, b});
        }
    }
    return m;
}

MeshData sphere(f32 radius, i32 segments, i32 rings) {
    segments = std::max(3, segments);
    rings = std::max(2, rings);
    MeshData m;
    for (i32 r = 0; r <= rings; ++r) {
        const f32 v = static_cast<f32>(r) / static_cast<f32>(rings);
        const f32 polar = pi * v; // 0 at +Y
        for (i32 s = 0; s <= segments; ++s) {
            const f32 u = static_cast<f32>(s) / static_cast<f32>(segments);
            const f32 a = 2.0f * pi * u;
            const Vec3f n{std::sin(polar) * std::cos(a), std::cos(polar), -std::sin(polar) * std::sin(a)};
            m.vertices.push_back({n * radius, n, {u, v}});
        }
    }
    const u32 row = static_cast<u32>(segments) + 1;
    for (u32 r = 0; r < static_cast<u32>(rings); ++r) {
        for (u32 s = 0; s < static_cast<u32>(segments); ++s) {
            const u32 a = r * row + s, b = a + 1, c = a + row + 1, d = a + row;
            // a above d; going round (s up) is anticlockwise from +Y.
            if (r != 0) {
                m.indices.insert(m.indices.end(), {a, d, b});
            }
            if (r + 1 != static_cast<u32>(rings)) {
                m.indices.insert(m.indices.end(), {b, d, c});
            }
        }
    }
    return m;
}

MeshData cylinder(f32 radius, f32 height, i32 segments) {
    segments = std::max(3, segments);
    MeshData m;
    const f32 h = height * 0.5f;
    for (i32 s = 0; s <= segments; ++s) {
        const f32 u = static_cast<f32>(s) / static_cast<f32>(segments);
        const f32 a = 2.0f * pi * u;
        const Vec3f n{std::cos(a), 0.0f, -std::sin(a)};
        m.vertices.push_back({{n.x * radius, h, n.z * radius}, n, {u, 0.0f}});
        m.vertices.push_back({{n.x * radius, -h, n.z * radius}, n, {u, 1.0f}});
    }
    for (u32 s = 0; s < static_cast<u32>(segments); ++s) {
        const u32 top = s * 2, bottom = top + 1, next_top = top + 2, next_bottom = top + 3;
        m.indices.insert(m.indices.end(), {top, bottom, next_bottom, top, next_bottom, next_top});
    }
    const std::vector<Vec2f> ring = circle(radius, segments);
    cap(m, ring, h, true);
    cap(m, ring, -h, false);
    return m;
}

MeshData cone(f32 radius, f32 height, i32 segments) {
    segments = std::max(3, segments);
    MeshData m;
    const f32 h = height * 0.5f;
    const f32 slope = radius / height; // the side's normal leans up by this
    for (i32 s = 0; s < segments; ++s) {
        // One triangle a segment, its normals smooth round the side.
        const f32 a0 = 2.0f * pi * static_cast<f32>(s) / static_cast<f32>(segments);
        const f32 a1 = 2.0f * pi * static_cast<f32>(s + 1) / static_cast<f32>(segments);
        const f32 am = (a0 + a1) * 0.5f;
        const auto normal = [&](f32 a) { return normalize(Vec3f{std::cos(a), slope, -std::sin(a)}); };
        const u32 base = static_cast<u32>(m.vertices.size());
        m.vertices.push_back({{radius * std::cos(a0), -h, -radius * std::sin(a0)}, normal(a0), {0.0f, 1.0f}});
        m.vertices.push_back({{radius * std::cos(a1), -h, -radius * std::sin(a1)}, normal(a1), {1.0f, 1.0f}});
        m.vertices.push_back({{0.0f, h, 0.0f}, normal(am), {0.5f, 0.0f}});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
    }
    cap(m, circle(radius, segments), -h, false);
    return m;
}

MeshData torus(f32 radius, f32 tube, i32 segments, i32 sides) {
    segments = std::max(3, segments);
    sides = std::max(3, sides);
    MeshData m;
    for (i32 s = 0; s <= segments; ++s) {
        const f32 u = static_cast<f32>(s) / static_cast<f32>(segments);
        const f32 a = 2.0f * pi * u;
        const Vec3f out{std::cos(a), 0.0f, -std::sin(a)}; // from the axis to the tube's centre
        for (i32 t = 0; t <= sides; ++t) {
            const f32 v = static_cast<f32>(t) / static_cast<f32>(sides);
            const f32 b = 2.0f * pi * v;
            const Vec3f n = out * std::cos(b) + Vec3f{0.0f, std::sin(b), 0.0f};
            m.vertices.push_back({out * radius + n * tube, n, {u, v}});
        }
    }
    const u32 row = static_cast<u32>(sides) + 1;
    for (u32 s = 0; s < static_cast<u32>(segments); ++s) {
        for (u32 t = 0; t < static_cast<u32>(sides); ++t) {
            const u32 a = s * row + t, b = a + 1, c = a + row + 1, d = a + row;
            m.indices.insert(m.indices.end(), {a, d, c, a, c, b});
        }
    }
    return m;
}

MeshData star_prism(i32 points, f32 outer, f32 inner, f32 height) {
    points = std::max(3, points);
    MeshData m;
    const f32 h = height * 0.5f;
    std::vector<Vec2f> outline;
    for (i32 i = 0; i < points * 2; ++i) {
        const f32 a = pi * 0.5f + pi * static_cast<f32>(i) / static_cast<f32>(points); // a point toward -Z
        const f32 r = i % 2 == 0 ? outer : inner;
        outline.push_back({r * std::cos(a), -r * std::sin(a)});
    }
    cap(m, outline, h, true);
    cap(m, outline, -h, false);
    const std::size_t n = outline.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2f p = outline[i], q = outline[(i + 1) % n];
        // The outline runs anticlockwise from +Y: seen from outside, p to q
        // goes left to right.
        quad(m, {p.x, -h, p.y}, {q.x, -h, q.y}, {q.x, h, q.y}, {p.x, h, p.y});
    }
    return m;
}

MeshData wedge(Vec3f size) {
    const Vec3f h = size * 0.5f;
    MeshData m;
    quad(m, {-h.x, -h.y, h.z}, {h.x, -h.y, h.z}, {h.x, h.y, -h.z}, {-h.x, h.y, -h.z});     // the slope
    quad(m, {h.x, -h.y, -h.z}, {-h.x, -h.y, -h.z}, {-h.x, h.y, -h.z}, {h.x, h.y, -h.z});   // the back, -Z
    quad(m, {-h.x, -h.y, -h.z}, {h.x, -h.y, -h.z}, {h.x, -h.y, h.z}, {-h.x, -h.y, h.z});   // the floor, -Y
    triangle(m, {h.x, -h.y, h.z}, {h.x, -h.y, -h.z}, {h.x, h.y, -h.z});                    // +X side
    triangle(m, {-h.x, -h.y, -h.z}, {-h.x, -h.y, h.z}, {-h.x, h.y, -h.z});                 // -X side
    return m;
}

MeshData octahedron(f32 radius) {
    MeshData m;
    const Vec3f top{0.0f, radius, 0.0f}, bottom{0.0f, -radius, 0.0f};
    const Vec3f ring[4] = {{radius, 0.0f, 0.0f}, {0.0f, 0.0f, -radius}, {-radius, 0.0f, 0.0f}, {0.0f, 0.0f, radius}};
    for (int i = 0; i < 4; ++i) {
        const Vec3f a = ring[i], b = ring[(i + 1) % 4];
        triangle(m, a, b, top);
        triangle(m, b, a, bottom);
    }
    return m;
}

} // namespace meshes

} // namespace kin
