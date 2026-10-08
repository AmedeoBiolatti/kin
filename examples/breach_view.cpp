#include "breach_view.hpp"
#include "breach_shader.hpp"

#include <kin/assets/content.hpp>
#include <kin/core/profile.hpp>
#include <kin/l10n/localization.hpp>
#include <kin/ui2/widgets.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace examples {
namespace {

constexpr Color ink = Color::rgb(226, 236, 242);
constexpr Color muted = Color::rgb(146, 170, 186);
constexpr Color teal = Color::rgb(103, 226, 206);
constexpr Color glass = Color::rgba(8, 14, 22, 205);
constexpr Color rim = Color::rgba(90, 200, 190, 80);
constexpr Color track = Color::rgba(30, 40, 50, 230);
constexpr float shell_life = 0.35f; // seconds a shield shows
constexpr std::array<Color, 3> enemy_colors{Color::rgb(241, 99, 103), Color::rgb(241, 182, 92), Color::rgb(161, 127, 237)};

float rand(RngKey& key, float lo, float hi) {
    const auto [next, value] = split(key);
    key = next;
    return rng_f32(value, lo, hi);
}

// The gun, in the overlay camera's space (it looks along -z from the origin).
MeshData make_gun() {
    MeshData gun;
    gun.append(meshes::box({0.08f, 0.1f, 0.36f}));
    gun.append(meshes::box({0.05f, 0.12f, 0.07f}), Mat4f::translation({0.0f, -0.09f, 0.1f}) * Mat4f::rotation_x(-15.0f));
    gun.append(meshes::box({0.025f, 0.025f, 0.1f}), Mat4f::translation({0.0f, 0.065f, -0.02f}));
    gun.append(meshes::cylinder(0.022f, 0.3f, 12), Mat4f::translation({0.0f, 0.02f, -0.32f}) * Mat4f::rotation_x(-90.0f));
    return gun;
}

} // namespace

void BreachView::init(Renderer2D& renderer, Breach& breach, const BreachViewOptions& options) {
    _options = options;
    _r3d = std::make_unique<Renderer3D>(renderer);
    if (_r3d->available() && !content_root().empty()) {
        _gun = std::make_unique<ModelFile>(*_r3d, content_root() / "models" / "gun.glb");
        _crate = std::make_unique<ModelFile>(*_r3d, content_root() / "models" / "crate.glb");
    }
    _box = _r3d->create_mesh(meshes::box({1.0f, 1.0f, 1.0f}));
    _wedge = _r3d->create_mesh(meshes::wedge({1.0f, 1.0f, 1.0f}));
    _shot = _r3d->create_mesh(meshes::sphere(0.08f, 10, 6));
    _core = _r3d->create_mesh(meshes::octahedron(0.18f));
    _spark = _r3d->create_mesh(meshes::box({0.05f, 0.05f, 0.05f}));
    _barrel = _r3d->create_mesh(make_gun());
    _flash = _r3d->create_mesh(meshes::sphere(1.0f, 10, 6));
    _shell = _r3d->create_mesh(meshes::sphere(1.0f, 20, 12));
    if (const auto spirv = breach_shader("shield.frag.spv"); !spirv.empty()) {
        _shield = _r3d->create_material_shader(spirv);
    }
    breach.use_enemy_meshes({_r3d->create_mesh(meshes::star_prism(5, 0.32f, 0.14f, 0.16f)),
                             _r3d->create_mesh(meshes::cylinder(0.4f, 0.16f, 6)),
                             _r3d->create_mesh(meshes::torus(0.26f, 0.07f, 20, 8))},
                            {Material3D{.color = enemy_colors[0], .emissive = 0.2f},
                             Material3D{.color = enemy_colors[1], .emissive = 0.15f},
                             Material3D{.color = enemy_colors[2], .emissive = 0.2f}});
    build_level(breach.level());
}

const Render3DStats& BreachView::stats() const {
    static const Render3DStats none{};
    return _r3d ? _r3d->stats() : none;
}

