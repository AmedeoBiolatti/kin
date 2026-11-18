#include "bake_bvh3d.hpp"
#include "hdr_encode3d.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <kin/assets/content.hpp>
#include <kin/core/jobs.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/core/rng.hpp>
#include <kin/render3d/baked_lighting3d.hpp>
#include <limits>
#include <sstream>

extern "C" {
float* kin_hdr_load(const unsigned char* bytes, int size, int* width, int* height);
void kin_hdr_free(float* pixels);
}

namespace kin {

namespace {

using bake::RayHit;
using bake::TriangleBvh;

using math3d_detail::pi;
using math3d_detail::radians;
Vec2f operator*(Vec2f a, f32 s) {
    return {a.x * s, a.y * s};
}
Vec2f operator+(Vec2f a, Vec2f b) {
    return {a.x + b.x, a.y + b.y};
}
bool black(LinearRgb c) {
    return !(c.r > 0.0f || c.g > 0.0f || c.b > 0.0f);
}

f32 srgb_to_linear(f32 c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// sRGB bytes to linear, once.
const std::array<f32, 256>& srgb_table() {
    static const std::array<f32, 256> table = [] {
        std::array<f32, 256> t{};
        for (u32 i = 0; i < 256; ++i)
            t[i] = srgb_to_linear(static_cast<f32>(i) / 255.0f);
        return t;
    }();
    return table;
}

f32 fract(f32 x) {
    return x - std::floor(x);
}

// An orthonormal frame round n (Duff et al.).
void frame_of(Vec3f n, Vec3f& t, Vec3f& b) {
    const f32 sign = std::copysign(1.0f, n.z);
    const f32 a = -1.0f / (sign + n.z);
    const f32 c = n.x * n.y * a;
    t = {1.0f + sign * n.x * n.x * a, sign * c, -sign * n.x};
    b = {c, sign + n.y * n.y * a, -n.y};
}

// Low-discrepancy pairs (Roberts' R2), turned by a random offset for each texel or probe.
constexpr f32 r2_a = 0.7548776662466927f, r2_b = 0.5698402909980532f;

struct Sequence {
    f32 u0 = 0.0f, v0 = 0.0f, j0 = 0.0f, k0 = 0.0f;
    explicit Sequence(RngKey key)
        : u0(rng_f32(fold_in(key, 0), 0.0f, 1.0f)), v0(rng_f32(fold_in(key, 1), 0.0f, 1.0f)),
          j0(rng_f32(fold_in(key, 2), 0.0f, 1.0f)), k0(rng_f32(fold_in(key, 3), 0.0f, 1.0f)) {}
    Vec2f dir(u32 i) const { return {fract(u0 + static_cast<f32>(i) * r2_a), fract(v0 + static_cast<f32>(i) * r2_b)}; }
    // A second, independent pair (where in the texel, or where on a light).
    Vec2f jitter(u32 i) const {
        return {fract(j0 + static_cast<f32>(i) * r2_b), fract(k0 + static_cast<f32>(i) * r2_a)};
    }
};

Vec3f cosine_direction(Vec2f u, Vec3f n) {
    Vec3f t, b;
    frame_of(n, t, b);
    const f32 r = std::sqrt(u.x), phi = 2.0f * pi * u.y;
    return normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(std::max(0.0f, 1.0f - u.x)));
}

Vec3f sphere_direction(Vec2f u) {
    const f32 z = 1.0f - 2.0f * u.x, r = std::sqrt(std::max(0.0f, 1.0f - z * z)), phi = 2.0f * pi * u.y;
    return {r * std::cos(phi), z, r * std::sin(phi)};
}

// --- The scene ------------------------------------------------------------------------------------------------------

struct Corner {
    Vec3f normal;
    Vec2f uv;
    Vec2f lightmap_uv;
    LinearRgb color; // the vertex colour, linear
};

struct Triangle {
    u32 surface = 0;
    Corner c[3];
    Vec3f geo; // unit face normal (counter-clockwise front)
};

struct Light {
    enum class Kind : u8 { Directional, Point, Spot } kind = Kind::Directional;
    Vec3f at{};   // a point light's position, or the way toward a directional one
    Vec3f axis{}; // a spot's (the way it shines)
    LinearRgb color{};
    f32 radius = 0.0f, size = 0.01f, cos_outer = 0.0f, cone_scale = 1.0f;
    bool baked = false; // all of its light baked (else only its bounce)
};

std::vector<Light> bake_lights(const Lights3D& lights) {
    std::vector<Light> out;
    const auto directional = [&](const DirectionalLight3D& d) {
        if (d.bake == LightBake3D::Realtime || black(d.color) || !(length_squared(d.direction) > 0.0f))
            return;
        out.push_back({Light::Kind::Directional,
                       normalize(d.direction),
                       {},
                       d.color,
                       0.0f,
                       0.0f,
                       0.0f,
                       1.0f,
                       d.bake == LightBake3D::Baked});
    };
    directional(lights.sun);
    directional(lights.fill);
    for (const PointLightSource3D& p : lights.points) {
        if (p.bake == LightBake3D::Realtime || black(p.color) || !(p.radius > 0.0f))
            continue;
        out.push_back({Light::Kind::Point,
                       p.position,
                       {},
                       p.color,
                       p.radius,
                       std::max(p.size, 0.01f),
                       0.0f,
                       1.0f,
                       p.bake == LightBake3D::Baked});
    }
    for (const SpotLightSource3D& s : lights.spots) {
        if (s.bake == LightBake3D::Realtime || black(s.color) || !(s.radius > 0.0f) ||
            !(length_squared(s.direction) > 0.0f))
            continue;
        // As the shader: full inside the inner angle, none past the outer, the square of the step between.
        const f32 outer = std::clamp(s.outer_angle, 0.1f, 89.9f);
        const f32 inner = std::clamp(s.inner_angle, 0.0f, outer - 0.1f);
        const f32 cos_outer = std::cos(radians(outer)), cos_inner = std::cos(radians(inner));
        out.push_back({Light::Kind::Spot, s.position, normalize(s.direction), s.color, s.radius,
                       std::max(s.size, 0.01f), cos_outer, 1.0f / std::max(cos_inner - cos_outer, 1e-4f),
                       s.bake == LightBake3D::Baked});
    }
    return out;
}

// The page a lightmapped surface's texels are on, and where.
struct Rect {
    u32 page = 0;
    u32 x = 0, y = 0, side = 0;
};

struct Page {
    u32 w = 0, h = 0;
    std::vector<i32> texel;        // the covering texel's index, or -1
    std::vector<LinearRgb> direct; // baked lights' direct light (what the lightmap holds of it)
    std::vector<LinearRgb> bounce; // every baked or mixed light's direct light (what bounces)
    std::vector<LinearRgb> indirect;
    std::vector<u8> valid; // covered and not inside something (after the first pass)
    std::vector<u8> known; // valid, or filled from valid neighbours by dilate(): what lookups read
};

struct Texel {
    u32 page = 0, x = 0, y = 0;
    u32 surface = 0;
    Vec3f position, normal, geo;
    Vec3f du, dv;    // world-space step of one texel along x and y
    f32 size = 0.0f; // its world size (the larger step)
};

struct Scene {
    std::span<const BakeSurface3D> surfaces;
    std::vector<Vec3f> corners; // world, three a triangle
    std::vector<Triangle> tris;
    std::vector<Light> lights;
    TriangleBvh bvh;
    BakeSky3D sky;
    Mat4f sky_turn = Mat4f::identity();
    f32 epsilon = 1e-4f; // scene-scaled ray offset
};

LinearRgb sky_light(const Scene& s, Vec3f d) {
    if (s.sky.panorama.empty()) {
        return d.y >= 0.0f ? s.sky.above : s.sky.below;
    }
    // As the shader's environment_uv: turned into the picture's space, then longitude and latitude.
    const Vec3f e = s.sky_turn.apply_vector(d);
    const f32 u = 0.5f + std::atan2(e.z, e.x) / (2.0f * pi);
    const f32 v = 0.5f - std::asin(std::clamp(e.y, -1.0f, 1.0f)) / pi;
    const i32 w = s.sky.panorama_size.x, h = s.sky.panorama_size.y;
    // Bilinear, wrapping round, clamped at the poles.
    const f32 fx = u * static_cast<f32>(w) - 0.5f, fy = v * static_cast<f32>(h) - 0.5f;
    const i32 x0 = static_cast<i32>(std::floor(fx)), y0 = static_cast<i32>(std::floor(fy));
    const f32 tx = fx - static_cast<f32>(x0), ty = fy - static_cast<f32>(y0);
    const auto at = [&](i32 x, i32 y) {
        x = ((x % w) + w) % w;
        y = std::clamp(y, 0, h - 1);
        const f32* p = s.sky.panorama.data() + 3 * (static_cast<std::size_t>(y) * w + x);
        return LinearRgb{p[0], p[1], p[2]};
    };
    const LinearRgb top = at(x0, y0) * (1.0f - tx) + at(x0 + 1, y0) * tx;
    const LinearRgb bottom = at(x0, y0 + 1) * (1.0f - tx) + at(x0 + 1, y0 + 1) * tx;
    return (top * (1.0f - ty) + bottom * ty) * s.sky.intensity;
}

LinearRgb texture_at(const BakeTexture3D& t, Vec2f uv) {
    const auto& lin = srgb_table();
    uv = {uv.x * t.uv_scale.x + t.uv_offset.x, uv.y * t.uv_scale.y + t.uv_offset.y};
    const f32 fx = uv.x * static_cast<f32>(t.size.x) - 0.5f, fy = uv.y * static_cast<f32>(t.size.y) - 0.5f;
    const i32 x0 = static_cast<i32>(std::floor(fx)), y0 = static_cast<i32>(std::floor(fy));
    const f32 tx = fx - static_cast<f32>(x0), ty = fy - static_cast<f32>(y0);
    const auto at = [&](i32 x, i32 y) {
        if (t.repeat) {
            x = ((x % t.size.x) + t.size.x) % t.size.x;
            y = ((y % t.size.y) + t.size.y) % t.size.y;
        } else {
            x = std::clamp(x, 0, t.size.x - 1);
            y = std::clamp(y, 0, t.size.y - 1);
        }
        const u8* p = t.rgba.data() + 4 * (static_cast<std::size_t>(y) * t.size.x + x);
        if (t.srgb)
            return LinearRgb{lin[p[0]], lin[p[1]], lin[p[2]]};
        return LinearRgb{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f};
    };
    const LinearRgb top = at(x0, y0) * (1.0f - tx) + at(x0 + 1, y0) * tx;
    const LinearRgb bottom = at(x0, y0 + 1) * (1.0f - tx) + at(x0 + 1, y0 + 1) * tx;
    return top * (1.0f - ty) + bottom * ty;
}

template <class T> T interpolate(const T& a, const T& b, const T& c, f32 u, f32 v) {
    return a * (1.0f - u - v) + b * u + c * v;
}

LinearRgb albedo_at(const Scene& s, const Triangle& tri, f32 u, f32 v) {
    const BakeSurface3D& surface = s.surfaces[tri.surface];
    LinearRgb a = surface.albedo * interpolate(tri.c[0].color, tri.c[1].color, tri.c[2].color, u, v);
    if (!surface.albedo_texture.empty()) {
        a = a * texture_at(surface.albedo_texture, interpolate(tri.c[0].uv, tri.c[1].uv, tri.c[2].uv, u, v));
    }
    return a;
}

// The light reaching a point facing n from one light (as the stock shaders: the light's colour times the windowed
// inverse square and the cone, times n.l), and the way and distance to the point on it to test for shadow.
LinearRgb unshadowed(const Light& l, Vec3f p, Vec3f n, Vec3f& toward, f32& distance, bool facing = true) {
    if (l.kind == Light::Kind::Directional) {
        toward = l.at;
        distance = std::numeric_limits<f32>::infinity();
        const f32 nl = facing ? dot(n, toward) : 1.0f;
        return nl > 0.0f ? l.color * nl : LinearRgb{};
    }
    const Vec3f to = l.at - p;
    const f32 d2 = dot(to, to);
    const f32 r2 = d2 / (l.radius * l.radius);
    const f32 window = std::clamp(1.0f - r2 * r2, 0.0f, 1.0f);
    f32 k = window * window / std::max(d2, l.size * l.size);
    distance = std::sqrt(d2);
    toward = distance > 0.0f ? to / distance : Vec3f{0.0f, 1.0f, 0.0f};
    if (l.kind == Light::Kind::Spot) {
        const f32 step = std::clamp((dot(-toward, l.axis) - l.cos_outer) * l.cone_scale, 0.0f, 1.0f);
        k *= step * step;
    }
    const f32 nl = facing ? dot(n, toward) : 1.0f;
    return k > 0.0f && nl > 0.0f ? l.color * (k * nl) : LinearRgb{};
}

// Whether the way from p (already pushed off its surface) toward a light is clear; for a point or spot light,
// toward a point jittered over its body (its size) so that its shadows are soft.
bool clear(const Scene& s, const Light& l, Vec3f p, Vec3f toward, f32 distance, Vec2f jitter) {
    if (l.kind == Light::Kind::Directional) {
        return !s.bvh.any(p, toward, std::numeric_limits<f32>::infinity());
    }
    const Vec3f on = l.at + sphere_direction(jitter) * (l.size * std::sqrt(jitter.x));
    const Vec3f to = on - p;
    const f32 d = length(to);
    (void)toward;
    (void)distance;
    if (!(d > s.epsilon))
        return true;
    return !s.bvh.any(p, to / d, d - s.epsilon);
}

// --- Parallel loops -------------------------------------------------------------------------------------------------

struct Runner {
    JobSystem* jobs = nullptr;
    std::stop_token stop;
    std::atomic<bool> stopped{false};

