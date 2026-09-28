#include <kin/platform/app.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/post_blur.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/sprite_catalog.hpp>
#include <kin/renderer/sprite_sheet.hpp>

#include <SDL3_image/SDL_image.h>

#include <cassert>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

bool has_log_event(const std::vector<kin::LogEvent>& events,
                   std::string_view category,
                   std::string_view message) {
    return std::ranges::any_of(events, [&](const kin::LogEvent& event) {
        return event.category == category && event.message == message;
    });
}

bool gpu_tests_required() {
    const char* value = std::getenv("KIN_REQUIRE_GPU_TESTS");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

void skip_or_require_gpu_test(std::string_view test_name, const std::string& reason) {
    if (gpu_tests_required()) {
        throw std::runtime_error(std::string(test_name) + ": GPU test unavailable: " + reason);
    }
    std::cerr << "[SKIP] " << test_name << ": " << reason
              << " (set KIN_REQUIRE_GPU_TESTS=1 to require GPU coverage)\n";
}

std::unique_ptr<kin::Renderer2D> try_create_gpu_renderer(kin::Window& window, std::string& unavailable_reason) {
    try {
        std::unique_ptr<kin::IRenderer2DBackend> backend = kin::make_render_backend(window, false, true);
        if (!backend || backend->name() != "SDL_GPU") {
            unavailable_reason = "SDL_GPU backend was not selected";
            return {};
        }
        return std::make_unique<kin::Renderer2D>(std::move(backend));
    } catch (const std::exception& e) {
        unavailable_reason = e.what();
    } catch (...) {
        unavailable_reason = "unknown exception while creating SDL_GPU backend";
    }
    return {};
}

bool pixel_near(const std::vector<kin::u8>& px, kin::Vec2i size, int x, int y, kin::Color color, int tolerance = 4) {
    if (x < 0 || y < 0 || x >= size.x || y >= size.y || px.empty()) {
        return false;
    }
    const std::size_t idx =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.x) + static_cast<std::size_t>(x)) * 4u;
    const auto near = [tolerance](kin::u8 a, kin::u8 b) {
        return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= tolerance;
    };
    return idx + 3u < px.size() &&
           near(px[idx + 0], color.r) &&
           near(px[idx + 1], color.g) &&
           near(px[idx + 2], color.b) &&
           near(px[idx + 3], color.a);
}

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
    kin::RendererBackendStats stats() const override { return backend_stats; }
    void reset_stats() override { backend_stats = {}; }
    void set_texture_batching_enabled(bool enabled) override { texture_batching = enabled; }
    bool texture_batching_enabled() const override { return texture_batching; }

    void clear(kin::Color color) override { last_color = color; ++clears; }
    void present() override { ++presents; }
    bool read_rgba(kin::Rectf region, std::vector<kin::u8>& out, kin::Vec2i& out_size) override {
        last_read_region = region;
        ++readbacks;
        out = readback_pixels;
        out_size = readback_size;
        return readback_supported;
    }

    void set_logical_size(kin::Vec2i size) override { logical_size = size; }
    void set_integer_logical_size(kin::Vec2i size) override { integer_size = size; }
    void push_native_coordinates() override {
        ++native_pushes;
        ++native_depth;
        native_max_depth = std::max(native_max_depth, native_depth);
    }
    void pop_native_coordinates() override {
        ++native_pops;
        --native_depth;
    }
    kin::Vec2i output_size() const override { return {320, 180}; }
    kin::Vec2f window_to_logical(kin::Vec2f window_px) const override { return {window_px.x * 0.5f, window_px.y * 0.5f}; }
    kin::Vec2f logical_to_window(kin::Vec2f logical) const override { return {logical.x * 2.0f, logical.y * 2.0f}; }

    void fill_rect(kin::Rectf rect, kin::Color color) override {
        last_rect = rect;
        last_color = color;
        ++rects;
    }
    kin::Texture create_texture_from_rgba(const kin::u8* pixels, kin::Vec2i size) override {
        assert(pixels != nullptr);
        ++textures;
        return kin::Texture{std::make_shared<FakeTextureBackend>(size)};
    }
    bool update_texture(const kin::Texture& texture, kin::Vec2i at, kin::Vec2i size, const kin::u8* pixels) override {
        assert(texture && pixels != nullptr);
        last_update_at = at;
        last_update_size = size;
        ++texture_updates;
        return true;
    }
    kin::Vec2i last_update_at{};
    kin::Vec2i last_update_size{};
    int texture_updates = 0;
    // Only the fixed-arity forms: the list form stays the interface default, which
    // is what a backend without wider sampler support gets.
    using kin::IRenderer2DBackend::draw_shader_surface;
    void draw_shader_surface(kin::Rectf, kin::ShaderHandle, const kin::ShaderParams&) override {
        shader_draw_sources.push_back(0);
    }
    void draw_shader_surface(kin::Rectf, kin::ShaderHandle, const kin::ShaderParams&,
                             const kin::Texture& source) override {
        shader_draw_sources.push_back(1);
        last_shader_source0 = source;
    }
    void draw_shader_surface(kin::Rectf, kin::ShaderHandle, const kin::ShaderParams&,
                             const kin::Texture& source0, const kin::Texture& source1) override {
        shader_draw_sources.push_back(2);
        last_shader_source0 = source0;
        last_shader_source1 = source1;
    }
    std::vector<int> shader_draw_sources;
    kin::Texture last_shader_source0;
    kin::Texture last_shader_source1;
    void draw_texture(const kin::Texture& texture, kin::Rectf dest) override {
        assert(texture);
        last_texture_dest = dest;
        ++texture_draws;
    }
    void draw_texture(const kin::Texture& texture, kin::Rectf source, kin::Rectf dest) override {
        assert(texture);
        last_texture_source = source;
        last_texture_dest = dest;
        ++texture_draws;
    }
    void draw_texture(const kin::Texture& texture, kin::Rectf source, kin::Rectf dest, kin::Color tint) override {
        draw_texture(texture, source, dest);
        last_texture_tint = tint;
        ++tinted_texture_draws;
    }
    void draw_texture(const kin::Texture& texture, kin::Rectf source, kin::Rectf dest, kin::Color tint, kin::f32 rotation, kin::Vec2f pivot) override {
        draw_texture(texture, source, dest, tint);
        last_texture_rotation = rotation;
        last_texture_pivot = pivot;
        ++rotated_texture_draws;
    }
    void draw_rect(kin::Rectf rect, kin::Color color) override {
        last_rect = rect;
        last_color = color;
        ++outlines;
    }
    void draw_line(kin::Vec2f a, kin::Vec2f b, kin::Color color) override {
        line_a = a;
        line_b = b;
        last_color = color;
        ++lines;
    }

    void set_viewport(kin::Rectf rect) override { viewports.push_back(rect); }
    void reset_viewport() override { ++viewport_resets; }
    void push_viewport(kin::Rectf rect) override { viewports.push_back(rect); ++viewport_pushes; }
    void pop_viewport() override { ++viewport_pops; }

    kin::Color last_color{};
    kin::Rectf last_rect{};
    kin::Rectf last_texture_source{};
    kin::Rectf last_texture_dest{};
    kin::Color last_texture_tint = kin::colors::white;
    kin::f32 last_texture_rotation = 0.0f;
    kin::Vec2f last_texture_pivot{0.5f, 0.5f};
    kin::Vec2f line_a{};
    kin::Vec2f line_b{};
    kin::Vec2i logical_size{};
    kin::Vec2i integer_size{};
    kin::Rectf last_read_region{};
    kin::Vec2i readback_size{};
    std::vector<kin::u8> readback_pixels;
    std::vector<kin::Rectf> viewports;
    int clears = 0;
    int presents = 0;
    int rects = 0;
    int textures = 0;
    int texture_draws = 0;
    int tinted_texture_draws = 0;
    int rotated_texture_draws = 0;
    int outlines = 0;
    int lines = 0;
    int readbacks = 0;
    int viewport_resets = 0;
    int viewport_pushes = 0;
    int viewport_pops = 0;
    int native_pushes = 0;
    int native_pops = 0;
    int native_depth = 0;
    int native_max_depth = 0;
    bool texture_batching = false;
    bool readback_supported = false;
    kin::RendererBackendStats backend_stats{};
};

