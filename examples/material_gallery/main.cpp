// material_gallery: a ring of pedestals, each showing a material imported from another engine (Unreal, here: free
// Fab packs) by kin_import_material --graphs, on a stone floor under a procedural sky. The materials are not in the
// repository (their licence allows using them in a project, not redistributing them); import your own and point the
// gallery at the folder:
//
//   material_gallery --content <kin_import_material --content folder> [--shot picture.png] [--exhibit N]
//
// --shot renders headless and writes a picture: with KIN_RENDER_BACKEND=gpu (headless runs draw in software otherwise).
//
// The exhibits are named below by their path in that folder; any that is missing is left out, said once.
// Graphs without an up-to-date cooked file are cooked beside their sources first, on worker threads (Assets3D would
// otherwise run glslc for each on the render thread); the materials then load one a frame behind a progress screen.
// Mouse drag: orbit. Wheel: zoom. Left / Right (A / D): previous / next exhibit. Up (W): the whole ring.
// Space: tour on or off. Esc: quit.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <kin/assets/image.hpp>
#include <kin/core/json_value.hpp>
#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/diagnostics3d.hpp>
#include <kin/render3d/material_graph3d.hpp>
#include <kin/render3d/mesh3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/timeofday3d.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/ui2/text.hpp>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace kin;
namespace fs = std::filesystem;

enum class Shape { Sphere, Box, Torus, Cylinder };

// How much of a material's Unreal graph kin drew: all of it; a graph with some outputs left to their standard value
// (a node kin has no counterpart for); or the standard material only (the graph could not be used at all).
enum class Status { Ready, Partial, Fallback };

struct ExhibitDesc {
    const char* label;
    const char* path; // in the content folder
    Shape shape;
    Status status;
    const char* note; // what kin did not draw as Unreal does (kin_import_material's notes, shortened)
};

// Free Fab packs, imported from their Unreal project (out/fab/run.sh in the material-graph worktree), grouped by how
// far the import got.
constexpr ExhibitDesc exhibits[] = {
    {"Wood 3", "m/WoodMaterial3/Materials/MI_Wood3.kinmaterials", Shape::Box, Status::Ready,
     "the whole graph: tint, grunge, roughness and normal controls"},
    {"Wood 7", "m/WoodMaterial3/Materials/MI_Wood7.kinmaterials", Shape::Cylinder, Status::Ready,
     "the same master material, another instance"},
    {"River rock", "m/WaterMaterials/Materials/M_River_Rock.kinmaterials", Shape::Sphere, Status::Ready,
     "the whole graph"},
    {"Riverbank", "m/WaterMaterials/Materials/M_Riverbank.kinmaterials", Shape::Box, Status::Ready, "the whole graph"},
    {"Zombie skin", "m/ZombiSkinMaterial/Material/MI_ZombieSkin_Inst.kinmaterials", Shape::Sphere, Status::Ready,
     "the whole graph, with 16-bit maps"},
    {"Toon PBR, rough metal", "m/StylizedToon/Materials/Demo/MI_Test_R05_M1.kinmaterials", Shape::Torus, Status::Ready,
     "the whole graph"},
    {"Chrome", "m/MWLandscapeAutoMaterial/Materials/MASTER/Basic/MTL_MW_Basic_Chrome.kinmaterials", Shape::Torus,
     Status::Ready, "the whole graph"},
    {"Colour checker", "m/MWLandscapeAutoMaterial/Materials/MASTER/Basic/MTL_MW_Basic_ColorChecker.kinmaterials",
     Shape::Box, Status::Ready, "the whole graph"},
    {"Pixel water", "m/WaterMaterials/Materials/M_Water_Pixelated.kinmaterials", Shape::Sphere, Status::Ready,
     "the whole graph, animated by time"},

    {"Stone tiles 3", "m/Materials_StoneTiles/Materials/Instances/MI_StoneTiles_03.kinmaterials", Shape::Box,
     Status::Partial, "parallax is Custom HLSL: every output falls back, untextured"},
    {"Fancy grid, azure", "m/FancyDev/Materials/MI_fancy_grid_azure.kinmaterials", Shape::Box, Status::Partial,
     "clear coat: lit as the default model"},
    {"Sand", "m/WaterMaterials/Materials/M_Sand.kinmaterials", Shape::Sphere, Status::Partial,
     "Object Position: colour and roughness fall back, untextured"},
    {"Rapids", "m/WaterMaterials/Materials/M_Rapids.kinmaterials", Shape::Sphere, Status::Partial,
     "Scene Depth, Depth Fade: colour and opacity fall back"},

    {"Ocean", "m/WaterMaterials/Materials/M_Ocean.kinmaterials", Shape::Sphere, Status::Fallback,
     "over five textures: standard material only, untextured"},
    {"Ocean, cheaper", "m/WaterMaterials/Materials/M_Ocean_Cheaper.kinmaterials", Shape::Sphere, Status::Fallback,
     "over 64 floats of parameters: standard material only"},
    {"Pond scum", "m/WaterMaterials/Materials/M_PondScum.kinmaterials", Shape::Box, Status::Fallback,
     "over five textures: standard material only, untextured"},
    {"Island landscape", "m/MWLandscapeAutoMaterial/Materials/Landscape/MTL_MWAM_Landscape_IslandExample.kinmaterials",
     Shape::Box, Status::Fallback, "landscape material, over five textures: standard only"},
};
constexpr const char* floor_material = "m/WoodMaterial3/Materials/MI_Wood5.kinmaterials";
constexpr const char* pedestal_material =
    "m/MWLandscapeAutoMaterial/Materials/MASTER/Basic/MTL_MW_Basic_Gray.kinmaterials";