    // body(begin, end) over [0, count) in chunks; chunks run in any order on any thread, each writes only its own.
    template <class F> void chunks(u64 count, u64 chunk, F&& body) {
        const u64 n = (count + chunk - 1) / chunk;
        const auto run = [&](u64 c) {
            if (stopped.load(std::memory_order_relaxed))
                return;
            if (stop.stop_requested()) {
                stopped = true;
                return;
            }
            body(c * chunk, std::min(count, (c + 1) * chunk));
        };
        if (jobs && n > 1) {
            jobs->parallel_for(static_cast<i32>(n), [&](i32 c) { run(static_cast<u64>(c)); });
        } else {
            for (u64 c = 0; c < n; ++c)
                run(c);
        }
    }
};

// --- Lightmap lookups -----------------------------------------------------------------------------------------------

// The bounced light leaving a lightmapped surface's point (albedo applied by the caller): its lightmap's direct
// light of every baked and mixed light plus what the last pass gathered, bilinear over the texels round it.
LinearRgb lightmap_light(const std::vector<Page>& pages, const Rect& rect, Vec2f lightmap_uv) {
    const Page& page = pages[rect.page];
    const f32 fx = static_cast<f32>(rect.x) + lightmap_uv.x * static_cast<f32>(rect.side) - 0.5f;
    const f32 fy = static_cast<f32>(rect.y) + lightmap_uv.y * static_cast<f32>(rect.side) - 0.5f;
    const i32 x0 = static_cast<i32>(std::floor(fx)), y0 = static_cast<i32>(std::floor(fy));
    const f32 tx = fx - static_cast<f32>(x0), ty = fy - static_cast<f32>(y0);
    LinearRgb sum{};
    f32 weight = 0.0f;
    for (i32 j = 0; j < 2; ++j) {
        for (i32 i = 0; i < 2; ++i) {
            const i32 x = std::clamp(x0 + i, static_cast<i32>(rect.x), static_cast<i32>(rect.x + rect.side - 1));
            const i32 y = std::clamp(y0 + j, static_cast<i32>(rect.y), static_cast<i32>(rect.y + rect.side - 1));
            const std::size_t at = static_cast<std::size_t>(y) * page.w + x;
            if (!page.known[at])
                continue;
            const f32 w = (i ? tx : 1.0f - tx) * (j ? ty : 1.0f - ty) + 1e-6f;
            sum += (page.bounce[at] + page.indirect[at]) * w;
            weight += w;
        }
    }
    return weight > 0.0f ? sum * (1.0f / weight) : LinearRgb{};
}

// Fills texels that are not valid from their valid neighbours (the mean of those of the eight round them), `steps`
// times, growing a texel a step; `known` becomes the texels that have a value (`valid` is left as it was).
void dilate(Page& page, std::vector<LinearRgb>& values, u32 steps) {
    std::vector<u8> known = page.valid;
    std::vector<LinearRgb> next = values;
    for (u32 step = 0; step < steps; ++step) {
        std::vector<u8> grown = known;
        bool any = false;
        for (u32 y = 0; y < page.h; ++y) {
            for (u32 x = 0; x < page.w; ++x) {
                const std::size_t at = static_cast<std::size_t>(y) * page.w + x;
                if (known[at])
                    continue;
                LinearRgb sum{};
                u32 n = 0;
                for (i32 dy = -1; dy <= 1; ++dy) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        const i32 nx = static_cast<i32>(x) + dx, ny = static_cast<i32>(y) + dy;
                        if (nx < 0 || ny < 0 || nx >= static_cast<i32>(page.w) || ny >= static_cast<i32>(page.h))
                            continue;
                        const std::size_t n_at = static_cast<std::size_t>(ny) * page.w + nx;
                        if (known[n_at]) {
                            sum += values[n_at];
                            ++n;
                        }
                    }
                }
                if (n > 0) {
                    next[at] = sum * (1.0f / static_cast<f32>(n));
                    grown[at] = 1;
                    any = true;
                }
            }
        }
        values = next;
        known.swap(grown);
        if (!any)
            break;
    }
    page.known = std::move(known);
}