void test_facade_with_fake_backend() {
    std::vector<kin::LogEvent> log_events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
        .memory_events = &log_events,
    });

    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    assert(has_log_event(log_events, "render", "renderer created"));

    assert(renderer.backend_name() == "fake");
    raw->backend_stats.texture_draws_submitted = 7;
    raw->backend_stats.texture_batch_flushes = 3;
    assert(renderer.backend_stats().saved_texture_draws() == 4);
    renderer.reset_backend_stats();
    assert(renderer.backend_stats().texture_draws_submitted == 0);
    renderer.set_texture_batching_enabled(true);
    assert(renderer.texture_batching_enabled());

    renderer.clear(1, 2, 3, 4);
    assert(raw->clears == 1);
    const kin::Color clear_color{1, 2, 3, 4};
    assert(raw->last_color == clear_color);

    renderer.set_logical_size(640, 360);
    assert(has_log_event(log_events, "render", "logical size set"));
    const kin::Vec2i logical_size{640, 360};
    assert(raw->logical_size == logical_size);

    renderer.set_integer_logical_size({320, 180});
    const kin::Vec2i integer_size{320, 180};
    assert(raw->integer_size == integer_size);

    {
        auto native = renderer.scoped_native_coordinates();
        assert(raw->native_pushes == 1);
        assert(raw->native_pops == 0);
        assert(raw->native_depth == 1);
        {
            auto nested = renderer.scoped_native_coordinates();
            assert(raw->native_pushes == 2);
            assert(raw->native_depth == 2);
            auto moved = std::move(nested);
            assert(raw->native_pushes == 2);
            assert(raw->native_depth == 2);
        }
        assert(raw->native_pops == 1);
        assert(raw->native_depth == 1);
    }
    assert(raw->native_pops == 2);
    assert(raw->native_depth == 0);
    assert(raw->native_max_depth == 2);

    const std::array<kin::u8, 16> pixels{
        255, 0, 0, 255,
        0, 255, 0, 255,
        0, 0, 255, 255,
        255, 255, 255, 255,
    };
    const kin::Texture texture = renderer.create_texture_from_rgba(pixels.data(), 2, 2);
    assert(texture);
    const kin::Vec2i texture_size{2, 2};
    assert(texture.size() == texture_size);
    assert(raw->textures == 1);

    // Part of a texture replaced in place: forwarded when inside it, refused
    // (and the backend never asked) when not.
    assert(renderer.update_texture(texture, {1, 0}, {1, 2}, pixels.data()));
    assert(raw->texture_updates == 1);
    assert((raw->last_update_at == kin::Vec2i{1, 0}) && (raw->last_update_size == kin::Vec2i{1, 2}));
    assert(!renderer.update_texture(texture, {1, 1}, {2, 2}, pixels.data()));
    assert(!renderer.update_texture(texture, {0, 0}, {0, 1}, pixels.data()));
    assert(!renderer.update_texture(kin::Texture{}, {0, 0}, {1, 1}, pixels.data()));
    assert(raw->texture_updates == 1);

    renderer.draw_texture(texture, {40.0f, 50.0f, 60.0f, 70.0f});
    assert(raw->texture_draws == 1);
    const kin::Rectf texture_dest{40.0f, 50.0f, 60.0f, 70.0f};
    assert(raw->last_texture_dest == texture_dest);

    renderer.draw_texture(texture, {0.0f, 0.0f, 1.0f, 1.0f}, {70.0f, 80.0f, 90.0f, 100.0f});
    assert(raw->texture_draws == 2);
    const kin::Rectf texture_source{0.0f, 0.0f, 1.0f, 1.0f};
    const kin::Rectf texture_sub_dest{70.0f, 80.0f, 90.0f, 100.0f};
    assert(raw->last_texture_source == texture_source);
    assert(raw->last_texture_dest == texture_sub_dest);

    renderer.draw_texture(texture,
                          {0.0f, 0.0f, 1.0f, 1.0f},
                          {90.0f, 80.0f, 70.0f, 60.0f},
                          kin::Color::rgba(200, 180, 160, 140));
    assert(raw->texture_draws == 3);
    assert(raw->tinted_texture_draws == 1);
    assert(raw->last_texture_tint == kin::Color::rgba(200, 180, 160, 140));

    renderer.draw_texture(texture,
                          {0.0f, 0.0f, 1.0f, 1.0f},
                          {100.0f, 90.0f, 80.0f, 70.0f},
                          kin::Color::rgba(10, 20, 30, 40),
                          45.0f,
                          {0.25f, 0.75f});
    assert(raw->texture_draws == 4);
    assert(raw->tinted_texture_draws == 2);
    assert(raw->rotated_texture_draws == 1);
    assert(raw->last_texture_tint == kin::Color::rgba(10, 20, 30, 40));
    assert(raw->last_texture_rotation == 45.0f);
    assert((raw->last_texture_pivot == kin::Vec2f{0.25f, 0.75f}));

    const kin::Sprite sprite{
        .texture = texture,
        .source = {1.0f, 1.0f, 1.0f, 1.0f},
    };
    renderer.draw_sprite(sprite, {10.0f, 11.0f}, {12.0f, 13.0f});
    assert(raw->texture_draws == 5);
    const kin::Rectf sprite_source{1.0f, 1.0f, 1.0f, 1.0f};
    const kin::Rectf sprite_dest{10.0f, 11.0f, 12.0f, 13.0f};
    assert(raw->last_texture_source == sprite_source);
    assert(raw->last_texture_dest == sprite_dest);

    renderer.draw_sprite(sprite,
                         {20.0f, 21.0f, 22.0f, 23.0f},
                         kin::Color::rgba(90, 80, 70, 60),
                         90.0f,
                         {1.0f, 0.0f});
    assert(raw->texture_draws == 6);
    assert(raw->rotated_texture_draws == 2);
    assert(raw->last_texture_tint == kin::Color::rgba(90, 80, 70, 60));
    assert(raw->last_texture_rotation == 90.0f);
    assert((raw->last_texture_pivot == kin::Vec2f{1.0f, 0.0f}));

    renderer.fill_rect({10.0f, 20.0f}, {30.0f, 40.0f}, kin::Color{5, 6, 7, 8});
    assert(raw->rects == 1);
    const kin::Rectf rect{10.0f, 20.0f, 30.0f, 40.0f};
    assert(raw->last_rect == rect);

    renderer.draw_rect({11.0f, 22.0f, 33.0f, 44.0f}, kin::Color::rgb(9, 10, 11));
    assert(raw->outlines == 1);
    const kin::Rectf outline{11.0f, 22.0f, 33.0f, 44.0f};
    assert(raw->last_rect == outline);
    const kin::Color outline_color{9, 10, 11, 255};
    assert(raw->last_color == outline_color);

    renderer.draw_line({1.0f, 2.0f}, {3.0f, 4.0f}, kin::Color::rgba(12, 13, 14, 15));
    assert(raw->lines == 1);
    const kin::Vec2f line_a{1.0f, 2.0f};
    const kin::Vec2f line_b{3.0f, 4.0f};
    assert(raw->line_a == line_a);
    assert(raw->line_b == line_b);

    const kin::Vec2f logical = renderer.window_to_logical({20.0f, 40.0f});
    const kin::Vec2f expected_logical{10.0f, 20.0f};
    assert(logical == expected_logical);
    const kin::Vec2f window_px = renderer.logical_to_window({10.0f, 20.0f});
    const kin::Vec2f expected_window_px{20.0f, 40.0f};
    assert(window_px == expected_window_px);

    std::vector<kin::u8> read_pixels;
    kin::Vec2i read_size{};
    assert(!renderer.read_rgba({2.0f, 4.0f, 8.0f, 10.0f}, read_pixels, read_size));
    assert(raw->readbacks == 1);
    const kin::Rectf read_region{2.0f, 4.0f, 8.0f, 10.0f};
    assert(raw->last_read_region == read_region);
    raw->readback_supported = true;
    raw->readback_size = {1, 1};
    raw->readback_pixels = {7, 8, 9, 10};
    assert(renderer.read_rgba({5.0f, 6.0f, 7.0f, 8.0f}, read_pixels, read_size));
    assert(read_size == raw->readback_size);
    assert(read_pixels == raw->readback_pixels);

    {
        auto viewport = renderer.scoped_viewport({1.0f, 2.0f, 3.0f, 4.0f});
        assert(raw->viewport_pushes == 1);
        (void)viewport;
    }
    assert(raw->viewport_pops == 1);

    renderer.present();
    assert(raw->presents == 1);
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
    });
}