void BreachView::build_level(const BreachLevel& level) {
    // The ground floor in 2 m tiles with seams; raised floors, walls and
    // ramps a cell at a time, a little apart so the cells read.
    for (int z = 0; z < BreachLevel::depth; z += 2) {
        for (int x = 0; x < BreachLevel::width; x += 2) {
            const u8 shade = static_cast<u8>(66 + ((x * 7 + z * 13) % 11));
            _tiles.push_back({Mat4f::translation({float(x) + 1, -0.05f, float(z) + 1}) * Mat4f::scaling({1.94f, 0.1f, 1.94f}),
                              Color::rgb(shade, static_cast<u8>(shade + 8), static_cast<u8>(shade + 18))});
        }
    }
    for (int z = 0; z < BreachLevel::depth; ++z) {
        for (int x = 0; x < BreachLevel::width; ++x) {
            const BreachLevel::Cell& c = level.at(x, z);
            const float cx = float(x) + 0.5f, cz = float(z) + 0.5f;
            if (c.kind == BreachLevel::Kind::Wall) {
                _walls.push_back({Mat4f::translation({cx, c.low / 2, cz}) * Mat4f::scaling({1.0f, c.low, 1.0f}),
                                  (x + z) % 2 == 0 ? Color::rgb(96, 108, 126) : Color::rgb(88, 100, 118)});
            } else if (c.crate) {
                _crates.push_back({cx, 0.0f, cz});
            } else if (c.kind == BreachLevel::Kind::Floor && c.low > 0) {
                const u8 g = static_cast<u8>(c.low > 1.2f ? 120 : 104);
                _raised.push_back({Mat4f::translation({cx, c.low / 2, cz}) * Mat4f::scaling({0.97f, c.low, 0.97f}),
                                   Color::rgb(static_cast<u8>(g - 30), static_cast<u8>(g - 12), g)});
            } else if (c.kind == BreachLevel::Kind::Ramp) {
                if (c.low > 0) {
                    _raised.push_back({Mat4f::translation({cx, c.low / 2, cz}) * Mat4f::scaling({1.0f, c.low, 1.0f}),
                                       Color::rgb(80, 96, 110)});
                }
                // The wedge rises toward -z; turned to rise along the ramp's axis.
                const float turn = c.axis == 0 ? -90.0f : c.axis == 1 ? 90.0f : c.axis == 2 ? 180.0f : 0.0f;
                _ramps.push_back({Mat4f::translation({cx, (c.low + c.high) / 2, cz}) * Mat4f::rotation_y(turn) *
                                      Mat4f::scaling({1.0f, c.high - c.low, 1.0f}),
                                  Color::rgb(112, 150, 156)});
            }
        }
    }
}

