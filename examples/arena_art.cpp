#include "arena_art.hpp"

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
// Procedural textures: shapes are signed distance functions (px, negative
// inside), filled with 1px antialiasing and composited back to front.

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

class Canvas {
public:
    Canvas(int w, int h) : _w(w), _h(h), _px(static_cast<std::size_t>(w * h)) {}

    template <class Sdf, class Paint>
    void fill(Sdf&& sdf, Paint&& paint) {
        for (int y = 0; y < _h; ++y) {
            for (int x = 0; x < _w; ++x) {
                const Vec2f p{x + .5f, y + .5f};
                const float d = sdf(p);
                const float coverage = std::clamp(.5f - d, 0.0f, 1.0f);
                if (coverage > 0) {
                    over(x, y, paint(p, d), coverage);
                }
            }
        }
    }

    // One colour whose opacity is `alpha(p)`: glows and light shapes.
    template <class Alpha>
    void glow(Rgba color, Alpha&& alpha) {
        fill([](Vec2f) { return -1.0f; }, [&](Vec2f p, float) {
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
    std::vector<Rgba> _px;
};

float circle(Vec2f p, Vec2f center, float radius) { return length(sub(p, center)) - radius; }

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

// Hull paint: a dark outline, a bevelled rim, light from the top left.
auto shaded(Rgba base, Rgba outline, Vec2f center, float size) {
    return [=](Vec2f p, float d) {
        if (d > -1.6f) {
            return outline;
        }
        const float light = std::clamp(1.0f - ((p.x - center.x) + (p.y - center.y)) / (size * 1.8f), .72f, 1.22f);
        Rgba c = brighter(base, light);
        if (d > -4.0f) {
            c = mix(c, rgb(255, 255, 255), .2f);
        }
        return c;
    };
}

float hash(int x, int y) {
    u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

// Smooth value noise, 0..1, with features about `cell` px apart.
float value_noise(Vec2f p, float cell) {
    const float fx = p.x / cell, fy = p.y / cell;
    const int x = static_cast<int>(std::floor(fx)), y = static_cast<int>(std::floor(fy));
    const float tx = fx - x, ty = fy - y;
    const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    const float a = hash(x, y), b = hash(x + 1, y), c = hash(x, y + 1), d = hash(x + 1, y + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

Texture make_player(Renderer2D& renderer) {
    Canvas canvas(64, 64);
    const std::array<Vec2f, 8> hull{{{60, 32}, {30, 51}, {26, 42}, {7, 50}, {14, 32}, {7, 14}, {26, 22}, {30, 13}}};
    canvas.fill([&](Vec2f p) { return polygon(p, hull); }, shaded(rgb(72, 204, 206), rgb(12, 38, 46), {32, 32}, 28));
    canvas.fill([](Vec2f p) { return capsule(p, {16, 32}, {34, 32}, 1.4f); }, [](Vec2f, float) { return rgb(22, 84, 94); });
    canvas.fill([](Vec2f p) { return (length({(p.x - 41) / 9, (p.y - 32) / 5}) - 1) * 5; },
                [](Vec2f p, float d) { return d > -1.2f ? rgb(20, 60, 70) : p.y < 31 ? rgb(236, 255, 252) : rgb(150, 226, 232); });
    return canvas.upload(renderer);
}

Texture make_chaser(Renderer2D& renderer) {
    Canvas canvas(64, 64);
    const std::vector<Vec2f> body = star({32, 32}, 5, 30, 13, -pi / 2);
    canvas.fill([&](Vec2f p) { return polygon(p, body); }, shaded(rgb(255, 100, 112), rgb(64, 12, 24), {32, 32}, 30));
    canvas.fill([](Vec2f p) { return circle(p, {32, 33}, 8); },
                [](Vec2f p, float d) { return d > -1.4f ? rgb(90, 20, 30) : p.y < 31 ? rgb(255, 236, 226) : rgb(255, 170, 160); });
    return canvas.upload(renderer);
}

Texture make_brute(Renderer2D& renderer) {
    Canvas canvas(64, 64);
    const std::vector<Vec2f> plate = regular({32, 32}, 6, 30, pi / 6);
    const std::vector<Vec2f> inner = regular({32, 32}, 6, 18, pi / 6);
    canvas.fill([&](Vec2f p) { return polygon(p, plate); }, shaded(rgb(246, 168, 66), rgb(60, 32, 10), {32, 32}, 30));
    canvas.fill([&](Vec2f p) { return polygon(p, inner); }, shaded(rgb(96, 62, 38), rgb(40, 22, 10), {32, 32}, 18));
    for (int i = 0; i < 6; ++i) {
        const Vec2f rivet{32 + std::cos(i * pi / 3) * 24, 32 + std::sin(i * pi / 3) * 24};
        canvas.fill([&](Vec2f p) { return circle(p, rivet, 2.4f); }, [](Vec2f, float d) { return d > -.8f ? rgb(80, 44, 16) : rgb(255, 222, 160); });
    }
    canvas.fill([](Vec2f p) { return capsule(p, {24, 32}, {40, 32}, 3); }, [](Vec2f, float) { return rgb(255, 238, 170); });
    return canvas.upload(renderer);
}

Texture make_orbiter(Renderer2D& renderer) {
    Canvas canvas(64, 64);
    for (int i = 0; i < 4; ++i) {
        const float angle = pi / 4 + i * pi / 2;
        const std::vector<Vec2f> fin = regular({32 + std::cos(angle) * 25, 32 + std::sin(angle) * 25}, 4, 6, angle);
        canvas.fill([&](Vec2f p) { return polygon(p, fin); }, shaded(rgb(120, 86, 200), rgb(34, 20, 64), {32, 32}, 30));
    }
    canvas.fill([](Vec2f p) { return std::abs(length(sub(p, {32, 32})) - 20) - 4.5f; },
                shaded(rgb(158, 118, 236), rgb(36, 22, 70), {32, 32}, 26));
    canvas.fill([](Vec2f p) { return circle(p, {32, 32}, 10); },
                [](Vec2f p, float d) { return d > -1.4f ? rgb(40, 24, 76) : p.x + p.y < 58 ? rgb(170, 140, 240) : rgb(112, 80, 196); });
    return canvas.upload(renderer);
}

// Enemy bullets: round (the player's shots are streaks), a white core in a hot
// pink-red rim with a dark edge, so they read against any enemy or light.
Texture make_bullet(Renderer2D& renderer) {
    Canvas canvas(24, 24);
    canvas.fill([](Vec2f p) { return circle(p, {12, 12}, 11); }, [](Vec2f, float d) {
        return d > -1.6f ? rgb(52, 0, 22) : d > -5.5f ? rgb(255, 58, 110) : rgb(255, 238, 244);
    });
    return canvas.upload(renderer);
}

// A thin ring: the player's ground marker and the orbiters' charge warning.
Texture make_ring(Renderer2D& renderer) {
    Canvas canvas(64, 64);
    canvas.fill([](Vec2f p) { return std::abs(length(sub(p, {32, 32})) - 28) - 1.6f; }, [](Vec2f, float) { return rgb(255, 255, 255); });
    return canvas.upload(renderer);
}

Texture make_core(Renderer2D& renderer) {
    Canvas canvas(32, 32);
    const std::vector<Vec2f> gem = regular({16, 16}, 6, 14, 0);
    canvas.fill([&](Vec2f p) { return polygon(p, gem); }, [](Vec2f p, float d) {
        if (d > -1.4f) {
            return rgb(16, 64, 40);
        }
        return p.x < 16 && p.y < 16 ? rgb(200, 255, 222) : p.x + p.y < 32 ? rgb(96, 236, 160) : rgb(46, 176, 110);
    });
    return canvas.upload(renderer);
}

Texture make_shot(Renderer2D& renderer) {
    Canvas canvas(32, 16);
    canvas.glow(rgb(255, 255, 255), [](Vec2f p) {
        const float d = std::max(0.0f, capsule(p, {8, 8}, {24, 8}, 0));
        const float t = std::clamp(1 - d / 7, 0.0f, 1.0f);
        return t * t;
    });
    canvas.fill([](Vec2f p) { return capsule(p, {9, 8}, {25, 8}, 2.2f); }, [](Vec2f, float) { return rgb(255, 255, 255); });
    return canvas.upload(renderer);
}

// A flashlight beam for Light2D::shape: bright near the ship, fading out and
// toward the edges of a 32-degree half-angle cone pointing right.
Texture make_cone(Renderer2D& renderer) {
    Canvas canvas(128, 128);
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

// Steel deck plates: four panels per tile, seams, bolts and wear.
Texture make_floor(Renderer2D& renderer) {
    constexpr int size = 256;
    Canvas canvas(size, size);
    canvas.fill([](Vec2f) { return -1.0f; }, [](Vec2f p, float) {
        const float wear = value_noise(p, 48) * .06f + value_noise(p, 9) * .03f + hash(int(p.x), int(p.y)) * .02f;
        Rgba c = brighter(rgb(38, 46, 57), .92f + wear);
        const int lx = int(p.x) % 128, ly = int(p.y) % 128;
        if (lx == 0 || ly == 0) {
            c = rgb(16, 21, 28);
        } else if (lx == 1 || ly == 1) {
            c = rgb(56, 67, 80);
        } else if (lx == 127 || ly == 127) {
            c = brighter(c, .8f);
        }
        return c;
    });
    for (int py = 0; py < 2; ++py) {
        for (int px = 0; px < 2; ++px) {
            for (const Vec2f corner : {Vec2f{9, 9}, Vec2f{119, 9}, Vec2f{9, 119}, Vec2f{119, 119}}) {
                const Vec2f at{px * 128 + corner.x, py * 128 + corner.y};
                canvas.fill([&](Vec2f p) { return circle(p, at, 3); },
                            [&](Vec2f p, float d) { return d > -1 ? rgb(20, 26, 33) : p.x + p.y < at.x + at.y ? rgb(96, 108, 122) : rgb(62, 72, 84); });
            }
        }
    }
    // A worn walkway stripe and a few scuffs.
    canvas.fill([](Vec2f p) { return std::abs(p.y - 192) - 10; }, [](Vec2f p, float) {
        return rgb(50, 60, 72, value_noise(p, 14) > .45f ? .55f : .3f);
    });
    for (int i = 0; i < 6; ++i) {
        const Vec2f a{hash(i, 1) * 240 + 8, hash(i, 2) * 240 + 8};
        const Vec2f b = add(a, {hash(i, 3) * 40 - 20, hash(i, 4) * 12 - 6});
        canvas.fill([&](Vec2f p) { return capsule(p, a, b, .6f); }, [](Vec2f, float) { return rgb(70, 82, 96, .6f); });
    }
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
    _floor = make_floor(renderer);
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
    const int tile_x = int(camera.offset.x) / 256, tile_y = int(camera.offset.y) / 256;
    for (int y = tile_y; y <= tile_y + 4; ++y) {
        for (int x = tile_x; x <= tile_x + 5; ++x) {
            const auto p = camera.world_to_screen({float(x * 256), float(y * 256)});
            renderer.draw_texture(_floor, {0, 0, 256, 256}, {p.x, p.y, 256, 256});
            // Recessed cable channels with travelling power indicators.
            renderer.fill_rect({p.x + 120, p.y + 2, 16, 252}, Color::rgb(12, 18, 25));
            renderer.draw_line({p.x + 122, p.y + 2}, {p.x + 122, p.y + 254}, Color::rgb(34, 70, 82));
            const float pulse = std::fmod(arena.elapsed * 45 + float((x + y) * 37), 224.0f);
            renderer.fill_rect({p.x + 126, p.y + 12 + pulse, 3, 14}, Color::rgb(92, 104, 118));
            if ((x + y) % 3 == 0) {
                renderer.fill_rect({p.x + 178, p.y + 180, 48, 36}, Color::rgb(14, 20, 27));
                for (int i = 0; i < 5; ++i) {
                    renderer.fill_rect({p.x + 183, p.y + 185 + i * 6.0f, 38, 2}, Color::rgb(44, 56, 68));
                }
            }
        }
    }
    // Reactor landmarks make camera movement and aim direction legible. Their
    // rotors turn with simulation time, so they freeze with pause.
    for (int y = 256; y < 2048; y += 512) {
        for (int x = 256; x < 3072; x += 512) {
            const auto p = camera.world_to_screen({float(x), float(y)});
            if (p.x < -60 || p.x > camera.viewport.x + 60 || p.y < -60 || p.y > camera.viewport.y + 60) {
                continue;
            }
            renderer.fill_rect({p.x - 38, p.y - 38, 76, 76}, Color::rgb(16, 24, 32));
            renderer.draw_rect({p.x - 38, p.y - 38, 76, 76}, Color::rgb(58, 82, 96));
            renderer.draw_rect({p.x - 28, p.y - 28, 56, 56}, Color::rgb(40, 62, 76));
            for (int i = 0; i < 8; ++i) {
                const float angle = i * .785398f + arena.elapsed * .3f;
                const Vec2f a{p.x + std::cos(angle) * 15, p.y + std::sin(angle) * 15};
                const Vec2f b{p.x + std::cos(angle + .45f) * 24, p.y + std::sin(angle + .45f) * 24};
                renderer.draw_line(a, b, Color::rgb(104, 116, 130));
            }
            renderer.fill_rect({p.x - 6, p.y - 6, 12, 12}, Color::rgb(224, 156, 78));
            for (int i = 0; i < 4; ++i) {
                const float sx = p.x - 36 + i * 20.0f;
                renderer.draw_line({sx, p.y + 43}, {sx + 7, p.y + 50}, Color::rgb(190, 150, 70));
                renderer.draw_line({sx, p.y - 50}, {sx + 7, p.y - 43}, Color::rgb(190, 150, 70));
            }
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
    for (const EcsEntity entity : arena.enemies()) {
        if (entity.get<Enemy>()->kind == 2 && entity.get<Enemy>()->cooldown < .45f) {
            const Vec2f s = camera.world_to_screen(entity.get<Transform2D>()->pos);
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

    for (const EcsEntity entity : arena.enemies()) {
        const Vec2f p = entity.get<Transform2D>()->pos;
        if (!visible(p, 32)) {
            continue;
        }
        const Enemy& enemy = *entity.get<Enemy>();
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

void ArenaPainter::draw(Renderer2D& renderer, Arena& arena, const Camera2D& camera) {
    draw_floor(renderer, arena, camera);

    const RenderView view{.camera = &camera, .culling_enabled = true};
    // Light the environment only: enemies, shots and the player are drawn after,
    // at full colour, so threats read the same in any light.
    collect_lights(arena, camera);
    _lit = _lighting.apply(renderer, {0, 0, camera.viewport.x, camera.viewport.y}, ambient, _lights);
    arena.collect_entities(_world, view); // enemies: the ECS TextureRenderer path
    _world.flush(renderer, view);

    collect_effects(arena, view);
    _emissive.flush(renderer, view);
    {
        const auto additive = renderer.scoped_blend_mode(BlendMode::Additive);
        _glow.flush(renderer, view);
    }
    const Color body = arena.hurt() ? Color::rgb(255, 150, 140) : arena.dashing() ? Color::rgb(220, 255, 255) : colors::white;
    const Vec2f ship = camera.world_to_screen(arena.player);
    renderer.draw_texture(_player, {0, 0, 64, 64}, {ship.x - 24, ship.y - 24, 48, 48}, body, degrees(arena.facing()), {.5f, .5f});
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