// --- Packing --------------------------------------------------------------------------------------------------------

u32 next_pow2(u32 v) {
    u32 p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

// Squares packed on shelves, largest first, into pages of `page_size`, each page then cut down to the power of two
// that holds what it got.
std::vector<Rect> pack(std::span<const u32> sides, u32 page_size, u32 padding, std::vector<Vec2i>& page_sizes) {
    std::vector<u32> order(sides.size());
    for (u32 i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return sides[a] > sides[b]; });
    std::vector<Rect> rects(sides.size());
    struct Shelf {
        u32 y, height, x;
    };
    struct Open {
        std::vector<Shelf> shelves;
        u32 top = 0;
        u32 used_w = 0, used_h = 0;
    };
    std::vector<Open> pages;
    for (const u32 i : order) {
        if (sides[i] == 0)
            continue;
        const u32 cell = sides[i] + 2 * padding;
        bool placed = false;
        for (u32 p = 0; p < pages.size() && !placed; ++p) {
            Open& page = pages[p];
            for (Shelf& shelf : page.shelves) {
                if (cell <= shelf.height && shelf.x + cell <= page_size) {
                    rects[i] = {p, shelf.x + padding, shelf.y + padding, sides[i]};
                    shelf.x += cell;
                    page.used_w = std::max(page.used_w, shelf.x);
                    placed = true;
                    break;
                }
            }
            if (!placed && page.top + cell <= page_size) {
                page.shelves.push_back({page.top, cell, cell});
                rects[i] = {p, padding, page.top + padding, sides[i]};
                page.top += cell;
                page.used_w = std::max(page.used_w, cell);
                placed = true;
            }
            if (placed)
                page.used_h = page.top;
        }
        if (!placed) {
            pages.push_back({});
            Open& page = pages.back();
            page.shelves.push_back({0, cell, cell});
            page.top = cell;
            page.used_w = cell;
            page.used_h = cell;
            rects[i] = {static_cast<u32>(pages.size() - 1), padding, padding, sides[i]};
        }
    }
    page_sizes.clear();
    for (const Open& page : pages) {
        page_sizes.push_back({static_cast<i32>(std::min(page_size, next_pow2(page.used_w))),
                              static_cast<i32>(std::min(page_size, next_pow2(page.used_h)))});
    }
    return rects;
}

// --- Rasterising ----------------------------------------------------------------------------------------------------

f32 edge(Vec2f a, Vec2f b, Vec2f p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

void rasterise(const Scene& s, u32 first_tri, u32 tri_count, const Rect& rect, Page& page, std::vector<Texel>& texels) {
    for (u32 t = first_tri; t < first_tri + tri_count; ++t) {
        const Triangle& tri = s.tris[t];
        Vec2f q[3];
        for (u32 k = 0; k < 3; ++k) {
            q[k] = {static_cast<f32>(rect.x) + tri.c[k].lightmap_uv.x * static_cast<f32>(rect.side),
                    static_cast<f32>(rect.y) + tri.c[k].lightmap_uv.y * static_cast<f32>(rect.side)};
        }
        const f32 area = edge(q[0], q[1], q[2]);
        if (std::fabs(area) < 1e-12f)
            continue;
        const Vec3f A = s.corners[3 * t], B = s.corners[3 * t + 1], C = s.corners[3 * t + 2];
        // World step a texel: E = J T, J = E T^-1 (E the world edges, T the texel-space edges).
        const Vec2f t1{q[1].x - q[0].x, q[1].y - q[0].y}, t2{q[2].x - q[0].x, q[2].y - q[0].y};
        const f32 det = t1.x * t2.y - t2.x * t1.y;
        const Vec3f e1 = B - A, e2 = C - A;
        const Vec3f du = (e1 * t2.y - e2 * t1.y) / det;
        const Vec3f dv = (e2 * t1.x - e1 * t2.x) / det;
        const i32 x0 =
            std::max(static_cast<i32>(rect.x), static_cast<i32>(std::floor(std::min({q[0].x, q[1].x, q[2].x}))));
        const i32 x1 = std::min(static_cast<i32>(rect.x + rect.side - 1),
                                static_cast<i32>(std::ceil(std::max({q[0].x, q[1].x, q[2].x}))));
        const i32 y0 =
            std::max(static_cast<i32>(rect.y), static_cast<i32>(std::floor(std::min({q[0].y, q[1].y, q[2].y}))));
        const i32 y1 = std::min(static_cast<i32>(rect.y + rect.side - 1),
                                static_cast<i32>(std::ceil(std::max({q[0].y, q[1].y, q[2].y}))));
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                const std::size_t at = static_cast<std::size_t>(y) * page.w + x;
                if (page.texel[at] >= 0)
                    continue;
                const Vec2f p{static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f};
                f32 w0 = edge(q[1], q[2], p) / area, w1 = edge(q[2], q[0], p) / area, w2 = edge(q[0], q[1], p) / area;
                constexpr f32 tolerance = -1e-5f;
                if (w0 < tolerance || w1 < tolerance || w2 < tolerance)
                    continue;
                page.texel[at] = static_cast<i32>(texels.size());
                Texel texel;
                texel.page = rect.page;
                texel.x = static_cast<u32>(x);
                texel.y = static_cast<u32>(y);
                texel.surface = tri.surface;
                texel.position = A * w0 + B * w1 + C * w2;
                const Vec3f n = tri.c[0].normal * w0 + tri.c[1].normal * w1 + tri.c[2].normal * w2;
                texel.normal = length_squared(n) > 1e-12f ? normalize(n) : tri.geo;
                texel.geo = tri.geo;
                texel.du = du;
                texel.dv = dv;
                texel.size = std::max(length(du), length(dv));
                texels.push_back(texel);
            }
        }
    }
}

f32 world_area(const Scene& s, u32 first, u32 count) {
    f32 a = 0.0f;
    for (u32 t = first; t < first + count; ++t) {
        a += 0.5f * length(cross(s.corners[3 * t + 1] - s.corners[3 * t], s.corners[3 * t + 2] - s.corners[3 * t]));
    }
    return a;
}

f32 lightmap_area(const Scene& s, u32 first, u32 count) {
    f32 a = 0.0f;
    for (u32 t = first; t < first + count; ++t) {
        const Triangle& tri = s.tris[t];
        a += 0.5f * std::fabs(edge(tri.c[0].lightmap_uv, tri.c[1].lightmap_uv, tri.c[2].lightmap_uv));
    }
    return a;
}

// --- Shading a hit --------------------------------------------------------------------------------------------------

struct Shading {
    const Scene* scene;
    const std::vector<Page>* pages;
    const std::vector<Rect>* rects; // by surface (side 0: none)
    bool bounce_light = true;       // false: hits only emit (no bounce counted yet)
};

// The radiance leaving a hit toward the ray, and whether it was the back of a one-sided face.
LinearRgb hit_radiance(const Shading& sh, const RayHit& hit, Vec3f origin, Vec3f dir, bool& back, Vec2f jitter) {
    const Scene& s = *sh.scene;
    const Triangle& tri = s.tris[hit.triangle];
    const BakeSurface3D& surface = s.surfaces[tri.surface];
    back = hit.back && !surface.double_sided;
    if (back)
        return {};
    LinearRgb out = surface.emission;
    if (!surface.emission_texture.empty() && !black(out))
        out = out *
              texture_at(surface.emission_texture, interpolate(tri.c[0].uv, tri.c[1].uv, tri.c[2].uv, hit.u, hit.v));
    if (!sh.bounce_light)
        return out;
    const LinearRgb albedo = albedo_at(s, tri, hit.u, hit.v);
    if (black(albedo))
        return out;
    const Rect& rect = (*sh.rects)[tri.surface];
    if (rect.side > 0) {
        const Vec2f uv = interpolate(tri.c[0].lightmap_uv, tri.c[1].lightmap_uv, tri.c[2].lightmap_uv, hit.u, hit.v);
        return out + albedo * lightmap_light(*sh.pages, rect, uv);
    }
    // No lightmap: its direct light from every baked or mixed light, worked out here (one shadow ray a light).
    const Vec3f p = origin + dir * hit.t;
    Vec3f n = normalize(interpolate(tri.c[0].normal, tri.c[1].normal, tri.c[2].normal, hit.u, hit.v));
    Vec3f geo = tri.geo;
    if (hit.back) {
        n = -n;
        geo = -geo;
    }
    const Vec3f from = p + geo * s.epsilon;
    LinearRgb direct{};
    for (const Light& l : s.lights) {
        Vec3f toward;
        f32 distance;
        const LinearRgb c = unshadowed(l, p, n, toward, distance);
        if (!black(c) && clear(s, l, from, toward, distance, jitter))
            direct += c;
    }
    return out + albedo * direct;
}

} // namespace

