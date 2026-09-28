#include <kin/renderer/render_graph.hpp>
#include <kin/renderer/render_profile.hpp>
#include <kin/renderer/static_texture_layer.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

class FakeTextureBackend final : public kin::ITextureBackend {
public:
    explicit FakeTextureBackend(kin::Vec2i size)
        : _size(size) {
    }

    kin::Vec2i size() const override { return _size; }

private:
    kin::Vec2i _size{};
};

class FakeBackend final : public kin::IRenderer2DBackend {
public:
    std::string_view name() const override { return "fake"; }
    void clear(kin::Color color) override {
        colors.push_back(color);
        commands.push_back("clear");
    }
    void present() override {}
    void set_logical_size(kin::Vec2i) override {}
    void set_integer_logical_size(kin::Vec2i) override {}
    kin::Vec2i output_size() const override { return {320, 180}; }
    kin::Vec2f window_to_logical(kin::Vec2f value) const override { return value; }
    kin::Vec2f logical_to_window(kin::Vec2f value) const override { return value; }
    kin::Texture create_texture_from_rgba(const kin::u8*, kin::Vec2i size) override {
        return kin::Texture{std::make_shared<FakeTextureBackend>(size)};
    }
    void draw_texture(const kin::Texture&, kin::Rectf dest) override {
        rects.push_back(dest);
        commands.push_back("texture");
    }
    void draw_texture(const kin::Texture&, kin::Rectf, kin::Rectf dest) override {
        rects.push_back(dest);
        commands.push_back("sprite");
    }
    void fill_rect(kin::Rectf rect, kin::Color color) override {
        rects.push_back(rect);
        colors.push_back(color);
        commands.push_back("fill");
    }
    void draw_rect(kin::Rectf rect, kin::Color color) override {
        rects.push_back(rect);
        colors.push_back(color);
        commands.push_back("rect");
    }
    void draw_line(kin::Vec2f, kin::Vec2f, kin::Color color) override {
        colors.push_back(color);
        commands.push_back("line");
    }
    void set_viewport(kin::Rectf) override {}
    void reset_viewport() override {}
    void push_viewport(kin::Rectf rect) override {
        rects.push_back(rect);
        commands.push_back("push");
    }
    void pop_viewport() override { commands.push_back("pop"); }

    std::vector<std::string> commands;
    std::vector<kin::Rectf> rects;
    std::vector<kin::Color> colors;
};

void test_render_queue_sorts_and_culls() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};

    kin::RenderQueue queue{kin::RenderSortMode::LayerThenY};
    queue.fill_rect({.layer = 1, .y = 20.0f, .use_y = true}, {20.0f, 20.0f, 4.0f, 4.0f}, kin::Color::rgb(20, 20, 20));
    queue.fill_rect({.layer = 1, .y = 10.0f, .use_y = true}, {10.0f, 10.0f, 4.0f, 4.0f}, kin::Color::rgb(10, 10, 10));
    queue.fill_rect({.layer = 0}, {-100.0f, -100.0f, 4.0f, 4.0f}, kin::Color::rgb(1, 1, 1));

    kin::RenderView view{.cull_rect = {0.0f, 0.0f, 40.0f, 40.0f}, .culling_enabled = true};
    queue.cull(view);
    queue.flush(renderer);

    assert(raw->rects.size() == 2);
    assert((raw->rects[0] == kin::Rectf{10.0f, 10.0f, 4.0f, 4.0f}));
    assert((raw->rects[1] == kin::Rectf{20.0f, 20.0f, 4.0f, 4.0f}));
}

void test_render_queue_pass_masks_and_text_command() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};

    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    queue.fill_rect({.pass_mask = kin::render_pass_mask::world}, {0.0f, 0.0f, 1.0f, 1.0f}, kin::Color::rgb(10, 10, 10));
    queue.fill_rect({.pass_mask = kin::render_pass_mask::ui}, {2.0f, 0.0f, 1.0f, 1.0f}, kin::Color::rgb(20, 20, 20));
    queue.draw_text({.pass_mask = kin::render_pass_mask::ui},
                    "KIN",
                    {4.0f, 0.0f},
                    {4.0f, 0.0f, 18.0f, 7.0f},
                    1.0f,
                    kin::colors::white,
                    [](kin::Renderer2D& renderer) {
                        renderer.draw_rect({4.0f, 0.0f, 18.0f, 7.0f}, kin::colors::white);
                    });

    queue.flush(renderer, kin::render_pass_mask::ui);

    assert(raw->commands.size() == 2);
    assert(raw->commands[0] == "fill");
    assert(raw->commands[1] == "rect");
    assert(raw->rects[0].x == 2.0f);
}

void test_render_graph_pass_toggles_and_stats() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::RenderQueue queue;
    kin::RenderGraph graph;
    int world_runs = 0;
    int debug_runs = 0;
    graph.passes().add({
        .id = {"world"},
        .run = [&](kin::RenderPassContext& ctx) {
            ++world_runs;
            ctx.queue.fill_rect({.layer = 0}, {0.0f, 0.0f, 1.0f, 1.0f}, kin::colors::white);
        },
    });
    graph.passes().add({
        .id = {"debug"},
        .name = "Debug",
        .enabled = false,
        .dependencies = {{"world"}},
        .run = [&](kin::RenderPassContext&) {
            ++debug_runs;
        },
    });

    graph.execute(renderer, queue);
    queue.flush(renderer);
    assert(world_runs == 1);
    assert(debug_runs == 0);
    assert(graph.passes().find("world")->stats.frames == 1);
    assert(raw->commands.size() == 1);

    assert(graph.passes().set_enabled("debug", true));
    graph.execute(renderer, queue);
    assert(debug_runs == 1);
}

