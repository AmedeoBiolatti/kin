// planar_atrium: an authored scene (content/scenes/atrium.kinscene) with mirrors and portals as entities (Mirror3D,
// Portal3D), run by a SceneSession3D, which renders their pictures; its materials are material graphs (marble,
// brushed brass with verdigris, a checkered marble floor, terrazzo, sandstone, glowing portal trim) and its motion a
// Lua script (content/scripts/atrium.lua: an orb flying through the portals by scene3d.portal_step).
//
//   planar_atrium [--camera N] [--shot picture.png] [--frames N] [--content folder]
//
// --camera: 0 orbits the atrium, 1 looks at the mirror, 2 at the pool, 3 into the west portal, 4 walks through the
// portals (the keys 0-4 too). --shot renders headless and writes a picture after --frames frames (with
// KIN_RENDER_BACKEND=gpu). The first run compiles the material graphs with glslc (some seconds). Esc: quit.
#include "planar_surfaces.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <kin/assets/image.hpp>
#include <kin/render3d/assets3d.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/planar_views3d.hpp>
#include <kin/scene/scene_session3d.hpp>
#include <memory>
#include <sol/sol.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace kin;

struct Options {
    std::filesystem::path content = ATRIUM_CONTENT, shot;
    int camera = 0;
    int frames = 90;
};

class Atrium final : public Scene {
public:
    explicit Atrium(Options o) : _o(std::move(o)), _camera(_o.camera) {}
    std::string_view name() const override { return "Atrium"; }

    void on_enter(SceneContext& ctx) override {
        _renderer = ctx.renderer3d;
        if (!_renderer) {
            _own = std::make_unique<Renderer3D>(ctx.renderer);
            _renderer = _own.get();
        }
        if (!_renderer->available())
            throw std::runtime_error("needs the GPU 3D renderer (for --shot: KIN_RENDER_BACKEND=gpu)");
        _assets = std::make_unique<Assets3D>(*_renderer, _o.content);
        const auto surfaces = create_planar_surfaces(*_renderer, planar_surfaces);
        require(surfaces.diagnostics);
        SceneSession3DOptions options;
        options.viewport = ctx.renderer.output_size();
        options.planar_surfaces = *surfaces;
        options.scripts.app = &ctx.app;
        options.scripts.input = &ctx.input;
        options.scripts.bind = [this](sol::state& lua) {
            _lua = &lua;
            lua["atrium_camera"] = _camera;
        };
        _session = std::make_unique<SceneSession3D>(*_renderer, *_assets, options);
        if (!_session->load("scenes/atrium.kinscene"))
            require(_session->diagnostics());
    }

    void update(SceneContext& ctx) override {
        if (ctx.input.pressed(Key::Escape))
            ctx.app.quit();
        const std::array keys{Key::Num0, Key::Num1, Key::Num2, Key::Num3, Key::Num4};
        for (int i = 0; i < int(keys.size()); ++i)
            if (ctx.input.pressed(keys[size_t(i)]) && _lua) {
                _camera = i;
                (*_lua)["atrium_camera"] = i;
            }
        if (!_session->step(ctx.dt))
            require(_session->diagnostics());
    }

    void render(SceneContext& ctx) override {
        const auto size = ctx.renderer.output_size();
        if (size != _size && !_session->resize(size))
            require(_session->diagnostics());
        _size = size;
        if (!_session->render(ctx.alpha))
            require(_session->diagnostics());
        ctx.renderer.clear();
        ctx.renderer.draw_texture(_session->view()->texture(), {0, 0, f32(size.x), f32(size.y)});
        if (!_o.shot.empty() && ++_frame == _o.frames) {
            Image image;
            if (!ctx.renderer.read_rgba({0, 0, f32(size.x), f32(size.y)}, image.rgba, image.size) ||
                !save_image(image, _o.shot))
                throw std::runtime_error("cannot write " + _o.shot.string());
            std::cout << "planar_atrium: wrote " << _o.shot.string() << '\n';
            ctx.app.quit();
        }
    }

    kin::EcsWorld* world() override { return _session ? _session->world() : nullptr; }
    void on_exit(SceneContext&) override {
        _session.reset();
        _assets.reset();
        _own.reset();
    }

private:
    static void require(const std::vector<std::string>& diagnostics) {
        if (diagnostics.empty())
            return;
        std::string text;
        for (const auto& line : diagnostics)
            text += line + '\n';
        throw std::runtime_error(text);
    }

    Options _o;
    Renderer3D* _renderer = nullptr;
    std::unique_ptr<Renderer3D> _own;
    std::unique_ptr<Assets3D> _assets;
    std::unique_ptr<SceneSession3D> _session;
    sol::state* _lua = nullptr;
    Vec2i _size{};
    int _camera = 0, _frame = 0;
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
            else if (a == "--content")
                o.content = value();
            else if (a == "--camera")
                o.camera = std::clamp(std::stoi(value()), 0, 4);
            else if (a == "--frames")
                o.frames = std::max(1, std::stoi(value()));
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
        scenes.push(std::make_unique<Atrium>(o));
        return run_scene_app({.window = {.title = "Atrium", .width = 1280, .height = 720, .max_fps = 60},
                              .headless = headless,
                              .render_headless = true,
                              .rendering_3d = true},
                             scenes);
    } catch (const std::exception& e) {
        std::cerr << "planar_atrium: " << e.what() << '\n';
        return 1;
    }
}