constexpr f32 floor_size = 120.0f, ring_radius = 6.5f, pedestal_height = 0.9f, pi = std::numbers::pi_v<f32>;

struct Options {
    fs::path content, shot;
    int exhibit = -1; // -1: the ring
    int frames = 90;  // before the shot
};

struct Exhibit {
    std::string label, pack, note;
    Status status = Status::Ready;
    Vec3f position{};
    Mesh3D mesh;
    Material3D material;
};

f32 approach(f32 value, f32 target, f32 rate, f32 dt) {
    return value + (target - value) * (1 - std::exp(-rate * dt));
}

// "m/WoodMaterial3/..." -> "WoodMaterial3" (with spaces for underscores: the HUD's font has none).
std::string pack_of(std::string_view path) {
    if (path.starts_with("m/"))
        path.remove_prefix(2);
    std::string pack(path.substr(0, path.find('/')));
    std::replace(pack.begin(), pack.end(), '_', ' ');
    return pack;
}

Color status_color(Status s) {
    return s == Status::Ready     ? Color::rgb(90, 220, 130)
           : s == Status::Partial ? Color::rgb(250, 190, 70)
                                  : Color::rgb(240, 95, 80);
}

const char* status_name(Status s) {
    return s == Status::Ready ? "READY" : s == Status::Partial ? "PARTIAL" : "FALLBACK";
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// The "graph" members of a parsed .kinmaterials, at any depth (a file's base material, its parts', its variants').
void graphs_named(const JsonValue& v, std::vector<std::string>& out) {
    for (const auto& [key, member] : v.members())
        if (key == "graph" && member.is_string())
            out.push_back(member.as_string());
        else
            graphs_named(member, out);
    for (const auto& item : v.items())
        graphs_named(item, out);
}

// Graphs cooked beside their sources (<graph>.spv, as kin_material_graph cook writes it) on worker threads: Assets3D
// otherwise compiles an uncooked graph with glslc as it loads it, seconds of the render thread each. A graph that will
// not cook is left as it is (Assets3D then says why).
class Cooker {
public:
    ~Cooker() {
        _stop = true;
        for (auto& t : _threads)
            t.join();
    }
    void start(std::vector<fs::path> graphs) {
        _graphs = std::move(graphs);
        const unsigned workers = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
        for (unsigned i = 0; i < workers && i < _graphs.size(); ++i)
            _threads.emplace_back([this] {
                for (size_t g; !_stop && (g = _next++) < _graphs.size(); ++_done)
                    cook(_graphs[g]);
            });
    }
    size_t total() const { return _graphs.size(); }
    size_t done() const { return std::min(_done.load(), _graphs.size()); }
    bool finished() const { return done() == total(); }

private:
    static void cook(const fs::path& graph) {
        std::string why;
        const auto cooked = cook_material_graph3d(read_file(graph), &why);
        if (!cooked) {
            std::cerr << "material_gallery: " << graph.string() << " does not cook: " << why << '\n';
            return;
        }
        const auto bytes = pack_cooked_material_graph3d(*cooked);
        const fs::path part = graph.string() + ".spv.part"; // (renamed whole: never a half-written cooked file)
        {
            std::ofstream out(part, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            if (!out) {
                std::cerr << "material_gallery: cannot write " << part.string() << '\n';
                return;
            }
        }
        std::error_code ec;
        fs::rename(part, graph.string() + ".spv", ec);
    }

    std::vector<fs::path> _graphs;
    std::vector<std::thread> _threads;
    std::atomic<size_t> _next{0}, _done{0};
    std::atomic<bool> _stop{false};
};

class Gallery final : public Scene {
public:
    explicit Gallery(Options o) : _o(std::move(o)), _focus(_o.exhibit) {}
    std::string_view name() const override { return "Material gallery"; }

    void update(SceneContext& ctx) override {
        if (!_r)
            setup(ctx);
        const auto& in = ctx.input;
        if (in.pressed(Key::Escape))
            ctx.app.quit();
        if (_loading && !_cooker.finished()) {
            if (!_o.shot.empty()) // (a shot has no window to keep answering)
                while (!_cooker.finished())
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
            else
                return;
        }
        if (_loading) {
            // One material a frame, so the window keeps answering while graphs compile and maps decode (seconds
            // each when the graphs are not cooked); a shot has no window to keep alive, so it loads them all now.
            while (!load_step() && !_o.shot.empty()) {
            }
            return;
        }
        _time += ctx.dt;
        const int count = int(_exhibits.size());
        const auto step = [&](int by) {
            _tour = false;
            _focus = _focus < 0 ? (by > 0 ? 0 : count - 1) : (_focus + by + count) % count;
            _user_yaw = 0;
        };
        if (count > 0 && (in.pressed(Key::Right) || in.pressed(Key::D)))
            step(1);
        if (count > 0 && (in.pressed(Key::Left) || in.pressed(Key::A)))
            step(-1);
        if (in.pressed(Key::Up) || in.pressed(Key::W)) {
            _tour = false;
            _focus = -1;
        }
        if (in.pressed(Key::Space)) {
            _tour = !_tour;
            _tour_clock = 0;
        }
        if (_tour && count > 0 && (_tour_clock += ctx.dt) > 6) {
            _tour_clock = 0;
            _focus = (_focus + 1) % count;
            _user_yaw = 0;
        }
        for (const auto button : {MouseButton::Left, MouseButton::Right})
            if (in.mouse_held(button)) {
                const Vec2f d{in.mouse_pos().x - _last_mouse.x, in.mouse_pos().y - _last_mouse.y};
                if (_dragging) {
                    _user_yaw -= d.x * 0.006f;
                    _user_pitch = std::clamp(_user_pitch + d.y * 0.004f, -0.35f, 0.9f);
                }
                _dragging = true;
                break;
            } else if (button == MouseButton::Right)
                _dragging = false;
        _last_mouse = in.mouse_pos();
        _zoom = std::clamp(_zoom * std::pow(0.9f, in.frame_mouse_wheel_y()), 0.4f, 2.5f);

        // Where the camera wants to be: round the exhibit in focus (from outside the ring), or above the ring.
        Vec3f target{0, 0.6f, 0};
        f32 yaw = _time * 0.05f, distance = 15, pitch = 0.55f;
        if (_focus >= 0 && _focus < count) {
            const auto& e = _exhibits[size_t(_focus)];
            target = e.position + Vec3f{0, pedestal_height + 0.55f, 0};
            yaw = std::atan2(e.position.x, e.position.z);
            distance = 3.2f;
            pitch = 0.22f;
        }
        yaw += _user_yaw;
        // The shortest way round to the new yaw.
        f32 dy = std::remainder(yaw - _yaw, 2 * pi);
        _yaw = approach(_yaw, _yaw + dy, 3, ctx.dt);
        _target = lerp(_target, target, 1 - std::exp(-3 * ctx.dt));
        _distance = approach(_distance, distance * _zoom, 3, ctx.dt);
        _pitch = approach(_pitch, std::clamp(pitch + _user_pitch, 0.02f, 1.3f), 3, ctx.dt);
        if (_first) { // no flight in from nowhere
            _first = false;
            _yaw = yaw;
            _target = target;
            _distance = distance * _zoom;
            _pitch = pitch;
        }
    }

    void render(SceneContext& ctx) override {
        if (!_r)
            return;
        const auto size = ctx.renderer.output_size();
        if (_loading) {
            loading_screen(ctx, size);
            return;
        }
        if (!_view.valid())
            _view = _r->create_view(size, {.samples = 4, .hdr = true}).value_or({});
        else if (size != _size)
            (void)_r->resize(_view, size);
        _size = size;

        _scene.clear();
        _scene.time = _time;
        const Vec3f eye =
            _target +
            Vec3f{std::cos(_pitch) * std::sin(_yaw), std::sin(_pitch), std::cos(_pitch) * std::cos(_yaw)} * _distance;
        _scene.camera = Camera3D::look_at(eye, _target, 50);
        _scene.camera.z_far = 200;
        (void)_scene.draw(_floor_mesh, Mat4f::identity(), _floor);
        for (size_t i = 0; i < _exhibits.size(); ++i) {
            const auto& e = _exhibits[i];
            (void)_scene.draw(_pedestal_mesh, Mat4f::translation(e.position + Vec3f{0, pedestal_height / 2, 0}),
                              _pedestal);
            Material3D band{.color = status_color(e.status), .unlit = true, .casts_shadow = false};
            (void)_scene.draw(_band_mesh, Mat4f::translation(e.position + Vec3f{0, pedestal_height - 0.06f, 0}), band);
            const f32 spin = _time * 0.3f + f32(i);
            (void)_scene.draw(e.mesh,
                              Mat4f::trs(e.position + Vec3f{0, pedestal_height + 0.55f, 0},
                                         Quatf::axis_angle({0, 1, 0}, spin), {1, 1, 1}),
                              e.material);
        }
        if (!_r->render(_view, _scene)) {
            if (ctx.report)
                ctx.report->fail("3D render failed");
            ctx.app.quit();
            return;
        }
        ctx.renderer.clear(Color::rgb(0, 0, 0));
        ctx.renderer.draw_texture(_view.texture(), {0, 0, f32(size.x), f32(size.y)});
        hud(ctx, size);

        if (!_o.shot.empty() && ++_frame == _o.frames) {
            Image image;
            if (!ctx.renderer.read_rgba({0, 0, f32(size.x), f32(size.y)}, image.rgba, image.size) ||
                !save_image(image, _o.shot))
                throw std::runtime_error("cannot write " + _o.shot.string());
            std::cout << "material_gallery: wrote " << _o.shot.string() << '\n';
            ctx.app.quit();
        }
    }

private:
    void loading_screen(SceneContext& ctx, Vec2i size) {
        auto& r = ctx.renderer;
        const f32 w = f32(size.x), h = f32(size.y);
        const int total = int(std::size(exhibits)) + 2;
        r.clear(Color::rgb(14, 16, 20));
        ui2::draw_text(r, "Material gallery", {w / 2 - 192, h / 2 - 80}, 4, colors::white);
        std::string line;
        f32 progress = 0;
        if (!_cooker.finished()) {
            line = "Cooking graphs " + std::to_string(_cooker.done()) + " / " + std::to_string(_cooker.total()) +
                   "   once: they are kept beside their sources";
            progress = f32(_cooker.done()) / f32(_cooker.total());
        } else {
            line = "Loading " + std::to_string(_step + 1) + " / " + std::to_string(total) + "   " +
                   (_step < 2 ? std::string("floor and pedestals") : std::string(exhibits[_step - 2].label));
            progress = f32(_step) / f32(total);
        }
        ui2::draw_text(r, line, {w / 2 - 300, h / 2 - 10}, 2, Color::rgb(200, 205, 215));
        r.fill_rect({w / 2 - 300, h / 2 + 24, 600, 10}, Color::rgb(40, 44, 52));
        r.fill_rect({w / 2 - 300, h / 2 + 24, 600 * progress, 10}, Color::rgb(90, 220, 130));
    }

    void hud(SceneContext& ctx, Vec2i size) {
        auto& r = ctx.renderer;
        const f32 w = f32(size.x), h = f32(size.y);
        r.fill_rect({0, h - 44, w, 44}, Color::rgba(10, 12, 16, 200));
        ui2::draw_text(r, "Drag orbit   Wheel zoom   Left/Right exhibit   Up ring   Space tour   Esc quit",
                       {14, h - 32}, 2, Color::rgb(200, 205, 215));
        r.fill_rect({0, 0, w, 72}, Color::rgba(10, 12, 16, 200));
        if (_focus >= 0 && _focus < int(_exhibits.size())) {
            const auto& e = _exhibits[size_t(_focus)];
            const std::string count = std::to_string(_focus + 1) + " / " + std::to_string(_exhibits.size());
            ui2::draw_text(r, count, {14, 10}, 3, Color::rgb(170, 175, 185));
            ui2::draw_text(r, status_name(e.status), {130, 10}, 3, status_color(e.status));
            ui2::draw_text(r, e.label, {290, 10}, 3, colors::white);
            ui2::draw_text(r, e.note + "   " + e.pack, {14, 44}, 2, Color::rgb(185, 200, 210));
        } else {
            int counts[3] = {};
            for (const auto& e : _exhibits)
                ++counts[int(e.status)];
            ui2::draw_text(r, "Material gallery: Unreal materials from free Fab packs, in kin", {14, 10}, 3,
                           colors::white);
            constexpr const char* meaning[] = {": whole graph", ": some outputs fall back", ": standard material only"};
            f32 x = 14;
            for (const Status s : {Status::Ready, Status::Partial, Status::Fallback}) {
                const std::string text = std::to_string(counts[int(s)]) + " " + status_name(s) + meaning[int(s)];
                ui2::draw_text(r, text, {x, 44}, 2, status_color(s));
                x += 12 * f32(text.size()) + 36;
            }
        }
        if (_tour)
            ui2::draw_text(r, "TOUR", {w - 80, 12}, 2, Color::rgb(250, 210, 120));
    }

    // One .kinmaterials on a mesh: a one-part model for load_materials to bind to.
    bool bind(const char* path, const Mesh3D& mesh, Material3D& out) {
        if (!fs::exists(_o.content / path)) {
            std::cerr << "material_gallery: not found, left out: " << (_o.content / path).string() << '\n';
            return false;
        }
        auto model = std::make_shared<Model3D>();
        model->parts.push_back({.name = "part", .mesh = mesh});
        const auto binding = _assets->load_materials(path, model).value_or({});
        if (!binding) {
            std::cerr << "material_gallery: " << path << ": " << diagnostics3d_text(_assets->diagnostics()) << '\n';
            return false;
        }
        out = binding->materials.at(0);
        _models.push_back(model);
        _bindings.push_back(binding.shared());
        return true;
    }

    void setup(SceneContext& ctx) {
        _r = ctx.renderer3d;
        if (!_r) { // (a headless run: no shared 3D renderer)
            _own_renderer = std::make_unique<Renderer3D>(ctx.renderer);
            _r = _own_renderer.get();
        }
        if (!_r->available())
            throw std::runtime_error("needs the GPU 3D renderer (for --shot: KIN_RENDER_BACKEND=gpu)");
        _assets = std::make_unique<Assets3D>(*_r, _o.content);
        const auto mesh = [&](const MeshData3D& data) {
            auto m = _r->create_mesh(data);
            if (!m)
                throw std::runtime_error("cannot make a mesh");
            return *m;
        };
        _floor_mesh = mesh(meshes::plane({floor_size, floor_size}, 16));
        _pedestal_mesh = mesh(meshes::box({1.3f, pedestal_height, 1.3f}));
        _band_mesh = mesh(meshes::box({1.34f, 0.05f, 1.34f})); // the status, round the pedestal's top
        _shapes = {mesh(meshes::sphere(0.55f, 48, 24)), mesh(meshes::box({0.85f, 0.85f, 0.85f})),
                   mesh(meshes::torus(0.4f, 0.17f, 48, 24)), mesh(meshes::cylinder(0.42f, 0.95f, 40))};
        _loading = true;
        _cooker.start(uncooked_graphs());

        _scene.sky = {.enabled = true, .turbidity = 2.5f, .sun_intensity = 1.0f};
        _scene.lights.sky_lights_scene = true;
        apply({.hours = 15.5f, .day_of_year = 200, .latitude = 45}, _scene.lights, _scene.sky, _scene.fog);
        _scene.lights.shadow_area = {{-ring_radius - 3, -1, -ring_radius - 3}, {ring_radius + 3, 4, ring_radius + 3}};
        _scene.lights.shadow_resolution = 2048;
        _scene.fog.enabled = true;
        _scene.fog.density = 0.02f;
        _scene.fog.colour_from_sky = true;
    }

    // The graphs the gallery's materials use that have no cooked file at least as new as their source.
    std::vector<fs::path> uncooked_graphs() const {
        std::vector<std::string> named;
        const auto scan = [&](const char* path) {
            if (const auto parsed = parse_json(read_file(_o.content / path)); parsed.ok())
                graphs_named(*parsed.value, named);
        };
        scan(floor_material);
        scan(pedestal_material);
        for (const auto& d : exhibits)
            scan(d.path);
        std::sort(named.begin(), named.end());
        named.erase(std::unique(named.begin(), named.end()), named.end());
        std::vector<fs::path> uncooked;
        for (const auto& name : named) {
            const fs::path graph = _o.content / name, cooked = graph.string() + ".spv";
            std::error_code a, b;
            const auto source_time = fs::last_write_time(graph, a), cooked_time = fs::last_write_time(cooked, b);
            if (!a && (b || cooked_time < source_time))
                uncooked.push_back(graph);
        }
        return uncooked;
    }

    // Binds the next material (the floor's and pedestals' first, then the exhibits'); true when all are.
    bool load_step() {
        if (_step == 0) {
            if (bind(floor_material, _floor_mesh, _floor))
                _floor.uv_scale = {floor_size / 2.5f, floor_size / 2.5f}; // a plank pattern about 2.5 m across
            else
                _floor = {.color = Color::rgb(90, 90, 95), .roughness = 0.8f};
            _floor.wrap_u = _floor.wrap_v = Wrap3D::Repeat;
        } else if (_step == 1) {
            if (!bind(pedestal_material, _pedestal_mesh, _pedestal))
                _pedestal = {.color = Color::rgb(60, 60, 64), .roughness = 0.6f};
        } else if (_step - 2 < std::size(exhibits)) {
            const auto& d = exhibits[_step - 2];
            Exhibit e{.label = d.label,
                      .pack = pack_of(d.path),
                      .note = d.note,
                      .status = d.status,
                      .mesh = _shapes[size_t(d.shape)]};
            if (bind(d.path, e.mesh, e.material)) {
                // (what the import wrote decides: a graph is drawn, or not)
                if (!e.material.surface)
                    e.status = Status::Fallback;
                else if (e.status == Status::Fallback)
                    e.status = Status::Partial;
                _exhibits.push_back(std::move(e));
            }
        }
        if (++_step < std::size(exhibits) + 2)
            return false;
        for (size_t i = 0; i < _exhibits.size(); ++i) {
            const f32 a = 2 * pi * f32(i) / f32(_exhibits.size());
            _exhibits[i].position = {ring_radius * std::sin(a), 0, ring_radius * std::cos(a)};
        }
        if (_exhibits.empty())
            std::cerr << "material_gallery: no exhibits in " << _o.content.string()
                      << " (import some materials first: kin_import_material unreal ... --graphs)\n";
        if (_focus >= int(_exhibits.size()))
            _focus = -1;
        _loading = false;
        return true;
    }

    Options _o;
    Renderer3D* _r = nullptr;
    std::unique_ptr<Renderer3D> _own_renderer;
    std::unique_ptr<Assets3D> _assets;
    View3D _view;
    Vec2i _size{};
    Scene3D _scene;
    Mesh3D _floor_mesh, _pedestal_mesh, _band_mesh;
    std::array<Mesh3D, 4> _shapes; // by Shape
    Cooker _cooker;
    size_t _step = 0; // the next load_step()
    bool _loading = false;
    Material3D _floor, _pedestal;
    std::vector<Exhibit> _exhibits;
    std::vector<std::shared_ptr<Model3D>> _models;
    std::vector<std::shared_ptr<const MaterialBinding3D>> _bindings;
    int _focus = -1, _frame = 0;
    bool _tour = false, _dragging = false, _first = true;
    f32 _time = 0, _tour_clock = 0, _yaw = 0, _pitch = 0.5f, _distance = 15, _zoom = 1, _user_yaw = 0, _user_pitch = 0;
    Vec3f _target{};
    Vec2f _last_mouse{};
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
            if (a == "--content")
                o.content = value();
            else if (a == "--shot")
                o.shot = value();
            else if (a == "--exhibit")
                o.exhibit = std::stoi(value());
            else if (a == "--frames")
                o.frames = std::stoi(value());
            else
                runtime.push_back(argv[i]);
        }
        if (o.content.empty()) {
            std::cerr << "usage: material_gallery --content <folder of imported materials> [--shot picture.png] "
                         "[--exhibit N] [--frames N]\n";
            return 2;
        }
        auto headless = parse_headless_options(int(runtime.size()), runtime.data());
        if (!o.shot.empty()) {
            headless.enabled = true;
            headless.render = true; // on the GPU
            headless.frames = o.frames + 1;
        }
        SceneManager scenes;
        scenes.push(std::make_unique<Gallery>(o));
        return run_scene_app({.window = {.title = "Material gallery", .width = 1280, .height = 720, .max_fps = 60},
                              .headless = headless,
                              .render_headless = true,
                              .rendering_3d = true},
                             scenes);
    } catch (const std::exception& e) {
        std::cerr << "material_gallery: " << e.what() << '\n';
        return 1;
    }
}