void BreachView::update(Breach& breach, float dt) {
    for (const BreachEvent& e : breach.events) {
        switch (e.kind) {
        case BreachEvent::Kind::Fire:
            _tracers.push_back({e.pos, e.to, 0.07f});
            _flash_time = 0.05f;
            _recoil = 1.0f;
            // A point light's colour is its intensity a unit away and falls with
            // the square of the distance, no brighter than at its size: about
            // what the flashes were before (brighter close by, dimmer far off).
            _flashes.push_back({e.pos, {16.0f, 11.5f, 5.0f}, 7.0f, 0.06f, 0.06f, 0.8f});
            for (int i = 0; i < 3; ++i) {
                _sparks.push_back({e.to, {rand(_rng, -2, 2), rand(_rng, 0, 3), rand(_rng, -2, 2)}, rand(_rng, .1f, .25f),
                                   Color::rgb(255, 220, 160)});
            }
            break;
        case BreachEvent::Kind::Hit:
        case BreachEvent::Kind::Kill: {
            const bool kill = e.kind == BreachEvent::Kind::Kill;
            const Color c = enemy_colors[static_cast<std::size_t>(std::clamp(e.enemy_kind, 0, 2))];
            for (int i = 0; i < (kill ? 16 : 6); ++i) {
                _sparks.push_back({e.pos, {rand(_rng, -4, 4), rand(_rng, -1, 5), rand(_rng, -4, 4)}, rand(_rng, .2f, .5f), c});
            }
            _numbers.push_back({e.pos, 0.7f, kill});
            if (!kill && _shield) {
                _shells.push_back({e.pos, shell_life, c});
            }
            if (kill) {
                const LinearColor l = to_linear(c);
                _flashes.push_back({e.pos, Vec3f{l.r, l.g, l.b} * 8.0f, 5.0f, 0.25f, 0.25f, 1.2f}); // a fireball
            }
            break;
        }
        case BreachEvent::Kind::Pickup:
            for (int i = 0; i < 8; ++i) {
                _sparks.push_back({e.pos, {rand(_rng, -2, 2), rand(_rng, 0, 3), rand(_rng, -2, 2)}, rand(_rng, .2f, .4f),
                                   Color::rgb(140, 255, 190)});
            }
            break;
        case BreachEvent::Kind::EnemyFire:
            _flashes.push_back({e.pos, {3.6f, 0.9f, 2.3f}, 3.0f, 0.08f, 0.08f});
            break;
        case BreachEvent::Kind::Hurt:
        case BreachEvent::Kind::Jump:
            break;
        }
    }
    breach.events.clear();
    for (Spark& s : _sparks) {
        s.velocity.y -= 9.0f * dt;
        s.pos += s.velocity * dt;
        s.life -= dt;
    }
    for (Tracer& t : _tracers) t.life -= dt;
    for (Flash& f : _flashes) f.life -= dt;
    for (Shell& s : _shells) s.life -= dt;
    std::erase_if(_shells, [](const Shell& s) { return s.life <= 0; });
    std::erase_if(_flashes, [](const Flash& f) { return f.life <= 0; });
    for (Number& n : _numbers) {
        n.pos.y += 1.4f * dt;
        n.life -= dt;
    }
    std::erase_if(_sparks, [](const Spark& s) { return s.life <= 0; });
    std::erase_if(_tracers, [](const Tracer& t) { return t.life <= 0; });
    std::erase_if(_numbers, [](const Number& n) { return n.life <= 0; });
    if (_sparks.size() > 4096) _sparks.erase(_sparks.begin(), _sparks.begin() + static_cast<std::ptrdiff_t>(_sparks.size() - 4096));
    _flash_time = std::max(0.0f, _flash_time - dt);
    _recoil = std::max(0.0f, _recoil - dt * 9.0f);
    _walked += std::sqrt(breach.velocity.x * breach.velocity.x + breach.velocity.z * breach.velocity.z) * dt * (breach.grounded ? 1.0f : 0.0f);
}

