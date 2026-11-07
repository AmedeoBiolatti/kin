// kin_material_lineup: the material lineup (tools/import_material/lineup/lineup.json) in kin, its materials imported
// from Godot or Unreal by kin_import_material --graphs, for comparing with those engines' own pictures.
//
//   kin_material_lineup --scene lineup.json --content <root> --materials <folder in it> --out picture.png
//   kin_material_lineup --scene snapshot.json --content <root> --meshes <folder> --material <path> --out picture.png
//
// <root> holds quad.glb and sphere.glb (make_assets.py; or --meshes, a folder of them anywhere) and
// <folder>/<Material>.kinmaterials. --material names the material of the objects whose material is "*" (a snapshot of
// one material: snapshot.json), a path in <folder> without .kinmaterials. As the other engines'
// scripts: an LDR view in the linear 2D pipeline (no tone curve, linear light encoded to sRGB), no MSAA, one sun
// without shadow, no ambient, a black background. --anisotropy N filters the materials' textures as Unreal's default
// does (8; kin's default is off).
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <kin/assets/image.hpp>
#include <kin/core/json_value.hpp>
#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/diagnostics3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/runtime/scene_app.hpp>
#include <map>
#include <memory>
#include <stdexcept>

namespace {
using namespace kin;
namespace fs = std::filesystem;

struct Options {
    fs::path scene, content, materials, meshes, out;
    std::string material; // the material of the objects whose material is "*"
    int frames = 8;
    f32 time = 0;       // the scene's clock (a material's time)
    u32 anisotropy = 1; // Scene3D::anisotropy (Unreal's default, r.MaxAnisotropy: 8)
};

Vec3f vec3(const JsonValue& v) {
    const auto& a = v.items();
    return {f32(a.at(0).as_number()), f32(a.at(1).as_number()), f32(a.at(2).as_number())};
}

class Lineup final : public Scene {
public:
    explicit Lineup(Options o) : _o(std::move(o)) {
        std::ifstream in(_o.scene);
        const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        auto parsed = parse_json(text);
        if (!parsed.ok())
            throw std::runtime_error("cannot read " + _o.scene.string());
        _doc = std::move(*parsed.value);
        const auto& size = _doc.find("size")->items();
        _width = int(size.at(0).as_number());
        _height = int(size.at(1).as_number());
    }
    int width() const { return _width; }
    int height() const { return _height; }
    std::string_view name() const override { return "Material lineup"; }