// --- Public helpers -------------------------------------------------------------------------------------------------

std::string_view light_bake_name(LightBake3D mode) {
    return mode == LightBake3D::Baked ? "baked" : mode == LightBake3D::Mixed ? "mixed" : "realtime";
}

std::optional<LightBake3D> light_bake_from_name(std::string_view name) {
    if (name == "realtime")
        return LightBake3D::Realtime;
    if (name == "mixed")
        return LightBake3D::Mixed;
    if (name == "baked")
        return LightBake3D::Baked;
    return std::nullopt;
}

BakeSky3D bake_sky(const Lights3D& lights) {
    BakeSky3D sky;
    sky.above = lights.ambient;
    sky.below = lights.ground_ambient.enabled ? lights.ground_ambient.color : lights.ambient;
    return sky;
}

LinearRgb LightmapPage3D::at(Vec2i texel) const {
    if (texel.x < 0 || texel.y < 0 || texel.x >= size.x || texel.y >= size.y)
        return {};
    const f32* p = rgb.data() + 3 * (static_cast<std::size_t>(texel.y) * size.x + texel.x);
    return {p[0], p[1], p[2]};
}

namespace {

f32 probe_axis(f32 p, f32 lo, f32 hi, u32 n, u32& i0, f32& t) {
    if (n <= 1 || !(hi > lo)) {
        i0 = 0;
        t = 0.0f;
        return 0.0f;
    }
    const f32 f = std::clamp((p - lo) / (hi - lo), 0.0f, 1.0f) * static_cast<f32>(n - 1);
    i0 = std::min(static_cast<u32>(f), n - 2);
    t = f - static_cast<f32>(i0);
    return (hi - lo) / static_cast<f32>(n - 1);
}

// A probe's weight for a point `q` (beyond the trilinear one): less when it is behind the surface (facing `n`), and
// by Chebyshev's inequality on its distances toward the point (its `visibility`, 12 floats; null: seen), along each
// axis, none where it surely cannot see it; never quite none, so that a point every probe is hidden from still takes
// their mean.
f32 probe_weight(const f32* visibility, Vec3f at, Vec3f q, Vec3f n) {
    const Vec3f to = q - at;
    const f32 r = length(to);
    const Vec3f d = r > 1e-4f ? to / r : n;
    const f32 facing = 0.5f * (dot(-d, n) + 1.0f);
    f32 w = facing * facing + 0.2f;
    if (visibility) {
        // Each axis's way toward the point: its distance along that axis against the probe's distances that way
        // (Chebyshev), the three weighed by the direction's squared components (a wall across x hides what lies past
        // it along x, whichever way the point is).
        f32 seen = 1.0f;
        const f32 offset[3] = {to.x, to.y, to.z};
        const f32 weight[3] = {d.x * d.x, d.y * d.y, d.z * d.z};
        for (u32 a = 0; a < 3; ++a) {
            const f32* m = visibility + 4 * a + (offset[a] >= 0.0f ? 0 : 2);
            const f32 o = std::fabs(offset[a]);
            const f32 variance = std::fabs(m[1] - m[0] * m[0]) + 1e-4f;
            const f32 chebyshev = o <= m[0] ? 1.0f : variance / (variance + (o - m[0]) * (o - m[0]));
            seen *= std::pow(chebyshev, weight[a]);
        }
        w *= seen * seen * seen;
    }
    return std::max(w, 1e-4f);
}

} // namespace

LinearRgb BakedProbes3D::light(Vec3f position, Vec3f normal) const {
    if (empty())
        return {};
    // As kin_baked_probes in kin_material.inc.glsl: the point moved a quarter of a spacing along the normal, the eight
    // probes round it weighed trilinearly, by facing and by visibility, the weights then made to sum to one.
    const Vec3f size = area.max - area.min;
    const auto spacing_of = [](f32 extent, u32 n) { return n > 1 ? extent / static_cast<f32>(n - 1) : 1.0f; };
    const Vec3f spacing{spacing_of(size.x, count[0]), spacing_of(size.y, count[1]), spacing_of(size.z, count[2])};
    const Vec3f q = position + normal * (0.25f * std::min({spacing.x, spacing.y, spacing.z}));
    u32 i[3];
    f32 t[3];
    probe_axis(q.x, area.min.x, area.max.x, count[0], i[0], t[0]);
    probe_axis(q.y, area.min.y, area.max.y, count[1], i[1], t[1]);
    probe_axis(q.z, area.min.z, area.max.z, count[2], i[2], t[2]);
    f32 c[12] = {};
    f32 total = 0.0f;
    for (u32 corner = 0; corner < 8; ++corner) {
        const u32 dx = corner & 1, dy = (corner >> 1) & 1, dz = (corner >> 2) & 1;
        const u32 x = std::min(i[0] + dx, count[0] - 1), y = std::min(i[1] + dy, count[1] - 1),
                  z = std::min(i[2] + dz, count[2] - 1);
        const f32 trilinear = (dx ? t[0] : 1.0f - t[0]) * (dy ? t[1] : 1.0f - t[1]) * (dz ? t[2] : 1.0f - t[2]);
        const std::size_t probe = (static_cast<std::size_t>(z) * count[1] + y) * count[0] + x;
        const Vec3f at{area.min.x + spacing.x * static_cast<f32>(x), area.min.y + spacing.y * static_cast<f32>(y),
                       area.min.z + spacing.z * static_cast<f32>(z)};
        const f32 w =
            trilinear * probe_weight(visibility.empty() ? nullptr : visibility.data() + 12 * probe, at, q, normal);
        const f32* p = sh.data() + 12 * probe;
        for (u32 k = 0; k < 12; ++k)
            c[k] += p[k] * w;
        total += w;
    }
    const f32 scale = total > 0.0f ? 1.0f / total : 0.0f;
    const auto channel = [&](u32 k) {
        return std::max(0.0f, (c[4 * k] + c[4 * k + 1] * normal.x + c[4 * k + 2] * normal.y + c[4 * k + 3] * normal.z) *
                                  scale);
    };
    return {channel(0), channel(1), channel(2)};
}

f32 BakedProbes3D::cover(Vec3f p) const {
    if (empty())
        return 0.0f;
    const auto inside = [](f32 v, f32 lo, f32 hi, u32 n) {
        const f32 spacing = n > 1 ? (hi - lo) / static_cast<f32>(n - 1) : 1.0f;
        const f32 margin = std::min(v - lo, hi - v);
        return std::clamp(margin / std::max(0.5f * spacing, 1e-6f) + 1.0f, 0.0f, 1.0f);
    };
    return std::min({inside(p.x, area.min.x, area.max.x, count[0]), inside(p.y, area.min.y, area.max.y, count[1]),
                     inside(p.z, area.min.z, area.max.z, count[2])});
}

Lights3D drawn_lights(const Lights3D& lights) {
    Lights3D out = lights;
    if (out.sun.bake == LightBake3D::Baked)
        out.sun.color = {};
    if (out.fill.bake == LightBake3D::Baked)
        out.fill.color = {};
    std::erase_if(out.points, [](const PointLightSource3D& p) { return p.bake == LightBake3D::Baked; });
    std::erase_if(out.spots, [](const SpotLightSource3D& p) { return p.bake == LightBake3D::Baked; });
    return out;
}

// --- The bake -------------------------------------------------------------------------------------------------------