void test_default_render_graph_flushes_pass_masks() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    kin::RenderGraph graph;
    graph.add_default_passes();

    kin::Camera2D camera;
    camera.offset = {100.0f, 0.0f};
    camera.set_viewport({20.0f, 20.0f});
    const kin::RenderView view{
        .projection = kin::RenderProjection::TopDown2D,
        .camera = &camera,
        .culling_enabled = true,
    };

    queue.fill_rect({.pass_mask = kin::render_pass_mask::world}, {0.0f, 0.0f, 4.0f, 4.0f}, kin::colors::white);
    queue.fill_rect({.pass_mask = kin::render_pass_mask::ui}, {0.0f, 0.0f, 4.0f, 4.0f}, kin::Color::rgb(20, 20, 20));

    graph.execute(renderer, queue, &view);

    assert(raw->commands.size() == 1);
    assert(raw->commands[0] == "fill");
    assert(raw->colors[0].r == 20);
}

void test_backend_capabilities_are_reported_by_facade() {
    auto backend = std::make_unique<FakeBackend>();
    kin::Renderer2D renderer{std::move(backend)};

    const kin::RendererBackendCapabilities caps = renderer.capabilities();
    assert(caps.immediate_2d);
    assert(caps.queued_2d);
    assert(!caps.rendering_3d);
}

void test_material_fallback_and_profiles() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::Material2D material{.tint = kin::Color::rgb(128, 255, 255)};
    kin::RenderQueue queue;
    queue.fill_rect({.layer = 0},
                    {0.0f, 0.0f, 2.0f, 2.0f},
                    kin::Color::rgb(200, 100, 50),
                    {.material = &material});
    queue.flush(renderer);
    assert(raw->colors.back().r == 100);
    assert(raw->colors.back().g == 100);
    assert(raw->colors.back().b == 50);

    kin::RenderGraph graph;
    const kin::RenderProfile top_down = kin::top_down_2d_profile();
    top_down.install(graph);
    assert(graph.passes().find("world") != nullptr);
    kin::Camera2D camera;
    camera.offset = {10.0f, 20.0f};
    camera.set_viewport({100.0f, 80.0f});
    const kin::RenderView view = top_down.make_view(&camera);
    assert(view.projection == kin::RenderProjection::TopDown2D);
    assert(kin::render_view_visible(view, {10.0f, 20.0f, 1.0f, 1.0f}));
}

void test_queue_flush_applies_camera_view() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::Camera2D camera;
    camera.offset = {10.0f, 20.0f};
    camera.set_viewport({100.0f, 100.0f});
    const kin::RenderView view{
        .projection = kin::RenderProjection::TopDown2D,
        .camera = &camera,
        .culling_enabled = true,
    };

    kin::RenderQueue queue;
    queue.fill_rect({.layer = 0}, {15.0f, 25.0f, 4.0f, 4.0f}, kin::colors::white);
    queue.draw_line({.layer = 1}, {10.0f, 20.0f}, {12.0f, 24.0f}, kin::colors::white);
    queue.flush(renderer, view);

    const kin::Rectf expected_rect{5.0f, 5.0f, 4.0f, 4.0f};
    assert(raw->rects[0] == expected_rect);
    assert(raw->commands[1] == "line");
}

void test_rgba_canvas_blits_sprite_from_catalog_pixels() {
    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{2, 1})};
    kin::SpriteCatalog catalog;
    catalog.set_texture("atlas", texture);
    catalog.add({
        .id = "red",
        .texture_id = "atlas",
        .source = {0.0f, 0.0f, 1.0f, 1.0f},
    });
    catalog.add({
        .id = "half",
        .texture_id = "atlas",
        .source = {1.0f, 0.0f, 1.0f, 1.0f},
    });

    const std::vector<kin::u8> atlas{
        200, 10, 20, 255,
        20, 200, 40, 128,
    };
    kin::RgbaCanvas canvas{{2, 1}};
    assert(canvas.blit_sprite(catalog, "red", atlas, {2, 1}, {0, 0}));
    assert(canvas.blit_sprite(catalog, "half", atlas, {2, 1}, {0, 0}));
    assert(canvas.has_visible_pixels());

    const std::vector<kin::u8>& pixels = canvas.pixels();
    assert(pixels[3] == 255);
    assert(pixels[0] < 200);
    assert(pixels[1] > 10);
    assert(pixels[4 + 3] == 0);
}

} // namespace

int main() {
    test_render_queue_sorts_and_culls();
    test_render_queue_pass_masks_and_text_command();
    test_render_graph_pass_toggles_and_stats();
    test_default_render_graph_flushes_pass_masks();
    test_backend_capabilities_are_reported_by_facade();
    test_material_fallback_and_profiles();
    test_queue_flush_applies_camera_view();
    test_rgba_canvas_blits_sprite_from_catalog_pixels();
    return 0;
}