void test_sprite_sheet() {
    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{38, 20})};
    kin::SpriteSheet sheet{
        texture,
        {
            .tile_w = 8,
            .tile_h = 8,
            .spacing = 2,
            .margin = 1,
        },
    };

    assert(sheet.valid());
    assert(sheet.cols() == 3);
    assert(sheet.rows() == 2);
    const kin::Vec2i tile_size{8, 8};
    assert(sheet.tile_size() == tile_size);

    const kin::Sprite first = sheet.sprite(0, 0);
    assert(first.valid());
    const kin::Rectf first_source{1.0f, 1.0f, 8.0f, 8.0f};
    assert(first.source == first_source);

    const kin::Sprite second_row = sheet.sprite(1, 1);
    const kin::Rectf second_row_source{11.0f, 11.0f, 8.0f, 8.0f};
    assert(second_row.source == second_row_source);

    const kin::Sprite indexed = sheet.sprite(4);
    assert(indexed.source == second_row_source);

    assert(!sheet.sprite(-1).valid());
    assert(!sheet.sprite(3, 0).valid());
    assert(!sheet.sprite(0, 2).valid());
}

void test_sprite_catalog_resolves_refs_and_sheets() {
    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{32, 16})};
    kin::SpriteCatalog catalog;
    catalog.set_texture("actors", "actors.png", texture);
    catalog.add_sheet({.id = "actors.sheet", .texture_id = "actors", .grid = {.tile_w = 8, .tile_h = 8}});
    catalog.add_sheet_sprite("hero.walk.0", "actors.sheet", 0, {16.0f, 16.0f});
    catalog.add_sheet_sprite("hero.walk.5", "actors.sheet", 5, {16.0f, 16.0f});

    const kin::TextureAssetRef* texture_ref = catalog.texture("actors");
    assert(texture_ref != nullptr);
    assert(texture_ref->path == "actors.png");

    const kin::SpriteRef ref = catalog.ref("hero.walk.5");
    assert(ref);
    assert(ref.id == "hero.walk.5");

    kin::ResolvedSprite resolved;
    assert(catalog.resolve(ref.id, resolved));
    assert(resolved.sprite.texture.valid());
    assert(resolved.sprite.texture.size() == texture.size());
    assert(resolved.sprite.source.x == 8.0f);
    assert(resolved.sprite.source.y == 8.0f);
    assert((resolved.size == kin::Vec2f{16.0f, 16.0f}));
}