Result3D<BakedLightingData3D> bake_lighting(std::span<const BakeSurface3D> surfaces, const Lights3D& lights,
                                            const BakeSky3D& sky, const LightBakeSettings3D& settings,
                                            const LightBakeControl3D& control) {
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const auto refuse = [](std::string why) {
        return Result3D<BakedLightingData3D>{std::nullopt, {"bake_lighting: " + std::move(why)}};
    };
    if (!(settings.texels_per_unit > 0.0f) || settings.page_size < 64 || settings.page_size > 4096 ||
        (settings.page_size & (settings.page_size - 1)) != 0 || settings.min_texels < 1 ||
        settings.max_texels < settings.min_texels || settings.samples < 1 || settings.bounces > 8 ||
        !(settings.probe_spacing > 0.0f) || settings.probe_samples < 1 || settings.max_probes < 8) {
        return refuse("invalid settings");
    }
    if (!sky.panorama.empty() &&
        (sky.panorama_size.x <= 0 || sky.panorama_size.y <= 0 ||
         sky.panorama.size() < 3 * static_cast<std::size_t>(sky.panorama_size.x) * sky.panorama_size.y)) {
        return refuse("the sky's panorama is smaller than its size");
    }
    const auto report = [&](f32 done, std::string_view stage) {
        if (control.progress)
            control.progress(done, stage);
    };

    Scene scene;
    scene.surfaces = surfaces;
    scene.sky = sky;
    scene.sky_turn = Mat4f::rotation_y(-sky.rotation);
    scene.lights = bake_lights(lights);
    const auto& lin = srgb_table();

    // The world's triangles, surface by surface (each surface's a run).
    std::vector<u32> first_tri(surfaces.size() + 1, 0);
    Bounds3D lit_bounds{
        {std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()},
        {std::numeric_limits<f32>::lowest(), std::numeric_limits<f32>::lowest(), std::numeric_limits<f32>::lowest()}};
    Bounds3D all_bounds = lit_bounds;
    for (std::size_t si = 0; si < surfaces.size(); ++si) {
        const BakeSurface3D& surface = surfaces[si];
        first_tri[si] = static_cast<u32>(scene.tris.size());
        if (!surface.mesh) {
            return refuse("surface " + std::to_string(si) + " has no mesh");
        }
        const MeshData3D& mesh = *surface.mesh;
        if (Status3D valid = mesh.validate(); !valid) {
            return refuse("surface " + std::to_string(si) + ": " + valid.diagnostics.front());
        }
        if (surface.lightmapped && !mesh.lightmapped()) {
            return refuse("surface " + std::to_string(si) + " is lightmapped but its mesh has no lightmap UVs");
        }
        if (!(surface.texel_scale > 0.0f)) {
            return refuse("surface " + std::to_string(si) + " has a texel scale that is not above 0");
        }
        const Mat4f& m = surface.transform;
        Mat4f normals = m.inverse().transposed();
        const bool mirrored = m.determinant() < 0.0f;
        const u32 n = static_cast<u32>(mesh.triangle_count());
        for (u32 t = 0; t < n; ++t) {
            u32 idx[3];
            for (u32 k = 0; k < 3; ++k)
                idx[k] = mesh.indices.empty() ? 3 * t + k : mesh.indices[3 * t + k];
            if (mirrored)
                std::swap(idx[1], idx[2]);
            Triangle tri;
            tri.surface = static_cast<u32>(si);
            Vec3f world[3];
            for (u32 k = 0; k < 3; ++k) {
                const MeshVertex3D& v = mesh.vertices[idx[k]];
                world[k] = m.apply(v.position);
                const Vec3f wn = normals.apply_vector(v.normal);
                tri.c[k].normal = length_squared(wn) > 0.0f ? normalize(wn) : Vec3f{0.0f, 1.0f, 0.0f};
                tri.c[k].uv = v.uv;
                tri.c[k].lightmap_uv = mesh.lightmapped() ? mesh.lightmap_uv[idx[k]] : Vec2f{};
                tri.c[k].color = {lin[v.color.r], lin[v.color.g], lin[v.color.b]};
                scene.corners.push_back(world[k]);
                for (Bounds3D* b : {&all_bounds, surface.lightmapped ? &lit_bounds : nullptr}) {
                    if (!b)
                        continue;
                    b->min = {std::min(b->min.x, world[k].x), std::min(b->min.y, world[k].y),
                              std::min(b->min.z, world[k].z)};
                    b->max = {std::max(b->max.x, world[k].x), std::max(b->max.y, world[k].y),
                              std::max(b->max.z, world[k].z)};
                }
            }
            const Vec3f g = cross(world[1] - world[0], world[2] - world[0]);
            tri.geo = length_squared(g) > 0.0f ? normalize(g) : Vec3f{0.0f, 1.0f, 0.0f};
            scene.tris.push_back(tri);
        }
    }
    first_tri[surfaces.size()] = static_cast<u32>(scene.tris.size());
    BakedLightingData3D result;
    result.stats.surfaces = static_cast<u32>(surfaces.size());
    result.stats.triangles = static_cast<u32>(scene.tris.size());
    result.placements.resize(surfaces.size());
    if (scene.tris.empty()) {
        return result3d(std::move(result));
    }
    const f32 scene_size = length(all_bounds.max - all_bounds.min);
    scene.epsilon = std::max(1e-5f, scene_size * 2e-6f);
    scene.bvh.build(scene.corners);
    report(0.02f, "scene");

    Runner runner{control.jobs, control.stop};
    std::atomic<u64> rays{0};

    // Lightmap sizes, packing and texels.
    std::vector<u32> sides(surfaces.size(), 0);
    for (std::size_t si = 0; si < surfaces.size(); ++si) {
        if (!surfaces[si].lightmapped)
            continue;
        const u32 first = first_tri[si], count = first_tri[si + 1] - first;
        const f32 a = lightmap_area(scene, first, count);
        if (!(a > 1e-8f)) {
            return refuse("surface " + std::to_string(si) + "'s lightmap UVs cover no area");
        }
        const f32 side =
            settings.texels_per_unit * surfaces[si].texel_scale * std::sqrt(world_area(scene, first, count) / a);
        const u32 limit = std::min(settings.max_texels, settings.page_size - 2 * settings.padding);
        sides[si] = std::clamp(static_cast<u32>(std::ceil(side)), std::min(settings.min_texels, limit), limit);
        ++result.stats.lightmapped;
    }
    std::vector<Vec2i> page_sizes;
    std::vector<Rect> rects = pack(sides, settings.page_size, settings.padding, page_sizes);
    std::vector<Page> pages(page_sizes.size());
    for (std::size_t p = 0; p < pages.size(); ++p) {
        Page& page = pages[p];
        page.w = static_cast<u32>(page_sizes[p].x);
        page.h = static_cast<u32>(page_sizes[p].y);
        const std::size_t n = static_cast<std::size_t>(page.w) * page.h;
        page.texel.assign(n, -1);
        page.direct.assign(n, {});
        page.bounce.assign(n, {});
        page.indirect.assign(n, {});
        page.valid.assign(n, 0);
        page.known.assign(n, 0);
    }
    std::vector<Texel> texels;
    for (std::size_t si = 0; si < surfaces.size(); ++si) {
        if (sides[si] == 0)
            continue;
        const Rect& r = rects[si];
        rasterise(scene, first_tri[si], first_tri[si + 1] - first_tri[si], r, pages[r.page], texels);
        const f32 w = static_cast<f32>(pages[r.page].w), h = static_cast<f32>(pages[r.page].h);
        result.placements[si] = {r.page,
                                 {static_cast<f32>(r.side) / w, static_cast<f32>(r.side) / h, static_cast<f32>(r.x) / w,
                                  static_cast<f32>(r.y) / h}};
    }
    result.stats.pages = static_cast<u32>(pages.size());
    result.stats.texels = texels.size();
    for (const Texel& t : texels)
        pages[t.page].valid[static_cast<std::size_t>(t.y) * pages[t.page].w + t.x] = 1;
    report(0.05f, "lightmap layout");

    const RngKey root = make_key(settings.seed);
    constexpr u64 chunk = 64;

    // Direct light at each texel: jittered over the texel's footprint (antialiased shadow edges) and the light's
    // body (soft shadows).
    const u32 direct_samples = std::clamp(settings.samples / 4, 4u, 64u);
    runner.chunks(texels.size(), chunk, [&](u64 begin, u64 end) {
        u64 local_rays = 0;
        for (u64 i = begin; i < end; ++i) {
            const Texel& t = texels[i];
            Page& page = pages[t.page];
            const std::size_t at = static_cast<std::size_t>(t.y) * page.w + t.x;
            const Sequence seq(fold_in(fold_in(root, 1), i));
            LinearRgb baked{}, all{};
            for (const Light& l : scene.lights) {
                Vec3f toward;
                f32 distance;
                const LinearRgb c = unshadowed(l, t.position, t.normal, toward, distance);
                if (black(c))
                    continue;
                u32 seen = 0;
                for (u32 k = 0; k < direct_samples; ++k) {
                    const Vec2f j = seq.jitter(k);
                    const Vec3f p = t.position + t.du * (j.x - 0.5f) + t.dv * (j.y - 0.5f) +
                                    t.geo * std::max(scene.epsilon, 0.01f * t.size);
                    if (clear(scene, l, p, toward, distance, seq.dir(k)))
                        ++seen;
                }
                local_rays += direct_samples;
                const LinearRgb lit = c * (static_cast<f32>(seen) / static_cast<f32>(direct_samples));
                all += lit;
                if (l.baked)
                    baked += lit;
            }
            page.direct[at] = baked;
            page.bounce[at] = all;
        }
        rays += local_rays;
    });
    if (runner.stopped)
        return refuse("stopped");
    report(0.15f, "direct light");

    // Indirect light, pass by pass: each pass gathers the light leaving what its rays hit, lit by the last pass's
    // light (and every baked or mixed light's direct light), so pass k holds k bounces.
    const u32 passes = std::max(1u, settings.bounces);
    std::vector<f32> back_share(texels.size(), 0.0f);
    std::vector<LinearRgb> gathered(texels.size());
    for (u32 pass = 0; pass < passes; ++pass) {
        // The last pass's light where its texels are not valid (uncovered, inside), from their neighbours, for the
        // lookups; the page's arrays are read only while this pass runs.
        for (Page& page : pages) {
            dilate(page, page.bounce, 2);
            dilate(page, page.indirect, 2);
        }
        Shading shading{&scene, &pages, &rects, settings.bounces > 0};
        runner.chunks(texels.size(), chunk, [&](u64 begin, u64 end) {
            u64 local_rays = 0;
            for (u64 i = begin; i < end; ++i) {
                const Texel& t = texels[i];
                const Sequence seq(fold_in(fold_in(fold_in(root, 2), pass), i));
                LinearRgb sum{};
                u32 backs = 0;
                for (u32 k = 0; k < settings.samples; ++k) {
                    Vec3f d = cosine_direction(seq.dir(k), t.normal);
                    if (dot(d, t.geo) <= 0.0f)
                        d = normalize(d - t.geo * (2.0f * dot(d, t.geo)));
                    const Vec2f j = seq.jitter(k);
                    const Vec3f o = t.position + t.du * (j.x - 0.5f) + t.dv * (j.y - 0.5f) +
                                    t.geo * std::max(scene.epsilon, 0.01f * t.size);
                    RayHit hit;
                    if (!scene.bvh.nearest(o, d, std::numeric_limits<f32>::infinity(), hit)) {
                        sum += sky_light(scene, d);
                        continue;
                    }
                    bool back = false;
                    sum += hit_radiance(shading, hit, o, d, back, j);
                    backs += back ? 1 : 0;
                }
                local_rays += settings.samples;
                gathered[i] = sum * (1.0f / static_cast<f32>(settings.samples));
                if (pass == 0)
                    back_share[i] = static_cast<f32>(backs) / static_cast<f32>(settings.samples);
            }
            rays += local_rays;
        });
        if (runner.stopped)
            return refuse("stopped");
        // The texels' validity is known from the first pass: one that mostly sees backs is inside something.
        for (Page& page : pages)
            std::fill(page.indirect.begin(), page.indirect.end(), LinearRgb{});
        for (std::size_t i = 0; i < texels.size(); ++i) {
            const Texel& t = texels[i];
            Page& page = pages[t.page];
            const std::size_t at = static_cast<std::size_t>(t.y) * page.w + t.x;
            if (pass == 0 && back_share[i] > settings.backface_limit) {
                page.valid[at] = 0;
            }
            page.indirect[at] = gathered[i];
        }
        report(0.15f + 0.6f * static_cast<f32>(pass + 1) / static_cast<f32>(passes), "indirect light");
    }
    for (std::size_t i = 0; i < texels.size(); ++i)
        result.stats.inside_texels += back_share[i] > settings.backface_limit ? 1 : 0;

    // Smoothing what the samples left noisy: an edge-aware blur (à-trous, three steps) of the indirect light among
    // valid texels of the same surface facing the same way and lying close.
    if (settings.denoise) {
        constexpr f32 kernel[3] = {3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f};
        for (Page& page : pages) {
            std::vector<LinearRgb> next = page.indirect;
            for (u32 step = 1; step <= 4; step *= 2) {
                for (u32 y = 0; y < page.h; ++y) {
                    for (u32 x = 0; x < page.w; ++x) {
                        const std::size_t at = static_cast<std::size_t>(y) * page.w + x;
                        if (!page.valid[at])
                            continue;
                        const Texel& c = texels[static_cast<std::size_t>(page.texel[at])];
                        const f32 sigma = 1.5f * c.size * static_cast<f32>(step);
                        LinearRgb sum{};
                        f32 weight = 0.0f;
                        for (i32 dy = -2; dy <= 2; ++dy) {
                            for (i32 dx = -2; dx <= 2; ++dx) {
                                const i32 nx = static_cast<i32>(x) + dx * static_cast<i32>(step);
                                const i32 ny = static_cast<i32>(y) + dy * static_cast<i32>(step);
                                if (nx < 0 || ny < 0 || nx >= static_cast<i32>(page.w) ||
                                    ny >= static_cast<i32>(page.h))
                                    continue;
                                const std::size_t n_at = static_cast<std::size_t>(ny) * page.w + nx;
                                if (!page.valid[n_at])
                                    continue;
                                const Texel& q = texels[static_cast<std::size_t>(page.texel[n_at])];
                                if (q.surface != c.surface)
                                    continue;
                                const f32 facing = std::max(0.0f, dot(q.normal, c.normal));
                                const f32 nw = std::pow(facing, 32.0f);
                                const f32 d2 = length_squared(q.position - c.position);
                                const f32 pw = std::exp(-d2 / (2.0f * sigma * sigma));
                                const f32 w = kernel[std::abs(dx)] * kernel[std::abs(dy)] * nw * pw;
                                sum += page.indirect[n_at] * w;
                                weight += w;
                            }
                        }
                        if (weight > 0.0f)
                            next[at] = sum * (1.0f / weight);
                    }
                }
                page.indirect = next;
            }
        }
    }

    // The pages: baked direct plus indirect, the gaps round each chart filled from inside it.
    result.pages.resize(pages.size());
    for (std::size_t p = 0; p < pages.size(); ++p) {
        Page& page = pages[p];
        dilate(page, page.direct, settings.padding + 2);
        dilate(page, page.indirect, settings.padding + 2);
        dilate(page, page.bounce, settings.padding + 2);
        LightmapPage3D& out = result.pages[p];
        out.size = {static_cast<i32>(page.w), static_cast<i32>(page.h)};
        out.rgb.resize(static_cast<std::size_t>(page.w) * page.h * 3);
        for (std::size_t i = 0; i < static_cast<std::size_t>(page.w) * page.h; ++i) {
            const LinearRgb c = page.direct[i] + page.indirect[i];
            out.rgb[3 * i] = c.r;
            out.rgb[3 * i + 1] = c.g;
            out.rgb[3 * i + 2] = c.b;
        }
        // (Lookups by the probes below read `known`: the texels round the charts, filled.)
    }
    report(0.8f, "lightmaps");

    // Probes.
    if (settings.probes) {
        Bounds3D area = settings.probe_area;
        const bool given = area.max.x >= area.min.x && area.max.y >= area.min.y && area.max.z >= area.min.z &&
                           length_squared(area.max - area.min) > 0.0f;
        if (!given) {
            area = result.stats.lightmapped > 0 ? lit_bounds : all_bounds;
            const Vec3f grow{settings.probe_spacing, settings.probe_spacing, settings.probe_spacing};
            area.min = area.min - grow;
            area.max = area.max + grow;
        }
        f32 spacing = settings.probe_spacing;
        std::array<u32, 3> count{};
        for (;;) {
            const Vec3f e = area.max - area.min;
            count = {std::max(2u, static_cast<u32>(std::ceil(e.x / spacing)) + 1),
                     std::max(2u, static_cast<u32>(std::ceil(e.y / spacing)) + 1),
                     std::max(2u, static_cast<u32>(std::ceil(e.z / spacing)) + 1)};
            if (static_cast<u64>(count[0]) * count[1] * count[2] <= settings.max_probes)
                break;
            spacing *= 1.25f;
        }
        BakedProbes3D& probes = result.probes;
        probes.area = area;
        probes.count = count;
        const u32 n = count[0] * count[1] * count[2];
        probes.sh.assign(static_cast<std::size_t>(n) * 12, 0.0f);
        probes.visibility.assign(static_cast<std::size_t>(n) * 12, 0.0f);
        // How far a ray that leaves the scene counts as having gone: past the grid's far corner, at most 200 units (its
        // square must fit a half float on the GPU).
        const f32 open = std::min(length(area.max - area.min) + 2.0f * settings.probe_spacing, 200.0f);
        std::vector<u8> probe_valid(n, 1);
        const Vec3f step{(area.max.x - area.min.x) / static_cast<f32>(count[0] - 1),
                         (area.max.y - area.min.y) / static_cast<f32>(count[1] - 1),
                         (area.max.z - area.min.z) / static_cast<f32>(count[2] - 1)};
        Shading shading{&scene, &pages, &rects, true};
        runner.chunks(n, 16, [&](u64 begin, u64 end) {
            u64 local_rays = 0;
            for (u64 i = begin; i < end; ++i) {
                const u32 x = static_cast<u32>(i % count[0]), y = static_cast<u32>((i / count[0]) % count[1]),
                          z = static_cast<u32>(i / (static_cast<u64>(count[0]) * count[1]));
                const Vec3f p{area.min.x + step.x * static_cast<f32>(x), area.min.y + step.y * static_cast<f32>(y),
                              area.min.z + step.z * static_cast<f32>(z)};
                const Sequence seq(fold_in(fold_in(root, 3), i));
                LinearRgb l0{};
                LinearRgb l1[3] = {};
                u32 backs = 0;
                f32 lobe[6][3] = {}; // each way's weight, distance and distance squared
                const u32 k_n = settings.probe_samples;
                for (u32 k = 0; k < k_n; ++k) {
                    const Vec3f d = sphere_direction(seq.dir(k));
                    RayHit hit;
                    LinearRgb c;
                    f32 went = open;
                    if (!scene.bvh.nearest(p, d, std::numeric_limits<f32>::infinity(), hit)) {
                        c = sky_light(scene, d);
                    } else {
                        bool back = false;
                        c = hit_radiance(shading, hit, p, d, back, seq.jitter(k));
                        backs += back ? 1 : 0;
                        went = std::min(hit.t, open);
                    }
                    // The six ways' distances, each ray weighed by the sixteenth power of its cosine with the way (a
                    // lobe about 25 degrees round it).
                    const f32 axis[6] = {d.x, -d.x, d.y, -d.y, d.z, -d.z};
                    for (u32 a = 0; a < 6; ++a) {
                        if (axis[a] <= 0.0f)
                            continue;
                        f32 wa = axis[a] * axis[a];
                        wa *= wa;
                        wa *= wa;
                        wa *= wa;
                        lobe[a][0] += wa;
                        lobe[a][1] += wa * went;
                        lobe[a][2] += wa * went * went;
                    }
                    l0 += c;
                    l1[0] += c * d.x;
                    l1[1] += c * d.y;
                    l1[2] += c * d.z;
                }
                local_rays += k_n;
                // Radiance to irradiance over pi, as a + b.n: a = Y00 L00, b = (2/3) Y1 L1 (L the projection,
                // 4 pi / K a sample; Y00 = 0.282095, Y1 = 0.488603).
                const f32 w = 4.0f * pi / static_cast<f32>(k_n);
                LinearRgb a = l0 * (w * 0.282095f * 0.282095f);
                Vec3f b[3]; // by channel
                const f32 b_scale = w * (2.0f / 3.0f) * 0.488603f * 0.488603f;
                b[0] = Vec3f{l1[0].r, l1[1].r, l1[2].r} * b_scale;
                b[1] = Vec3f{l1[0].g, l1[1].g, l1[2].g} * b_scale;
                b[2] = Vec3f{l1[0].b, l1[1].b, l1[2].b} * b_scale;
                // Baked lights' direct light (a light from one way l, colour c: a += c / 4, b += c l / 2).
                for (const Light& l : scene.lights) {
                    if (!l.baked)
                        continue;
                    Vec3f toward;
                    f32 distance;
                    const LinearRgb c = unshadowed(l, p, Vec3f{0.0f, 1.0f, 0.0f}, toward, distance, false);
                    if (black(c))
                        continue;
                    u32 seen = 0;
                    constexpr u32 shadow_rays = 8;
                    for (u32 k = 0; k < shadow_rays; ++k)
                        seen += clear(scene, l, p, toward, distance, seq.jitter(k)) ? 1 : 0;
                    local_rays += shadow_rays;
                    const LinearRgb lit = c * (static_cast<f32>(seen) / static_cast<f32>(shadow_rays));
                    a += lit * 0.25f;
                    b[0] += toward * (lit.r * 0.5f);
                    b[1] += toward * (lit.g * 0.5f);
                    b[2] += toward * (lit.b * 0.5f);
                }
                f32* out = probes.sh.data() + 12 * i;
                const f32 channel_a[3] = {a.r, a.g, a.b};
                for (u32 c = 0; c < 3; ++c) {
                    out[4 * c] = channel_a[c];
                    out[4 * c + 1] = b[c].x;
                    out[4 * c + 2] = b[c].y;
                    out[4 * c + 3] = b[c].z;
                }
                probe_valid[i] = static_cast<f32>(backs) / static_cast<f32>(k_n) > settings.backface_limit ? 0 : 1;
                // (One inside something sees nothing: all its ways at 0.)
                f32* seen = probes.visibility.data() + 12 * i;
                for (u32 a = 0; a < 6; ++a) {
                    const bool any = probe_valid[i] && lobe[a][0] > 0.0f;
                    seen[2 * a] = any ? lobe[a][1] / lobe[a][0] : 0.0f;
                    seen[2 * a + 1] = any ? lobe[a][2] / lobe[a][0] : 0.0f;
                }
            }
            rays += local_rays;
        });
        if (runner.stopped)
            return refuse("stopped");
        // Probes inside something take the mean of their valid neighbours, growing outward.
        for (u32 i = 0; i < n; ++i)
            result.stats.inside_probes += probe_valid[i] ? 0 : 1;
        for (u32 round = 0; round < count[0] + count[1] + count[2]; ++round) {
            std::vector<u8> next = probe_valid;
            bool any = false, missing = false;
            for (u32 i = 0; i < n; ++i) {
                if (probe_valid[i])
                    continue;
                missing = true;
                const i32 x = static_cast<i32>(i % count[0]), y = static_cast<i32>((i / count[0]) % count[1]),
                          z = static_cast<i32>(i / (count[0] * count[1]));
                f32 sum[12] = {};
                u32 m = 0;
                const i32 offsets[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                for (const auto& o : offsets) {
                    const i32 nx = x + o[0], ny = y + o[1], nz = z + o[2];
                    if (nx < 0 || ny < 0 || nz < 0 || nx >= static_cast<i32>(count[0]) ||
                        ny >= static_cast<i32>(count[1]) || nz >= static_cast<i32>(count[2]))
                        continue;
                    const u32 j =
                        (static_cast<u32>(nz) * count[1] + static_cast<u32>(ny)) * count[0] + static_cast<u32>(nx);
                    if (!probe_valid[j])
                        continue;
                    for (u32 k = 0; k < 12; ++k)
                        sum[k] += probes.sh[12 * static_cast<std::size_t>(j) + k];
                    ++m;
                }
                if (m > 0) {
                    for (u32 k = 0; k < 12; ++k)
                        probes.sh[12 * static_cast<std::size_t>(i) + k] = sum[k] / static_cast<f32>(m);
                    next[i] = 1;
                    any = true;
                }
            }
            probe_valid.swap(next);
            if (!missing || !any)
                break;
        }
        result.stats.probes = n;
    }
    report(1.0f, "probes");
    result.stats.rays = rays.load();
    result.stats.seconds = std::chrono::duration<f64>(Clock::now() - started).count();
    return result3d(std::move(result));
}

// --- Files ----------------------------------------------------------------------------------------------------------

namespace {

// probes.bin: this, the probe count (u32), then their 12 floats each, then their visibility's 12 each.
constexpr char probes_magic[4] = {'K', 'B', 'P', '2'};

} // namespace

Status3D save_baked_lighting(const BakedLightingData3D& baked, const std::filesystem::path& folder) {
    const auto fail = [](std::string why) { return status3d(false, {"save_baked_lighting: " + std::move(why)}); };
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        return fail("cannot make " + folder.string() + ": " + error.message());
    }
    const auto write = [&](const std::filesystem::path& path, std::string_view bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(out);
    };
    std::ostringstream json;
    JsonWriter w(json);
    w.begin_object();
    w.field("version", 1u);
    w.key("pages").begin_array();
    for (std::size_t p = 0; p < baked.pages.size(); ++p) {
        const LightmapPage3D& page = baked.pages[p];
        const std::string name = "lightmap_" + std::to_string(p) + ".hdr";
        if (page.rgb.size() != 3 * static_cast<std::size_t>(page.size.x) * page.size.y || page.size.x <= 0) {
            return fail("page " + std::to_string(p) + " has the wrong number of texels");
        }
        if (!write(folder / name, encode_hdr3d(page.size, page.rgb))) {
            return fail("cannot write " + (folder / name).string());
        }
        w.begin_object().field("file", name).field("width", page.size.x).field("height", page.size.y).end_object();
    }
    w.end_array();
    w.key("placements").begin_array();
    for (const LightmapPlacement3D& p : baked.placements) {
        w.begin_array()
            .value(p.page)
            .value(static_cast<f64>(p.scale_offset.x))
            .value(static_cast<f64>(p.scale_offset.y))
            .value(static_cast<f64>(p.scale_offset.z))
            .value(static_cast<f64>(p.scale_offset.w))
            .end_array();
    }
    w.end_array();
    if (!baked.probes.empty()) {
        const BakedProbes3D& probes = baked.probes;
        w.key("probes").begin_object().field("file", "probes.bin");
        w.key("min")
            .begin_array()
            .value(static_cast<f64>(probes.area.min.x))
            .value(static_cast<f64>(probes.area.min.y))
            .value(static_cast<f64>(probes.area.min.z))
            .end_array();
        w.key("max")
            .begin_array()
            .value(static_cast<f64>(probes.area.max.x))
            .value(static_cast<f64>(probes.area.max.y))
            .value(static_cast<f64>(probes.area.max.z))
            .end_array();
        w.key("count").begin_array().value(probes.count[0]).value(probes.count[1]).value(probes.count[2]).end_array();
        w.end_object();
        std::string bytes(probes_magic, 4);
        const u32 n = probes.size();
        bytes.append(reinterpret_cast<const char*>(&n), 4);
        bytes.append(reinterpret_cast<const char*>(probes.sh.data()), probes.sh.size() * sizeof(f32));
        std::vector<f32> visibility = probes.visibility;
        if (visibility.size() != probes.sh.size()) // (probes made without: they see everything)
            visibility.assign(probes.sh.size(), 1e6f);
        bytes.append(reinterpret_cast<const char*>(visibility.data()), visibility.size() * sizeof(f32));
        if (!write(folder / "probes.bin", bytes)) {
            return fail("cannot write " + (folder / "probes.bin").string());
        }
    }
    w.end_object();
    json << '\n';
    if (!write(folder / "lighting.json", json.str())) {
        return fail("cannot write " + (folder / "lighting.json").string());
    }
    return status3d(true);
}