void BreachView::fill_scene(Breach& breach, const BreachHudState& state) {
    _scene.clear();
    Camera3D& camera = _scene.camera;
    camera.position = breach.eye_position();
    camera.rotation = Quatf::yaw_pitch(state.yaw, state.pitch);
    camera.fov = 75.0f;
    camera.z_near = 0.05f;
    _scene.lights.sun = {.direction = {-0.4f, 1.0f, 0.25f}, .color = {1.0f, 0.97f, 0.92f}};
    _scene.lights.ambient = {0.25f, 0.28f, 0.35f};
    // The sun's shadows over the whole deck, a little above the walls.
    _scene.lights.shadow_area = _options.shadows ? Bounds3{{0.0f, -0.2f, 0.0f}, {float(BreachLevel::width), 4.0f, float(BreachLevel::depth)}}
                                                 : Bounds3{};
    if (_options.lights) {
        for (const Flash& f : _flashes) {
            _scene.add_light({.position = f.pos, .color = f.color * (f.life / f.length), .radius = f.radius, .size = f.size});
        }
    }

    _scene.draw_instanced(_box, _tiles);
    _scene.draw_instanced(_box, _raised);
    _scene.draw_instanced(_box, _walls);
    _scene.draw_instanced(_wedge, _ramps);
    for (const Vec3f& at : _crates) {
        if (_crate && _crate->model()) {
            _scene.draw(_crate->model(), Mat4f::translation(at));
        } else {
            _scene.draw(_box, Mat4f::translation(at + Vec3f{0.0f, BreachLevel::crate_height / 2, 0.0f}),
                        {.color = Color::rgb(150, 110, 70)});
        }
    }
    {
        KIN_PROFILE_SCOPE("example.breach.ecs");
        breach.render_state().propagate_transforms();
        breach.render_state().collect(_scene);
    }
    auto shots = _scene.add_instances(_shot, breach.shots.size(),
                                      {.color = Color::rgb(255, 110, 190), .emissive = 2.5f, .unlit = true, .casts_shadow = false});
    for (std::size_t i = 0; i < breach.shots.size(); ++i) {
        shots[i] = {Mat4f::translation(breach.shots[i].pos), kin::colors::white};
    }
    const float t = breach.elapsed;
    auto cores = _scene.add_instances(_core, breach.pickups.size(), {.color = Color::rgb(120, 255, 170), .emissive = 1.2f});
    for (std::size_t i = 0; i < breach.pickups.size(); ++i) {
        cores[i] = {Mat4f::translation(breach.pickups[i].pos + Vec3f{0.0f, 0.1f * std::sin(t * 3 + float(i)), 0.0f}) *
                        Mat4f::rotation_y(t * 90.0f + float(i) * 40.0f),
                    kin::colors::white};
    }
    // Sparks and tracers add light: additive, in any order, never hiding anything.
    auto sparks = _scene.add_instances(_spark, _sparks.size(), {.emissive = 2.0f, .unlit = true, .blend = Blend3D::Additive});
    for (std::size_t i = 0; i < _sparks.size(); ++i) {
        const float size = std::clamp(_sparks[i].life * 4.0f, 0.3f, 1.0f);
        sparks[i] = {Mat4f::translation(_sparks[i].pos) * Mat4f::scaling({size, size, size}), _sparks[i].color};
    }
    // Shields: a hit enemy's shell, flashing and fading, its lines climbing.
    for (const Shell& shell : _shells) {
        const float strength = shell.life / shell_life;
        Material3D material{.color = shell.color, .double_sided = true, .blend = Blend3D::Additive, .casts_shadow = false,
                            .shader = _shield};
        material.shader_params = {breach.elapsed, strength * strength, 6.0f, 0.8f};
        _scene.draw(_shell, Mat4f::translation(shell.pos) * Mat4f::scaling({0.62f, 0.62f, 0.62f}), material);
    }
    // Tracers: thin boxes from the muzzle to where the shot hit, thinning out.
    auto tracers = _scene.add_instances(_box, _tracers.size(),
                                        {.color = Color::rgb(170, 250, 255), .emissive = 3.0f, .unlit = true, .blend = Blend3D::Additive});
    for (std::size_t i = 0; i < _tracers.size(); ++i) {
        const Vec3f d = _tracers[i].to - _tracers[i].from;
        const float len = std::max(0.01f, length(d));
        const float w = 0.025f * std::clamp(_tracers[i].life / 0.07f, 0.2f, 1.0f);
        tracers[i] = {Mat4f::translation((_tracers[i].from + _tracers[i].to) * 0.5f) * Mat4f::rotation(Quatf::look_rotation(d)) *
                          Mat4f::scaling({w, w, len}),
                      kin::colors::white};
    }

    // The gun, in front of everything: kicked back after a shot, bobbing as the player walks.
    Scene3D& overlay = _scene.overlay();
    overlay.camera = Camera3D{};
    overlay.camera.position = {};
    overlay.camera.fov = 60.0f;
    overlay.camera.z_near = 0.02f;
    overlay.cull = false;
    const float bob = std::sin(_walked * 1.6f) * 0.012f;
    const Mat4f gun = Mat4f::translation({0.2f, -0.17f + bob - _recoil * 0.01f, -0.5f + _recoil * 0.05f}) *
                      Mat4f::rotation_x(_recoil * 6.0f) * Mat4f::rotation_y(3.0f) * Mat4f::scaling({0.6f, 0.6f, 0.6f});
    if (_gun && _gun->model()) {
        overlay.draw(_gun->model(), gun);
    } else {
        overlay.draw(_barrel, gun, {.color = Color::rgb(70, 82, 96), .emissive = 0.05f});
    }
    if (_flash_time > 0) {
        const float s = 0.035f + 0.02f * (_flash_time / 0.05f);
        overlay.draw(_flash, gun * Mat4f::translation({0.0f, 0.02f, -0.5f}) * Mat4f::scaling({s, s, s * 1.6f}),
                     {.color = Color::rgb(255, 230, 160), .emissive = 6.0f, .unlit = true, .blend = Blend3D::Additive});
    }
}

