// Summit's render meshes (docs/3d_plan.md, P4a and the look pass of P4b): the level's boxes as chamfered, noise-roughened rock
// with colour from strata, moss and snow; the rock that is only for the eye (buttresses, scree, boulders); and the ground and the
// ranges round the mountain. Pure functions of the level: no GPU, no randomness but the seed's hash.
//
// The collision is the boxes, so the visible rock of a piece keeps to its box: a vertex is moved by a few centimetres along the
// box's own normal, never far from where a hand will find it. The decor (buttresses, scree, boulders) is a list of chunks
// (`decor_chunks`) that both this file's meshes and Game's colliders are made from: each chunk's collider is the convex hull of its
// mesh's vertices, so what you see is what you stand on (P4b's first version had no collider for it; the follow-up added them),
// placed where nobody climbs or walks (checked against the route).

#include "environment.hpp"
#include "level.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace summit {

namespace {

u32 mix(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// A value in 0..1 from three lattice coordinates and a seed.
f32 lattice(i32 x, i32 y, i32 z, u32 seed) {
    return f32(mix(u32(x) * 73856093u ^ u32(y) * 19349663u ^ u32(z) * 83492791u ^ seed) & 0xFFFFu) / 65535.0f;
}

// Smooth value noise at `p` (lattice cell `scale` metres).
f32 value_noise(Vec3f p, f32 scale, u32 seed) {
    const Vec3f q = p * (1.0f / scale);
    const i32 x0 = i32(std::floor(q.x)), y0 = i32(std::floor(q.y)), z0 = i32(std::floor(q.z));
    const f32 fx = q.x - f32(x0), fy = q.y - f32(y0), fz = q.z - f32(z0);
    const auto s = [](f32 t) { return t * t * (3.0f - 2.0f * t); };
    const f32 sx = s(fx), sy = s(fy), sz = s(fz);
    f32 c[2][2][2];
    for (i32 i = 0; i < 2; ++i)
        for (i32 j = 0; j < 2; ++j)
            for (i32 k = 0; k < 2; ++k) c[i][j][k] = lattice(x0 + i, y0 + j, z0 + k, seed);
    const auto l = [](f32 a, f32 b, f32 t) { return a + (b - a) * t; };
    return l(l(l(c[0][0][0], c[1][0][0], sx), l(c[0][1][0], c[1][1][0], sx), sy), l(l(c[0][0][1], c[1][0][1], sx), l(c[0][1][1], c[1][1][1], sx), sy), sz);
}

// Three octaves: broad lumps, blocks and grain, 0..1.
f32 fbm(Vec3f p, f32 scale, u32 seed) {
    return 0.55f * value_noise(p, scale, seed) + 0.3f * value_noise(p, scale * 0.42f, seed + 7u) + 0.15f * value_noise(p, scale * 0.18f, seed + 13u);
}

f32 smooth(f32 a, f32 b, f32 x) {
    const f32 t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

Vec3f col(Color c) { return {f32(c.r) / 255.0f, f32(c.g) / 255.0f, f32(c.b) / 255.0f}; }
Color to_color(Vec3f v) {
    const auto b = [](f32 x) { return u8(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return Color::rgb(b(v.x), b(v.y), b(v.z));
}
Vec3f lerp3(Vec3f a, Vec3f b, f32 t) { return a + (b - a) * std::clamp(t, 0.0f, 1.0f); }

// What a surface is, for its colour.
enum class Surface : u8 { Rock, Smooth, Roof, Hold, Lip, Earth, Scree };

struct Shade {
    Surface surface = Surface::Rock;
    Vec3f base{0.6f, 0.55f, 0.5f};
    u32 seed = 0;
    f32 height = 150.0f;   // the mountain's, for the snow line
    f32 base_y = 0.0f;     // base camp's
};

// The colour of rock at a world point with a world normal: strata (tilted bands, ochre and rust among the grey), mottle and a fine
// grain, moss on the low upward faces, snow on the high ones, dark where it faces down.
Color rock_colour(const Shade& look, Vec3f p, Vec3f n) {
    const f32 alt = p.y - look.base_y;
    const f32 up = n.y;
    Vec3f c = look.base;
    // Only low frequencies here: a vertex a metre apart cannot carry more (the texture has the detail).
    const f32 blotch = fbm(p, 6.0f, look.seed);
    const f32 grain = 0.5f;
    // Bands about 0.6 to 1.4 m thick on a gentle dip, a hash of the band number picks its tone.
    const f32 band_coord = p.y * 0.9f + p.x * 0.07f + p.z * 0.05f + 2.0f * value_noise(p, 9.0f, look.seed + 5u);
    const i32 band = i32(std::floor(band_coord));
    const f32 band_tone = lattice(band, 3, 7, look.seed + 99u);
    const f32 rust = smooth(0.65f, 0.95f, lattice(band, 11, 5, look.seed + 3u));
    c = lerp3(c, Vec3f{0.50f, 0.34f, 0.24f}, 0.45f * rust);
    f32 k = 0.78f + 0.40f * blotch + 0.16f * (band_tone - 0.5f) + 0.08f * (grain - 0.5f);
    if (look.surface == Surface::Smooth) k = 0.92f + 0.10f * (blotch - 0.5f) + 0.04f * (grain - 0.5f);
    c = c * k;
    // Darker on what faces down, a little lighter on what faces up.
    c = c * (0.74f + 0.26f * smooth(-0.8f, 0.5f, up));
    if (look.surface == Surface::Earth) {
        // Grass and meadow on the tops of the ground, earth on its sides; dry and sparse higher up.
        const f32 top = smooth(0.55f, 0.9f, up);
        const f32 low = 1.0f - smooth(0.15f, 0.7f, alt / look.height);
        const Vec3f grass = lerp3(Vec3f{0.30f, 0.36f, 0.19f}, Vec3f{0.34f, 0.40f, 0.20f}, blotch);
        const Vec3f meadow = lerp3(Vec3f{0.40f, 0.38f, 0.25f}, Vec3f{0.33f, 0.31f, 0.22f}, blotch);
        const Vec3f green = lerp3(meadow, grass, low);
        c = lerp3(c, green * (0.8f + 0.4f * grain), top * (0.55f + 0.45f * low));
    } else if (look.surface == Surface::Rock || look.surface == Surface::Scree) {
        if (look.surface == Surface::Rock && up > 0.7f) {
        // A bench or a flat top low down is meadow, not bare rock.
        const f32 low = 1.0f - smooth(0.15f, 0.7f, alt / look.height);
        const Vec3f meadow = lerp3(Vec3f{0.30f, 0.36f, 0.19f}, Vec3f{0.36f, 0.35f, 0.22f}, blotch);
        c = lerp3(c, meadow * (0.85f + 0.3f * blotch), smooth(0.7f, 0.9f, up) * low * 0.85f);
    }
    // Moss and lichen low down on the upward faces and in the cracks' shade.
        const f32 low = 1.0f - smooth(0.05f, 0.5f, alt / look.height);
        const f32 moss = smooth(0.35f, 0.8f, up) * low * smooth(0.35f, 0.7f, fbm(p, 2.5f, look.seed + 41u));
        c = lerp3(c, Vec3f{0.24f, 0.33f, 0.14f} * (0.8f + 0.4f * grain), 0.7f * moss);
        // Yellow lichen splashes.
        const f32 lichen = smooth(0.7f, 0.85f, value_noise(p, 3.0f, look.seed + 61u)) * low * 0.6f;
        c = lerp3(c, Vec3f{0.55f, 0.52f, 0.20f}, lichen);
    }
    // Snow, patchy at the edge of the band: more where the noise is low.
    f32 snow = snow_cover(alt, look.height, up);
    if (snow > 0.0f) {
        snow = smooth(0.0f, 1.0f, snow * 1.5f - 0.5f * fbm(p, 1.6f, look.seed + 71u) + 0.1f);
        c = lerp3(c, Vec3f{0.88f, 0.91f, 0.96f}, snow);
    }
    return to_color(c);
}

// ---- a chamfered box as triangles ------------------------------------------------------------------------------------------

struct Emitter {
    MeshData* mesh = nullptr;
    MeshData* top = nullptr;        // faces that look up go here when it is set (they take another texture)
    const Shade* look = nullptr;
    Mat4f world{};
    Vec3f scale_to_unit{1, 1, 1};   // 1 / half, for the displacement's radial direction
    f32 amp = 0.0f;                 // displacement (m) at 1 noise unit
    f32 uv_scale = 1.0f / 3.0f;
    bool uv_world = true;
    bool smooth = false;            // the faces' grid vertices get normals from the displaced surface (no facets)
    u32 noise_seed = 0;

    // A local point to the world, moved along its radial direction by the noise: a function of position only, so that the pieces
    // of one box that share a point agree on it (no cracks).
    Vec3f place(Vec3f local) const {
        Vec3f w = world.apply(local);
        if (amp > 0.0f) {
            Vec3f d{local.x * scale_to_unit.x, local.y * scale_to_unit.y, local.z * scale_to_unit.z};
            const f32 m = std::max(1e-4f, length(d));
            d = d * (1.0f / m);
            const Vec3f dw = world.apply(d) - world.apply({0, 0, 0});
            const f32 n = fbm(w, 3.0f, noise_seed) - 0.5f;
            w = w + dw * (2.0f * amp * n);
        }
        return w;
    }
    // One triangle (points in the world), flat shaded; `hint` is the outward direction it must face.
    void tri(Vec3f a, Vec3f b, Vec3f c, Vec3f hint) {
        Vec3f n = cross(b - a, c - a);
        const f32 len = length(n);
        if (len < 1e-9f) return;
        n = n * (1.0f / len);
        if (dot(n, hint) < 0.0f) {
            std::swap(b, c);
            n = n * -1.0f;
        }
        const Vec3f mid = (a + b + c) * (1.0f / 3.0f);
        const Color colour = rock_colour(*look, mid, n);
        MeshData* sink = (top != nullptr && n.y > 0.7f) ? top : mesh;
        // Mapping by the dominant axis of the normal: continuous across a face's triangles, in metres.
        const f32 ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
        const auto uv = [&](Vec3f p) -> Vec2f {
            if (ay >= ax && ay >= az) return {p.x * uv_scale, p.z * uv_scale};
            if (ax >= az) return {p.z * uv_scale, p.y * uv_scale};
            return {p.x * uv_scale, p.y * uv_scale};
        };
        const u32 first = u32(sink->vertices.size());
        for (const Vec3f& p : {a, b, c}) {
            MeshVertex v;
            v.position = p;
            v.normal = n;
            v.uv = uv(p);
            v.color = colour;
            sink->vertices.push_back(v);
        }
        sink->indices.insert(sink->indices.end(), {first, first + 1, first + 2});
    }
    void quad(Vec3f a, Vec3f b, Vec3f c, Vec3f d, Vec3f hint) {
        tri(a, b, c, hint);
        tri(a, c, d, hint);
    }
    // A triangle with the normals given (smooth shading across a face's grid).
    void tri_smooth(Vec3f a, Vec3f b, Vec3f c, Vec3f na, Vec3f nb, Vec3f nc, Vec3f hint) {
        Vec3f n = cross(b - a, c - a);
        if (length(n) < 1e-9f) return;
        if (dot(n, hint) < 0.0f) {
            std::swap(b, c);
            std::swap(nb, nc);
        }
        const Vec3f nm = normalize(na + nb + nc);
        const f32 ax = std::fabs(nm.x), ay = std::fabs(nm.y), az = std::fabs(nm.z);
        const auto uv = [&](Vec3f p) -> Vec2f {
            if (ay >= ax && ay >= az) return {p.x * uv_scale, p.z * uv_scale};
            if (ax >= az) return {p.z * uv_scale, p.y * uv_scale};
            return {p.x * uv_scale, p.y * uv_scale};
        };
        MeshData* sink = (top != nullptr && nm.y > 0.7f) ? top : mesh;
        const u32 first = u32(sink->vertices.size());
        const Vec3f ps[3] = {a, b, c}, ns[3] = {na, nb, nc};
        for (int i = 0; i < 3; ++i) {
            MeshVertex v;
            v.position = ps[i];
            v.normal = ns[i];
            v.uv = uv(ps[i]);
            v.color = rock_colour(*look, ps[i], ns[i]);
            sink->vertices.push_back(v);
        }
        sink->indices.insert(sink->indices.end(), {first, first + 1, first + 2});
    }
    // The normal of the displaced surface at a local point of a face (finite differences along the face's two axes).
    Vec3f surface_normal(Vec3f local, i32 u, i32 v, Vec3f hint) const {
        const f32 e = 0.25f;
        Vec3f du{}, dv{};
        (u == 0 ? du.x : u == 1 ? du.y : du.z) = e;
        (v == 0 ? dv.x : v == 1 ? dv.y : dv.z) = e;
        const Vec3f p0 = place(local), pu = place(local + du), pv = place(local + dv);
        Vec3f n = cross(pu - p0, pv - p0);
        const f32 len = length(n);
        if (len < 1e-12f) return normalize(hint);
        n = n * (1.0f / len);
        return dot(n, hint) < 0.0f ? n * -1.0f : n;
    }
};

// A box of half-sizes `h` with every edge chamfered by `bevel`, its faces in cells of about `cell` metres, appended to `mesh` in
// the world through `e.world`. Points are placed through Emitter::place (the noise), so a displaced box stays closed.
void chamfered_box(Emitter& e, Vec3f h, f32 bevel, f32 cell) {
    const f32 hs[3] = {h.x, h.y, h.z};
    const f32 b = std::clamp(bevel, 0.0f, 0.45f * std::min({hs[0], hs[1], hs[2]}));
    std::vector<f32> lines[3];
    for (i32 a = 0; a < 3; ++a) {
        const f32 ext = hs[a] - b;
        const i32 n = std::clamp(i32(std::ceil(2.0f * ext / cell)), 1, 48);
        for (i32 i = 0; i <= n; ++i) lines[a].push_back(-ext + 2.0f * ext * f32(i) / f32(n));
    }
    const auto pt = [&](i32 a, f32 sa, i32 u, f32 vu, i32 v, f32 vv) {
        f32 c[3];
        c[a] = sa * hs[a];
        c[u] = vu;
        c[v] = vv;
        return e.place({c[0], c[1], c[2]});
    };
    // The faces.
    for (i32 a = 0; a < 3; ++a) {
        for (i32 s = -1; s <= 1; s += 2) {
            const i32 u = (a + 1) % 3, v = (a + 2) % 3;
            Vec3f outward{};
            (a == 0 ? outward.x : a == 1 ? outward.y : outward.z) = f32(s);
            const Vec3f hint = e.world.apply(outward) - e.world.apply({0, 0, 0});
            const auto local_at = [&](std::size_t i, std::size_t j) {
                f32 c[3];
                c[a] = f32(s) * hs[a];
                c[u] = lines[u][i];
                c[v] = lines[v][j];
                return Vec3f{c[0], c[1], c[2]};
            };
            for (std::size_t j = 0; j + 1 < lines[v].size(); ++j) {
                for (std::size_t i = 0; i + 1 < lines[u].size(); ++i) {
                    if (e.smooth) {
                        const Vec3f l00 = local_at(i, j), l10 = local_at(i + 1, j), l11 = local_at(i + 1, j + 1), l01 = local_at(i, j + 1);
                        const Vec3f p00 = e.place(l00), p10 = e.place(l10), p11 = e.place(l11), p01 = e.place(l01);
                        const Vec3f n00 = e.surface_normal(l00, u, v, hint), n10 = e.surface_normal(l10, u, v, hint),
                                    n11 = e.surface_normal(l11, u, v, hint), n01 = e.surface_normal(l01, u, v, hint);
                        e.tri_smooth(p00, p10, p11, n00, n10, n11, hint);
                        e.tri_smooth(p00, p11, p01, n00, n11, n01, hint);
                    } else {
                        e.quad(pt(a, f32(s), u, lines[u][i], v, lines[v][j]), pt(a, f32(s), u, lines[u][i + 1], v, lines[v][j]),
                               pt(a, f32(s), u, lines[u][i + 1], v, lines[v][j + 1]), pt(a, f32(s), u, lines[u][i], v, lines[v][j + 1]), hint);
                    }
                }
            }
        }
    }
    if (b <= 0.0f) return;
    // The edges: between face (a, sa) and face (c, sc), along the third axis w.
    for (i32 a = 0; a < 3; ++a) {
        for (i32 c = a + 1; c < 3; ++c) {
            const i32 w = 3 - a - c;
            for (i32 sa = -1; sa <= 1; sa += 2) {
                for (i32 sc = -1; sc <= 1; sc += 2) {
                    Vec3f outward{};
                    (a == 0 ? outward.x : a == 1 ? outward.y : outward.z) += f32(sa);
                    (c == 0 ? outward.x : c == 1 ? outward.y : outward.z) += f32(sc);
                    const Vec3f hint = e.world.apply(outward) - e.world.apply({0, 0, 0});
                    for (std::size_t i = 0; i + 1 < lines[w].size(); ++i) {
                        const auto at = [&](f32 va, f32 vc, f32 vw) {
                            f32 p[3];
                            p[a] = va;
                            p[c] = vc;
                            p[w] = vw;
                            return e.place({p[0], p[1], p[2]});
                        };
                        const f32 w0 = lines[w][i], w1 = lines[w][i + 1];
                        const f32 ha = f32(sa) * hs[a], hc = f32(sc) * hs[c];
                        const f32 ia = f32(sa) * (hs[a] - b), ic = f32(sc) * (hs[c] - b);
                        // From face a's border (a at its full, c pulled in) to face c's (c full, a pulled in).
                        e.quad(at(ha, ic, w0), at(ha, ic, w1), at(ia, hc, w1), at(ia, hc, w0), hint);
                    }
                }
            }
        }
    }
    // The corners: a triangle of the three borders.
    for (i32 sx = -1; sx <= 1; sx += 2) {
        for (i32 sy = -1; sy <= 1; sy += 2) {
            for (i32 sz = -1; sz <= 1; sz += 2) {
                const f32 x = f32(sx), y = f32(sy), z = f32(sz);
                const Vec3f A = e.place({x * h.x, y * (h.y - b), z * (h.z - b)});
                const Vec3f B = e.place({x * (h.x - b), y * h.y, z * (h.z - b)});
                const Vec3f C = e.place({x * (h.x - b), y * (h.y - b), z * h.z});
                e.tri(A, B, C, e.world.apply({x, y, z}) - e.world.apply({0, 0, 0}));
            }
        }
    }
}

u32 piece_seed(const SummitLevel& level, std::size_t i) { return u32(level.seed) * 2654435761u + u32(i) * 40503u; }

// A rock tone for a piece: the hash picks between warm and cool greys and the odd ochre.
Vec3f rock_base(u32 seed, f32 alt_fraction) {
    const f32 r = f32(mix(seed ^ 0x9e3779b9u) & 0xFFFFu) / 65535.0f;
    const Vec3f warm{0.52f, 0.43f, 0.35f}, cool{0.45f, 0.45f, 0.47f}, ochre{0.58f, 0.47f, 0.31f}, pale{0.62f, 0.62f, 0.64f};
    Vec3f c = r < 0.45f ? lerp3(warm, cool, r / 0.45f) : r < 0.8f ? lerp3(cool, ochre, (r - 0.45f) / 0.35f) : lerp3(ochre, warm, (r - 0.8f) / 0.2f);
    // Paler and bluer higher up (the high rock is bare and frost-shattered).
    return lerp3(c, pale, 0.5f * smooth(0.3f, 1.0f, alt_fraction)) * 1.28f;
}

} // namespace

std::vector<RenderMeshData> build_render_meshes(const SummitLevel& level) {
    enum Class { Rock, Smooth, Roof, Hold, Lip, Ground, Count };
    std::vector<RenderMeshData> out(Count);
    const char* names[Count] = {"rock", "smooth", "roof", "hold", "lip", "ground"};
    for (i32 i = 0; i < Count; ++i) {
        out[std::size_t(i)].name = names[i];
        // The texture, normal map and tint are the view's (it has the GPU): white here, times the vertex colours.
        out[std::size_t(i)].material = {.color = colors::white, .roughness = 0.94f};
    }
    const f32 height = std::max(1.0f, level.height());
    for (std::size_t i = 0; i < level.pieces.size(); ++i) {
        const Piece& p = level.pieces[i];
        Class c = Rock;
        Surface surface = Surface::Rock;
        f32 bevel = 0.12f, amp = 0.035f, cell = 1.3f;
        switch (p.kind) {
        case PieceKind::Equipment: continue; // drawn from these same box descriptors by SummitView
        case PieceKind::Ground: c = Ground; surface = Surface::Earth; bevel = 0.0f; amp = 0.03f; cell = 1.2f; break;
        case PieceKind::Ramp:
            c = p.tag == 20 ? Smooth : Ground; surface = p.tag == 20 ? Surface::Smooth : Surface::Earth;
            bevel = 0.0f; amp = 0.03f; cell = 1.2f; break;
        case PieceKind::Cliff: c = Rock; cell = 0.9f; break;
        case PieceKind::Boulder: c = Rock; bevel = 0.22f; amp = 0.04f; cell = 0.8f; break;
        case PieceKind::Roof: c = Roof; surface = Surface::Roof; bevel = 0.05f; amp = 0.02f; cell = 1.0f; break;
        case PieceKind::Smooth: c = Smooth; surface = Surface::Smooth; bevel = 0.06f; amp = 0.012f; cell = 0.9f; break;
        case PieceKind::Hold: c = Hold; surface = Surface::Hold; bevel = 0.03f; amp = 0.0f; cell = 0.4f; break;
        case PieceKind::Lip: c = Lip; surface = Surface::Lip; bevel = 0.04f; amp = 0.0f; cell = 0.5f; break;
        case PieceKind::Post: c = Roof; surface = Surface::Roof; bevel = 0.03f; amp = 0.0f; cell = 0.5f; break;
        }
        Shade look;
        look.surface = surface;
        look.seed = piece_seed(level, i);
        look.height = height;
        look.base_y = level.start.y;
        const f32 alt_fraction = std::clamp((p.centre.y - level.start.y) / height, 0.0f, 1.0f);
        look.base = rock_base(look.seed, alt_fraction);
        if (surface == Surface::Smooth) look.base = {0.56f, 0.59f, 0.66f};
        if (surface == Surface::Roof) look.base = look.base * 0.78f;
        if (surface == Surface::Hold) look.base = {0.85f, 0.45f, 0.18f};
        if (surface == Surface::Lip) look.base = {0.78f, 0.70f, 0.55f};
        if (surface == Surface::Earth) look.base = {0.42f, 0.33f, 0.25f};
        Emitter e;
        e.mesh = &out[std::size_t(c)].mesh;
        // The tops of rock (a bench, a boulder) are meadow, moss and snow: they go with the ground's meshes and its texture.
        if (c == Rock) e.top = &out[std::size_t(Ground)].mesh;
        e.look = &look;
        e.world = Mat4f::trs(p.centre, p.rotation, {1.0f, 1.0f, 1.0f});
        e.scale_to_unit = {1.0f / p.half.x, 1.0f / p.half.y, 1.0f / p.half.z};
        e.amp = amp;
        e.smooth = true;
        e.noise_seed = look.seed + 1u;
        chamfered_box(e, p.half, bevel, cell);
    }
    std::vector<RenderMeshData> kept;
    for (RenderMeshData& m : out) {
        if (!m.mesh.vertices.empty()) kept.push_back(std::move(m));
    }
    return kept;
}

// ---- the rock that is only for the eye -------------------------------------------------------------------------------------

namespace {

// Distance from `p` to the route (the legs as segments) and the camps.
f32 route_distance(const SummitLevel& level, Vec3f p) {
    f32 best = 1e9f;
    const auto segment = [&](Vec3f a, Vec3f b) {
        const Vec3f ab = b - a;
        const f32 l2 = std::max(1e-6f, dot(ab, ab));
        const f32 t = std::clamp(dot(p - a, ab) / l2, 0.0f, 1.0f);
        best = std::min(best, length(p - (a + ab * t)));
    };
    for (const Leg& l : level.route) segment(l.from, l.to);
    for (const Leg& l : level.expedition.sheltered_route) segment(l.from, l.to);
    if (level.expedition.enabled) {
        for (Vec3f point : {level.expedition.briefing, level.expedition.beacon, level.expedition.extraction})
            best = std::min(best, length(p - point) - 3.0f);
    }
    for (const Camp& c : level.camps) {
        best = std::min(best, length(p - c.spawn) - 3.0f);
        best = std::min(best, length(p - c.marker) - 2.0f);
    }
    for (const RopeSpec& r : level.ropes) segment(r.anchor, r.anchor - Vec3f{0.0f, r.length, 0.0f});
    return best;
}

struct Rng {
    u32 s;
    explicit Rng(u32 seed) : s(mix(seed ^ 0xA511E9B3u)) {}
    f32 uni() {
        s = mix(s + 0x9e3779b9u);
        return f32(s & 0xFFFFFFu) / 16777216.0f;
    }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * uni(); }
};

} // namespace

namespace {

// What a decor chunk looks like, from its kind (the colour class and the strata's).
Surface decor_surface(DecorKind kind) { return kind == DecorKind::Buttress ? Surface::Rock : Surface::Scree; }

// A chunk's triangles appended to `mesh`. The cell size follows the chunk's thinnest side (a coarse grid on a 10 m block, a
// finer one on a pebble).
void emit_chunk(MeshData& mesh, const SummitLevel& level, const DecorChunk& c) {
    const f32 height = std::max(1.0f, level.height());
    Shade look;
    look.surface = decor_surface(c.kind);
    look.seed = c.seed;
    look.height = height;
    look.base_y = level.start.y;
    look.base = rock_base(c.seed, std::clamp((c.centre.y - level.start.y) / height, 0.0f, 1.0f));
    Emitter e;
    e.mesh = &mesh;
    e.look = &look;
    e.world = Mat4f::trs(c.centre, c.rotation, {1.0f, 1.0f, 1.0f});
    e.scale_to_unit = {1.0f / c.half.x, 1.0f / c.half.y, 1.0f / c.half.z};
    e.amp = c.amp;
    e.noise_seed = c.seed + 5u;
    chamfered_box(e, c.half, c.bevel, std::max(0.5f, 0.45f * std::min({c.half.x, c.half.y, c.half.z})));
}

} // namespace

MeshData decor_chunk_mesh(const SummitLevel& level, const DecorChunk& chunk) {
    MeshData m;
    emit_chunk(m, level, chunk);
    return m;
}

std::vector<Vec3f> decor_chunk_points(const SummitLevel& level, const DecorChunk& chunk) {
    const MeshData m = decor_chunk_mesh(level, chunk);
    // Faces are separate triangles: the same point comes many times, so keep each once (to a millimetre).
    std::vector<Vec3f> pts;
    pts.reserve(m.vertices.size() / 4);
    std::vector<std::array<i32, 3>> keys;
    keys.reserve(m.vertices.size());
    for (std::size_t i = 0; i < m.vertices.size(); ++i) {
        const Vec3f& p = m.vertices[i].position;
        keys.push_back({i32(std::lround(p.x * 1000.0f)), i32(std::lround(p.y * 1000.0f)), i32(std::lround(p.z * 1000.0f))});
    }
    std::vector<u32> order(m.vertices.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = u32(i);
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return keys[a] < keys[b]; });
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (i == 0 || keys[order[i]] != keys[order[i - 1]]) pts.push_back(m.vertices[order[i]].position);
    }
    return pts;
}

std::vector<DecorChunk> decor_chunks(const SummitLevel& level) {
    std::vector<DecorChunk> out;

    // Buttresses: wings of leaning, chamfered rock at the ends of each wide cliff, and a pair of lower shoulders in front, so
    // that the face stops being a straight cut. They stand outside the face's width (the bench is 26 m wide), so no route goes
    // there. Since they collide (P4b follow-up) a block that would meet the route is left out.
    for (std::size_t i = 0; i < level.pieces.size(); ++i) {
        const Piece& p = level.pieces[i];
        if ((p.kind != PieceKind::Cliff && p.kind != PieceKind::Smooth) || p.half.x < 10.0f) continue;
        Rng rng(piece_seed(level, i) + 77u);
        const Quatf yaw = p.rotation;
        const f32 top = p.half.y; // local y of the top face
        for (i32 side = -1; side <= 1; side += 2) {
            // Two or three blocks stepping outward and down from the cliff's end.
            const i32 count = 2 + i32(rng.uni() * 1.99f);
            f32 x = f32(side) * (p.half.x + 0.8f);
            f32 y_top = top - rng.range(0.0f, 1.2f);
            for (i32 k = 0; k < count; ++k) {
                // Every draw in a fixed order (argument order is the compiler's choice).
                const f32 w = rng.range(2.2f, 4.2f), d = rng.range(5.0f, 11.0f);
                const f32 h = std::min(p.half.y * 2.0f, rng.range(6.0f, 11.0f) + f32(count - k) * 1.5f);
                // In front of the back of the cliff: the block's front is level with the face or set back a little.
                const f32 z = p.half.z - d - rng.range(-0.5f, 1.8f);
                const Vec3f local{x + f32(side) * w * 0.7f, y_top - h * 0.5f, z};
                const Vec3f centre = p.centre + yaw.rotate(local);
                const f32 lean = f32(side) * rng.range(-3.0f, 9.0f);
                const f32 nod = rng.range(-4.0f, 4.0f);
                const Quatf tilt = Quatf::axis_angle({0, 0, 1}, lean) * Quatf::axis_angle({1, 0, 0}, nod);
                const Quatf jitter = Quatf::axis_angle({0, 1, 0}, rng.range(-14.0f, 14.0f));
                DecorChunk c;
                c.kind = DecorKind::Buttress;
                c.centre = centre;
                c.half = {w, h * 0.5f, d * 0.5f};
                c.rotation = yaw * jitter * tilt;
                c.bevel = 0.35f + 0.25f * rng.uni();
                // Small: the collider is the hull of the mesh, so a bump is what you could not stand in.
                c.amp = 0.02f + 0.015f * rng.uni();
                c.seed = piece_seed(level, i) + u32(k) * 131u + u32(side + 2) * 17u;
                x += f32(side) * w * 1.2f;
                y_top -= h * rng.range(0.25f, 0.5f);
                if (route_distance(level, centre) < std::max({w, h * 0.5f, d * 0.5f}) + 1.5f) continue;
                out.push_back(c);
            }
        }
    }

    // Scree and boulders on the ground tops, off the route: small chamfered stones, bigger ones at the foot of the faces.
    for (std::size_t i = 0; i < level.pieces.size(); ++i) {
        const Piece& p = level.pieces[i];
        if (p.kind != PieceKind::Ground && p.kind != PieceKind::Ramp) continue;
        Rng rng(piece_seed(level, i) + 5u);
        const f32 area = 4.0f * p.half.x * p.half.z;
        const i32 want = std::clamp(i32(area / 60.0f), 2, 22);
        const Vec3f up = p.rotation.rotate({0, 1, 0});
        for (i32 k = 0; k < want; ++k) {
            const f32 x = rng.range(-p.half.x, p.half.x), z = rng.range(-p.half.z, p.half.z);
            const bool big = rng.uni() < 0.2f;
            const f32 size = big ? rng.range(0.7f, 1.5f) : rng.range(0.18f, 0.6f);
            const Vec3f top_local{x, p.half.y, z};
            const Vec3f at = p.centre + p.rotation.rotate(top_local) + up * (size * 0.25f);
            if (route_distance(level, at) < size + 3.2f) continue;
            // Not where another ground piece covers this one (a bench over the ramp's end).
            bool covered = false;
            for (std::size_t j = 0; j < level.pieces.size() && !covered; ++j) {
                if (j == i || (level.pieces[j].kind != PieceKind::Ground && level.pieces[j].kind != PieceKind::Ramp && level.pieces[j].kind != PieceKind::Cliff))
                    continue;
                const Piece& q = level.pieces[j];
                const Vec3f l = q.rotation.conjugate().rotate(at - q.centre);
                covered = std::fabs(l.x) < q.half.x + 0.2f && std::fabs(l.y) < q.half.y + 0.05f && std::fabs(l.z) < q.half.z + 0.2f;
            }
            if (covered) continue;
            const f32 hx = size * rng.range(0.8f, 1.5f), hy = size * rng.range(0.5f, 0.9f), hz = size * rng.range(0.8f, 1.5f);
            const f32 turn = rng.range(0.0f, 360.0f);
            const f32 rock = rng.range(-12.0f, 12.0f);
            DecorChunk c;
            c.kind = big ? DecorKind::Boulder : DecorKind::Scree;
            c.centre = at;
            c.half = {hx, hy, hz};
            c.rotation = p.rotation * Quatf::axis_angle({0, 1, 0}, turn) * Quatf::axis_angle({1, 0, 0}, rock);
            c.bevel = 0.25f * std::min({hx, hy, hz});
            c.amp = std::min(0.04f, 0.12f * size);
            c.seed = piece_seed(level, i) + u32(k) * 7919u;
            out.push_back(c);
        }
    }
    return out;
}

std::vector<RenderMeshData> build_decor_meshes(const SummitLevel& level) {
    std::vector<RenderMeshData> out(2);
    out[0].name = "buttress";
    out[1].name = "scree";
    for (RenderMeshData& m : out) m.material = {.color = colors::white, .roughness = 0.95f};
    for (const DecorChunk& c : decor_chunks(level)) emit_chunk(out[c.kind == DecorKind::Buttress ? 0 : 1].mesh, level, c);
    std::vector<RenderMeshData> kept;
    for (RenderMeshData& m : out) {
        if (!m.mesh.vertices.empty()) kept.push_back(std::move(m));
    }
    return kept;
}

// ---- the ground and the ranges ---------------------------------------------------------------------------------------------

namespace {

// The ground's height under (x, z): a low rolling meadow near the mountain, rising into ranges 600 m and more away.
f32 terrain_height(const SummitLevel& level, f32 x, f32 z, f32 cx, f32 cz) {
    const f32 base = level.start.y - 0.9f;
    const f32 dx = x - cx, dz = z - cz;
    const f32 d = std::sqrt(dx * dx + dz * dz);
    const u32 s = u32(level.seed) * 977u + 5u;
    const Vec3f p{x, 0.0f, z};
    // The rolling meadow: always below the base camp field's top near the mountain, a few metres of swell farther out.
    const f32 near = (fbm(p, 55.0f, s) - 0.5f) * 10.0f;
    const f32 swell = smooth(60.0f, 360.0f, d);
    f32 h = base - 1.5f * (1.0f - swell) + near * swell - 4.0f * (1.0f - smooth(0.0f, 70.0f, d)) * 0.0f;
    // A valley floor that dips away from the mountain's foot, then the ranges: ridged noise (1 - |2n - 1|), tallest far out.
    const f32 ridge_w = smooth(380.0f, 1100.0f, d);
    const f32 r1 = 1.0f - std::fabs(2.0f * fbm(p, 520.0f, s + 11u) - 1.0f);
    const f32 r2 = 1.0f - std::fabs(2.0f * fbm(p, 190.0f, s + 29u) - 1.0f);
    h += ridge_w * (330.0f * r1 * r1 + 70.0f * r2 * r2 * r1);
    // Foothills.
    h += smooth(150.0f, 520.0f, d) * (1.0f - ridge_w) * 36.0f * fbm(p, 120.0f, s + 3u);
    return h;
}

} // namespace

std::vector<RenderMeshData> build_terrain_meshes(const SummitLevel& level) {
    std::vector<RenderMeshData> out(1);
    out[0].name = "terrain";
    out[0].material = {.color = colors::white, .casts_shadow = false, .roughness = 0.97f};
    MeshData& mesh = out[0].mesh;
    const Bounds3 b = level.bounds;
    const f32 cx = 0.5f * (b.min.x + b.max.x), cz = 0.5f * (b.min.z + b.max.z);
    constexpr i32 rings = 80, segments = 128;
    constexpr f32 outer = 2600.0f;
    const f32 height = std::max(1.0f, level.height());
    const u32 s = u32(level.seed) * 977u + 5u;
    // A polar grid, rings packed near the mountain (spacing grows with distance).
    std::vector<Vec3f> pos;
    pos.reserve(std::size_t(rings + 1) * std::size_t(segments));
    for (i32 r = 0; r <= rings; ++r) {
        const f32 t = f32(r) / f32(rings);
        const f32 d = outer * std::pow(t, 2.3f);
        for (i32 a = 0; a < segments; ++a) {
            const f32 ang = 2.0f * pi * f32(a) / f32(segments);
            const f32 x = cx + d * std::cos(ang), z = cz + d * std::sin(ang);
            pos.push_back({x, terrain_height(level, x, z, cx, cz), z});
        }
    }
    const auto colour_at = [&](Vec3f p, Vec3f n) {
        const f32 alt = p.y - level.start.y;
        const f32 d = std::sqrt((p.x - cx) * (p.x - cx) + (p.z - cz) * (p.z - cz));
        const f32 blotch = fbm(p, 24.0f, s + 7u), fine = 0.5f;
        const f32 slope = 1.0f - std::clamp(n.y, 0.0f, 1.0f);
        const Vec3f grass = lerp3(Vec3f{0.26f, 0.33f, 0.16f}, Vec3f{0.33f, 0.38f, 0.19f}, blotch);
        const Vec3f dry = lerp3(Vec3f{0.40f, 0.36f, 0.23f}, Vec3f{0.34f, 0.29f, 0.21f}, blotch);
        const Vec3f rock = lerp3(Vec3f{0.44f, 0.42f, 0.40f}, Vec3f{0.52f, 0.50f, 0.50f}, blotch);
        // Meadow low down and near, drier and then bare rock with height and with slope, snow on the high ranges.
        Vec3f c = lerp3(grass, dry, smooth(60.0f, 200.0f, alt) * 0.8f + 0.2f * fine);
        c = lerp3(c, rock, std::max(smooth(0.25f, 0.55f, slope), smooth(140.0f, 320.0f, alt)));
        c = c * (0.85f + 0.3f * fine);
        const f32 snow = smooth(190.0f, 300.0f, alt + 40.0f * (fbm(p, 60.0f, s + 17u) - 0.5f)) * smooth(0.1f, 0.5f, n.y + 0.2f);
        c = lerp3(c, Vec3f{0.86f, 0.89f, 0.94f}, snow);
        // Far ridges a little bluer (the fog does the rest).
        c = lerp3(c, Vec3f{0.50f, 0.56f, 0.66f}, smooth(900.0f, 2400.0f, d) * 0.25f);
        (void)height;
        return to_color(c);
    };
    // Vertex normals from the height field's own slope (central differences), colours per vertex: no facets.
    const auto normal_at = [&](Vec3f p) {
        const f32 e = 6.0f;
        const f32 hx = terrain_height(level, p.x + e, p.z, cx, cz) - terrain_height(level, p.x - e, p.z, cx, cz);
        const f32 hz = terrain_height(level, p.x, p.z + e, cx, cz) - terrain_height(level, p.x, p.z - e, cx, cz);
        return normalize(Vec3f{-hx / (2.0f * e), 1.0f, -hz / (2.0f * e)});
    };
    std::vector<Vec3f> normals(pos.size());
    for (std::size_t i = 0; i < pos.size(); ++i) normals[i] = normal_at(pos[i]);
    mesh.vertices.reserve(pos.size());
    for (std::size_t i = 0; i < pos.size(); ++i) {
        MeshVertex v;
        v.position = pos[i];
        v.normal = normals[i];
        v.uv = {pos[i].x * 0.2f, pos[i].z * 0.2f};
        v.color = colour_at(pos[i], normals[i]);
        mesh.vertices.push_back(v);
    }
    const auto emit = [&](u32 a, u32 bb, u32 c) {
        // Counter-clockwise seen from above.
        const Vec3f n = cross(pos[bb] - pos[a], pos[c] - pos[a]);
        if (n.y >= 0.0f) mesh.indices.insert(mesh.indices.end(), {a, bb, c});
        else mesh.indices.insert(mesh.indices.end(), {a, c, bb});
    };
    for (i32 r = 0; r < rings; ++r) {
        for (i32 a = 0; a < segments; ++a) {
            const i32 a1 = (a + 1) % segments;
            const u32 p00 = u32(r * segments + a), p01 = u32(r * segments + a1);
            const u32 p10 = u32((r + 1) * segments + a), p11 = u32((r + 1) * segments + a1);
            if (r == 0) {
                emit(p00, p10, p11);
            } else {
                emit(p00, p10, p11);
                emit(p00, p11, p01);
            }
        }
    }
    return out;
}

} // namespace summit
