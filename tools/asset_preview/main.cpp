#include "asset_preview_shader.hpp"
#include "options.hpp"

#include <cstdio>
#include <iostream>
#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/diagnostics3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/ui2/context.hpp>
#include <memory>

namespace kin::asset_preview {
namespace {
class Preview final : public Scene {
    Options _options;
    std::unique_ptr<Renderer3D> _renderer;
    std::unique_ptr<Assets3D> _assets;
    std::shared_ptr<const Model3D> _model;
    std::shared_ptr<const MaterialBinding3D> _binding;
    std::vector<Material3D> _materials;
    std::vector<Pose3D> _poses;
    std::vector<std::vector<Mat4f>> _joints;
    MaterialShader3D _inspect;
    View3D _view;
    Scene3D _scene;
    Mesh3D _box;
    ui2::Context _ui;
    Vec3f _offset{};
    float _radius = 1, _zoom = 1, _elapsed = 0;
    bool _frozen = false;
    bool _diagnostics_active = false;
    int _material_mode = -1, _reloads = 0;
    std::string _error;
    Vec2i pixels(Renderer2D& r) const {
        auto native = r.scoped_native_coordinates();
        return r.output_size();
    }
    bool reload() {
        try {
            const auto path = _options.model.is_absolute() ? _options.model : _options.root / _options.model;
            const auto size = content_file_size(path);
            if (!size || *size > 32u * 1024u * 1024u)
                throw std::runtime_error("model missing or exceeds the preview's 32 MiB source limit");
            _assets->clear(); // old handles remain live until the candidate is valid
            auto candidate = _assets->load_model(_options.model);
            if (!candidate)
                throw std::runtime_error(diagnostics3d_text(_assets->diagnostics()));
            if (candidate->parts.size() > 256 || candidate->skeletons.size() > 16)
                throw std::runtime_error("preview supports at most 256 parts and 16 skeletons");
            const auto bounds = candidate->bounds;
            const float radius = bounds.radius();
            if (!std::isfinite(radius) || radius < 1e-6f || radius > 100000)
                throw std::runtime_error("model needs finite, nondegenerate bounds");
            if (_options.part >= int(candidate->parts.size()))
                throw std::runtime_error("selected part is outside the model");
            if (!_options.clip.empty() && !std::any_of(candidate->clips.begin(), candidate->clips.end(),
                                                       [&](const auto& clip) { return clip.name == _options.clip; }))
                throw std::runtime_error("animation clip not found: " + _options.clip);
            auto sidecar = _options.materials;
            if (sidecar.empty()) {
                sidecar = _options.model;
                sidecar.replace_extension(".kinmaterials");
                const auto full = sidecar.is_absolute() ? sidecar : _options.root / sidecar;
                if (!content_file_exists(full))
                    sidecar.clear();
            }
            std::shared_ptr<const MaterialBinding3D> binding;
            if (!sidecar.empty()) {
                auto loaded = _assets->load_materials(sidecar, candidate.shared(), _options.variant);
                if (!loaded)
                    throw std::runtime_error(diagnostics3d_text(_assets->diagnostics()));
                binding = loaded.shared();
            } else if (!_options.variant.empty())
                throw std::runtime_error("a material variant requires a .kinmaterials sidecar");
            std::vector<Pose3D> poses;
            std::vector<std::vector<Mat4f>> joints;
            for (const auto& skeleton : candidate->skeletons) {
                poses.push_back(skeleton.rest_pose());
                joints.emplace_back();
                compute_joint_matrices(skeleton, poses.back(), joints.back());
            }
            // Commit only after all import, binding and pose checks succeed.
            _model = candidate.shared();
            _binding = std::move(binding);
            _poses = std::move(poses);
            _joints = std::move(joints);
            _radius = radius;
            const auto centre = bounds.center();
            _offset = {-centre.x, -bounds.min.y, -centre.z};
            _material_mode = -1;
            _error.clear();
            ++_reloads;
            _view.reset_history();
            return true;
        } catch (const std::exception& error) {
            _error = error.what();
            std::cerr << "Asset preview reload: " << _error << '\n';
            return false;
        }
    }
    void prepare_materials() {
        if (_material_mode == _options.mode)
            return;
        _materials.clear();
        _materials.reserve(_model->parts.size());
        for (size_t i = 0; i < _model->parts.size(); ++i) {
            auto material = _binding ? _binding->materials[i] : _model->parts[i].material;
            if (_options.mode == 7) {
                material = Material3D{
                    .color = Color::rgb(160, 230, 215), .unlit = true, .casts_shadow = false, .wireframe = true};
            } else if (_options.mode) {
                if (!_inspect)
                    throw std::runtime_error("inspection shader unavailable; rebuild with glslc");
                material.shader = _inspect;
                material.shader_params.fill(0);
                material.shader_params[0] = float(_options.mode);
                if (_options.mode != 1)
                    material.blend = material.blend == Blend3D::Mask ? Blend3D::Mask : Blend3D::Opaque;
                material.casts_shadow = false;
            }
            _materials.push_back(std::move(material));
        }
        _material_mode = _options.mode;
    }
    void hud(SceneContext& ctx) {
        if (!_options.hud && _error.empty())
            return;
        const auto size = ctx.renderer.output_size();
        const auto& stats = _renderer->stats();
        const float width = std::min(700.f, float(size.x) - 16);
        _ui.begin(ctx.input, ctx.renderer);
        ui2::Panel panel{.bounds = {8, 8, width, 112}, .color = Color::rgba(15, 20, 28, 225)};
        ui2::run(_ui, panel);
        const auto label = [&](float y, std::string text, Color color = colors::white) {
            ui2::Label item{.bounds = {18, y, width - 20, 20},
                            .text = std::move(text),
                            .text_style = {.scale = 1.5f, .color = color},
                            .overflow = ui2::TextOverflow::Ellipsis};
            ui2::run(_ui, item);
        };
        label(12, _options.model.filename().string() + "   " + std::string(modes[_options.mode]) + "   " +
                      (_frozen ? "frozen" : "live") + "   F1 stats");
        label(33, "RMB orbit / MMB pan / wheel zoom / HOME frame / TAB mode / . part");
        label(54, "R reload / B bounds / G floor / S sun / A AO / O ortho / SPACE freeze");
        char line[160];
        std::snprintf(line, sizeof(line), "%u draws / %llu triangles / CPU record %.3f ms / feature targets %.2f MiB",
                      stats.draws, static_cast<unsigned long long>(stats.triangles), stats.cpu_ms,
                      double(stats.feature_bytes) / (1024 * 1024));
        label(75, line, Color::rgb(155, 225, 205));
        const auto dimensions = _model->bounds.size();
        std::snprintf(line, sizeof(line), "Authored units: %.3g x %.3g x %.3g / %zu parts / part %d / H hide HUD",
                      dimensions.x, dimensions.y, dimensions.z, _model->parts.size(), _options.part);
        label(96, _error.empty() ? line : _error,
              _error.empty() ? Color::rgb(175, 185, 205) : Color::rgb(255, 160, 120));
        _ui.end();
    }
    Vec3f _pan{};

public:
    explicit Preview(Options options) : _options(std::move(options)) {}
    std::string_view name() const override { return "Asset preview"; }
    void on_enter(SceneContext& ctx) override {
        _renderer = std::make_unique<Renderer3D>(ctx.renderer);
        if (!_renderer->available())
            throw std::runtime_error("asset preview requires the GPU backend");
        _assets = std::make_unique<Assets3D>(
            *_renderer, _options.root, Assets3D::Limits{.models = 2, .textures = 128, .materials = 16, .shaders = 16});
        _view = _renderer->create_view(pixels(ctx.renderer), {.samples = 2});
        if (!_view)
            throw std::runtime_error("preview render target could not be created");
        const auto shader = asset_preview_shader("inspect.frag.spv");
        if (!shader.empty())
            _inspect = _renderer->create_material_shader(shader);
        _box = _renderer->create_mesh(meshes::box({1, 1, 1}));
        if (!reload())
            throw std::runtime_error(_error);
        if (_options.inspect) {
            JsonWriter json(std::cout);
            json.begin_object();
            write_report(json);
            json.end_object();
            std::cout << '\n';
        }
    }
    void on_exit(SceneContext&) override {
        _scene.clear();
        _materials.clear();
        _binding.reset();
        _model.reset();
        _view = {};
        _box = {};
        _inspect = {};
        _assets.reset();
        _renderer.reset();
    }
    void collect_actions(InputActionContext& actions) const override {
        for (const auto* action : {"mode", "bounds", "ground", "shadows", "ao", "projection", "freeze", "reload",
                                   "home", "part", "hud", "quit"})
            actions.add("preview." + std::string(action), action);
    }
    void update(SceneContext& ctx) override {
        const auto pressed = [&](const char* name) { return ctx.input.pressed("preview." + std::string(name)); };
        if (pressed("quit"))
            ctx.app.quit();
        if (pressed("mode"))
            _options.mode = (_options.mode + 1) % int(std::size(modes));
        if (pressed("bounds"))
            _options.bounds = !_options.bounds;
        if (pressed("ground"))
            _options.ground = !_options.ground;
        if (pressed("shadows"))
            _options.shadows = !_options.shadows;
        if (pressed("ao"))
            _options.ao = !_options.ao;
        if (pressed("projection")) {
            _options.orthographic = !_options.orthographic;
            _view.reset_history();
        }
        if (pressed("freeze"))
            _frozen = !_frozen;
        if (pressed("hud"))
            _options.hud = !_options.hud;
        if (pressed("part"))
            _options.part = (_options.part + 2) % int(_model->parts.size() + 1) - 1;
        if (pressed("home")) {
            _zoom = 1;
            _pan = {};
            _options.yaw = 35;
            _options.pitch = 25;
            _elapsed = 0;
        }
        if (pressed("reload"))
            reload();
        if (!_frozen)
            _elapsed += ctx.dt;
    }
    void render(SceneContext& ctx) override {
        auto& renderer = ctx.renderer;
        renderer.clear(Color::rgb(24, 29, 38));
        const auto size = pixels(renderer);
        if (size.x < 1 || size.y < 1)
            return;
        if (size != _view.size() && !_renderer->resize(_view, size))
            throw std::runtime_error("preview resize failed");
        if (!_frozen) {
            const auto delta = ctx.input.frame_mouse_motion();
            if (ctx.input.mouse_held(MouseButton::Right)) {
                _options.yaw -= delta.x * .3f;
                _options.pitch = std::clamp(_options.pitch + delta.y * .3f, -85.f, 85.f);
            }
            if (ctx.input.mouse_held(MouseButton::Middle))
                _pan += (_scene.camera.right() * (-delta.x) + _scene.camera.up() * delta.y) * (_radius * _zoom * .002f);
            _zoom = std::clamp(_zoom * std::exp(-ctx.input.frame_mouse_wheel_y() * .12f), .15f, 20.f);
        }
        prepare_materials();
        _scene.clear();
        const float fit = 1 / std::min(1.f, float(size.x) / size.y);
        const Vec3f target = Vec3f{0, _model->bounds.size().y * .5f, 0} + _pan;
        _scene.camera = Camera3D::orbit(target, _options.yaw + _elapsed * _options.turntable, _options.pitch,
                                        _radius * 3.2f * _zoom * fit, 45);
        _scene.camera.z_near = std::max(_radius * .001f, 1e-6f);
        _scene.camera.z_far = std::max(_radius * 100, length(_scene.camera.position) + _radius * 10);
        if (_options.orthographic) {
            _scene.camera.projection = Projection3D::Orthographic;
            _scene.camera.height = _radius * 2.5f * _zoom * fit;
        }
        _scene.lights.sun.direction = {-.4f, 1, .5f};
        _scene.lights.sun.color = {1.1f, 1.05f, 1};
        _scene.lights.ambient = {.3f, .35f, .42f};
        _scene.lights.shadow_resolution = 2048;
        _scene.lights.shadow_cascades = 1;
        _scene.lights.shadow_area = {{-_radius * 2, -_radius * .05f, -_radius * 2},
                                     {_radius * 2, _radius * 2.5f, _radius * 2}};
        if (!_options.shadows || _options.mode != 0)
            _scene.lights.shadow_area = {};
        _scene.ambient_occlusion.enabled = _options.ao && _options.mode == 0;
        _scene.ambient_occlusion.half_resolution = true;
        _scene.ambient_occlusion.radius = _radius * .12f;
        for (size_t i = 0; i < _model->skeletons.size(); ++i) {
            if (const auto* clip = _model->find_clip(_options.clip, u32(i)); clip && !_options.clip.empty())
                sample_clip(*clip, _model->skeletons[i], _options.time + _elapsed, true, _poses[i]);
            compute_joint_matrices(_model->skeletons[i], _poses[i], _joints[i]);
        }
        const auto placement = Mat4f::translation(_offset);
        for (size_t i = 0; i < _model->parts.size(); ++i) {
            if (_options.part >= 0 && int(i) != _options.part)
                continue;
            const auto& part = _model->parts[i];
            const auto transform = placement * part.transform;
            if (part.skin >= 0)
                _scene.draw_skinned(part.mesh, _joints.at(size_t(part.skin)), transform, _materials[i]);
            else
                _scene.draw(part.mesh, transform, _materials[i]);
        }
        if (_options.ground)
            _scene.draw(_box,
                        Mat4f::translation({0, -_radius * .012f, 0}) *
                            Mat4f::scaling({_radius * 4, _radius * .02f, _radius * 4}),
                        {.color = Color::rgb(95, 101, 109), .roughness = .85f});
        if (_options.bounds) {
            const auto bounds = _model->bounds;
            _scene.draw(_box, Mat4f::translation(bounds.center() + _offset) * Mat4f::scaling(bounds.size()),
                        {.color = Color::rgb(100, 240, 170), .unlit = true, .casts_shadow = false, .wireframe = true});
        }
        if (!_renderer->render(_view, _scene))
            throw std::runtime_error("asset preview render failed");
        _diagnostics_active = ctx.debug_options && bool(ctx.debug_options->record_status);
        record_render3d_diagnostics(ctx.debug_options, _renderer->stats(), _view.size());
        const auto logical = renderer.output_size();
        renderer.draw_texture(_view.texture(), {0, 0, float(logical.x), float(logical.y)});
        hud(ctx);
    }
    void write_report(JsonWriter& json) const override {
        json.field("model", _options.model.generic_string())
            .field("mode", modes[_options.mode])
            .field("part", _options.part)
            .field("reloads", _reloads)
            .field("reload_error", _error)
            .field("frozen", _frozen)
            .field("bounds", _options.bounds)
            .field("ground", _options.ground)
            .field("shadows", _options.shadows)
            .field("ao", _options.ao)
            .field("orthographic", _options.orthographic)
            .field("hud", _options.hud);
        json.field("diagnostics_active", _diagnostics_active).field("time", _options.time + _elapsed);
        if (!_model)
            return;
        json.field("width", _view.size().x)
            .field("height", _view.size().y)
            .field("source_parts", u64(_model->parts.size()))
            .field("skeletons", u64(_model->skeletons.size()))
            .field("clips", u64(_model->clips.size()))
            .field("material_binding", bool(_binding));
        const auto& stats = _renderer->stats();
        json.field("draws", stats.draws)
            .field("triangles", stats.triangles)
            .field("feature_bytes", stats.feature_bytes);
        const auto bounds = _model->bounds;
        json.key("bounds_min").begin_array().value(bounds.min.x).value(bounds.min.y).value(bounds.min.z).end_array();
        json.key("bounds_max").begin_array().value(bounds.max.x).value(bounds.max.y).value(bounds.max.z).end_array();
        json.key("parts").begin_array();
        for (size_t i = 0; i < _model->parts.size(); ++i) {
            const auto& part = _model->parts[i];
            const auto& m = _binding ? _binding->materials[i] : part.material;
            json.begin_object()
                .field("index", u64(i))
                .field("name", part.name)
                .field("triangles", part.mesh.triangle_count())
                .field("textured", bool(m.texture))
                .field("normal_map", bool(m.normal))
                .field("mr_map", bool(m.metallic_roughness))
                .field("metallic", m.metallic)
                .field("roughness", m.roughness)
                .key("base_srgba")
                .begin_array()
                .value(u32(m.color.r))
                .value(u32(m.color.g))
                .value(u32(m.color.b))
                .value(u32(m.color.a))
                .end_array()
                .end_object();
        }
        json.end_array().key("diagnostics").begin_array();
        for (const auto& diagnostic : _model->asset_diagnostics)
            json.value(diagnostic);
        json.end_array();
    }
};
} // namespace
} // namespace kin::asset_preview