void BreachView::draw(Renderer2D& renderer, Breach& breach, Input& input, const BreachHudState& state, float dt) {
    const Vec2i out = renderer.output_size();
    const float fit = std::min(static_cast<float>(out.x) / _playfield.w, static_cast<float>(out.y) / _playfield.h);
    const Vec2i pixels{std::max(1, static_cast<int>(std::lround(_playfield.w * fit))),
                       std::max(1, static_cast<int>(std::lround(_playfield.h * fit)))};
    if (!_view.valid()) {
        // HDR with bloom: shots, sparks and flashes past white glow.
        _view = _r3d->create_view(pixels, {.samples = 4, .background = Color::rgb(18, 26, 36), .hdr = _options.bloom,
                                           .bloom = _options.bloom ? 0.5f : 0.0f, .bloom_threshold = 1.0f});
    } else if (_view.size() != pixels) {
        _r3d->resize(_view, pixels);
    }
    if (_gun) _gun->poll();
    if (_crate) _crate->poll();
    {
        KIN_PROFILE_SCOPE("example.breach.scene");
        fill_scene(breach, state);
    }
    {
        KIN_PROFILE_SCOPE("example.breach.render");
        _r3d->render(_view, _scene);
    }
    renderer.draw_texture(_view.texture(), _playfield);
    draw_hud(renderer, breach, input, state, dt);
}