void test_sdl_backend_smoke() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({
        .title = "renderer-test",
        .width = 64,
        .height = 64,
        .hidden = true,
    });

    kin::Renderer2D renderer{window};
    renderer.set_logical_size(64, 64);
    const std::array<kin::u8, 16> pixels{
        255, 0, 0, 255,
        0, 255, 0, 255,
        0, 0, 255, 255,
        255, 255, 255, 255,
    };
    const kin::Texture texture = renderer.create_texture_from_rgba(pixels.data(), 2, 2);
    assert(texture);
    const kin::Vec2i texture_size{2, 2};
    assert(texture.size() == texture_size);

    renderer.clear(10, 20, 30);
    renderer.draw_texture(texture, kin::Vec2f{24.0f, 24.0f}, kin::Vec2f{16.0f, 16.0f});
    renderer.draw_sprite(kin::Sprite{.texture = texture, .source = {0.0f, 0.0f, 1.0f, 1.0f}},
                         {44.0f, 24.0f}, {8.0f, 8.0f});
    renderer.fill_rect({4.0f, 4.0f}, {16.0f, 16.0f}, 200, 120, 40);
    renderer.draw_rect({3.0f, 3.0f, 18.0f, 18.0f}, kin::colors::white);
    renderer.draw_line({0.0f, 0.0f}, {63.0f, 63.0f}, kin::Color::rgb(255, 0, 0));

    {
        auto viewport = renderer.scoped_viewport({0.0f, 0.0f, 32.0f, 32.0f});
        renderer.fill_rect({0.0f, 0.0f}, {8.0f, 8.0f}, kin::colors::white);
        (void)viewport;
    }

    renderer.present();
    assert(renderer.output_size().x > 0);
    assert(renderer.output_size().y > 0);
}

