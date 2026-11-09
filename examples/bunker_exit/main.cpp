// bunker_exit: a small scene for three of kin's 3D features at once.
//
//  - Auto exposure (ViewOptions3D::auto_exposure): the camera walks from a dark concrete bunker out into the afternoon
//    sun and back. Outside, the view is too bright for a moment and settles; back inside it is too dark and opens up
//    more slowly, as eyes do.
//  - Decals (Scene3D::draw_decal): scorch marks, a painted exit arrow, hazard stripes by the door and bullet holes with
//    a normal map, projected onto the floor and walls and lit with them. Every picture is made in code.
//  - Morph targets (MeshData3D::morph_targets, Renderer3D::set_morph_weights): two jelly sentinels, a sphere with
//    "squash", "spikes" and "breathe" shapes, bounce and bristle when the camera comes near. The second is a
//    Renderer3D::copy_mesh of the first, so it shows weights of its own.
//
//   bunker_exit [--shot picture.png --frames N] [--start seconds] [--fixed-exposure]
//
// --shot renders headless (with KIN_RENDER_BACKEND=gpu) and writes the picture after N frames; --start begins the walk
// that far in (0: deep inside, 8: outside looking back). Keys: E auto exposure on/off, D decals on/off, Space pause
// the walk, Esc quit.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <kin/assets/image.hpp>
#include <kin/render3d/mesh3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/ui2/text.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace kin;
namespace fs = std::filesystem;

constexpr f32 pi = 3.14159265f;
constexpr f32 walk_period = 16.0f; // seconds for in, out and back

struct Options {
    fs::path shot;
    int frames = 90;
    f32 start = 0.0f;
    bool fixed_exposure = false;
};

// --- Pictures made in code (RGBA8, rows top to bottom) ---