void BreachView::draw_hud(Renderer2D& renderer, const Breach& breach, Input& input, const BreachHudState& state, float dt) {
    const Camera3D& camera = _scene.camera;
    // Damaged plates' health over them, nearest first, at most 48.
    std::vector<std::pair<float, int>> damaged;
    const auto& positions = breach.enemy_positions();
    for (int slot = 0; slot < static_cast<int>(positions.size()); ++slot) {
        const float hp = breach.enemy_hp(slot), max = Breach::enemy_max_hp(Breach::enemy_kind(slot));
        if (hp < max && hp > 0) {
            damaged.push_back({length_squared(positions[static_cast<std::size_t>(slot)] - camera.position), slot});
        }
    }
    std::sort(damaged.begin(), damaged.end());
    damaged.resize(std::min<std::size_t>(damaged.size(), 48));
    for (const auto& [d2, slot] : damaged) {
        if (d2 > 30 * 30) break;
        const auto at = camera.project(positions[static_cast<std::size_t>(slot)] + Vec3f{0.0f, 0.6f, 0.0f}, _playfield);
        if (!at) continue;
        const float w = std::clamp(600.0f / std::sqrt(d2), 16.0f, 60.0f);
        const float share = breach.enemy_hp(slot) / Breach::enemy_max_hp(Breach::enemy_kind(slot));
        renderer.fill_rect({at->pos.x - w / 2 - 1, at->pos.y - 3, w + 2, 6}, Color::rgba(10, 14, 20, 200));
        renderer.fill_rect({at->pos.x - w / 2, at->pos.y - 2, w * share, 4}, enemy_colors[1]);
    }
    // The crosshair.
    const Vec2f c{_playfield.x + _playfield.w / 2, _playfield.y + _playfield.h / 2};
    const Color cross = breach.hurt() ? Color::rgb(255, 140, 120) : Color::rgba(230, 250, 250, 220);
    renderer.fill_rect({c.x - 9, c.y - 1, 6, 2}, cross);
    renderer.fill_rect({c.x + 3, c.y - 1, 6, 2}, cross);
    renderer.fill_rect({c.x - 1, c.y - 9, 2, 6}, cross);
    renderer.fill_rect({c.x - 1, c.y + 3, 2, 6}, cross);

    // Text, at the display's resolution (as Signal Siege's HUD).
    const auto native = renderer.scoped_native_coordinates();
    const Vec2i output = renderer.output_size();
    if (output.x <= 0 || output.y <= 0) return;
    const float fit = std::min(output.x / 1280.0f, output.y / 800.0f);
    const Rectf area{(output.x - 1280 * fit) * .5f, (output.y - 800 * fit) * .5f, 1280 * fit, 800 * fit};
    const auto viewport = renderer.scoped_viewport(area);
    const float dpi = std::isfinite(state.display_scale) && state.display_scale > 0 ? state.display_scale : 1;
    const float scale = fit / dpi; // logical units to these
    const float w = area.w / dpi, h = area.h / dpi;
    const auto style = [&](float points, Color color) {
        const float size = std::min(32.0f, points * dpi);
        return ui2::TextStyle{.font = ui2::system_ui_font(size), .scale = points * dpi / size, .color = color};
    };
    const auto text = [&](std::string_view value, float x, float y, float points, Color color) {
        _ui.text(value, {x * dpi, y * dpi}, style(points, color));
    };
    const auto width = [&](std::string_view value, float points) {
        const ui2::TextStyle s = style(points, ink);
        return ui2::measure_text(s.font, value, s.scale).x / dpi;
    };
    const auto centered = [&](std::string_view value, float y, float points, Color color) {
        text(value, (w - width(value, points)) * .5f, y, points, color);
    };
    const auto px = [&](Rectf r) { return Rectf{r.x * dpi, r.y * dpi, r.w * dpi, r.h * dpi}; };
    const auto panel = [&](Rectf r) {
        ui2::Panel p{.bounds = px(r), .color = glass, .border = rim};
        ui2::run(_ui, p);
    };
    const auto bar = [&](Rectf r, float value, Color fill) {
        ui2::ProgressBar b{.bounds = px(r), .value = std::clamp(value, 0.0f, 1.0f), .fill = fill, .background = track};
        ui2::run(_ui, b);
    };
    _ui.begin(input, renderer, dt);
    // Damage numbers where hits landed.
    for (const Number& n : _numbers) {
        const auto at = camera.project(n.pos, _playfield);
        if (!at) continue;
        const Color color = n.kill ? Color::rgba(255, 220, 140, static_cast<u8>(std::clamp(n.life / .7f, 0.0f, 1.0f) * 255))
                                   : Color::rgba(255, 255, 255, static_cast<u8>(std::clamp(n.life / .7f, 0.0f, 1.0f) * 230));
        text(n.kill ? "+1" : "1", at->pos.x * scale - 4, at->pos.y * scale - 10, n.kill ? 15 : 12, color);
    }
    panel({16, 16, 300, 64});
    text(tr("breach.name"), 30, 23, 13, teal);
    text(tr("hud.hull"), 30, 46, 11, muted);
    const float hull = std::clamp(float(breach.health) / 100.0f, 0.0f, 1.0f);
    bar({78, 50, 188, 9}, hull, hull > .5f ? Color::rgb(102, 210, 163) : hull > .25f ? Color::rgb(236, 190, 90) : Color::rgb(240, 96, 90));
    text(std::to_string(std::max(0, breach.health)), 276, 45, 12, ink);

    panel({w * .5f - 130, 12, 260, 74});
    const int remaining = std::max(0, 90 - int(breach.elapsed));
    char timer[16];
    std::snprintf(timer, sizeof timer, "%02d:%02d", remaining / 60, remaining % 60);
    centered(timer, 16, 30, remaining <= 10 ? Color::rgb(255, 214, 140) : ink);
    centered(tr("hud.wave_of", {{"wave", std::min(6, 1 + int(breach.elapsed / 15))}, {"waves", 6}}), 56, 12, muted);
    bar({w * .5f - 110, 76, 220, 3}, breach.elapsed / 90, teal);

    panel({w - 236, 16, 220, 64});
    text(std::to_string(breach.kills), w - 220, 22, 26, ink);
    text(tr("hud.kills"), w - 220, 54, 10, muted);
    text(std::to_string(breach.collected), w - 120, 22, 26, Color::rgb(120, 255, 170));
    text(tr("hud.cores"), w - 120, 54, 10, muted);

    const std::string line = tr(state.autoplay ? "hud.stats_autoplay" : "hud.stats",
                                {{"enemies", breach.enemy_count()}, {"shots", int(breach.shots.size())}, {"draws", int(stats().draws)}});
    text(line, w - width(line, 10) - 20, h - 24, 10, muted);
    if (breach.over()) {
        centered(tr(breach.health > 0 ? "results.secured" : "results.lost"), h * .42f, 30, breach.health > 0 ? teal : Color::rgb(240, 96, 90));
    } else if (state.paused) {
        centered(tr("hud.paused"), h * .42f, 30, ink);
        centered(tr("breach.paused_hint"), h * .42f + 44, 12, muted);
    } else if (!state.captured && !state.autoplay) {
        centered(tr("breach.controls"), h - 52, 11, muted);
    }
    _ui.end();
}

} // namespace examples