void test_sdl_render_targets_and_gradients() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({
        .title = "rt-test",
        .width = 64,
        .height = 64,
        .hidden = true,
    });

    kin::Renderer2D renderer{window};

    const kin::RendererBackendCapabilities caps = renderer.capabilities();
    assert(caps.render_targets);
    assert(caps.gradients);

    // Render-target round-trip: draw an opaque fill into a target and read it back
    // while bound (opaque → premultiplied == straight, so the assert is simple).
    kin::RenderTarget target = renderer.create_render_target({32, 32}, kin::ScaleMode::Linear);
    assert(target.valid());
    assert((target.size() == kin::Vec2i{32, 32}));
    {
        auto bind = renderer.scoped_render_target(target);
        renderer.clear(kin::colors::transparent);
        renderer.fill_rect({0.0f, 0.0f}, {32.0f, 32.0f}, kin::Color::rgb(200, 100, 50));

        std::vector<kin::u8> pixels;
        kin::Vec2i size{};
        const bool ok = renderer.read_rgba({0.0f, 0.0f, 32.0f, 32.0f}, pixels, size);
        assert(ok);
        assert(size.x == 32 && size.y == 32);
        const std::size_t center =
            (static_cast<std::size_t>(size.y / 2) * static_cast<std::size_t>(size.x) +
             static_cast<std::size_t>(size.x / 2)) * 4u;
        assert(pixels[center + 0] == 200);
        assert(pixels[center + 1] == 100);
        assert(pixels[center + 2] == 50);
        assert(pixels[center + 3] == 255);
    }

    // Horizontal gradient: left edge ≈ start (red), right edge ≈ end (blue).
    renderer.set_logical_size(64, 64);
    renderer.clear(kin::colors::black);
    renderer.fill_gradient_rect({0.0f, 0.0f, 64.0f, 64.0f},
                                kin::Gradient{
                                    .start = kin::Color::rgb(255, 0, 0),
                                    .end = kin::Color::rgb(0, 0, 255),
                                    .direction = kin::GradientDirection::Horizontal,
                                });

    std::vector<kin::u8> grad;
    kin::Vec2i grad_size{};
    const bool grad_ok = renderer.read_rgba({0.0f, 0.0f, 64.0f, 64.0f}, grad, grad_size);
    assert(grad_ok);
    assert(grad_size.x > 1 && grad_size.y > 0);
    const std::size_t row =
        static_cast<std::size_t>(grad_size.y / 2) * static_cast<std::size_t>(grad_size.x) * 4u;
    const std::size_t left = row;
    const std::size_t right = row + static_cast<std::size_t>(grad_size.x - 1) * 4u;
    assert(grad[left + 0] > 200 && grad[left + 2] < 60);   // left is red-ish
    assert(grad[right + 2] > 200 && grad[right + 0] < 60); // right is blue-ish

    renderer.present();
}

void test_render_target_pool_reuse() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({
        .title = "pool-test",
        .width = 64,
        .height = 64,
        .hidden = true,
    });
    kin::Renderer2D renderer{window};

    assert(renderer.render_target_pool_size() == 0);
    {
        auto a = renderer.acquire_render_target({16, 16});
        assert(a.valid());
        assert(renderer.render_target_pool_size() == 1);
        {
            auto b = renderer.acquire_render_target({16, 16});
            assert(b.valid());
            assert(renderer.render_target_pool_size() == 2); // both in use → distinct entries
        }
        // b released here → its slot is free
        auto c = renderer.acquire_render_target({16, 16});
        assert(c.valid());
        assert(renderer.render_target_pool_size() == 2); // reused the freed slot, no growth

        auto d = renderer.acquire_render_target({8, 8});
        assert(d.valid());
        assert(renderer.render_target_pool_size() == 3); // different size → new entry
    }
    // a, c, d released now
    renderer.trim_render_target_pool();
    assert(renderer.render_target_pool_size() == 0);
}

void test_render_target_move_assign_releases() {
    // Regression: PooledTarget move-assignment must release the target it currently
    // holds, otherwise the per-frame glass pattern (held = acquire(); held = {};)
    // leaks a render target every iteration and exhausts GPU memory.
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({
        .title = "pool-move-test",
        .width = 64,
        .height = 64,
        .hidden = true,
    });
    kin::Renderer2D renderer{window};

    kin::PooledTarget held;
    for (int frame = 0; frame < 50; ++frame) {
        held = renderer.acquire_render_target({32, 32}); // assign over the previous one
        assert(held.valid());
        held = {};                                       // release (move-assign empty)
    }
    // One transient entry reused every iteration — never grows with the frame count.
    assert(renderer.render_target_pool_size() <= 1);
}

