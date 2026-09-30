#include "arena_art.hpp"

#include <kin/core/profile.hpp>
#include <kin/renderer/shader.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <span>

namespace examples {
namespace {

constexpr float pi = 3.14159265f;

Vec2f add(Vec2f a, Vec2f b) { return {a.x + b.x, a.y + b.y}; }
Vec2f sub(Vec2f a, Vec2f b) { return {a.x - b.x, a.y - b.y}; }
Vec2f mul(Vec2f a, float k) { return {a.x * k, a.y * k}; }
float dot(Vec2f a, Vec2f b) { return a.x * b.x + a.y * b.y; }
float length(Vec2f a) { return std::sqrt(dot(a, a)); }
float degrees(Vec2f direction) { return std::atan2(direction.y, direction.x) * 180.0f / pi; }

// ---------------------------------------------------------------------------
// Procedural textures. Shapes are signed distance functions in design units
// (negative inside); a canvas renders them at `scale` pixels per unit with 1px
// antialiasing, back to front. Paint functions get the point, the distance and
// the outward surface normal, which the material shading uses for bevels.

struct Rgba {
    float r = 0, g = 0, b = 0, a = 0;
};

Rgba rgb(int r, int g, int b, float a = 1) { return {r / 255.0f, g / 255.0f, b / 255.0f, a}; }
Rgba mix(Rgba a, Rgba b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
Rgba brighter(Rgba c, float k) {
    return {std::min(1.0f, c.r * k), std::min(1.0f, c.g * k), std::min(1.0f, c.b * k), c.a};
}

float hash(int x, int y) {
    u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

// Smooth value noise, 0..1, with features about `cell` units apart.
float value_noise(Vec2f p, float cell) {
    const float fx = p.x / cell, fy = p.y / cell;
    const int x = static_cast<int>(std::floor(fx)), y = static_cast<int>(std::floor(fy));
    const float tx = fx - x, ty = fy - y;
    const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    const float a = hash(x, y), b = hash(x + 1, y), c = hash(x, y + 1), d = hash(x + 1, y + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

class Canvas {
public:
    // A `width` x `height` design-unit canvas stored at `scale` pixels per unit.
    Canvas(float width, float height, float scale)
        : _w(static_cast<int>(width * scale)), _h(static_cast<int>(height * scale)), _scale(scale),
          _px(static_cast<std::size_t>(_w * _h)) {}

    template <class Sdf, class Paint>
    void fill(Sdf&& sdf, Paint&& paint) {
        fill({0, 0, _w / _scale, _h / _scale}, sdf, paint);
    }

    // Only the pixels inside `area` (design units): small details on big canvases.
    template <class Sdf, class Paint>
    void fill(Rectf area, Sdf&& sdf, Paint&& paint) {
        const float e = .5f / _scale;
        const int x0 = std::max(0, int(std::floor(area.x * _scale)) - 1), y0 = std::max(0, int(std::floor(area.y * _scale)) - 1);
        const int x1 = std::min(_w, int(std::ceil((area.x + area.w) * _scale)) + 1);
        const int y1 = std::min(_h, int(std::ceil((area.y + area.h) * _scale)) + 1);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const Vec2f p{(x + .5f) / _scale, (y + .5f) / _scale};
                const float d = sdf(p);
                const float coverage = std::clamp(.5f - d * _scale, 0.0f, 1.0f);
                if (coverage <= 0) {
                    continue;
                }
                Vec2f n{sdf(Vec2f{p.x + e, p.y}) - sdf(Vec2f{p.x - e, p.y}), sdf(Vec2f{p.x, p.y + e}) - sdf(Vec2f{p.x, p.y - e})};
                const float len = length(n);
                n = len > 1e-6f ? mul(n, 1 / len) : Vec2f{0, 0};
                over(x, y, paint(p, d, n), coverage);
            }
        }
    }

    // One colour whose opacity is `alpha(p)`: glows, shadows and light shapes.
    template <class Alpha>
    void glow(Rgba color, Alpha&& alpha) {
        fill([](Vec2f) { return -1.0f; }, [&](Vec2f p, float, Vec2f) {
            Rgba c = color;
            c.a *= std::clamp(alpha(p), 0.0f, 1.0f);
            return c;
        });
    }

    Texture upload(Renderer2D& renderer) const {
        std::vector<u8> bytes(_px.size() * 4);
        for (std::size_t i = 0; i < _px.size(); ++i) {
            const Rgba& c = _px[i];
            bytes[i * 4 + 0] = static_cast<u8>(std::lround(std::clamp(c.r, 0.0f, 1.0f) * 255));
            bytes[i * 4 + 1] = static_cast<u8>(std::lround(std::clamp(c.g, 0.0f, 1.0f) * 255));
            bytes[i * 4 + 2] = static_cast<u8>(std::lround(std::clamp(c.b, 0.0f, 1.0f) * 255));
            bytes[i * 4 + 3] = static_cast<u8>(std::lround(std::clamp(c.a, 0.0f, 1.0f) * 255));
        }
        Texture texture = renderer.create_texture_from_rgba(bytes.data(), {_w, _h});
        if (texture) {
            renderer.set_scale_mode(texture, ScaleMode::Linear);
        }
        return texture;
    }

private:
    void over(int x, int y, Rgba c, float coverage) {
        Rgba& d = _px[static_cast<std::size_t>(y * _w + x)];
        const float a = c.a * coverage;
        const float out = a + d.a * (1 - a);
        if (out <= 0) {
            return;
        }
        d.r = (c.r * a + d.r * d.a * (1 - a)) / out;
        d.g = (c.g * a + d.g * d.a * (1 - a)) / out;
        d.b = (c.b * a + d.b * d.a * (1 - a)) / out;
        d.a = out;
    }

    int _w, _h;
    float _scale;
    std::vector<Rgba> _px;
};

float circle(Vec2f p, Vec2f center, float radius) { return length(sub(p, center)) - radius; }

float box(Vec2f p, Rectf r, float round = 0) {
    const Vec2f c{r.x + r.w / 2, r.y + r.h / 2};
    const Vec2f q{std::abs(p.x - c.x) - r.w / 2 + round, std::abs(p.y - c.y) - r.h / 2 + round};
    return length({std::max(q.x, 0.0f), std::max(q.y, 0.0f)}) + std::min(std::max(q.x, q.y), 0.0f) - round;
}

float capsule(Vec2f p, Vec2f a, Vec2f b, float radius) {
    const Vec2f pa = sub(p, a), ba = sub(b, a);
    const float t = std::clamp(dot(pa, ba) / dot(ba, ba), 0.0f, 1.0f);
    return length(sub(pa, mul(ba, t))) - radius;
}

// Signed distance to a simple polygon (after Inigo Quilez).
float polygon(Vec2f p, std::span<const Vec2f> v) {
    float d = dot(sub(p, v[0]), sub(p, v[0]));
    float sign = 1;
    for (std::size_t i = 0, j = v.size() - 1; i < v.size(); j = i, ++i) {
        const Vec2f e = sub(v[j], v[i]), w = sub(p, v[i]);
        const Vec2f b = sub(w, mul(e, std::clamp(dot(w, e) / dot(e, e), 0.0f, 1.0f)));
        d = std::min(d, dot(b, b));
        const bool c1 = p.y >= v[i].y, c2 = p.y < v[j].y, c3 = e.x * w.y > e.y * w.x;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) {
            sign = -sign;
        }
    }
    return sign * std::sqrt(d);
}

std::vector<Vec2f> star(Vec2f c, int points, float outer, float inner, float turn) {
    std::vector<Vec2f> v;
    for (int i = 0; i < points * 2; ++i) {
        const float angle = turn + i * pi / points;
        const float r = i % 2 == 0 ? outer : inner;
        v.push_back({c.x + std::cos(angle) * r, c.y + std::sin(angle) * r});
    }
    return v;
}

std::vector<Vec2f> regular(Vec2f c, int sides, float radius, float turn) {
    return star(c, sides, radius, radius, turn);
}

// A hard-surface material: a dark outline, a body lit broadly from the top left,
// and a bevelled rim whose brightness follows the edge's normal, so every plate
// reads as raised metal.
struct Material {
    Rgba base;
    Rgba outline;
    float outline_width = .9f;
    float bevel = 2.4f;
    float gloss = .45f;
    float grain = .03f;
};

constexpr Vec2f to_light{-.6f, -.8f}; // toward the light, top left

auto metal(Material m, Vec2f center, float radius) {
    return [=](Vec2f p, float d, Vec2f n) {
        if (d > -m.outline_width) {
            return m.outline;
        }
        const float broad = std::clamp(1.05f - dot(sub(p, center), {.6f, .8f}) / (radius * 2.4f), .72f, 1.2f);
        Rgba c = brighter(m.base, broad * (1 + (hash(int(p.x * 8), int(p.y * 8)) - .5f) * m.grain));
        const float depth = -d - m.outline_width;
        if (depth < m.bevel) {
            const float rim = 1 - depth / m.bevel;
            const float facing = dot(n, to_light);
            c = facing > 0 ? mix(c, rgb(255, 255, 255), facing * rim * m.gloss) : mix(c, rgb(0, 0, 0), -facing * rim * .4f);
        }
        return c;
    };
}

// A glowing part (eye, visor, nozzle): hot centre, dark rim.
auto emissive(Rgba core, Rgba edge, Rgba rim, float radius) {
    return [=](Vec2f, float d, Vec2f) {
        if (d > -.7f) {
            return rim;
        }
        return mix(core, edge, std::clamp(1 + d / radius, 0.0f, 1.0f));
    };
}

// Recessed line (panel seam): dark groove with a highlight on its lower-right lip.
template <class Sdf>
void seam(Canvas& canvas, Sdf&& sdf, float width = .5f) {
    canvas.fill([&](Vec2f p) { return std::abs(sdf(p)) - width; }, [](Vec2f, float, Vec2f) { return rgb(0, 0, 0, .55f); });
    canvas.fill([&](Vec2f p) { return std::abs(sdf(Vec2f{p.x - .6f, p.y - .6f})) - width * .6f; },
                [](Vec2f, float, Vec2f) { return rgb(255, 255, 255, .12f); });
}

constexpr float sprite_scale = 2; // sprites are drawn at about half their pixel size

Texture make_player(Renderer2D& renderer) {
    Canvas canvas(64, 64, sprite_scale);
    const std::array<Vec2f, 12> hull{{{61, 32}, {41, 25}, {31, 12}, {24, 13}, {23, 25}, {8, 21},
                                      {13, 32}, {8, 43}, {23, 39}, {24, 51}, {31, 52}, {41, 39}}};
    const auto hull_sdf = [&](Vec2f p) { return polygon(p, hull); };
    canvas.fill(hull_sdf, metal({.base = rgb(64, 198, 206), .outline = rgb(6, 30, 38)}, {34, 32}, 26));
    // Wing panels and the spine.
    seam(canvas, [](Vec2f p) { return capsule(p, {16, 32}, {50, 32}, 0); }, .45f);
    seam(canvas, [](Vec2f p) { return capsule(p, {26, 22}, {36, 27}, 0); }, .35f);
    seam(canvas, [](Vec2f p) { return capsule(p, {26, 42}, {36, 37}, 0); }, .35f);
    for (const float y : {17.0f, 47.0f}) {
        canvas.fill([&](Vec2f p) { return capsule(p, {26, y}, {30, y}, .9f); }, [](Vec2f, float, Vec2f) { return rgb(214, 252, 255); });
    }
    // Glass canopy with a highlight.
    const auto canopy = [](Vec2f p) { return (length({(p.x - 42) / 9.5f, (p.y - 32) / 5}) - 1) * 5; };
    canvas.fill(canopy, [](Vec2f p, float d, Vec2f) {
        if (d > -.8f) return rgb(8, 34, 44);
        const float t = std::clamp((p.x - 33) / 18, 0.0f, 1.0f);
        return mix(rgb(24, 70, 96), rgb(8, 26, 44), t);
    });
    canvas.fill([](Vec2f p) { return capsule(p, {37, 29.5f}, {43, 28.8f}, .9f); }, [](Vec2f, float, Vec2f) { return rgb(210, 250, 255, .9f); });
    // Engine nozzles.
    for (const float y : {27.0f, 37.0f}) {
        canvas.fill([&](Vec2f p) { return circle(p, {11.5f, y}, 2.6f); },
                    emissive(rgb(240, 255, 255), rgb(90, 200, 255), rgb(8, 30, 40), 2.6f));
    }
    return canvas.upload(renderer);
}

Texture make_chaser(Renderer2D& renderer) {
    Canvas canvas(64, 64, sprite_scale);
    const std::vector<Vec2f> body = star({32, 32}, 5, 30, 12.5f, -pi / 2);
    canvas.fill([&](Vec2f p) { return polygon(p, body); }, metal({.base = rgb(255, 98, 112), .outline = rgb(58, 10, 22)}, {32, 32}, 28));
    for (int i = 0; i < 5; ++i) {
        const float angle = -pi / 2 + i * 2 * pi / 5;
        const Vec2f tip{32 + std::cos(angle) * 25, 32 + std::sin(angle) * 25};
        seam(canvas, [&](Vec2f p) { return capsule(p, {32, 32}, tip, 0); }, .4f);
    }
    const std::vector<Vec2f> plate = regular({32, 32}, 5, 12, -pi / 2);
    canvas.fill([&](Vec2f p) { return polygon(p, plate); }, metal({.base = rgb(150, 36, 52), .outline = rgb(50, 8, 18), .bevel = 1.8f}, {32, 32}, 12));
    canvas.fill([](Vec2f p) { return circle(p, {32, 32.5f}, 5.2f); }, emissive(rgb(255, 244, 230), rgb(255, 120, 110), rgb(60, 8, 16), 5.2f));
    return canvas.upload(renderer);
}

Texture make_brute(Renderer2D& renderer) {
    Canvas canvas(64, 64, sprite_scale);
    const std::vector<Vec2f> plate = regular({32, 32}, 6, 30, pi / 6);
    canvas.fill([&](Vec2f p) { return polygon(p, plate); }, metal({.base = rgb(240, 162, 62), .outline = rgb(58, 30, 8), .bevel = 3}, {32, 32}, 30));
    const std::vector<Vec2f> seam_ring = regular({32, 32}, 6, 23, pi / 6);
    seam(canvas, [&](Vec2f p) { return polygon(p, seam_ring); }, .45f);
    const std::vector<Vec2f> inner = regular({32, 32}, 6, 17, pi / 6);
    canvas.fill([&](Vec2f p) { return polygon(p, inner); }, metal({.base = rgb(118, 76, 40), .outline = rgb(40, 20, 6), .bevel = 2}, {32, 32}, 17));
    for (int i = 0; i < 6; ++i) {
        const Vec2f rivet{32 + std::cos(i * pi / 3) * 26.5f, 32 + std::sin(i * pi / 3) * 26.5f};
        canvas.fill([&](Vec2f p) { return circle(p, rivet, 2.1f); }, metal({.base = rgb(255, 214, 150), .outline = rgb(70, 36, 10), .outline_width = .6f, .bevel = 1.2f, .gloss = .7f}, rivet, 2));
    }
    canvas.fill([](Vec2f p) { return capsule(p, {23, 32}, {41, 32}, 3.2f); }, emissive(rgb(255, 248, 210), rgb(255, 176, 70), rgb(44, 20, 4), 3.2f));
    return canvas.upload(renderer);
}

Texture make_orbiter(Renderer2D& renderer) {
    Canvas canvas(64, 64, sprite_scale);
    for (int i = 0; i < 4; ++i) {
        const float angle = pi / 4 + i * pi / 2;
        const std::vector<Vec2f> fin = regular({32 + std::cos(angle) * 25, 32 + std::sin(angle) * 25}, 4, 6, angle);
        canvas.fill([&](Vec2f p) { return polygon(p, fin); }, metal({.base = rgb(120, 86, 204), .outline = rgb(30, 16, 60), .bevel = 1.6f}, {32, 32}, 30));
    }
    const auto ring = [](Vec2f p) { return std::abs(length(sub(p, {32, 32})) - 20) - 4.8f; };
    canvas.fill(ring, metal({.base = rgb(160, 120, 238), .outline = rgb(34, 20, 70), .bevel = 1.8f}, {32, 32}, 24));
    for (int i = 0; i < 8; ++i) {
        const float angle = i * pi / 4 + pi / 8;
        const Vec2f dir{std::cos(angle), std::sin(angle)};
        seam(canvas, [&](Vec2f p) { return capsule(p, add({32, 32}, mul(dir, 16)), add({32, 32}, mul(dir, 24)), 0); }, .4f);
    }
    canvas.fill([](Vec2f p) { return circle(p, {32, 32}, 9.5f); }, metal({.base = rgb(70, 46, 136), .outline = rgb(24, 12, 50), .bevel = 1.6f}, {32, 32}, 9));
    canvas.fill([](Vec2f p) { return circle(p, {32, 32}, 4.2f); }, emissive(rgb(236, 220, 255), rgb(170, 120, 255), rgb(30, 14, 60), 4.2f));
    return canvas.upload(renderer);
}

// Enemy bullets: round (the player's shots are streaks), a white core in a hot
// pink-red rim with a dark edge, so they read against any enemy or light.
Texture make_bullet(Renderer2D& renderer) {
    Canvas canvas(24, 24, 1);
    canvas.fill([](Vec2f p) { return circle(p, {12, 12}, 11); }, [](Vec2f, float d, Vec2f) {
        return d > -1.6f ? rgb(52, 0, 22) : d > -5.5f ? rgb(255, 58, 110) : rgb(255, 238, 244);
    });
    return canvas.upload(renderer);
}

// A thin ring: the player's ground marker and the orbiters' charge warning.
Texture make_ring(Renderer2D& renderer) {
    Canvas canvas(64, 64, 1);
    canvas.fill([](Vec2f p) { return std::abs(length(sub(p, {32, 32})) - 28) - 1.6f; }, [](Vec2f, float, Vec2f) { return rgb(255, 255, 255); });
    return canvas.upload(renderer);
}

// A soft ground shadow, drawn under ships.
Texture make_shadow(Renderer2D& renderer) {
    Canvas canvas(64, 64, 1);
    canvas.glow(rgb(0, 0, 0), [](Vec2f p) {
        const float r = length({(p.x - 32) / 30, (p.y - 32) / 30});
        const float t = std::clamp((1 - r) / .55f, 0.0f, 1.0f);
        return .8f * t * t * (3 - 2 * t);
    });
    return canvas.upload(renderer);
}

// Cores: a faceted green gem; each facet catches the light differently.
Texture make_core(Renderer2D& renderer) {
    Canvas canvas(32, 32, sprite_scale);
    const std::vector<Vec2f> gem = regular({16, 16}, 6, 14, 0);
    canvas.fill([&](Vec2f p) { return polygon(p, gem); }, [](Vec2f p, float d, Vec2f) {
        if (d > -1.1f) return rgb(12, 60, 36);
        const float angle = std::atan2(p.y - 16, p.x - 16);
        const float facet = std::floor((angle + pi) / (pi / 3));
        const float light = .72f + .28f * std::cos(facet * pi / 3 - 3.7f);
        Rgba c = brighter(rgb(70, 224, 150), light);
        if (length(sub(p, {16, 16})) < 6) c = mix(c, rgb(200, 255, 225), .55f);
        return c;
    });
    canvas.fill([](Vec2f p) { return capsule(p, {10, 11}, {13, 9}, .8f); }, [](Vec2f, float, Vec2f) { return rgb(255, 255, 255, .85f); });
    return canvas.upload(renderer);
}

Texture make_shot(Renderer2D& renderer) {
    Canvas canvas(32, 16, 1);
    canvas.glow(rgb(255, 255, 255), [](Vec2f p) {
        const float d = std::max(0.0f, capsule(p, {8, 8}, {24, 8}, 0));
        const float t = std::clamp(1 - d / 7, 0.0f, 1.0f);
        return t * t;
    });
    canvas.fill([](Vec2f p) { return capsule(p, {9, 8}, {25, 8}, 2.2f); }, [](Vec2f, float, Vec2f) { return rgb(255, 255, 255); });
    return canvas.upload(renderer);
}

// A flashlight beam for Light2D::shape: bright near the ship, fading out and
// toward the edges of a 32-degree half-angle cone pointing right.
Texture make_cone(Renderer2D& renderer) {
    Canvas canvas(128, 128, 1);
    canvas.glow(rgb(255, 255, 255), [](Vec2f p) {
        const Vec2f d{(p.x - 64) / 64, (p.y - 64) / 64};
        const float r = length(d);
        if (r >= 1 || d.x <= 0) {
            return 0.0f;
        }
        const float angle = std::abs(std::atan2(d.y, d.x));
        const float across = std::clamp((.56f - angle) / .3f, 0.0f, 1.0f);
        const float along = (1 - r * r) * (1 - r * r);
        return across * across * along;
    });
    return canvas.upload(renderer);
}

// Deck tiles: one 256-unit world tile of four bevelled steel panels with
// rivets, brushed grain and wear. Variants break up the repetition: 1 swaps a
// panel for a floor grate, 2 paints hazard chevrons, 3 adds scuffs and a patch.
Texture make_floor(Renderer2D& renderer, int variant) {
    Canvas canvas(256, 256, 1);
    const float tint = .94f + hash(variant, 7) * .08f;
    canvas.fill([](Vec2f) { return -1.0f; }, [](Vec2f, float, Vec2f) { return rgb(14, 18, 24); }); // gaps
    for (int py = 0; py < 2; ++py) {
        for (int px = 0; px < 2; ++px) {
            const Rectf panel{px * 128.0f + 2, py * 128.0f + 2, 124, 124};
            const int id = py * 2 + px;
            const bool grate = variant == 1 && id == 3;
            canvas.fill(panel, [&](Vec2f p) { return box(p, panel, 3); }, [&](Vec2f p, float d, Vec2f n) {
                const float wear = value_noise(add(p, {float(variant * 91), float(id * 57)}), 40) * .08f +
                                   value_noise(p, 7) * .03f;
                const float brushed = (hash(int(p.y * 2), id) - .5f) * .025f;
                Rgba c = brighter(grate ? rgb(22, 27, 34) : rgb(44, 52, 63), tint * (.94f + wear + brushed));
                if (-d < 2.5f && !grate) {
                    const float facing = dot(n, to_light);
                    c = facing > 0 ? mix(c, rgb(120, 134, 150), facing * .35f) : mix(c, rgb(6, 8, 12), -facing * .5f);
                }
                return c;
            });
            if (grate) {
                for (float x = panel.x + 10; x < panel.x + panel.w - 6; x += 8) {
                    const Rectf bar{x, panel.y + 8, 3.2f, panel.h - 16};
                    canvas.fill(bar, [&](Vec2f p) { return box(p, bar, 1); },
                                metal({.base = rgb(58, 66, 78), .outline = rgb(10, 12, 16), .outline_width = .5f, .bevel = 1, .gloss = .3f}, {x, panel.y + 62}, 4));
                }
                continue;
            }
            // Rivets along each panel's rim.
            for (float t = 10; t < 120; t += 27) {
                for (const Vec2f at : {Vec2f{panel.x + t, panel.y + 6}, Vec2f{panel.x + t, panel.y + panel.h - 6},
                                       Vec2f{panel.x + 6, panel.y + t}, Vec2f{panel.x + panel.w - 6, panel.y + t}}) {
                    canvas.fill({at.x - 3, at.y - 3, 6, 6}, [&](Vec2f p) { return circle(p, at, 1.5f); },
                                metal({.base = rgb(88, 98, 112), .outline = rgb(16, 20, 26), .outline_width = .4f, .bevel = .9f, .gloss = .6f}, at, 1.5f));
                }
            }
            if (variant == 2 && id == 0) {
                // Worn hazard chevrons painted across the panel.
                const Rectf band{panel.x + 18, panel.y + 50, 88, 24};
                canvas.fill(band, [&](Vec2f p) { return box(p, band); }, [&](Vec2f p, float, Vec2f) {
                    const bool stripe = std::fmod(p.x - p.y + 400, 16.0f) < 8;
                    const float worn = value_noise(p, 5) > .38f ? .5f : .18f;
                    return stripe ? rgb(214, 160, 52, worn) : rgb(20, 20, 20, worn * .8f);
                });
            }
            if (variant == 3 && id == 1) {
                const Rectf patch{panel.x + 30, panel.y + 34, 52, 40};
                canvas.fill(patch, [&](Vec2f p) { return box(p, patch, 2); },
                            metal({.base = rgb(52, 60, 70), .outline = rgb(14, 18, 24), .outline_width = .6f, .bevel = 1.6f, .gloss = .3f}, {panel.x + 56, panel.y + 54}, 26));
            }
        }
    }
    // Scuffs.
    for (int i = 0; i < (variant == 3 ? 10 : 4); ++i) {
        const Vec2f a{hash(i, variant * 3 + 1) * 230 + 12, hash(i, variant * 3 + 2) * 230 + 12};
        const Vec2f b = add(a, {hash(i, variant * 3 + 5) * 36 - 18, hash(i, variant * 3 + 6) * 12 - 6});
        canvas.fill({std::min(a.x, b.x) - 2, std::min(a.y, b.y) - 2, std::abs(b.x - a.x) + 4, std::abs(b.y - a.y) + 4},
                    [&](Vec2f p) { return capsule(p, a, b, .35f); }, [](Vec2f, float, Vec2f) { return rgb(120, 132, 146, .35f); });
    }
    return canvas.upload(renderer);
}

// A reactor housing (a bolted octagon around a dark well) and its fan, drawn
// rotating over it.
Texture make_reactor(Renderer2D& renderer) {
    Canvas canvas(64, 64, sprite_scale);
    const std::vector<Vec2f> housing = regular({32, 32}, 8, 31, pi / 8);
    canvas.fill([&](Vec2f p) { return polygon(p, housing); }, metal({.base = rgb(62, 70, 82), .outline = rgb(10, 12, 16), .bevel = 2.6f, .gloss = .35f}, {32, 32}, 30));
    for (int i = 0; i < 8; ++i) {
        const Vec2f bolt{32 + std::cos(i * pi / 4) * 26, 32 + std::sin(i * pi / 4) * 26};
        canvas.fill([&](Vec2f p) { return circle(p, bolt, 1.6f); },
                    metal({.base = rgb(120, 130, 144), .outline = rgb(18, 22, 28), .outline_width = .5f, .bevel = 1, .gloss = .6f}, bolt, 1.6f));
    }
    canvas.fill([](Vec2f p) { return circle(p, {32, 32}, 21); }, [](Vec2f p, float d, Vec2f n) {
        if (d > -1.2f) return rgb(8, 10, 14);
        const float shade = std::clamp(-dot(n, to_light), 0.0f, 1.0f) * std::clamp(1 + d / 6, 0.0f, 1.0f);
        return mix(rgb(20, 24, 30), rgb(46, 52, 62), shade);
    });
    for (const Vec2f lamp : {Vec2f{10, 10}, Vec2f{54, 10}, Vec2f{10, 54}, Vec2f{54, 54}}) {
        canvas.fill([&](Vec2f p) { return circle(p, lamp, 2.2f); }, emissive(rgb(255, 230, 170), rgb(240, 150, 60), rgb(40, 22, 6), 2.2f));
    }
    return canvas.upload(renderer);
}

Texture make_fan(Renderer2D& renderer) {
    Canvas canvas(64, 64, sprite_scale);
    for (int i = 0; i < 6; ++i) {
        const float angle = i * pi / 3;
        const Vec2f a{32 + std::cos(angle) * 6, 32 + std::sin(angle) * 6};
        const Vec2f b{32 + std::cos(angle + .5f) * 18, 32 + std::sin(angle + .5f) * 18};
        canvas.fill([&](Vec2f p) { return capsule(p, a, b, 3.2f); },
                    metal({.base = rgb(96, 106, 120), .outline = rgb(14, 16, 22), .outline_width = .6f, .bevel = 1.4f, .gloss = .5f}, {32, 32}, 18));
    }
    canvas.fill([](Vec2f p) { return circle(p, {32, 32}, 6); }, emissive(rgb(255, 214, 150), rgb(214, 130, 50), rgb(30, 16, 4), 6));
    return canvas.upload(renderer);
}

// Hazard stripes for the arena's boundary walls.
Texture make_edge(Renderer2D& renderer) {
    Canvas canvas(64, 16, 2);
    canvas.fill([](Vec2f) { return -1.0f; }, [](Vec2f p, float, Vec2f) {
        const bool stripe = std::fmod(p.x + p.y, 16.0f) < 8;
        Rgba c = stripe ? rgb(206, 150, 46) : rgb(24, 24, 26);
        c = brighter(c, .9f + value_noise(p, 6) * .15f);
        if (p.y < 1.2f || p.y > 14.8f) c = rgb(10, 10, 12);
        return c;
    });
    return canvas.upload(renderer);
}

// ---------------------------------------------------------------------------

Color with_alpha(Color c, float alpha) {
    c.a = static_cast<u8>(std::clamp(alpha, 0.0f, 1.0f) * 255);
    return c;
}

constexpr std::array<Color, 3> kind_glow{Color::rgb(255, 92, 100), Color::rgb(255, 172, 64), Color::rgb(176, 136, 255)};
constexpr Color ambient = Color::rgb(118, 126, 150);

ParticleBurst burst(Vec2f at, int count, ParticleRange speed, ParticleRange life, float size, Color from, Color to) {
    return {.position = at, .count = count, .speed = speed, .lifetime = life, .start_size = size, .end_size = 0,
            .start_color = from, .end_color = to, .render = {.sprite_id = "halo"}};
}

} // namespace

void ArenaPainter::init(Renderer2D& renderer, Arena& arena) {
    _player = make_player(renderer);
    _enemies = {make_chaser(renderer), make_brute(renderer), make_orbiter(renderer)};
    _core = make_core(renderer);
    _bullet = make_bullet(renderer);
    _ring = make_ring(renderer);
    _shot = make_shot(renderer);
    _cone = make_cone(renderer);
    for (int i = 0; i < 4; ++i) {
        _floors[static_cast<std::size_t>(i)] = make_floor(renderer, i);
    }
    _shadow = make_shadow(renderer);
    _reactor = make_reactor(renderer);
    _fan = make_fan(renderer);
    _edge = make_edge(renderer);
    _halo = _lighting.falloff(renderer);
    const Vec2i halo_size = _halo.size();
    _sprites.set_texture("halo", _halo);
    _sprites.add({.id = "halo", .texture_id = "halo", .source = {0, 0, float(halo_size.x), float(halo_size.y)}});
    _particles.reserve(4096);
    arena.use_enemy_textures(_enemies, {{{28, 28}, {38, 38}, {32, 32}}});
    _ready = true;
}

void ArenaPainter::update(Arena& arena, float dt) {
    for (const ArenaEvent& e : arena.events) {
        switch (e.kind) {
        case ArenaEvent::Kind::Hit:
            _particles.burst(burst(e.pos, 6, {60, 200}, {.1f, .22f}, 5, Color::rgb(255, 220, 180), Color::rgba(255, 120, 60, 0)));
            break;
        case ArenaEvent::Kind::Kill: {
            const Color c = kind_glow[static_cast<std::size_t>(std::clamp(e.enemy_kind, 0, 2))];
            _particles.burst(burst(e.pos, 14, {50, 240}, {.2f, .45f}, 8, c, with_alpha(c, 0)));
            break;
        }
        case ArenaEvent::Kind::Pickup:
            _particles.burst(burst(e.pos, 8, {40, 140}, {.2f, .4f}, 6, Color::rgb(150, 255, 190), Color::rgba(40, 255, 130, 0)));
            break;
        case ArenaEvent::Kind::Dash:
            _particles.burst(burst(e.pos, 10, {120, 240}, {.12f, .3f}, 6, Color::rgba(120, 220, 255, 150), Color::rgba(40, 150, 255, 0)));
            break;
        case ArenaEvent::Kind::Hurt:
            _particles.burst(burst(e.pos, 12, {60, 220}, {.18f, .35f}, 8, Color::rgb(255, 150, 130), Color::rgba(255, 40, 40, 0)));
            break;
        case ArenaEvent::Kind::Fire:
        case ArenaEvent::Kind::EnemyFire:
            break; // shots draw themselves; these cue sound
        }
    }
    arena.events.clear();

    // Twin thruster plumes while moving: two particles per step, blown backwards.
    const Vec2f travel = arena.travel();
    if (length(travel) > 0) {
        const Vec2f facing = arena.facing();
        const Vec2f side{-facing.y, facing.x};
        const Vec2f rear = sub(arena.player, mul(facing, 13));
        const float speed = arena.dashing() ? 560.0f : 240.0f;
        _thrust_timer += dt * 37;
        for (const float sign : {-1.0f, 1.0f}) {
            const float jitter = std::sin(_thrust_timer + sign * 1.7f) * 40;
            _particles.emit(Particle{
                .position = add(rear, mul(side, sign * 8)),
                .velocity = add(mul(facing, -speed), mul(side, jitter)),
                .lifetime = arena.dashing() ? .26f : .16f,
                .start_size = arena.dashing() ? 10.0f : 7.0f,
                .end_size = 0,
                .start_color = Color::rgba(130, 215, 255, 200),
                .end_color = Color::rgba(40, 90, 255, 0),
                .render = {.sprite_id = "halo"},
            });
        }
    }
    _particles.update(dt);
}

void ArenaPainter::draw_floor(Renderer2D& renderer, const Arena& arena, const Camera2D& camera) {
    // Deck tiles: a variant per tile from a hash of its coordinates, so the
    // details never line up into a visible grid. (Drawn 1:1 and unrotated: the
    // software backend pays heavily for scaled or rotated copies.)
    const int tile_x = int(std::floor(camera.offset.x / 256)), tile_y = int(std::floor(camera.offset.y / 256));
    for (int y = tile_y; y <= tile_y + int(camera.viewport.y / 256) + 1; ++y) {
        for (int x = tile_x; x <= tile_x + int(camera.viewport.x / 256) + 1; ++x) {
            const u32 h = static_cast<u32>(x * 73856093) ^ static_cast<u32>(y * 19349663);
            const u32 mixed = (h ^ (h >> 13)) * 0x5bd1e995u;
            const int variant = (mixed >> 8) % 7 < 4 ? 0 : 1 + int((mixed >> 12) % 3);
            const auto p = camera.world_to_screen({float(x * 256), float(y * 256)});
            renderer.draw_texture(_floors[static_cast<std::size_t>(variant)], {0, 0, 256, 256}, {p.x, p.y, 256, 256});
        }
    }
    // The arena's boundary: hazard strips along the inside of its edge (the
    // camera never shows past it; ships are kept 20 units clear).
    const auto wall = [&](Vec2f a, Vec2f b) {
        const Vec2f s = camera.world_to_screen(a), e = camera.world_to_screen(b);
        const Rectf r{std::min(s.x, e.x), std::min(s.y, e.y), std::abs(e.x - s.x), std::abs(e.y - s.y)};
        if (r.x > camera.viewport.x || r.y > camera.viewport.y || r.x + r.w < 0 || r.y + r.h < 0) {
            return;
        }
        const bool across = r.w >= r.h;
        const float run = across ? r.w : r.h, depth = across ? r.h : r.w;
        for (float t = 0; t < run; t += 64) {
            const float len = std::min(64.0f, run - t);
            const Rectf src{0, 0, len * 2, 32};
            if (across) {
                renderer.draw_texture(_edge, src, {r.x + t, r.y, len, depth});
            } else {
                // Rotate the strip a quarter turn about its centre.
                const Vec2f c{r.x + r.w / 2, r.y + t + len / 2};
                renderer.draw_texture(_edge, src, {c.x - len / 2, c.y - depth / 2, len, depth}, colors::white, 90, {.5f, .5f});
            }
        }
    };
    constexpr float w = 3072, h = 2048, t = 16;
    wall({0, 0}, {w, t});
    wall({0, h - t}, {w, h});
    wall({0, t}, {t, h - t});
    wall({w - t, t}, {w, h - t});

    // Reactor landmarks make camera movement and aim direction legible. Their
    // fans turn with simulation time, so they freeze with pause.
    for (int y = 256; y < 2048; y += 512) {
        for (int x = 256; x < 3072; x += 512) {
            const auto p = camera.world_to_screen({float(x), float(y)});
            if (p.x < -60 || p.x > camera.viewport.x + 60 || p.y < -60 || p.y > camera.viewport.y + 60) {
                continue;
            }
            renderer.draw_texture(_shadow, {0, 0, 64, 64}, {p.x - 44, p.y - 40, 96, 96}, Color::rgba(255, 255, 255, 150));
            renderer.draw_texture(_reactor, {0, 0, 128, 128}, {p.x - 40, p.y - 40, 80, 80});
            renderer.draw_texture(_fan, {0, 0, 128, 128}, {p.x - 26, p.y - 26, 52, 52}, colors::white,
                                  arena.elapsed * 17 + float(x + y), {.5f, .5f});
        }
    }
}

void ArenaPainter::collect_lights(const Arena& arena, const Camera2D& camera) {
    _lights.clear();
    const Vec2f view = camera.viewport;
    const auto on_screen = [&](Vec2f s, float r) { return s.x > -r && s.y > -r && s.x < view.x + r && s.y < view.y + r; };
    const Vec2f player = camera.world_to_screen(arena.player);
    const Vec2f facing = arena.facing();
    _lights.push_back({.position = player, .radius = 250, .color = Color::rgb(140, 205, 255), .intensity = .9f});
    _lights.push_back({.position = player, .radius = 460, .color = Color::rgb(215, 238, 255), .intensity = .85f,
                       .shape = _cone, .rotation = degrees(facing)});
    if (arena.muzzle_flash()) {
        _lights.push_back({.position = camera.world_to_screen(add(arena.player, mul(facing, 22))), .radius = 150,
                           .color = Color::rgb(255, 226, 160), .intensity = 1.6f});
    }
    for (int y = 256; y < 2048; y += 512) {
        for (int x = 256; x < 3072; x += 512) {
            const Vec2f s = camera.world_to_screen({float(x), float(y)});
            if (on_screen(s, 170)) {
                _lights.push_back({.position = s, .radius = 160, .color = Color::rgb(255, 176, 96),
                                   .intensity = .45f + .15f * std::sin(arena.elapsed * 2.1f + float(x + y) * .01f)});
            }
        }
    }
    for (const Impact& impact : arena.impacts()) {
        const Vec2f s = camera.world_to_screen(impact.pos);
        if (on_screen(s, 90)) {
            _lights.push_back({.position = s, .radius = 90, .color = Color::rgb(255, 164, 84), .intensity = 1.1f * impact.life / .3f});
        }
    }
    for (const VisibleEnemy& enemy : _visible) {
        if (enemy.kind == 2 && enemy.cooldown < .45f) {
            const Vec2f s = camera.world_to_screen(enemy.pos);
            if (on_screen(s, 70)) {
                _lights.push_back({.position = s, .radius = 70, .color = Color::rgb(210, 160, 255), .intensity = .6f});
            }
        }
    }
}

void ArenaPainter::collect_effects(const Arena& arena, const RenderView& view) {
    _emissive.clear();
    _glow.clear();
    const auto visible = [&](Vec2f p, float r) { return render_view_visible(view, {p.x - r, p.y - r, r * 2, r * 2}); };
    const auto halo = [&](Vec2f p, float size, Color color) {
        _glow.draw_texture({}, _halo, {p.x - size / 2, p.y - size / 2, size, size}, color);
    };

    const auto sprite = [&](RenderQueue& queue, const Texture& texture, Vec2f p, float size, Color tint, float rotation = 0) {
        queue.draw_texture({}, texture, {p.x - size / 2, p.y - size / 2, size, size}, tint, {}, rotation);
    };

    for (const VisibleEnemy& enemy : _visible) {
        const Vec2f p = enemy.pos;
        if (!visible(p, 32)) {
            continue;
        }
        if (enemy.kind == 1) {
            // Armor left, once damaged: three pips over the plate.
            for (int i = 0; i < 3 && enemy.hp < 3; ++i) {
                const bool left = float(i) < enemy.hp;
                _emissive.fill_rect({}, {p.x - 9 + i * 7.0f, p.y - 26, 5, 3}, left ? Color::rgb(255, 214, 120) : Color::rgba(60, 40, 24, 200));
            }
        } else if (enemy.kind == 2 && enemy.cooldown < .45f) {
            // About to fire: a ring closes in on the orbiter.
            const float t = enemy.cooldown / .45f;
            sprite(_emissive, _ring, p, 30 + 34 * t, Color::rgba(226, 180, 255, u8(255 - 150 * t)));
            halo(p, 30, Color::rgba(190, 140, 255, 90));
        }
    }
    for (const Pickup& pickup : arena.pickups) {
        if (!visible(pickup.pos, 16)) {
            continue;
        }
        // Blinks in its last three seconds.
        if (pickup.life < 3 && std::fmod(pickup.life * 5, 1.0f) < .4f) {
            continue;
        }
        const float bob = std::sin(arena.elapsed * 3 + pickup.pos.x * .02f) * 2;
        halo(pickup.pos, 22, Color::rgba(60, 255, 140, 60));
        sprite(_emissive, _core, {pickup.pos.x, pickup.pos.y + bob}, 14, colors::white, arena.elapsed * 40 + pickup.pos.x);
    }
    for (const Impact& impact : arena.impacts()) {
        const float t = impact.life / .3f;
        halo(impact.pos, 18 + (1 - t) * 30, with_alpha(Color::rgb(255, 180, 100), t * .45f));
    }
    for (const Shot& shot : arena.shots) {
        if (!visible(shot.pos, 16)) {
            continue;
        }
        if (shot.hostile) {
            const float pulse = 1 + .08f * std::sin(arena.elapsed * 18 + shot.pos.x * .05f);
            halo(shot.pos, 28, Color::rgba(255, 50, 110, 90));
            sprite(_emissive, _bullet, shot.pos, 14 * pulse, colors::white);
        } else {
            halo(shot.pos, 16, Color::rgba(60, 170, 255, 70));
            _emissive.draw_texture({}, _shot, {shot.pos.x - 10, shot.pos.y - 4, 20, 8}, Color::rgb(150, 236, 255), {}, degrees(shot.velocity));
        }
    }

    const Vec2f facing = arena.facing();
    if (arena.dashing()) {
        for (int i = 3; i > 0; --i) {
            const Vec2f p = sub(arena.player, mul(arena.travel(), float(i) * 16));
            _glow.draw_texture({}, _player, {p.x - 24, p.y - 24, 48, 48}, Color::rgba(60, 180, 240, u8(90 - i * 25)), {}, degrees(facing));
        }
    }
    // The player's marker: a ground ring and a soft glow; the ship itself is drawn
    // last, over every effect.
    sprite(_emissive, _ring, arena.player, 58, Color::rgba(120, 230, 255, 120));
    halo(arena.player, 70, Color::rgba(40, 140, 230, 40));
    halo(sub(arena.player, mul(facing, 18)), 24, Color::rgba(90, 190, 255, 90));
    if (arena.muzzle_flash()) {
        halo(add(arena.player, mul(facing, 24)), 30, Color::rgba(255, 234, 170, 170));
    }
    submit_particles(_glow, _particles.particles(), {.sprites = &_sprites, .sort = false});
}

// The enemies near the view, gathered in one pass over the arena's storage;
// shadows, lights and effects then only visit these, however large the arena.
void ArenaPainter::gather_visible(const Arena& arena, const Camera2D& camera) {
    KIN_PROFILE_SCOPE("example.art.gather");
    constexpr float margin = 80; // covers the widest per-enemy effect (a 70px light)
    const Vec2f view = camera.viewport;
    _visible.clear();
    arena.each_enemy([&](Vec2f pos, const Enemy& enemy) {
        const Vec2f s = camera.world_to_screen(pos);
        if (s.x > -margin && s.y > -margin && s.x < view.x + margin && s.y < view.y + margin) {
            _visible.push_back({pos, enemy.kind, enemy.hp, enemy.cooldown});
        }
    });
}

void ArenaPainter::draw(Renderer2D& renderer, Arena& arena, const Camera2D& camera) {
    {
        KIN_PROFILE_SCOPE("example.art.floor");
        draw_floor(renderer, arena, camera);
    }
    gather_visible(arena, camera);

    const RenderView view{.camera = &camera, .culling_enabled = true};
    // Light the environment only: enemies, shots and the player are drawn after,
    // at full colour, so threats read the same in any light.
    {
        KIN_PROFILE_SCOPE("example.art.lighting");
        collect_lights(arena, camera);
        draw_shadows(renderer, arena, camera);
        _lit = _lighting.apply(renderer, {0, 0, camera.viewport.x, camera.viewport.y}, ambient, _lights);
    }
    {
        arena.collect_entities(_world, view); // enemies: the ECS TextureRenderer path
        KIN_PROFILE_SCOPE("example.art.enemies_flush");
        _world.flush(renderer, view);
    }
    {
        KIN_PROFILE_SCOPE("example.art.effects");
        collect_effects(arena, view);
        _emissive.flush(renderer, view);
        const auto additive = renderer.scoped_blend_mode(BlendMode::Additive);
        _glow.flush(renderer, view);
    }
    const Color body = arena.hurt() ? Color::rgb(255, 150, 140) : arena.dashing() ? Color::rgb(220, 255, 255) : colors::white;
    const Vec2f ship = camera.world_to_screen(arena.player);
    renderer.draw_texture(_player, {0, 0, 128, 128}, {ship.x - 24, ship.y - 24, 48, 48}, body, degrees(arena.facing()), {.5f, .5f});
}

// Soft contact shadows, cast down and to the right (away from the sprites'
// top-left key light), so ships sit above the deck. Drawn before lighting, so
// the floor's darkness and the shadows are lit together.
void ArenaPainter::draw_shadows(Renderer2D& renderer, const Arena& arena, const Camera2D& camera) {
    constexpr std::array<float, 3> size{34, 46, 40};
    const Color shade = colors::white;
    const auto cast = [&](Vec2f world, float s) {
        const Vec2f p = add(camera.world_to_screen(world), {5, 7});
        if (p.x < -s || p.y < -s || p.x > camera.viewport.x + s || p.y > camera.viewport.y + s) {
            return;
        }
        renderer.draw_texture(_shadow, {0, 0, 64, 64}, {p.x - s / 2, p.y - s / 2, s, s}, shade);
    };
    for (const VisibleEnemy& enemy : _visible) {
        cast(enemy.pos, size[static_cast<std::size_t>(std::clamp(enemy.kind, 0, 2))]);
    }
    cast(arena.player, 44);
}

void ArenaPainter::enable_post_process(Renderer2D& renderer) {
    const Vec2i size = renderer.output_size();
    if (_post_enabled && size == _post_size) {
        return;
    }
    _post_enabled = true;
    _post_size = size;
    const ShaderHandle bright = renderer.builtin_shader(BuiltinShader::BloomBright);
    const ShaderHandle blur = renderer.builtin_shader(BuiltinShader::BloomBlur);
    const ShaderHandle combine = renderer.builtin_shader(BuiltinShader::BloomCombine);
    const ShaderHandle vignette = renderer.builtin_shader(BuiltinShader::Vignette);
    const ShaderHandle grade = renderer.builtin_shader(BuiltinShader::ColorGrade);
    if (!bright || !blur || !combine || !vignette || !grade || size.x <= 0 || size.y <= 0) {
        return; // no shader support (software backend): present as drawn
    }
    const auto params = [](std::initializer_list<float> values) {
        ShaderParams p;
        std::copy(values.begin(), values.end(), p.uniforms.begin());
        return p;
    };
    const float tx = 1.0f / float(size.x), ty = 1.0f / float(size.y);
    _post = {
        {.shader = bright, .params = params({.8f, 1.0f})},
        {.shader = blur, .params = params({tx * 2, 0})},
        {.shader = blur, .params = params({0, ty * 2})},
        {.shader = blur, .params = params({tx * 5, 0})},
        {.shader = blur, .params = params({0, ty * 5})},
        {.shader = combine, .params = params({.6f}), .sample_original = true},
        {.shader = vignette, .params = params({.45f, .85f, .6f})},
        {.shader = grade, .params = params({1.02f, 1.06f, 1.05f, 0, .8f, .9f, 1.0f, .06f})},
    };
    renderer.set_post_process(_post);
}

void ArenaPainter::disable_post_process(Renderer2D& renderer) {
    if (_post_enabled) {
        renderer.clear_post_process();
        _post_enabled = false;
        _post_size = {};
    }
}

} // namespace examples
