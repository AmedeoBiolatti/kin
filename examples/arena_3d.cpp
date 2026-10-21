#include "arena_3d.hpp"

#if defined(KIN_EXPERIMENTAL_3D)
#include <algorithm>
#include <cmath>
#include <kin/core/profile.hpp>

namespace examples {
namespace {

constexpr float ppm = ArenaPainter3D::pixels_per_metre;
constexpr float arena_w = 3072 / ppm, arena_d = 2048 / ppm; // 48 x 32 m
constexpr float fly_height = 0.45f;                         // where ships, enemies and shots fly
constexpr float tile = 2.0f;

// The arena's 2D point at `height` metres.
Vec3f ground(Vec2f p, float height = 0.0f) {
    return {p.x / ppm, height, p.y / ppm};
}

// The chase camera looking at `target`: behind and above it.
Camera3D chase(Vec3f target) {
    Camera3D camera = Camera3D::look_at(target + Vec3f{0.0f, 12.0f, 7.5f}, target + Vec3f{0.0f, 0.0f, -0.6f}, 50.0f);
    camera.z_near = 0.5f;
    return camera;
}

// Where the camera may look so the view stays over the deck (a little past
// its walls), as the 2D camera keeps to the arena: the deck's edges seen at
// the view's top, bottom and sides, measured from what it looks at.
Vec3f keep_over_deck(Vec3f target, Rectf view) {
    const Camera3D camera = chase({});
    const auto edge = [&](Vec2f point) {
        return hit_plane_y(camera.ray({view.x + point.x * view.w, view.y + point.y * view.h}, view)).value_or(Vec3f{});
    };
    const float top = -edge({0.5f, 0.0f}).z, bottom = edge({0.5f, 1.0f}).z;
    const float side = edge({1.0f, 0.5f}).x; // half the deck's width seen across the middle
    const float margin = 1.0f;
    const auto clamp = [](float v, float lo, float hi) { return lo > hi ? (lo + hi) * 0.5f : std::clamp(v, lo, hi); };
    return {clamp(target.x, side - margin, arena_w - side + margin), target.y,
            clamp(target.z, top - margin, arena_d - bottom + margin)};
}

constexpr std::array<Color, 3> enemy_colors{Color::rgb(241, 99, 103), Color::rgb(241, 182, 92),
                                            Color::rgb(161, 127, 237)};

// The player's ship: a dart pointing down -Z with two fins.
MeshData3D make_ship() {
    MeshData3D ship;
    ship.append(meshes::cone(0.22f, 0.8f, 16), Mat4f::rotation_x(-90.0f));
    ship.append(meshes::box({0.75f, 0.06f, 0.25f}), Mat4f::translation({0.0f, 0.0f, 0.22f}));
    ship.append(meshes::box({0.06f, 0.22f, 0.2f}), Mat4f::translation({0.0f, 0.12f, 0.28f}));
    return ship;
}

// A hexagonal plate, like the 2D brute.
MeshData3D make_plate() {
    return meshes::cylinder(0.3f, 0.14f, 6);
}

// Turned `degrees` about +Y, then moved to `at`: written out, as there may be
// tens of thousands a frame.
Mat4f turned_at(Vec3f at, float degrees) {
    const float r = degrees * 3.14159265f / 180.0f;
    const float c = std::cos(r), s = std::sin(r);
    Mat4f m;
    m.m[0] = c;
    m.m[2] = -s;
    m.m[8] = s;
    m.m[10] = c;
    m.m[12] = at.x;
    m.m[13] = at.y;
    m.m[14] = at.z;
    return m;
}

std::uint32_t hash(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x9E3779B1u ^ static_cast<std::uint32_t>(y) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    return h ^ (h >> 12);
}

} // namespace

void ArenaPainter3D::init(Renderer2D& renderer) {
    _r3d = std::make_unique<Renderer3D>(renderer);
    _tile = _r3d->create_mesh(meshes::box({tile - 0.08f, 0.1f, tile - 0.08f})).value_or({});
    _wall = _r3d->create_mesh(meshes::box({1.0f, 1.0f, 1.0f})).value_or({});
    _ship = _r3d->create_mesh(make_ship()).value_or({});
    _star = _r3d->create_mesh(meshes::star_prism(5, 0.26f, 0.12f, 0.14f)).value_or({});
    _plate = _r3d->create_mesh(make_plate()).value_or({});
    _ring = _r3d->create_mesh(meshes::torus(0.2f, 0.06f, 20, 8)).value_or({});
    _shot = _r3d->create_mesh(meshes::sphere(0.07f, 10, 6)).value_or({});
    _core = _r3d->create_mesh(meshes::octahedron(0.16f)).value_or({});
    _spark = _r3d->create_mesh(meshes::box({0.06f, 0.06f, 0.06f})).value_or({});

    // The deck: tiles with seams between them, a shade each from a hash; walls round it.
    for (int z = 0; z < static_cast<int>(arena_d / tile); ++z) {
        for (int x = 0; x < static_cast<int>(arena_w / tile); ++x) {
            const std::uint32_t h = hash(x, z);
            const u8 shade = static_cast<u8>(70 + h % 14);
            _tiles.push_back({Mat4f::translation({(static_cast<float>(x) + 0.5f) * tile, -0.05f,
                                                  (static_cast<float>(z) + 0.5f) * tile}),
                              h % 23 == 0
                                  ? Color::rgb(64, 96, 104)
                                  : Color::rgb(shade, static_cast<u8>(shade + 8), static_cast<u8>(shade + 16))});
        }
    }
    const float height = 0.8f, thick = 0.5f;
    const auto wall = [&](Vec3f center, Vec3f size) {
        _walls.push_back({Mat4f::translation(center) * Mat4f::scaling(size), Color::rgb(92, 104, 120)});
    };
    wall({arena_w / 2, height / 2, -thick / 2}, {arena_w + 2 * thick, height, thick});
    wall({arena_w / 2, height / 2, arena_d + thick / 2}, {arena_w + 2 * thick, height, thick});
    wall({-thick / 2, height / 2, arena_d / 2}, {thick, height, arena_d});
    wall({arena_w + thick / 2, height / 2, arena_d / 2}, {thick, height, arena_d});
}

bool ArenaPainter3D::available() const {
    return _r3d && _r3d->available();
}

const RenderStats3D& ArenaPainter3D::stats() const {
    static const RenderStats3D none{};
    return _r3d ? _r3d->stats() : none;
}

void ArenaPainter3D::fill_scene(const Arena& arena) {
    _scene.clear();
    const Vec3f player = ground(arena.player, fly_height);
    // Behind and above the player, looking down at it, but kept over the deck.
    _scene.camera = chase(keep_over_deck(player, _playfield));
    _scene.lights.sun = {.direction = {-0.45f, 1.0f, 0.35f}, .color = {1.05f, 1.0f, 0.95f}};
    _scene.lights.ambient = {0.22f, 0.25f, 0.32f};

    _scene.draw_instanced(_tile, _tiles);
    _scene.draw_instanced(_wall, _walls);

    // Enemies by kind, floating and turning; the time is the simulation's, so
    // a paused run holds still.
    const float t = arena.elapsed;
    for (auto& list : _enemies) {
        list.clear();
    }
    arena.each_enemy([&](Vec2f pos, const Enemy& e) {
        const int kind = std::clamp(e.kind, 0, 2);
        const float phase = static_cast<float>(e.slot) * 0.7f;
        const float bob = std::sin(t * 2.3f + phase) * 0.08f;
        const float turn = (kind == 2 ? 120.0f : 70.0f) * t + phase * 57.0f;
        Mat4f m = turned_at(ground(pos, fly_height + bob), turn);
        if (kind == 2) {
            static const Mat4f tilt = Mat4f::rotation_x(60.0f);
            m = m * tilt; // the ring stands half up, turning
        }
        _enemies[static_cast<std::size_t>(kind)].push_back({m, kin::colors::white});
    });
    _scene.draw_instanced(_star, _enemies[0], {.color = enemy_colors[0], .emissive = 0.15f});
    _scene.draw_instanced(_plate, _enemies[1], {.color = enemy_colors[1], .emissive = 0.1f});
    _scene.draw_instanced(_ring, _enemies[2], {.color = enemy_colors[2], .emissive = 0.15f});

    // The player's ship, turned to face where it aims.
    const Vec2f f = arena.facing();
    const Color body = arena.hurt()      ? Color::rgb(255, 150, 140)
                       : arena.dashing() ? Color::rgb(220, 255, 255)
                                         : Color::rgb(90, 220, 235);
    _scene.draw(_ship, Mat4f::translation(player) * Mat4f::rotation(Quatf::look_rotation({f.x, 0.0f, f.y})),
                {.emissive = 0.2f}, body);

    // Shots glow: the player's cyan, the enemies' pink.
    std::size_t friendly = 0;
    for (const Shot& s : arena.shots) {
        friendly += s.hostile ? 0 : 1;
    }
    auto ours =
        _scene.add_instances(_shot, friendly, {.color = Color::rgb(150, 245, 255), .emissive = 1.5f, .unlit = true})
            .value_or({});
    std::size_t i = 0;
    for (const Shot& s : arena.shots) {
        if (!s.hostile) {
            ours[i++] = {Mat4f::translation(ground(s.pos, fly_height)), kin::colors::white};
        }
    }
    auto theirs = _scene
                      .add_instances(_shot, arena.shots.size() - friendly,
                                     {.color = Color::rgb(255, 110, 190), .emissive = 1.5f, .unlit = true})
                      .value_or({});
    i = 0;
    for (const Shot& s : arena.shots) {
        if (s.hostile) {
            theirs[i++] = {Mat4f::translation(ground(s.pos, fly_height)) * Mat4f::scaling({1.3f, 1.3f, 1.3f}),
                           kin::colors::white};
        }
    }
    // Cores to pick up, spinning; sparks and impacts as glowing bits.
    auto cores =
        _scene.add_instances(_core, arena.pickups.size(), {.color = Color::rgb(120, 255, 170), .emissive = 0.8f})
            .value_or({});
    for (std::size_t k = 0; k < arena.pickups.size(); ++k) {
        const Pickup& p = arena.pickups[k];
        cores[k] = {Mat4f::translation(ground(p.pos, 0.35f + 0.08f * std::sin(t * 3.0f + static_cast<float>(k)))) *
                        Mat4f::rotation_y(t * 90.0f + static_cast<float>(k) * 40.0f),
                    kin::colors::white};
    }
    auto sparks = _scene
                      .add_instances(_spark, arena.sparks.size(),
                                     {.color = Color::rgb(255, 200, 120), .emissive = 1.2f, .unlit = true})
                      .value_or({});
    for (std::size_t k = 0; k < arena.sparks.size(); ++k) {
        const Spark& s = arena.sparks[k];
        const float size = std::clamp(s.life * 3.0f, 0.2f, 1.0f);
        sparks[k] = {Mat4f::translation(ground(s.pos, fly_height)) * Mat4f::scaling({size, size, size}),
                     kin::colors::white};
    }
    const auto& impacts = arena.impacts();
    auto flashes =
        _scene
            .add_instances(_shot, impacts.size(), {.color = Color::rgb(255, 230, 200), .emissive = 2.0f, .unlit = true})
            .value_or({});
    for (std::size_t k = 0; k < impacts.size(); ++k) {
        const float grow = 1.0f + (0.3f - impacts[k].life) * 12.0f;
        flashes[k] = {Mat4f::translation(ground(impacts[k].pos, fly_height)) * Mat4f::scaling({grow, grow, grow}),
                      Color::rgba(255, 255, 255, 255)};
    }
}

void ArenaPainter3D::draw(Renderer2D& renderer, const Arena& arena, Rectf playfield) {
    _playfield = playfield;
    // The view at the playfield's size on the screen, in its pixels; the
    // camera's reach measured in the same rectangle (fill_scene).
    const Vec2i out = renderer.output_size();
    const float fit = std::min(static_cast<float>(out.x) / playfield.w, static_cast<float>(out.y) / playfield.h);
    const Vec2i pixels{std::max(1, static_cast<int>(std::lround(playfield.w * fit))),
                       std::max(1, static_cast<int>(std::lround(playfield.h * fit)))};
    if (!_view.valid()) {
        _view = _r3d->create_view(pixels, {.samples = 4, .background = Color::rgb(12, 20, 28)}).value_or({});
    } else if (_view.size() != pixels) {
        _r3d->resize(_view, pixels);
    }
    {
        KIN_PROFILE_SCOPE("example.3d.scene");
        fill_scene(arena);
    }
    {
        KIN_PROFILE_SCOPE("example.3d.render");
        _r3d->render(_view, _scene);
    }
    renderer.draw_texture(_view.texture(), playfield);
}

std::optional<Vec2f> ArenaPainter3D::screen_to_world(Vec2f screen) const {
    const std::optional<Vec3f> hit = hit_plane_y(_scene.camera.ray(screen, _playfield), fly_height);
    if (!hit) {
        return std::nullopt;
    }
    return Vec2f{hit->x * ppm, hit->z * ppm};
}

std::optional<Vec2f> ArenaPainter3D::world_to_screen(Vec2f world, float height) const {
    const std::optional<ScreenPoint> p = _scene.camera.project(ground(world, height), _playfield);
    if (!p) {
        return std::nullopt;
    }
    return p->pos;
}

} // namespace examples
#endif