void test_blur_smooths_hard_edge() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({
        .title = "blur-test",
        .width = 64,
        .height = 64,
        .hidden = true,
    });
    kin::Renderer2D renderer{window};

    kin::RenderTarget src = renderer.create_render_target({32, 32}, kin::ScaleMode::Linear);
    assert(src.valid());
    {
        auto bind = renderer.scoped_render_target(src);
        renderer.clear(kin::colors::transparent);
        renderer.fill_rect({0.0f, 0.0f}, {16.0f, 32.0f}, kin::colors::white);          // left half white
        renderer.fill_rect({16.0f, 0.0f}, {16.0f, 32.0f}, kin::Color::rgb(0, 0, 0));   // right half black (opaque)
    }

    kin::PooledTarget blurred = kin::blur(renderer, src, {.passes = 2, .radius = 1.0f});
    assert(blurred.valid());
    assert((blurred.size() == kin::Vec2i{32, 32}));

    std::vector<kin::u8> px;
    kin::Vec2i size{};
    {
        auto bind = renderer.scoped_render_target(blurred.target());
        const bool ok = renderer.read_rgba({0.0f, 0.0f, 32.0f, 32.0f}, px, size);
        assert(ok);
        assert(size.x == 32 && size.y == 32);
    }

    const auto red_at = [&](int x, int y) -> int {
        const std::size_t idx =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.x) +
             static_cast<std::size_t>(x)) * 4u;
        return px[idx];
    };

    const int y = size.y / 2;
    // A hard white|black step has only 0/255 values; blurring turns it into a ramp,
    // so several intermediate (mid-gray) pixels must appear across the boundary.
    int mids = 0;
    for (int x = 0; x < size.x; ++x) {
        const int v = red_at(x, y);
        if (v > 40 && v < 215) {
            ++mids;
        }
    }
    assert(mids >= 3);
    // Content preserved: left stays brighter than right.
    assert(red_at(0, y) > red_at(size.x - 1, y));
}

void test_blur_degrades_on_fake_backend() {
    auto backend = std::make_unique<FakeBackend>();
    kin::Renderer2D renderer{std::move(backend)};
    assert(!renderer.capabilities().render_targets);

    kin::RenderTarget empty; // default-constructed → invalid
    assert(!kin::blur(renderer, empty, {}).valid());

    auto pooled = renderer.acquire_render_target({16, 16});
    assert(!pooled.valid()); // no render-target support → empty, nothing pooled
    assert(renderer.render_target_pool_size() == 0);
}

void test_capture_backdrop_round_trips() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "cap-test", .width = 64, .height = 64, .hidden = true});
    kin::Renderer2D renderer{window};
    assert(renderer.capabilities().render_targets);

    // Draw a known color into a target, then capture that region (SDL → readback fallback).
    kin::RenderTarget scene = renderer.create_render_target({32, 32}, kin::ScaleMode::Linear);
    kin::PooledTarget cap;
    {
        auto bind = renderer.scoped_render_target(scene);
        renderer.clear(kin::colors::transparent);
        renderer.fill_rect({0.0f, 0.0f}, {32.0f, 32.0f}, kin::Color::rgb(40, 160, 210));
        cap = renderer.capture_backdrop({0.0f, 0.0f, 32.0f, 32.0f});
    }
    assert(cap.valid());
    assert((cap.size() == kin::Vec2i{32, 32}));

    std::vector<kin::u8> px;
    kin::Vec2i sz{};
    {
        auto bind = renderer.scoped_render_target(cap.target());
        assert(renderer.read_rgba({0.0f, 0.0f, 32.0f, 32.0f}, px, sz));
    }
    assert(sz.x == 32 && sz.y == 32);
    const std::size_t c =
        (static_cast<std::size_t>(sz.y / 2) * static_cast<std::size_t>(sz.x) + static_cast<std::size_t>(sz.x / 2)) * 4u;
    const auto near = [](kin::u8 a, int b) { return std::abs(static_cast<int>(a) - b) <= 4; };
    assert(near(px[c + 0], 40));
    assert(near(px[c + 1], 160));
    assert(near(px[c + 2], 210));
    assert(px[c + 3] == 255);
}

void test_gpu_save_png_captures_bound_render_target() {
    constexpr std::string_view test_name = "test_gpu_save_png_captures_bound_render_target";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-save-rt-test", .width = 64, .height = 64, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;

        renderer->clear(kin::Color::rgb(220, 10, 30));
        kin::RenderTarget target = renderer->create_render_target({24, 16}, kin::ScaleMode::Nearest);
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "kin_gpu_rt_save_test.png";
        {
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgb(20, 180, 90));
            assert(renderer->save_png(path.string()));
        }

        SDL_Surface* raw = IMG_Load(path.string().c_str());
        std::filesystem::remove(path);
        assert(raw != nullptr);
        SDL_Surface* rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        assert(rgba != nullptr);
        assert(rgba->w == 24 && rgba->h == 16);
        std::vector<kin::u8> pixels(static_cast<std::size_t>(rgba->w) * static_cast<std::size_t>(rgba->h) * 4u);
        const auto* src = static_cast<const kin::u8*>(rgba->pixels);
        const std::size_t row_bytes = static_cast<std::size_t>(rgba->w) * 4u;
        for (int y = 0; y < rgba->h; ++y) {
            std::memcpy(pixels.data() + row_bytes * static_cast<std::size_t>(y),
                        src + static_cast<std::size_t>(rgba->pitch) * static_cast<std::size_t>(y),
                        row_bytes);
        }
        const kin::Vec2i size{rgba->w, rgba->h};
        SDL_DestroySurface(rgba);
        assert(pixel_near(pixels, size, 12, 8, kin::Color::rgb(20, 180, 90)));
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

