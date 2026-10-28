#include "simulation.hpp"
#include "slowlight3d_shader.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <kin/assets/image.hpp>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/graph_resources.hpp>
#include <kin/ui2/context.hpp>
#include <stdexcept>
#include <string>
#include <vector>
using namespace kin;
namespace {
constexpr Vec2i window_size{1100, 700}, view_size{520, 480}, field_size{384, 192};
constexpr Rectf whole{0, 0, 1100, 700};
constexpr float extent = 10;
constexpr slowlight::Point eye{12, 14, 18};
void check(bool value, const char* why) {
    if (!value)
        throw std::runtime_error(why);
}
struct Options {
    bool test = false, help = false, delay = false;
    double time = 1.5, speed_kmh = 100;
    int frames = 0;
    std::filesystem::path capture;
};
Options parse(int argc, char** argv) {
    Options options;
    const auto number = [](const std::string& text) {
        std::size_t used = 0;
        const double value = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(value))
            throw std::runtime_error("expected a finite number");
        return value;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (++i == argc)
                throw std::runtime_error("missing value for " + arg);
            return argv[i];
        };
        if (arg == "--test")
            options.test = true;
        else if (arg == "--delay")
            options.delay = true;
        else if (arg == "--help")
            options.help = true;
        else if (arg == "--capture")
            options.capture = next();
        else if (arg == "--time")
            options.time = number(next());
        else if (arg == "--speed")
            options.speed_kmh = number(next());
        else if (arg == "--frames") {
            const double frames = number(next());
            check(frames >= 1 && frames <= 10000 && frames == std::floor(frames),
                  "frames must be an integer in [1, 10000]");
            options.frames = int(frames);
        } else
            throw std::runtime_error("unknown option: " + arg);
    }
    check(options.time >= 0 && options.time <= 1e9, "time must be in [0, 1e9] seconds");
    check(options.speed_kmh >= 25 && options.speed_kmh <= 400, "speed must be in [25, 400] km/h");
    return options;
}
class Prototype {
    Renderer2D& r;
    Renderer3D r3d{r};
    ResourceGraphDesc graph;
    ResourceGraphCompiler compiler;
    GraphResources resources;
    DataBuffer history_buffer;
    ComputeShaderHandle transport;
    MaterialShader3D receiver;
    ShaderHandle probe;
    MaterialInputs3D inputs;
    Texture bound_field;
    Mesh3D ground;
    ui2::Context ui;
    bool delay_view = false;
    ShaderParams parameters(const slowlight::History& history, double speed, slowlight::Point camera) const {
        ShaderParams p;
        p.uniforms[0] = float(speed);
        p.uniforms[1] = float(history.count);
        p.uniforms[2] = float(slowlight::step);
        p.uniforms[3] = extent;
        p.uniforms[4] = float(camera.x);
        p.uniforms[5] = float(camera.y);
        p.uniforms[6] = float(camera.z);
        return p;
    }
    void prepare(bool reference) {
        const auto& plan = compiler.compile(graph, available_graph_features(r),
                                            {.cull_unused = !reference, .reuse_slots = !reference});
        const std::array imports{GraphImport{.resource = 0, .buffer = history_buffer}};
        check(resources.prepare(r, graph, plan, imports), "slow-light graph allocation failed");
        if (bound_field != resources.texture(1)) {
            const std::array textures{resources.texture(1)};
            const std::array buffers{history_buffer};
            inputs = r3d.create_material_inputs(receiver, textures, buffers).value_or({});
            check(bool(inputs), "prototype material bindings rejected");
            bound_field = textures[0];
        }
    }
    bool dispatch(const slowlight::History& history, double speed, slowlight::Point camera) {
        const auto p = parameters(history, speed, camera);
        const std::array outputs{resources.texture(1)};
        const std::array buffers{history_buffer};
        return r.dispatch_compute(transport, field_size, {.buffers = buffers, .outputs = outputs, .params = &p});
    }
    void upload(const slowlight::History& history, double speed) {
        check(history.subluminal(speed), "prototype needs subluminal sampled emitter motion");
        // Upload before preparing/recording any material consumer. The bounded
        // buffer stays imported across frames; simulation/history belong here.
        check(r.update_data_buffer(history_buffer, 0, sizeof(history.samples), history.samples.data()),
              "history upload failed");
    }
    void hud(Input& input, const slowlight::History& history, double speed_kmh, bool paused) {
        ui.begin(input, r);
        const auto label = [&](Rectf bounds, std::string text, float scale, Color colour) {
            ui2::Label value{
                .bounds = bounds, .text = std::move(text), .text_style = {.scale = scale, .color = colour}};
            ui2::run(ui, value);
        };
        label({20, 14, 1060, 28}, "SLOW LIGHT / A CLASSICAL PROTOTYPE", 2, Color::rgb(230, 237, 246));
        char line[180];
        std::snprintf(line, sizeof(line), "%.0f km/h   |   10 metres = %.2f s   |   observer time %.2f s   %s",
                      speed_kmh, 36 / speed_kmh, history.time(), paused ? "PAUSED" : "");
        label({20, 48, 1060, 22}, line, 1.5f, Color::rgb(157, 174, 196));
        label({20, 85, 520, 22},
              delay_view ? "SLOW LIGHT / DELAY 0-2 s (BLUE-ORANGE)" : "SLOW LIGHT / DELAYED ILLUMINATION", 1.5f,
              Color::rgb(255, 203, 126));
        label({560, 85, 520, 22}, "INSTANTANEOUS / SAME OBSERVER TIME", 1.5f, Color::rgb(151, 214, 224));
        std::snprintf(line, sizeof(line), "History %.2f / 4.00 s. Pink stripes = unavailable past. Emitter now: %s.",
                      -history.oldest(), history.samples[history.count - 1].light[3] > 0 ? "ON" : "OFF");
        label({20, 607, 1060, 22}, line, 1.4f, Color::rgb(204, 178, 202));
        label({20, 635, 1060, 22}, "SPACE pause   UP/DOWN light speed   D delay map   R restart   ESC quit", 1.4f,
              Color::rgb(185, 198, 215));
        label({20, 663, 1060, 22},
              "1 m grid. Rings show current source (gold) / occluder (cyan). Classical direct-light prototype.", 1.3f,
              Color::rgb(126, 145, 166));
        ui.end();
    }

public:
    explicit Prototype(Renderer2D& renderer) : r(renderer) {
        const auto compute_bytes = slowlight3d_shader("transport.comp.spv");
        transport = r.create_compute_shader({compute_bytes.data(), u32(compute_bytes.size())});
        receiver = r3d.create_material_shader(slowlight3d_shader("receiver.frag.spv")).value_or({});
        const auto probe_bytes = slowlight3d_shader("probe.frag.spv");
        probe = r.create_shader({.spirv = {probe_bytes.data(), u32(probe_bytes.size())}, .num_samplers = 1});
        history_buffer = r.create_data_buffer(sizeof(slowlight::History::samples));
        ground = r3d.create_mesh(meshes::box({extent * 2, .1f, extent * 2})).value_or({});
        check(transport && receiver && probe && history_buffer.valid() && ground, "prototype resources unavailable");
        graph.resources = {{.name = "history",
                            .kind = GraphResourceKind::Buffer,
                            .bytes = sizeof(slowlight::History::samples),
                            .imported = true},
                           {.name = "transport",
                            .kind = GraphResourceKind::StorageTexture,
                            .format = TextureFormat::Rgba16Float,
                            .size = field_size},
                           {.name = "slow", .kind = GraphResourceKind::ColorTarget, .size = view_size},
                           {.name = "instant", .kind = GraphResourceKind::ColorTarget, .size = view_size},
                           {.name = "depth",
                            .kind = GraphResourceKind::RenderAttachment,
                            .format = TextureFormat::Depth32Float,
                            .size = view_size}};
        graph.passes = {
            {.name = "transport",
             .reads = {0},
             .writes = {1},
             .required_features = graph_feature::compute | graph_feature::buffers},
            {.name = "receivers",
             .reads = {0, 1},
             .writes = {2, 3, 4},
             .required_features = graph_feature::rendering3d | graph_feature::attachments},
            {.name = "present", .reads = {2, 3}, .required_features = graph_feature::raster, .side_effect = true}};
    }
    void toggle_delay() { delay_view = !delay_view; }
    void picture(const slowlight::History& history, double speed_kmh, bool reference = false) {
        r.clear(Color::rgb(11, 16, 24));
        prepare(reference);
        upload(history, speed_kmh / 3.6);
        const auto& plan = compiler.compile(graph, available_graph_features(r),
                                            {.cull_unused = !reference, .reuse_slots = !reference});
        const std::array<std::function<bool()>, 3> operations{
            [&] { return dispatch(history, speed_kmh / 3.6, eye); },
            [&] {
                for (int side = 0; side < 2; ++side) {
                    Scene3D scene;
                    scene.camera = Camera3D::look_at({float(eye.x), float(eye.y), float(eye.z)}, {});
                    Material3D material{.shader = receiver,
                                        .shader_params = {float(side), side == 0 && delay_view ? 1.f : 0.f,
                                                          float(history.count), extent},
                                        .inputs = inputs};
                    if (!scene.draw(ground, Mat4f::translation({0, -.05f, 0}), material))
                        return false;
                    const auto prepared = r3d.prepare(scene).value_or({});
                    if (!prepared || !r3d.render_surface({resources.target(u32(2 + side)), resources.texture(4)},
                                                         prepared, {.background = Color::rgb(17, 24, 35)}))
                        return false;
                }
                return true;
            },
            [&] {
                r.draw_texture(resources.texture(2), {20, 116, 520, 480});
                r.draw_texture(resources.texture(3), {560, 116, 520, 480});
                return true;
            }};
        check(execute_resource_graph(plan, operations), "prototype graph recording failed");
        check(r3d.debug_stats().device_waits == 0 && r3d.debug_stats().taa_dispatches == 0 &&
                  r3d.debug_stats().shadow_draws == 0,
              "prototype invoked implicit stock work or waited");
    }
    void frame(Input& input, const slowlight::History& history, double speed, bool paused) {
        picture(history, speed);
        hud(input, history, speed, paused);
    }
    void test() {
        // Compare GPU transport against the CPU model already checked against
        // analytic flight times and exact photon/moving-sphere intersections.
        const auto verify = [&](const slowlight::History& h, double speed, slowlight::Point camera) {
            r.clear(colors::black);
            prepare(false);
            upload(h, speed);
            check(dispatch(h, speed, camera), "transport dispatch failed");
            const auto target = r.create_render_target(field_size);
            std::vector<u8> pixels;
            Vec2i actual;
            {
                auto scope = r.scoped_render_target(target);
                r.clear(colors::black);
                r.draw_shader_surface({0, 0, float(field_size.x), float(field_size.y)}, probe, {},
                                      resources.texture(1));
                check(r.read_rgba({0, 0, float(field_size.x), float(field_size.y)}, pixels, actual) &&
                          actual == field_size,
                      "transport probe readback failed");
            }
            r.present();
            for (int side = 0; side < 2; ++side)
                for (int y = 4; y < field_size.y; y += 13)
                    for (int x = 4; x < field_size.x / 2; x += 11) {
                        const slowlight::Point receiver{((x + .5) / (field_size.x / 2) * 2 - 1) * extent, 0,
                                                        ((y + .5) / field_size.y * 2 - 1) * extent};
                        const auto expected = slowlight::evaluate(h, receiver, camera, speed, side == 1);
                        const std::array values{std::clamp(expected.energy / 8, 0.0, 1.0),
                                                std::clamp(expected.delay / 4, 0.0, 1.0),
                                                .5 * (double(expected.blocked) + double(expected.known)), 1.0};
                        const auto at = std::size_t(y * field_size.x + side * field_size.x / 2 + x) * 4;
                        for (int channel = 0; channel < 4; ++channel)
                            if (std::abs(int(pixels[at + channel]) - int(std::lround(values[channel] * 255))) > 2) {
                                std::cerr << "transport mismatch side=" << side << " at " << x << ',' << y
                                          << " channel=" << channel << " actual=" << int(pixels[at + channel])
                                          << " expected=" << values[channel] * 255 << '\n';
                                throw std::runtime_error("GPU/CPU transport mismatch");
                            }
                    }
        };
        slowlight::History history;
        for (double time : {0.0, 1.5, 8.0}) {
            history.seek(time, slowlight::demo);
            verify(history, slowlight::default_speed, eye);
        }
        verify(history, 25 / 3.6, eye); // bounded history must explicitly report unknown pixels
        for (double crossing : {1.5, 2.0}) {
            slowlight::History moving;
            moving.seek(2, [=](double t) {
                return slowlight::Sample{{0, 10, 0, 65}, {float(6 * (t - crossing)), 5, 0, .35f}};
            });
            verify(moving, 10, {});
        }
        slowlight::History despawned;
        despawned.seek(
            2, [](double t) { return slowlight::Sample{{0, 10, 0, 65}, {0, 5, 0, t >= 1.4 && t < 1.6 ? .35f : 0.f}}; });
        verify(despawned, 10, {});
        history = {};
        history.seek(1.5, slowlight::demo);
        picture(history, 100);
        std::vector<u8> a, b;
        Vec2i actual;
        check(r.read_rgba(whole, a, actual), "optimized comparison readback failed");
        r.present();
        picture(history, 100, true);
        check(r.read_rgba(whole, b, actual), "reference comparison readback failed");
        r.present();
        check(a == b, "optimized/reference prototype pixels differ");
        std::size_t different = 0;
        for (int y = 120; y < 590; ++y)
            for (int x = 20; x < 540; ++x)
                different +=
                    a[std::size_t(y * window_size.x + x) * 4] != a[std::size_t(y * window_size.x + x + 540) * 4];
        check(different > 1000, "slow and instantaneous comparison is not visible");
        picture(history, 100);
        r.present();
        const auto before = r.backend_stats();
        const auto compilations = compiler.stats().compilations;
        const auto field = resources.texture(1);
        for (int i = 0; i < 8; ++i) {
            history.seek(1.6 + i * slowlight::step, slowlight::demo);
            picture(history, i & 1 ? 50 : 100);
            r.present();
        }
        const auto after = r.backend_stats();
        check(after.gpu_memory.allocations - before.gpu_memory.allocations ==
                      after.upload_staging_allocations - before.upload_staging_allocations &&
                  after.upload_staging_bytes <= before.upload_staging_bytes,
              "stable prototype grew GPU resource/staging capacity");
        check(compiler.stats().compilations == compilations && resources.texture(1) == field,
              "stable prototype recompiled or replaced bindings");
        history.seek(0, slowlight::demo);
        picture(history, 100);
        r.present();
        std::cout << "Slow-light GPU: CPU transport parity, startup/history exhaustion, moving occlusion, exact graph "
                     "parity, visible comparison, stable resources, reset\n";
    }
};
} // namespace
int main(int argc, char** argv) {
    try {
        auto options = parse(argc, argv);
        if (options.help) {
            std::cout << "kin_slowlight3d [--test] [--delay] [--time seconds] [--speed km/h] [--capture file.png] "
                         "[--frames N]\n"
                         "Standalone classical direct-light prototype; SPACE pause, UP/DOWN speed, D delay, R reset, "
                         "ESC quit.\n";
            return 0;
        }
        App app{{.max_fps = 60}};
        auto& window = app.create_window({.title = "Kin slow-light prototype",
                                          .width = window_size.x,
                                          .height = window_size.y,
                                          .hidden = options.test || !options.capture.empty()});
        Renderer2D renderer{make_render_backend(window, false, true)};
        Renderer3D r3d{renderer};
        const auto features = available_graph_features(renderer);
        if (!r3d.capabilities().material_inputs || !r3d.capabilities().surface_pass ||
            !(features & graph_feature::compute) || !(features & graph_feature::buffers) ||
            !(features & graph_feature::attachments)) {
            std::cerr
                << "Slow-light prototype requires native GPU surface/material inputs, compute and data attachments\n";
            return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
        }
        renderer.set_logical_size(window_size);
        Prototype prototype{renderer};
        if (options.delay)
            prototype.toggle_delay();
        if (options.test)
            prototype.test();
        slowlight::History history;
        history.seek(options.time, slowlight::demo);
        if (!options.capture.empty()) {
#if KIN_ENABLE_IMAGE_IO
            prototype.frame(app.input(), history, options.speed_kmh, true);
            Image image;
            check(renderer.read_rgba(whole, image.rgba, image.size), "capture readback failed");
            check(save_image(image, options.capture), "capture save failed");
            renderer.present();
#else
            throw std::runtime_error("--capture requires KIN_ENABLE_IMAGE_IO");
#endif
        } else if (!options.test) {
            bool paused = false;
            int frames = 0;
            app.run(
                [&](f32 dt) {
                    const auto& input = app.input();
                    if (input.pressed(Key::Escape))
                        app.quit();
                    if (input.pressed(Key::Space))
                        paused = !paused;
                    if (input.pressed(Key::D))
                        prototype.toggle_delay();
                    if (input.pressed(Key::R))
                        options.time = 0;
                    constexpr std::array<double, 5> speeds{25, 50, 100, 200, 400};
                    if (input.pressed(Key::Up))
                        for (double speed : speeds)
                            if (speed > options.speed_kmh) {
                                options.speed_kmh = speed;
                                break;
                            }
                    if (input.pressed(Key::Down))
                        for (auto i = speeds.rbegin(); i != speeds.rend(); ++i)
                            if (*i < options.speed_kmh) {
                                options.speed_kmh = *i;
                                break;
                            }
                    if (!paused)
                        options.time += dt;
                    history.seek(options.time, slowlight::demo);
                },
                [&](f32) {
                    prototype.frame(app.input(), history, options.speed_kmh, paused);
                    renderer.present();
                    if (options.frames && ++frames >= options.frames)
                        app.quit();
                });
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