f32 hash(i32 x, i32 y) {
    u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<f32>((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
}

// Smooth value noise in [0, 1], `scale` cells across the picture.
f32 noise(f32 x, f32 y) {
    const i32 xi = static_cast<i32>(std::floor(x)), yi = static_cast<i32>(std::floor(y));
    const f32 fx = x - std::floor(x), fy = y - std::floor(y);
    const f32 sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const f32 a = hash(xi, yi), b = hash(xi + 1, yi), c = hash(xi, yi + 1), d = hash(xi + 1, yi + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

f32 fbm(f32 x, f32 y) {
    return noise(x, y) * 0.5f + noise(x * 2.1f, y * 2.1f) * 0.3f + noise(x * 4.3f, y * 4.3f) * 0.2f;
}

using Pixels = std::vector<u8>;
constexpr i32 side = 256;

void put(Pixels& p, i32 x, i32 y, f32 r, f32 g, f32 b, f32 a) {
    u8* o = &p[(static_cast<std::size_t>(y) * side + x) * 4];
    o[0] = static_cast<u8>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
    o[1] = static_cast<u8>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
    o[2] = static_cast<u8>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
    o[3] = static_cast<u8>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// A scorch mark: soot, darkest in the middle, its edge ragged.
Pixels scorch() {
    Pixels p(side * side * 4);
    for (i32 y = 0; y < side; ++y)
        for (i32 x = 0; x < side; ++x) {
            const f32 u = (x + 0.5f) / side * 2 - 1, v = (y + 0.5f) / side * 2 - 1;
            const f32 r = std::sqrt(u * u + v * v) + (fbm(x / 22.0f, y / 22.0f) - 0.5f) * 0.45f;
            const f32 a = std::clamp((0.95f - r) / 0.5f, 0.0f, 1.0f);
            const f32 soot = 0.04f + 0.08f * fbm(x / 9.0f, y / 9.0f) + 0.1f * std::clamp(r, 0.0f, 1.0f);
            put(p, x, y, soot * 1.1f, soot, soot * 0.9f, a * (0.75f + 0.25f * fbm(x / 5.0f, y / 5.0f)));
        }
    return p;
}

// A painted arrow pointing at the picture's top (the decal box's -Z), the paint worn in places.
Pixels arrow() {
    Pixels p(side * side * 4);
    for (i32 y = 0; y < side; ++y)
        for (i32 x = 0; x < side; ++x) {
            const f32 u = (x + 0.5f) / side - 0.5f, v = (y + 0.5f) / side; // v: 0 top, 1 bottom
            const bool head = v < 0.45f && std::abs(u) < (v - 0.05f) * 0.95f;
            const bool shaft = v >= 0.45f && v < 0.95f && std::abs(u) < 0.13f;
            const f32 wear = std::clamp((fbm(x / 7.0f, y / 7.0f) - 0.3f) * 3.0f, 0.0f, 1.0f);
            put(p, x, y, 0.95f, 0.72f, 0.08f, (head || shaft) ? 0.92f * wear : 0.0f);
        }
    return p;
}

// Diagonal hazard stripes, chipped at the edges.
Pixels stripes() {
    Pixels p(side * side * 4);
    for (i32 y = 0; y < side; ++y)
        for (i32 x = 0; x < side; ++x) {
            const bool yellow = static_cast<i32>(std::floor((x + y) / 64.0f)) % 2 == 0;
            const f32 chip = std::clamp((fbm(x / 11.0f, y / 11.0f) - 0.25f) * 4.0f, 0.0f, 1.0f);
            if (yellow)
                put(p, x, y, 0.9f, 0.7f, 0.05f, chip);
            else
                put(p, x, y, 0.03f, 0.03f, 0.03f, chip);
        }
    return p;
}

// A bullet hole's colour (a dark pit in a pale ring of crushed concrete) and its normal map (a crater: the rim up, the
// pit down), green up.
std::pair<Pixels, Pixels> bullet_hole() {
    Pixels colour(side * side * 4), normals(side * side * 4);
    const auto height = [](f32 r) {
        return r < 0.2f ? -0.6f + r : r < 0.55f ? 0.25f * std::sin((r - 0.2f) / 0.35f * pi) : 0.0f;
    };
    for (i32 y = 0; y < side; ++y)
        for (i32 x = 0; x < side; ++x) {
            const f32 u = (x + 0.5f) / side * 2 - 1, v = (y + 0.5f) / side * 2 - 1;
            const f32 r = std::sqrt(u * u + v * v) + (noise(x / 6.0f, y / 6.0f) - 0.5f) * 0.12f;
            const f32 pit = r < 0.2f ? 1.0f : 0.0f;
            const f32 ring = std::clamp((0.7f - r) / 0.3f, 0.0f, 1.0f);
            const f32 shade = 0.55f + 0.15f * noise(x / 3.0f, y / 3.0f);
            put(colour, x, y, pit > 0 ? 0.05f : shade, pit > 0 ? 0.05f : shade * 0.97f, pit > 0 ? 0.05f : shade * 0.92f,
                std::max(pit, ring * 0.85f));
            // The slope of the height across the picture (x right, y down: green up is minus the down slope).
            const f32 e = 2.0f / side;
            const f32 ru = std::sqrt((u + e) * (u + e) + v * v), rv = std::sqrt(u * u + (v + e) * (v + e));
            const f32 dx = (height(ru) - height(r)) / e * 0.08f, dy = (height(rv) - height(r)) / e * 0.08f;
            const f32 n = 1.0f / std::sqrt(dx * dx + dy * dy + 1.0f);
            put(normals, x, y, 0.5f - 0.5f * dx * n, 0.5f + 0.5f * dy * n, 0.5f + 0.5f * n, 1.0f);
        }
    return {std::move(colour), std::move(normals)};
}

// --- The jelly sentinel: a sphere and three shapes to blend toward ---

MeshData3D sentinel() {
    MeshData3D data = meshes::sphere(0.55f, 48, 24);
    MorphTarget3D squash{"squash", {}, {}}, spikes{"spikes", {}, {}}, breathe{"breathe", {}, {}};
    for (const MeshVertex3D& v : data.vertices) {
        const Vec3f p = v.position, n = v.normal;
        // Squashed: half as tall, a third wider, its base kept on the ground (the sphere's bottom at -0.55).
        squash.positions.push_back({p.x * 0.35f, -(p.y + 0.55f) * 0.45f, p.z * 0.35f});
        // Spikes: tufts pushed out along the normal where a pattern of bumps peaks.
        const f32 bumps = std::max(0.0f, std::sin(n.x * 9.0f) * std::sin(n.y * 9.0f) * std::sin(n.z * 9.0f));
        spikes.positions.push_back(n * (0.35f * bumps * bumps));
        breathe.positions.push_back(n * 0.05f);
    }
    // The normals turn with the squash (an ellipsoid 1.35 wide and 0.55 tall: its normals are the sphere's divided by
    // those); the spikes and the breath keep the sphere's.
    for (const MeshVertex3D& v : data.vertices)
        squash.normals.push_back(normalize(Vec3f{v.normal.x / 1.35f, v.normal.y / 0.55f, v.normal.z / 1.35f}) -
                                 v.normal);
    data.morph_targets = {std::move(squash), std::move(spikes), std::move(breathe)};
    return data;
}

class Bunker final : public Scene {
public:
    explicit Bunker(Options o) : _o(std::move(o)), _walk(_o.start), _auto(!_o.fixed_exposure) {}
    std::string_view name() const override { return "Bunker exit"; }

    void update(SceneContext& ctx) override {
        if (!_r)
            setup(ctx);
        const auto& in = ctx.input;
        if (in.pressed(Key::Escape))
            ctx.app.quit();
        if (in.pressed(Key::E)) {
            _auto = !_auto;
            _view = {}; // made again with the new options
        }
        if (in.pressed(Key::D))
            _decals = !_decals;
        if (in.pressed(Key::Space))
            _paused = !_paused;
        _time += ctx.dt;
        if (!_paused)
            _walk += ctx.dt;
        animate_sentinels();
    }

    void render(SceneContext& ctx) override {
        if (!_r)
            return;
        const auto size = ctx.renderer.output_size();
        if (!_view.valid()) {
            ViewOptions3D options{.samples = 4, .hdr = true, .exposure = _auto ? 1.0f : 1.6f, .bloom = 0.04f};
            options.auto_exposure = {.enabled = _auto, .min_ev = -3.0f, .max_ev = 5.0f, .centre_weight = 0.4f};
            _view = _r->create_view(size, options).value_or({});
        } else if (size != _size) {
            (void)_r->resize(_view, size);
        }
        _size = size;

        _scene.clear();
        _scene.time = _time;
        place_camera();
        draw_bunker();
        if (_decals)
            draw_decals();
        for (std::size_t i = 0; i < _sentinels.size(); ++i) {
            const Sentinel& s = _sentinels[i];
            (void)_scene.draw(s.mesh, Mat4f::translation(s.position + Vec3f{0, 0.55f + s.hop, 0}), _jelly);
        }
        _scene.add_point_light(
            {.position = {0, 2.55f, -8.5f}, .color = {2.4f, 1.6f, 0.9f}, .radius = 9, .size = 0.15f});
        _scene.add_point_light(
            {.position = {2.8f, 2.4f, -3.0f}, .color = {0.9f, 0.6f, 0.35f}, .radius = 5, .size = 0.1f});

        if (!_r->render(_view, _scene)) {
            if (ctx.report)
                ctx.report->fail("3D render failed");
            ctx.app.quit();
            return;
        }
        auto& r = ctx.renderer;
        r.clear(Color::rgb(0, 0, 0));
        r.draw_texture(_view.texture(), {0, 0, f32(size.x), f32(size.y)});
        if (_o.shot.empty()) {
            std::string exposure = _auto ? "on" : "off";
            if (const auto adapted = _r->exposure(_view); _auto && adapted) {
                char text[32];
                std::snprintf(text, sizeof(text), "on (x%.2f)", *adapted);
                exposure = text;
            }
            const std::string line = "[E] auto exposure: " + exposure + "   [D] decals: " + (_decals ? "on" : "off") +
                                     "   [Space] " + (_paused ? "walk" : "pause");
            ui2::draw_text(r, line, {16, 14}, 2, Color::rgb(235, 235, 235));
        }
        if (!_o.shot.empty() && ++_frame == _o.frames) {
            Image image;
            if (!r.read_rgba({0, 0, f32(size.x), f32(size.y)}, image.rgba, image.size) || !save_image(image, _o.shot))
                throw std::runtime_error("cannot write " + _o.shot.string());
            std::cout << "bunker_exit: wrote " << _o.shot.string() << '\n';
            ctx.app.quit();
        }
    }

private:
    struct Sentinel {
        Mesh3D mesh;
        Vec3f position;
        f32 phase = 0, hop = 0;
    };

    Texture colour_texture(const Pixels& p) {
        Texture t = _r->renderer().create_texture_from_rgba(p.data(), side, side);
        _r->renderer().set_scale_mode(t, ScaleMode::Mipmapped);
        return t;
    }
    Texture data_texture(const Pixels& p) {
        Texture t = _r->renderer().create_texture({side, side}, TextureFormat::Rgba8, p.data());
        _r->renderer().set_scale_mode(t, ScaleMode::Mipmapped);
        return t;
    }

    void setup(SceneContext& ctx) {
        _r = ctx.renderer3d;
        if (!_r) { // (a headless run: no shared 3D renderer)
            _own_renderer = std::make_unique<Renderer3D>(ctx.renderer);
            _r = _own_renderer.get();
        }
        if (!_r->available())
            throw std::runtime_error("needs the GPU 3D renderer (for --shot: KIN_RENDER_BACKEND=gpu)");
        const auto caps = _r->capabilities();
        if (!caps.decals || !caps.auto_exposure || !caps.morph_targets)
            std::cerr << "bunker_exit: this device lacks" << (caps.decals ? "" : " decals")
                      << (caps.auto_exposure ? "" : " auto-exposure") << (caps.morph_targets ? "" : " morph-targets")
                      << "; the scene is drawn without them\n";
        const auto mesh = [&](const MeshData3D& data) {
            auto m = _r->create_mesh(data);
            if (!m)
                throw std::runtime_error("cannot make a mesh: " + diagnostics3d_text(m.diagnostics));
            return *m;
        };
        _box = mesh(meshes::box({1, 1, 1}));
        _ground = mesh(meshes::plane({80, 80}, 8));
        auto first = _r->create_mesh(sentinel());
        if (!first)
            throw std::runtime_error("cannot make the sentinel: " + diagnostics3d_text(first.diagnostics));
        _sentinels.push_back({*first, {1.8f, 0, -2.5f}, 0.0f});
        auto second = _r->copy_mesh(*first);
        _sentinels.push_back({second ? *second : *first, {-3.5f, 0, 4.0f}, 1.7f});

        _scorch = colour_texture(scorch());
        _arrow = colour_texture(arrow());
        _stripes = colour_texture(stripes());
        auto [hole, hole_normals] = bullet_hole();
        _hole = colour_texture(hole);
        _hole_normals = data_texture(hole_normals);

        _concrete = {.color = Color::rgb(150, 146, 138), .roughness = 0.9f};
        _grass = {.color = Color::rgb(92, 110, 70), .roughness = 0.95f};
        _jelly = {.color = Color::rgb(70, 200, 180), .emissive = 0.15f, .roughness = 0.25f};
        _jelly.receives_decals = false; // they bounce through the scorch marks unmarked

        _scene.sky = {.enabled = true, .turbidity = 2.5f, .sun_intensity = 6.0f};
        _scene.lights.sun = {.direction = {0.35f, 0.55f, 1.0f}, .color = {5.2f, 4.8f, 4.2f}};
        // Sky light from above, a little bounce from the ground; no global illumination, so the bunker gets it too (its
        // own occlusion darkens the corners).
        _scene.lights.ambient = {0.10f, 0.12f, 0.16f};
        _scene.lights.ground_ambient = {.enabled = true, .color = {0.05f, 0.05f, 0.04f}};
        _scene.lights.shadow_area = {{-12, -1, -12}, {12, 5, 12}};
        _scene.lights.shadow_resolution = 2048;
        _scene.ambient_occlusion.enabled = true;
        _scene.ambient_occlusion.radius = 0.8f;
    }

    // In from the back of the bunker (z -8) through the door (z 0) to the yard (z 7), turning to look back, and in
    // again: a cosine back and forth, `walk_period` seconds round.
    void place_camera() {
        const f32 s = 0.5f - 0.5f * std::cos(2 * pi * _walk / walk_period); // 0 inside, 1 outside
        const Vec3f eye{0.6f * std::sin(_walk * 0.4f), 1.6f, -7.5f + 15.0f * s};
        const f32 turn = std::clamp((s - 0.75f) / 0.25f, 0.0f, 1.0f); // looks back toward the door at the end
        const Vec3f ahead{0, 1.3f, eye.z + 6.0f}, back{0, 1.4f, 0.0f};
        const Vec3f target = ahead + (back - ahead) * (turn * turn * (3 - 2 * turn));
        _scene.camera = Camera3D::look_at(eye, target, 60);
        _scene.camera.z_near = 0.05f;
        _scene.camera.z_far = 200;
    }

    void slab(Vec3f center, Vec3f size, const Material3D& material) {
        (void)_scene.draw(_box, Mat4f::trs(center, Quatf::identity(), size), material);
    }

    // The room: 8 wide, 3 tall, 10 deep (z -10 to 0), walls 0.4 thick, a door 3 wide and 2.5 tall in its front wall.
    void draw_bunker() {
        (void)_scene.draw(_ground, Mat4f::identity(), _grass);
        slab({0, 0.02f, -5}, {8.8f, 0.04f, 10.8f}, _concrete);    // the floor, a little above the yard
        slab({0, 3.2f, -5}, {8.8f, 0.4f, 10.8f}, _concrete);      // the ceiling
        slab({-4.2f, 1.5f, -5}, {0.4f, 3.0f, 10.8f}, _concrete);  // left
        slab({4.2f, 1.5f, -5}, {0.4f, 3.0f, 10.8f}, _concrete);   // right
        slab({0, 1.5f, -10.2f}, {8.8f, 3.0f, 0.4f}, _concrete);   // back
        slab({-2.9f, 1.5f, 0.2f}, {2.6f, 3.0f, 0.4f}, _concrete); // front, left of the door
        slab({2.9f, 1.5f, 0.2f}, {2.6f, 3.0f, 0.4f}, _concrete);  // and right
        slab({0, 2.75f, 0.2f}, {3.2f, 0.5f, 0.4f}, _concrete);    // the lintel
        // A crate or two to throw shadows.
        slab({-2.6f, 0.45f, -7.5f}, {1.1f, 0.9f, 1.1f},
             Material3D{.color = Color::rgb(120, 90, 60), .roughness = 0.8f});
        slab({5.5f, 0.6f, 3.5f}, {1.2f, 1.2f, 1.2f}, Material3D{.color = Color::rgb(120, 90, 60), .roughness = 0.8f});
    }

    // A decal of `size` (the picture's width, the box's depth, the picture's height) at `at`, turned by `rotation`
    // (identity: onto the floor from above, the picture's top toward -Z).
    void decal(Vec3f at, Vec3f size, Quatf rotation, const Texture& texture, Color color = colors::white,
               const Texture& normal = {}) {
        Decal3D d{.transform = Mat4f::trs(at, rotation, size), .texture = texture, .normal = normal, .color = color};
        (void)_scene.draw_decal(d);
    }

    void draw_decals() {
        const Quatf toward_door = Quatf::axis_angle({0, 1, 0}, 180); // its top toward +Z
        const Quatf wall_front = Quatf::axis_angle({1, 0, 0}, 90);   // onto a wall facing +Z (projected along -Z)
        // Scorch marks, inside and in the yard (one across the threshold).
        decal({-1.2f, 0, -6.0f}, {2.6f, 1.0f, 2.6f}, Quatf::axis_angle({0, 1, 0}, 30), _scorch);
        decal({2.0f, 0, -2.2f}, {1.6f, 1.0f, 1.6f}, Quatf::axis_angle({0, 1, 0}, 110), _scorch);
        decal({0.6f, 0, 0.5f}, {3.4f, 1.2f, 3.4f}, Quatf::axis_angle({0, 1, 0}, 200), _scorch);
        decal({-4.0f, 0, 5.5f}, {3.0f, 1.0f, 3.0f}, Quatf::axis_angle({0, 1, 0}, 75), _scorch);
        // Arrows painted on the floor toward the door, and one on the back wall.
        for (int i = 0; i < 3; ++i)
            decal({0, 0, -8.0f + 2.4f * f32(i)}, {1.0f, 0.6f, 1.6f}, toward_door, _arrow);
        decal({0, 1.6f, -10.0f}, {1.2f, 0.6f, 1.8f}, wall_front, _arrow, Color::rgba(255, 255, 255, 200));
        // Hazard stripes on the door's outer frame, and bullet holes round it.
        decal({-1.85f, 1.25f, 0.4f}, {0.5f, 0.4f, 2.5f}, wall_front, _stripes);
        decal({1.85f, 1.25f, 0.4f}, {0.5f, 0.4f, 2.5f}, wall_front, _stripes);
        const std::array<Vec2f, 7> holes{
            {{-2.3f, 1.9f}, {-2.6f, 1.3f}, {-2.1f, 1.0f}, {2.4f, 2.1f}, {2.2f, 1.6f}, {2.9f, 1.4f}, {-0.4f, 2.8f}}};
        for (std::size_t i = 0; i < holes.size(); ++i)
            decal({holes[i].x, holes[i].y, 0.4f}, {0.22f, 0.3f, 0.22f},
                  wall_front * Quatf::axis_angle({0, 1, 0}, 47.0f * f32(i)), _hole, colors::white, _hole_normals);
    }

    // Bounce (squash as they land), breathe, and bristle when the camera is near.
    void animate_sentinels() {
        const Vec3f eye = _scene.camera.position;
        for (Sentinel& s : _sentinels) {
            const f32 t = _time * 1.6f + s.phase;
            const f32 cycle = t - std::floor(t);  // one hop a cycle
            const f32 air = std::sin(cycle * pi); // 0 on the ground, 1 at the top
            s.hop = 0.6f * air * air;
            const f32 squash = std::max(0.0f, 1.0f - air * 4.0f); // only near the ground
            const f32 near = std::clamp((3.5f - length(eye - s.position)) / 2.0f, 0.0f, 1.0f);
            const f32 breathe = 0.5f + 0.5f * std::sin(_time * 3.0f + s.phase);
            const std::array<f32, 3> weights{squash * 0.9f, near, breathe};
            if (const Status3D status = _r->set_morph_weights(s.mesh, weights); !status && !_morph_warned) {
                _morph_warned = true;
                std::cerr << "bunker_exit: " << diagnostics3d_text(status.diagnostics) << '\n';
            }
        }
    }

    Options _o;
    Renderer3D* _r = nullptr;
    std::unique_ptr<Renderer3D> _own_renderer;
    View3D _view;
    Vec2i _size{};
    Scene3D _scene;
    Mesh3D _box, _ground;
    std::vector<Sentinel> _sentinels;
    Texture _scorch, _arrow, _stripes, _hole, _hole_normals;
    Material3D _concrete, _grass, _jelly;
    f32 _time = 0, _walk = 0;
    int _frame = 0;
    bool _auto = true, _decals = true, _paused = false, _morph_warned = false;
};

} // namespace

int main(int argc, char** argv) {
    try {
        Options o;
        std::vector<char*> runtime{argv[0]};
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc)
                    throw std::runtime_error(a + " needs a value");
                return argv[++i];
            };
            if (a == "--shot")
                o.shot = value();
            else if (a == "--frames")
                o.frames = std::stoi(value());
            else if (a == "--start")
                o.start = std::stof(value());
            else if (a == "--fixed-exposure")
                o.fixed_exposure = true;
            else
                runtime.push_back(argv[i]);
        }
        auto headless = parse_headless_options(int(runtime.size()), runtime.data());
        if (!o.shot.empty()) {
            headless.enabled = true;
            headless.render = true; // on the GPU
            headless.frames = o.frames + 1;
        }
        SceneManager scenes;
        scenes.push(std::make_unique<Bunker>(o));
        return run_scene_app({.window = {.title = "Bunker exit", .width = 1280, .height = 720, .max_fps = 60},
                              .headless = headless,
                              .render_headless = true,
                              .rendering_3d = true},
                             scenes);
    } catch (const std::exception& e) {
        std::cerr << "bunker_exit: " << e.what() << '\n';
        return 1;
    }
}