void test_gpu_native_coordinates_disable_logical_presentation() {
    constexpr std::string_view test_name = "test_gpu_native_coordinates_disable_logical_presentation";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-native-test", .width = 160, .height = 120, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;

        renderer->set_logical_size(64, 64);
        renderer->clear(kin::colors::black);
        renderer->fill_rect({0.0f, 0.0f, 10.0f, 10.0f}, kin::Color::rgb(210, 20, 20));
        {
            const auto native = renderer->scoped_native_coordinates();
            renderer->fill_rect({80.0f, 80.0f, 10.0f, 10.0f}, kin::Color::rgb(30, 220, 80));
        }

        std::vector<kin::u8> px;
        kin::Vec2i size{};
        assert(renderer->read_rgba({0.0f, 0.0f, 64.0f, 64.0f}, px, size));
        assert(size.x == 120 && size.y == 120);
        assert(pixel_near(px, size, 5, 5, kin::Color::rgb(210, 20, 20)));
        assert(pixel_near(px, size, 85, 85, kin::Color::rgb(30, 220, 80)));
        assert(pixel_near(px, size, 45, 45, kin::colors::black));
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

void test_gpu_logical_transforms_are_immediate() {
    constexpr std::string_view test_name = "test_gpu_logical_transforms_are_immediate";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-transform-test", .width = 200, .height = 120, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;

        renderer->set_logical_size(100, 50);
        const kin::Vec2f logical = renderer->window_to_logical({100.0f, 60.0f});
        assert(std::abs(logical.x - 50.0f) < 0.01f);
        assert(std::abs(logical.y - 25.0f) < 0.01f);
        const kin::Vec2f window_px = renderer->logical_to_window({50.0f, 25.0f});
        assert(std::abs(window_px.x - 100.0f) < 0.01f);
        assert(std::abs(window_px.y - 60.0f) < 0.01f);
        kin::PooledTarget capture = renderer->capture_backdrop({0.0f, 0.0f, 10.0f, 20.0f});
        // The capture may be invalid before any draw on some drivers, but its size path
        // must have used the eager logical scale when the target was allocated.
        if (capture.valid()) {
            assert((capture.size() == kin::Vec2i{20, 40}));
        }
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

void test_gpu_native_pixel_size_and_pointer_mapping() {
    constexpr std::string_view test_name = "test_gpu_native_pixel_size_and_pointer_mapping";
    bool gpu_ready = false;
    try {
        kin::App app{};
        auto& window = app.create_window({.title = "gpu-dpi-test", .width = 320, .height = 240,
                                          .hidden = true, .high_pixel_density = true});
        std::string reason;
        auto renderer = try_create_gpu_renderer(window, reason);
        if (!renderer) { skip_or_require_gpu_test(test_name, reason); return; }
        gpu_ready = true;
        for (kin::Vec2i requested : {kin::Vec2i{320, 240}, kin::Vec2i{480, 360}}) {
            window.set_size(requested);
            SDL_SyncWindow(SDL_GetWindowFromID(window.id()));
            renderer->clear();
            const auto pixels = window.pixel_size(), units = window.size();
            assert(renderer->output_size() == pixels);
            const kin::Vec2f center{units.x * .5f, units.y * .5f};
            const auto native = renderer->window_to_logical(center);
            assert(std::abs(native.x - pixels.x * .5f) < .01f);
            assert(std::abs(native.y - pixels.y * .5f) < .01f);
            const auto back = renderer->logical_to_window(native);
            assert(std::abs(back.x - center.x) < .01f && std::abs(back.y - center.y) < .01f);
            renderer->present();
        }
        renderer->set_logical_size(160, 100);
        const auto units = window.size();
        const auto center = renderer->window_to_logical({units.x * .5f, units.y * .5f});
        assert(std::abs(center.x - 80) < .01f && std::abs(center.y - 50) < .01f);
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) throw;
        skip_or_require_gpu_test(test_name, e.what());
    }
}

void test_shader_surface_sources_on_fake_backend() {
    std::vector<kin::LogEvent> log_events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
        .memory_events = &log_events,
    });

    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    // Distinct widths tell the sources apart.
    const std::array<kin::u8, 12> white{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    const std::array<kin::Texture, 3> sources{
        renderer.create_texture_from_rgba(white.data(), {1, 1}),
        renderer.create_texture_from_rgba(white.data(), {2, 1}),
        renderer.create_texture_from_rgba(white.data(), {3, 1}),
    };
    const kin::Rectf rect{0.0f, 0.0f, 8.0f, 8.0f};
    const kin::ShaderHandle shader{1};
    const kin::ShaderParams params{};

    // A backend without list support gets the first two sources.
    renderer.draw_shader_surface(rect, shader, params, std::span<const kin::Texture>{sources});
    assert(raw->shader_draw_sources == std::vector<int>{2});
    assert((raw->last_shader_source0.size() == kin::Vec2i{1, 1}));
    assert((raw->last_shader_source1.size() == kin::Vec2i{2, 1}));
    renderer.draw_shader_surface(rect, shader, params, std::span<const kin::Texture>{sources.data(), 1});
    renderer.draw_shader_surface(rect, shader, params, std::span<const kin::Texture>{});
    assert((raw->shader_draw_sources == std::vector<int>{2, 1, 0}));

    // More sources than sampler slots is rejected before it reaches the backend.
    const std::vector<kin::Texture> too_many(kin::MaxShaderSamplers + 1, sources[0]);
    renderer.draw_shader_surface(rect, shader, params, too_many);
    assert(raw->shader_draw_sources.size() == 3);
    assert(has_log_event(log_events, "render", "draw_shader_surface: too many sources"));
    kin::set_logger_config({.sdl_sink = false});
}