Result3D<BakedLightingData3D> load_baked_lighting(const std::filesystem::path& folder, const BakeFileReader3D& read) {
    const auto fail = [&](std::string why) {
        return Result3D<BakedLightingData3D>{
            std::nullopt, {"load_baked_lighting: " + folder.generic_string() + ": " + std::move(why)}};
    };
    const auto bytes_of = [&](const std::filesystem::path& path) {
        return read ? read(path) : read_content_file(path);
    };
    const std::optional<std::string> text = bytes_of(folder / "lighting.json");
    if (!text) {
        return fail("no lighting.json");
    }
    JsonParseResult parsed = parse_json(*text);
    if (!parsed.ok() || !parsed.value->is_object()) {
        return fail("lighting.json is not a JSON object" + (parsed.error.empty() ? "" : ": " + parsed.error));
    }
    const JsonValue& doc = *parsed.value;
    if (doc.int_at("version", 0) != 1) {
        return fail("unknown version " + std::to_string(doc.int_at("version", 0)));
    }
    BakedLightingData3D baked;
    if (const JsonValue* pages = doc.find("pages")) {
        for (const JsonValue& page : pages->items()) {
            const std::string file = page.string_at("file");
            if (file.empty() || file.find("..") != std::string::npos) {
                return fail("a page has no file");
            }
            const std::optional<std::string> hdr = bytes_of(folder / file);
            if (!hdr) {
                return fail("missing " + file);
            }
            int w = 0, h = 0;
            float* pixels = kin_hdr_load(reinterpret_cast<const unsigned char*>(hdr->data()),
                                         static_cast<int>(hdr->size()), &w, &h);
            if (!pixels) {
                return fail(file + " is not a Radiance picture");
            }
            LightmapPage3D out;
            out.size = {w, h};
            out.rgb.assign(pixels, pixels + static_cast<std::size_t>(w) * h * 3);
            kin_hdr_free(pixels);
            if (w != page.int_at("width", -1) || h != page.int_at("height", -1)) {
                return fail(file + " is not the size lighting.json gives");
            }
            baked.pages.push_back(std::move(out));
        }
    }
    if (const JsonValue* placements = doc.find("placements")) {
        for (const JsonValue& p : placements->items()) {
            const auto& v = p.items();
            if (v.size() != 5) {
                return fail("a placement is not [page, sx, sy, ox, oy]");
            }
            LightmapPlacement3D placement{static_cast<u32>(v[0].as_int()),
                                          {static_cast<f32>(v[1].as_number()), static_cast<f32>(v[2].as_number()),
                                           static_cast<f32>(v[3].as_number()), static_cast<f32>(v[4].as_number())}};
            if (placement.placed() && placement.page >= baked.pages.size()) {
                return fail("a placement is on page " + std::to_string(placement.page) + " of " +
                            std::to_string(baked.pages.size()));
            }
            baked.placements.push_back(placement);
        }
    }
    if (const JsonValue* probes = doc.find("probes"); probes && probes->is_object()) {
        BakedProbes3D& out = baked.probes;
        const auto vec = [](const JsonValue* v) {
            const auto& i = v ? v->items() : JsonValue::Array{};
            return i.size() == 3 ? Vec3f{static_cast<f32>(i[0].as_number()), static_cast<f32>(i[1].as_number()),
                                         static_cast<f32>(i[2].as_number())}
                                 : Vec3f{};
        };
        out.area = {vec(probes->find("min")), vec(probes->find("max"))};
        const JsonValue* count = probes->find("count");
        if (!count || count->items().size() != 3) {
            return fail("the probes have no count");
        }
        for (u32 k = 0; k < 3; ++k) {
            const i64 c = count->items()[k].as_int();
            if (c < 2 || c > 4096) {
                return fail("a probe count is out of range");
            }
            out.count[k] = static_cast<u32>(c);
        }
        const std::optional<std::string> bin = bytes_of(folder / probes->string_at("file", "probes.bin"));
        if (!bin) {
            return fail("missing the probes' file");
        }
        const u64 n = static_cast<u64>(out.count[0]) * out.count[1] * out.count[2];
        u32 stored = 0;
        if (bin->size() < 8 || std::memcmp(bin->data(), probes_magic, 4) != 0) {
            return fail("the probes' file is not one");
        }
        std::memcpy(&stored, bin->data() + 4, 4);
        if (stored != n || bin->size() != 8 + 2 * n * 12 * sizeof(f32)) {
            return fail("the probes' file does not hold " + std::to_string(n) + " probes");
        }
        out.sh.resize(n * 12);
        out.visibility.resize(n * 12);
        std::memcpy(out.sh.data(), bin->data() + 8, out.sh.size() * sizeof(f32));
        std::memcpy(out.visibility.data(), bin->data() + 8 + out.sh.size() * sizeof(f32),
                    out.visibility.size() * sizeof(f32));
    }
    return result3d(std::move(baked));
}

} // namespace kin