int main(int argc, char** argv) {
    try {
        auto options = kin::asset_preview::parse(argc, argv);
        if (options.help) {
            std::cout << "kin_asset_preview --model FILE [--root ASSET_ROOT] [--materials FILE] [--variant NAME]\n"
                         "  --mode lit|base|normals|uv|metallic|roughness|alpha|wireframe\n"
                         "  --part N --clip NAME --time SECONDS --turntable DEGREES_PER_SECOND\n"
                         "  --bounds --hud --ao --ortho --no-ground --no-shadows --inspect\n"
                         "  --width 960 --height 640 --yaw 35 --pitch 25\n"
                         "  Standard Kin --headless --frames=N --screenshot=FILE --report=FILE --server --profile\n"
                         "  F1 runtime diagnostics; F2 timing capture; H preview HUD\n";
            return 0;
        }
        const auto headless = kin::parse_headless_options(int(options.runtime.size()), options.runtime.data());
        kin::WindowedAppConfig window{.title = "Kin asset preview",
                                      .width = options.width,
                                      .height = options.height,
                                      .max_fps = 60,
                                      .input_map = kin::asset_preview::input_map()};
        kin::SceneManager scenes;
        scenes.push(std::make_unique<kin::asset_preview::Preview>(std::move(options)));
        return kin::run_scene_app({.window = window, .headless = headless, .render_headless = true}, scenes);
    } catch (const std::exception& error) {
        std::cerr << "Asset preview: " << error.what() << '\n';
        return 1;
    }
}