    void update(SceneContext& ctx) override {
        if (!_r)
            load(ctx);
        _scene.clear();
        _scene.time = _o.time;
        _scene.anisotropy = _o.anisotropy;
        for (const auto& d : _draws)
            (void)_scene.draw(d.mesh, d.transform, d.material);
        const auto& cam = *_doc.find("camera");
        _scene.camera =
            Camera3D::look_at(vec3(*cam.find("eye")), vec3(*cam.find("target")), f32(cam.number_at("fov_y")));
        auto& l = _scene.lights;
        const auto& sun = *_doc.find("sun");
        const f32 e = f32(sun.number_at("intensity", 1.0));
        l.sun = {.direction = vec3(*sun.find("toward")), .color = {e, e, e}};
        l.fill.color = {0, 0, 0};
        l.ambient = {0, 0, 0};
        l.shadow_area = {}; // no shadow
        if (!_r->render(_view, _scene))
            throw std::runtime_error("render failed");
        ctx.renderer.clear(Color::rgb(0, 0, 0));
        ctx.renderer.draw_texture(_view.texture(), {0, 0, f32(_width), f32(_height)});
        if (++_frame == _o.frames) {
            Image image;
            if (!ctx.renderer.read_rgba({0, 0, f32(_width), f32(_height)}, image.rgba, image.size))
                throw std::runtime_error("capture failed");
            if (!save_image(image, _o.out))
                throw std::runtime_error("cannot write " + _o.out.string());
            std::cout << "lineup written: " << _o.out.string() << '\n';
            ctx.app.quit();
        }
    }

private:
    struct Draw {
        Mesh3D mesh;
        Mat4f transform;
        Material3D material;
    };
    void load(SceneContext& ctx) {
        if (!ctx.renderer.set_color_space(ColorSpace::Linear))
            throw std::runtime_error("linear colour space unavailable");
        _r = std::make_unique<Renderer3D>(ctx.renderer);
        if (!_r->available())
            throw std::runtime_error("needs the GPU backend");
        _assets = std::make_unique<Assets3D>(*_r, _o.content);
        _view = _r->create_view({_width, _height}, {.samples = 1, .background = Color::rgb(0, 0, 0), .hdr = false})
                    .value_or({});
        for (const auto& o : _doc.find("objects")->items()) {
            const std::string mesh = o.find("mesh")->as_string();
            std::string name = o.find("material")->as_string();
            if (name == "*") {
                if (_o.material.empty())
                    throw std::runtime_error("the scene's \"*\" needs --material");
                name = _o.material;
            }
            auto model = _assets
                             ->load_model(_o.meshes.empty() ? fs::path(mesh + ".glb")
                                                            : fs::absolute(_o.meshes / (mesh + ".glb")))
                             .value_or({});
            if (!model)
                throw std::runtime_error(diagnostics3d_text(_assets->diagnostics()));
            const auto binding =
                _assets->load_materials(_o.materials / (name + ".kinmaterials"), model.shared()).value_or({});
            if (!binding)
                throw std::runtime_error(name + ": " + diagnostics3d_text(_assets->diagnostics()));
            const Mat4f at = Mat4f::translation(vec3(*o.find("position")));
            for (size_t i = 0; i < model->parts.size(); ++i) {
                Material3D m = binding->materials.at(i);
                m.casts_shadow = false;
                m.receives_shadow = false;
                _draws.push_back({model->parts[i].mesh, at * model->parts[i].transform, m});
            }
            _models.push_back(model.shared());
            _bindings.push_back(binding.shared());
        }
    }

    Options _o;
    JsonValue _doc;
    int _width = 0, _height = 0, _frame = 0;
    std::unique_ptr<Renderer3D> _r;
    std::unique_ptr<Assets3D> _assets;
    View3D _view;
    Scene3D _scene;
    std::vector<Draw> _draws;
    std::vector<std::shared_ptr<const Model3D>> _models;
    std::vector<std::shared_ptr<const MaterialBinding3D>> _bindings;
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
            if (a == "--scene")
                o.scene = value();
            else if (a == "--content")
                o.content = value();
            else if (a == "--materials")
                o.materials = value();
            else if (a == "--meshes")
                o.meshes = value();
            else if (a == "--material")
                o.material = value();
            else if (a == "--out")
                o.out = value();
            else if (a == "--time")
                o.time = std::stof(value());
            else if (a == "--frames")
                o.frames = std::stoi(value());
            else if (a == "--anisotropy")
                o.anisotropy = u32(std::stoi(value()));
            else
                runtime.push_back(argv[i]);
        }
        if (o.scene.empty() || o.content.empty() || o.out.empty() || o.frames < 1) {
            std::cerr << "usage: kin_material_lineup --scene lineup.json --content <root> --materials <folder> "
                         "--out picture.png [--meshes <folder>] [--material <path>] [--time s] [--frames n] "
                         "[--anisotropy n]\n";
            return 2;
        }
        auto lineup = std::make_unique<Lineup>(o);
        auto headless = parse_headless_options(int(runtime.size()), runtime.data());
        headless.enabled = true;
        headless.frames = o.frames + 1;
        const int w = lineup->width(), h = lineup->height();
        SceneManager scenes;
        scenes.push(std::move(lineup));
        return run_scene_app(
            {.window =
                 {.title = "Material lineup", .width = w, .height = h, .vsync = false, .high_pixel_density = false},
             .headless = headless,
             .render_headless = true},
            scenes);
    } catch (const std::exception& e) {
        std::cerr << "kin_material_lineup: " << e.what() << '\n';
        return 1;
    }
}