// --- On the GPU -----------------------------------------------------------------------------------------------------

#include <kin/render3d/backend3d.hpp>
#include <kin/render3d/renderer3d.hpp>

namespace kin {

Result3D<BakedLighting3D> Renderer3D::create_baked_lighting(const BakedLightingData3D& baked) {
    _errors.clear();
    const auto fail = [&](std::string_view why) {
        fail3d(_errors, "Renderer3D::create_baked_lighting", why);
        return result3d(BakedLighting3D{}, _errors);
    };
    if (!_backend) {
        return fail("3D is not available");
    }
    for (const LightmapPage3D& page : baked.pages) {
        if (page.size.x <= 0 || page.size.y <= 0 || page.size.x > 4096 || page.size.y > 4096 ||
            page.rgb.size() != 3 * static_cast<std::size_t>(page.size.x) * page.size.y) {
            return fail("a page's size is outside 1 to 4096 or does not match its texels");
        }
    }
    for (const LightmapPlacement3D& p : baked.placements) {
        if (p.placed() && p.page >= baked.pages.size()) {
            return fail("a placement is on a page the bake does not have");
        }
    }
    const BakedProbes3D& probes = baked.probes;
    if (!probes.empty() &&
        (probes.count[0] < 2 || probes.count[1] < 2 || probes.count[2] < 2 || probes.count[0] > 2048 ||
         probes.count[1] > 2048 || probes.count[2] > 680 ||
         probes.sh.size() != static_cast<std::size_t>(probes.count[0]) * probes.count[1] * probes.count[2] * 12)) {
        return fail("the probes' grid is out of range or does not match their values");
    }
    std::shared_ptr<IBakedLightingBackend3D> backend = _backend->create_baked_lighting(baked);
    if (!backend) {
        return fail("the device cannot draw baked lighting (no light variants), or refused its textures");
    }
    return result3d(BakedLighting3D{std::move(backend), static_cast<u32>(baked.placements.size()), !probes.empty()},
                    _errors);
}

} // namespace kin