void test_gpu_shader_surface_binds_every_source() {
    constexpr std::string_view test_name = "test_gpu_shader_surface_binds_every_source";
    const std::filesystem::path spv = std::filesystem::path{KIN_TEST_SHADER_DIR} / "four_sources.frag.spv";
    if (!std::filesystem::exists(spv)) {
        skip_or_require_gpu_test(test_name, "test shader not compiled (glslc not found)");
        return;
    }
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-shader-sources-test", .width = 64, .height = 64, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;

        std::ifstream file(spv, std::ios::binary);
        const std::vector<kin::u8> code{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        kin::ShaderDesc desc{};
        desc.spirv = {code.data(), static_cast<kin::u32>(code.size())};
        desc.num_samplers = 4;
        const kin::ShaderHandle shader = renderer->create_shader(desc);
        assert(shader);

        // The shader adds slot 0's red, slot 1's green, slot 2's blue and slot 3's
        // grey, so every slot shows in the result only if it is bound correctly.
        const auto solid = [&](kin::u8 r, kin::u8 g, kin::u8 b) {
            const std::array<kin::u8, 4> px{r, g, b, 255};
            return renderer->create_texture_from_rgba(px.data(), {1, 1});
        };
        const std::array<kin::Texture, 4> sources{
            solid(200, 0, 0), solid(0, 150, 0), solid(0, 0, 100), solid(40, 40, 40)};
        kin::RenderTarget target = renderer->create_render_target({8, 8}, kin::ScaleMode::Nearest);
        std::vector<kin::u8> pixels;
        kin::Vec2i size{};
        {
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgb(0, 0, 0));
            renderer->draw_shader_surface({0.0f, 0.0f, 8.0f, 8.0f}, shader, {}, std::span<const kin::Texture>{sources});
            assert(renderer->read_rgba({0.0f, 0.0f, 8.0f, 8.0f}, pixels, size));
        }
        assert(pixel_near(pixels, size, 4, 4, kin::Color::rgb(240, 190, 140)));

        // With two sources, the two slots left over are bound white instead of unbound.
        {
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgb(0, 0, 0));
            renderer->draw_shader_surface({0.0f, 0.0f, 8.0f, 8.0f}, shader, {}, sources[0], sources[1]);
            assert(renderer->read_rgba({0.0f, 0.0f, 8.0f, 8.0f}, pixels, size));
        }
        assert(pixel_near(pixels, size, 4, 4, kin::Color::rgb(255, 255, 255)));
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

void test_post_process_degrades_on_fake_backend() {
    auto backend = std::make_unique<FakeBackend>();
    kin::Renderer2D renderer{std::move(backend)};
    assert(!renderer.capabilities().materials_2d);

    // No material support -> built-in shaders are null handles (callers degrade).
    assert(!renderer.builtin_shader(kin::BuiltinShader::Vignette));
    assert(!renderer.builtin_shader(kin::BuiltinShader::BloomCombine));

    // Setting / clearing a post-process chain is a no-op and must not throw.
    kin::PostProcessPass pass;
    pass.shader = kin::ShaderHandle{1};
    std::array<kin::PostProcessPass, 1> chain{pass};
    renderer.set_post_process(chain);
    renderer.clear_post_process();
}

} // namespace

int main() {
    test_facade_with_fake_backend();
    test_post_process_degrades_on_fake_backend();
    test_shader_surface_sources_on_fake_backend();
    test_sprite_sheet();
    test_sprite_catalog_resolves_refs_and_sheets();
    test_sdl_backend_smoke();
    test_sdl_render_targets_and_gradients();
    test_render_target_pool_reuse();
    test_render_target_move_assign_releases();
    test_blur_smooths_hard_edge();
    test_blur_degrades_on_fake_backend();
    test_capture_backdrop_round_trips();
    test_gpu_save_png_captures_bound_render_target();
    test_gpu_native_coordinates_disable_logical_presentation();
    test_gpu_logical_transforms_are_immediate();
    test_gpu_native_pixel_size_and_pointer_mapping();
    test_gpu_shader_surface_binds_every_source();
    return 0;
}
