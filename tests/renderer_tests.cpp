#include <kin/assets/image.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/color_grading.hpp>
#include <kin/platform/log.hpp>
#include <kin/core/jobs.hpp>
#include <kin/renderer/cached_target.hpp>
#include <kin/renderer/lighting.hpp>
#include <kin/renderer/post_blur.hpp>
#include <kin/renderer/render_view.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/shader_compiler.hpp>
#include <kin/renderer/shader_reflect.hpp>
#include <kin/renderer/sprite_catalog.hpp>
#include <kin/renderer/sprite_sheet.hpp>
#include <kin/platform/input.hpp>
#include <kin/renderer/render_queue.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/text.hpp>

#include <SDL3_image/SDL_image.h>

#include <cassert>
#include <chrono>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
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

bool color_near(kin::Color a, kin::Color b, int tolerance = 2) {
    const auto near = [tolerance](kin::u8 x, kin::u8 y) { return std::abs(static_cast<int>(x) - static_cast<int>(y)) <= tolerance; };
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b) && near(a.a, b.a);
}

std::vector<kin::u8> read_spirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
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

// The SDL backend skips SDL's logical presentation while the logical size equals
// the window (SDL would otherwise draw every line as triangles), and turns it
// back on when a resize makes the sizes differ: clear() re-checks.
void test_sdl_logical_size_follows_window_size() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "logical-follow", .width = 160, .height = 100, .hidden = true});
    kin::Renderer2D renderer{window};
    renderer.set_logical_size(160, 100);
    const auto near = [](kin::Vec2f a, kin::Vec2f b) {
        return std::abs(a.x - b.x) < 0.01f && std::abs(a.y - b.y) < 0.01f;
    };
    const auto resize = [&](kin::Vec2i size) {
        window.set_size(size);
        SDL_SyncWindow(SDL_GetWindowFromID(window.id()));
        renderer.present();
        renderer.clear();
        return renderer.output_size() == size;
    };

    renderer.clear();
    assert(near(renderer.window_to_logical({40.0f, 30.0f}), {40.0f, 30.0f}));
    renderer.draw_line({0.0f, 0.0f}, {159.0f, 99.0f}, kin::Color::rgba(255, 0, 0, 128));
    renderer.present();

    if (!resize({320, 200})) {
        std::cerr << "[SKIP] test_sdl_logical_size_follows_window_size: window resize not supported here\n";
        return;
    }
    assert(near(renderer.window_to_logical({160.0f, 100.0f}), {80.0f, 50.0f}));
    assert(near(renderer.logical_to_window({80.0f, 50.0f}), {160.0f, 100.0f}));

    assert(resize({160, 100}));
    assert(near(renderer.window_to_logical({40.0f, 30.0f}), {40.0f, 30.0f}));
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

// A texture released right after it is drawn, before the frame is flushed, still
// draws: the backend keeps it alive until the frame is submitted. And frames that
// drop textures that way keep working with GPU frame timing on, whose fence
// thread once ran SDL's resource cleanup behind the render thread's back.
void test_gpu_texture_released_before_flush() {
    constexpr std::string_view test_name = "test_gpu_texture_released_before_flush";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-release-test", .width = 64, .height = 64, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;

        const std::vector<kin::u8> white(8u * 8u * 4u, 255);
        std::vector<kin::u8> blue(8u * 8u * 4u, 0);
        for (std::size_t i = 2; i < blue.size(); i += 4) {
            blue[i] = 255;
            blue[i + 1] = 255;
        }
        renderer->clear(kin::Color::rgb(0, 0, 0));
        {
            kin::Texture texture = renderer->create_texture_from_rgba(white.data(), 8, 8);
            renderer->draw_texture(texture, kin::Rectf{8.0f, 8.0f, 16.0f, 16.0f});
        } // released while the draw is still queued
        std::vector<kin::u8> pixels;
        kin::Vec2i size;
        assert(renderer->read_rgba(kin::Rectf{0.0f, 0.0f, 64.0f, 64.0f}, pixels, size));
        assert(pixel_near(pixels, size, 16, 16, kin::Color::rgb(255, 255, 255)));
        assert(pixel_near(pixels, size, 40, 40, kin::Color::rgb(0, 0, 0)));
        renderer->present();

        renderer->set_gpu_timing_enabled(true);
        for (int frame = 0; frame < 120; ++frame) {
            renderer->clear(kin::Color::rgb(0, 0, 0));
            for (int i = 0; i < 8; ++i) {
                kin::Texture texture = renderer->create_texture_from_rgba((i % 2 ? blue : white).data(), 8, 8);
                renderer->draw_texture(texture, kin::Rectf{static_cast<kin::f32>(i * 8), 0.0f, 8.0f, 8.0f});
            }
            renderer->present();
        }
        renderer->set_gpu_timing_enabled(false);
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// With GPU timing on, a CPU that runs frames ahead of a busy GPU fills the
// timer, and the frames past it go untimed. Their fences once went back to
// SDL's pool still in flight, to be reset for the next submission (an upload,
// say), which the old frame's end then reported finished: SDL freed its staging
// buffer under the copy, and Vulkan lost the device. Whether that crashes is up
// to the driver's timing; this runs the untimed frames, uploads between them.
void test_gpu_timing_survives_a_gpu_behind() {
    constexpr std::string_view test_name = "test_gpu_timing_survives_a_gpu_behind";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-behind-test", .width = 512, .height = 512, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;

        const std::vector<kin::u8> white(8u * 8u * 4u, 255);
        const std::vector<kin::u8> big(1024u * 1024u * 4u, 128);
        const kin::Texture texture = renderer->create_texture_from_rgba(white.data(), 8, 8);
        renderer->set_gpu_timing_enabled(true);
        // Blended full-window quads: far more GPU work than CPU, on a real GPU
        // (about 0.5 ms a frame) and on a software one alike, without taking
        // long on the latter.
        for (int frame = 0; frame < 40; ++frame) {
            renderer->clear(kin::Color::rgb(0, 0, 0));
            for (int i = 0; i < 300; ++i) {
                renderer->draw_texture(texture, kin::Rectf{0.0f, 0.0f, 8.0f, 8.0f}, kin::Rectf{0.0f, 0.0f, 512.0f, 512.0f},
                                       kin::Color::rgba(255, 255, 255, 4));
            }
            renderer->present();
            // An upload after each frame: a submission that a recycled fence
            // would report finished too early, freeing its staging buffer.
            const kin::Texture streamed = renderer->create_texture_from_rgba(big.data(), 1024, 1024);
            renderer->draw_texture(streamed, kin::Rectf{0.0f, 0.0f, 16.0f, 16.0f});
        }
        renderer->present();
        // Samples still come, each spread over the untimed frames it covers.
        const kin::RendererBackendStats stats = renderer->backend_stats();
        assert(stats.gpu_frames_sampled > 0);
        assert(stats.last_gpu_frame_span >= 1 && stats.last_gpu_frame_ms > 0.0);
        renderer->set_gpu_timing_enabled(false);
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

void test_gpu_frame_timing() {
    constexpr std::string_view test_name = "test_gpu_frame_timing";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-timing-test", .width = 160, .height = 120, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;

        const auto draw_frame = [&] {
            renderer->clear(kin::colors::black);
            renderer->fill_rect({0.0f, 0.0f, 40.0f, 40.0f}, kin::Color::rgb(210, 20, 20));
            renderer->present();
        };
        // Off by default: no GPU time, but the present timings are there.
        draw_frame();
        assert(renderer->backend_stats().last_gpu_frame_ms == 0.0);
        assert(renderer->backend_stats().last_gpu_wait_ms >= 0.0);

        // On: a finished frame reports its GPU time a frame or two later.
        renderer->set_gpu_timing_enabled(true);
        renderer->set_gpu_timing_enabled(true); // idempotent
        bool timed = false;
        for (int frame = 0; frame < 500 && !timed; ++frame) {
            draw_frame();
            timed = renderer->backend_stats().last_gpu_frame_ms > 0.0;
            if (!timed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        assert(timed);
        assert(renderer->backend_stats().last_gpu_frame_ms < 10'000.0);

        // Off again: the timer stops (waiting for its frames) and frames still present.
        renderer->set_gpu_timing_enabled(false);
        assert(renderer->backend_stats().last_gpu_frame_ms == 0.0);
        draw_frame();
        // Left on at destruction, the timer goes before the device.
        renderer->set_gpu_timing_enabled(true);
        draw_frame();
        draw_frame();
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

// Fonts and textures can outlive the renderer that made them: the static
// system-font cache holds glyph atlases until exit. Releasing them must not touch
// the dead GPU device, and a new renderer at the same address must build its own
// atlases instead of drawing with the dead renderer's.
void test_gpu_resources_outlive_renderer() {
    constexpr std::string_view test_name = "test_gpu_resources_outlive_renderer";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-lifetime-test", .width = 64, .height = 32, .hidden = true});
        std::vector<kin::ui2::Font> fonts{kin::ui2::bitmap_font()};
        if (kin::ui2::system_ui_font_available()) {
            fonts.push_back(kin::ui2::system_ui_font(14.0f));
        }
        kin::Texture survivor;
        std::optional<kin::Renderer2D> renderer; // one slot, so both renderers share an address
        for (int round = 0; round < 2; ++round) {
            std::unique_ptr<kin::IRenderer2DBackend> backend = kin::make_render_backend(window, false, true);
            if (!backend || backend->name() != "SDL_GPU") {
                skip_or_require_gpu_test(test_name, "SDL_GPU backend was not selected");
                return;
            }
            gpu_ready = true;
            renderer.emplace(std::move(backend));
            if (round == 0) {
                const std::array<kin::u8, 4> px{255, 0, 0, 255};
                survivor = renderer->create_texture_from_rgba(px.data(), {1, 1});
            }
            kin::RenderTarget target = renderer->create_render_target({48, 24}, kin::ScaleMode::Nearest);
            for (const kin::ui2::Font& font : fonts) {
                std::vector<kin::u8> pixels;
                kin::Vec2i size{};
                {
                    const auto bind = renderer->scoped_render_target(target);
                    renderer->clear(kin::Color::rgb(0, 0, 0));
                    kin::ui2::draw_text(*renderer, font, "HI", {4.0f, 4.0f}, 1.0f, kin::Color::rgb(255, 255, 255));
                    assert(renderer->read_rgba({0.0f, 0.0f, 48.0f, 24.0f}, pixels, size));
                }
                std::size_t lit = 0;
                for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
                    lit += pixels[i] > 128 ? 1 : 0;
                }
                assert(lit > 0);
            }
            renderer.reset();
        }
        survivor = {}; // released after its device is gone
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

// Draws `scene` into a 64 x 64 target, lights it, and reads it back.
std::vector<kin::u8> lit_scene(kin::Renderer2D& renderer, kin::LightLayer& layer, kin::Color scene,
                               kin::Color ambient, std::span<const kin::Light2D> lights) {
    kin::RenderTarget target = renderer.create_render_target({64, 64}, kin::ScaleMode::Nearest);
    assert(target.valid());
    const auto bind = renderer.scoped_render_target(target);
    renderer.clear(scene);
    assert(layer.apply(renderer, {0.0f, 0.0f, 64.0f, 64.0f}, ambient, lights, 1.0f));
    std::vector<kin::u8> pixels;
    kin::Vec2i size{};
    assert(renderer.read_rgba({0.0f, 0.0f, 64.0f, 64.0f}, pixels, size));
    assert((size == kin::Vec2i{64, 64}));
    return pixels;
}

// A scene of sprites drawn one draw_texture() at a time, or as one draw_sprites().
std::vector<kin::u8> sprite_scene(kin::Renderer2D& renderer, const kin::Texture& texture,
                                  std::span<const kin::SpriteInstance> sprites, bool batched) {
    kin::RenderTarget target = renderer.create_render_target({64, 64}, kin::ScaleMode::Nearest);
    assert(target.valid());
    const auto bind = renderer.scoped_render_target(target);
    renderer.clear(kin::Color::rgb(10, 20, 30));
    renderer.push_viewport({8.0f, 4.0f, 48.0f, 56.0f});
    if (batched) {
        renderer.draw_sprites(texture, sprites);
    } else {
        for (const kin::SpriteInstance& s : sprites) {
            const kin::Rectf source = s.source.w > 0.0f ? s.source : kin::Rectf{0.0f, 0.0f, 4.0f, 4.0f};
            renderer.draw_texture(texture, source, s.dest, s.tint, s.rotation, s.pivot, s.flip);
        }
    }
    renderer.pop_viewport();
    std::vector<kin::u8> pixels;
    kin::Vec2i size{};
    assert(renderer.read_rgba({0.0f, 0.0f, 64.0f, 64.0f}, pixels, size));
    assert((size == kin::Vec2i{64, 64}));
    return pixels;
}

// draw_sprites() must draw what a draw_texture() per sprite draws: plain, tinted,
// from part of the texture, turned about a pivot, mirrored, overlapping in order,
// inside a viewport. Returns how many pixels differ by more than 2 in any channel.
int sprite_batch_mismatches(kin::Renderer2D& renderer) {
    std::array<kin::u8, 64> texels{};
    for (std::size_t i = 0; i < 16; ++i) {
        texels[i * 4 + 0] = static_cast<kin::u8>(40 + i * 13);
        texels[i * 4 + 1] = static_cast<kin::u8>(250 - i * 11);
        texels[i * 4 + 2] = static_cast<kin::u8>((i % 4) * 60);
        texels[i * 4 + 3] = 255;
    }
    const kin::Texture texture = renderer.create_texture_from_rgba(texels.data(), {4, 4});
    renderer.set_scale_mode(texture, kin::ScaleMode::Nearest);
    const std::array<kin::SpriteInstance, 7> sprites{{
        {.dest = {2.0f, 2.0f, 12.0f, 12.0f}},
        {.dest = {18.0f, 2.0f, 12.0f, 8.0f}, .source = {1.0f, 1.0f, 2.0f, 2.0f}, .tint = kin::Color::rgba(255, 128, 64, 200)},
        {.dest = {4.0f, 20.0f, 16.0f, 16.0f}, .rotation = 30.0f},
        {.dest = {24.0f, 24.0f, 10.0f, 14.0f}, .rotation = -45.0f, .pivot = {0.0f, 0.0f}},
        {.dest = {10.0f, 10.0f, 20.0f, 20.0f}, .tint = kin::Color::rgba(255, 255, 255, 128)},
        {.dest = {36.0f, 2.0f, 12.0f, 8.0f}, .source = {0.0f, 1.0f, 3.0f, 2.0f}, .flip = kin::Flip::X},
        {.dest = {34.0f, 36.0f, 12.0f, 14.0f}, .rotation = 20.0f, .pivot = {0.25f, 0.75f}, .flip = kin::Flip::XY},
    }};
    const std::vector<kin::u8> one_by_one = sprite_scene(renderer, texture, sprites, false);
    const std::vector<kin::u8> batched = sprite_scene(renderer, texture, sprites, true);
    int mismatches = 0;
    for (std::size_t i = 0; i < one_by_one.size(); i += 4) {
        for (std::size_t c = 0; c < 4; ++c) {
            if (std::abs(int(one_by_one[i + c]) - int(batched[i + c])) > 2) {
                ++mismatches;
                break;
            }
        }
    }
    // Something was drawn at all: the first sprite's top-left texel.
    assert(pixel_near(batched, {64, 64}, 12, 8, kin::Color::rgb(40, 250, 0), 3));
    return mismatches;
}

// A texture kept past its renderer (in a static cache, say) and dropped while
// a newer renderer runs must not touch the newer renderer's textures: SDL frees
// a renderer's textures with it, and may hand their addresses to new ones.
void test_textures_outliving_their_renderer() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "stale-texture-test", .width = 16, .height = 16, .hidden = true});
    std::vector<kin::Texture> stale;
    const std::array<kin::u8, 4> red{255, 0, 0, 255};
    {
        kin::Renderer2D old_renderer{window};
        for (int i = 0; i < 32; ++i) {
            stale.push_back(old_renderer.create_texture_from_rgba(red.data(), {1, 1}));
        }
    }
    kin::Renderer2D renderer{window};
    std::vector<kin::Texture> live;
    for (int i = 0; i < 32; ++i) {
        live.push_back(renderer.create_texture_from_rgba(red.data(), {1, 1}));
    }
    stale.clear();
    kin::RenderTarget target = renderer.create_render_target({16, 16}, kin::ScaleMode::Nearest);
    for (const kin::Texture& texture : live) {
        const auto bind = renderer.scoped_render_target(target);
        renderer.clear(kin::Color::rgb(0, 0, 0));
        renderer.draw_texture(texture, {0.0f, 0.0f, 16.0f, 16.0f});
        std::vector<kin::u8> pixels;
        kin::Vec2i size{};
        assert(renderer.read_rgba({0.0f, 0.0f, 16.0f, 16.0f}, pixels, size));
        assert(pixel_near(pixels, size, 8, 8, kin::Color::rgb(255, 0, 0)));
    }
}

// A 2 x 2 texture (red, green over blue, white) drawn mirrored every way, by
// each entry point, at 16 x 16: how many quadrants are the wrong colour.
int flipped_quadrant_errors(kin::Renderer2D& renderer) {
    const kin::Color red = kin::Color::rgb(255, 0, 0), green = kin::Color::rgb(0, 255, 0);
    const kin::Color blue = kin::Color::rgb(0, 0, 255), white = kin::Color::rgb(255, 255, 255);
    const std::array<kin::u8, 16> texels{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    const kin::Texture texture = renderer.create_texture_from_rgba(texels.data(), {2, 2});
    renderer.set_scale_mode(texture, kin::ScaleMode::Nearest);
    const kin::Sprite sprite{.texture = texture, .source = {0.0f, 0.0f, 2.0f, 2.0f}};
    struct Case {
        kin::Flip flip;
        float rotation;
        std::array<kin::Color, 4> quadrants; // top-left, top-right, bottom-left, bottom-right
    };
    const std::array<Case, 5> cases{{
        {kin::Flip::None, 0.0f, {red, green, blue, white}},
        {kin::Flip::X, 0.0f, {green, red, white, blue}},
        {kin::Flip::Y, 0.0f, {blue, white, red, green}},
        {kin::Flip::XY, 0.0f, {white, blue, green, red}},
        // Mirrored first, then turned a quarter clockwise about the centre.
        {kin::Flip::X, 90.0f, {white, green, blue, red}},
    }};
    int errors = 0;
    kin::RenderTarget target = renderer.create_render_target({64, 64}, kin::ScaleMode::Nearest);
    for (const Case& c : cases) {
        for (int way = 0; way < 4; ++way) {
            const kin::Rectf dest{24.0f, 24.0f, 16.0f, 16.0f};
            std::vector<kin::u8> pixels;
            kin::Vec2i size{};
            {
                const auto bind = renderer.scoped_render_target(target);
                renderer.clear(kin::Color::rgb(0, 0, 0));
                const kin::SpriteInstance instance{.dest = dest, .rotation = c.rotation, .flip = c.flip};
                switch (way) {
                case 0: renderer.draw_texture(texture, sprite.source, dest, kin::colors::white, c.rotation, {0.5f, 0.5f}, c.flip); break;
                case 1: renderer.draw_texture(texture, {}, dest, kin::colors::white, c.rotation, {0.5f, 0.5f}, c.flip); break;
                case 2: renderer.draw_sprite(sprite, dest, kin::colors::white, c.rotation, {0.5f, 0.5f}, c.flip); break;
                default: renderer.draw_sprites(texture, std::span{&instance, 1}); break;
                }
                assert(renderer.read_rgba({0.0f, 0.0f, 64.0f, 64.0f}, pixels, size));
            }
            const std::array<kin::Vec2i, 4> probes{{{28, 28}, {36, 28}, {28, 36}, {36, 36}}};
            for (std::size_t q = 0; q < 4; ++q) {
                if (!pixel_near(pixels, size, probes[q].x, probes[q].y, c.quadrants[q])) {
                    std::fprintf(stderr, "flip %d rotation %.0f way %d quadrant %zu wrong\n", int(c.flip), c.rotation, way, q);
                    ++errors;
                }
            }
        }
    }
    return errors;
}

void test_flipped_sprites_on_software_backend() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "flip-test", .width = 64, .height = 64, .hidden = true});
    kin::Renderer2D renderer{window};
    assert(flipped_quadrant_errors(renderer) == 0); // batched into geometry
    renderer.set_texture_batching_enabled(false);
    assert(flipped_quadrant_errors(renderer) == 0); // one SDL_RenderTextureRotated each
}

void test_sprite_batches_on_software_backend() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "sprite-batch-test", .width = 64, .height = 64, .hidden = true});
    kin::Renderer2D renderer{window};
    assert(sprite_batch_mismatches(renderer) == 0); // the default: one draw_texture each
}

void test_sprite_batches_on_gpu_backend() {
    constexpr std::string_view test_name = "test_sprite_batches_on_gpu_backend";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-sprite-batch-test", .width = 64, .height = 64, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        // Instanced: the corners are computed on the GPU, so allow a few edge pixels
        // of a turned sprite to round the other way.
        const int mismatches = sprite_batch_mismatches(*renderer);
        if (mismatches > 4) {
            throw std::runtime_error(std::string(test_name) + ": " + std::to_string(mismatches) + " pixels differ");
        }
        if (const int errors = flipped_quadrant_errors(*renderer); errors != 0) {
            throw std::runtime_error(std::string(test_name) + ": " + std::to_string(errors) + " flipped quadrants wrong");
        }
        // A batch big enough to be filled on workers draws the same pixels.
        std::vector<kin::SpriteInstance> many;
        for (int i = 0; i < 40000; ++i) {
            const float x = static_cast<float>((i * 37) % 60), y = static_cast<float>((i * 91) % 60);
            many.push_back({.dest = {x, y, i % 97 == 0 ? 0.0f : 4.0f, 4.0f}, .tint = kin::Color::rgba(255, 255, 255, 40),
                            .rotation = static_cast<float>(i % 5) * 20.0f});
        }
        std::array<kin::u8, 4> white{255, 255, 255, 255};
        const kin::Texture dot = renderer->create_texture_from_rgba(white.data(), {1, 1});
        const auto draw_many = [&](kin::JobSystem* jobs) {
            renderer->set_job_system(jobs);
            kin::RenderTarget target = renderer->create_render_target({64, 64}, kin::ScaleMode::Nearest);
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgb(0, 0, 0));
            renderer->draw_sprites(dot, many);
            std::vector<kin::u8> pixels;
            kin::Vec2i size{};
            assert(renderer->read_rgba({0.0f, 0.0f, 64.0f, 64.0f}, pixels, size));
            renderer->set_job_system(nullptr);
            return pixels;
        };
        kin::JobSystem jobs{{.workers = 3}};
        if (draw_many(nullptr) != draw_many(&jobs)) {
            throw std::runtime_error(std::string(test_name) + ": a batch filled on workers drew different pixels");
        }
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// write_texture: the fill writes the texels where they go (the upload memory
// on SDL_GPU), and may not use the renderer meanwhile.
void check_write_texture(kin::Renderer2D& renderer) {
    kin::Texture texture = renderer.create_texture_from_rgba(std::vector<kin::u8>(16u * 16u * 4u, 0).data(), {16, 16});
    const bool written = renderer.write_texture(texture, {4, 0}, {12, 16}, [](std::span<kin::u8> texels) {
        assert(texels.size() == 12u * 16u * 4u);
        for (std::size_t i = 0; i < texels.size(); i += 4) {
            texels[i] = 255;
            texels[i + 1] = static_cast<kin::u8>(i / 4 % 12 * 20);
            texels[i + 2] = 0;
            texels[i + 3] = 255;
        }
    });
    if (!written) {
        return; // a backend that cannot update textures
    }
    kin::RenderTarget target = renderer.create_render_target({16, 16}, kin::ScaleMode::Nearest);
    std::vector<kin::u8> px;
    kin::Vec2i size{};
    {
        const auto bind = renderer.scoped_render_target(target);
        renderer.clear(kin::Color::rgb(0, 0, 255));
        renderer.draw_texture(texture, kin::Rectf{0.0f, 0.0f, 16.0f, 16.0f});
        assert(renderer.read_rgba({0.0f, 0.0f, 16.0f, 16.0f}, px, size));
    }
    assert(pixel_near(px, size, 2, 8, kin::Color::rgba(0, 0, 0, 0), 2) ||
           pixel_near(px, size, 2, 8, kin::Color::rgb(0, 0, 255), 2)); // left untouched: transparent
    assert(pixel_near(px, size, 4, 8, kin::Color::rgb(255, 0, 0), 2));
    assert(pixel_near(px, size, 9, 8, kin::Color::rgb(255, 100, 0), 2));
    // Outside the texture: refused.
    assert(!renderer.write_texture(texture, {8, 8}, {9, 1}, [](std::span<kin::u8>) {}));
}

void test_write_texture_on_software_backend() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "write-texture-test", .width = 16, .height = 16, .hidden = true});
    kin::Renderer2D renderer{window};
    check_write_texture(renderer);
}

void test_gpu_write_texture() {
    constexpr std::string_view test_name = "test_gpu_write_texture";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-write-texture-test", .width = 16, .height = 16, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        check_write_texture(*renderer);
        // A fill that uploads would deadlock: it is refused.
        kin::Texture texture = renderer->create_texture_from_rgba(std::vector<kin::u8>(4u * 4u * 4u, 0).data(), {4, 4});
        bool refused = false;
        try {
            renderer->write_texture(texture, {0, 0}, {4, 4}, [&](std::span<kin::u8> texels) {
                std::fill(texels.begin(), texels.end(), kin::u8{0});
                const std::array<kin::u8, 4> red{255, 0, 0, 255};
                renderer->create_texture_from_rgba(red.data(), {1, 1});
            });
        } catch (const std::logic_error&) {
            refused = true;
        }
        assert(refused);
        renderer->present(); // and the renderer still works
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// An empty texture is cleared on the GPU, not sent zeros: even one the pool
// hands back that held other texels comes out transparent.
void test_gpu_empty_textures_are_clear() {
    constexpr std::string_view test_name = "test_gpu_empty_textures_are_clear";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-empty-texture-test", .width = 16, .height = 16, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        {
            const std::vector<kin::u8> red(16u * 16u * 4u, 255);
            const kin::Texture used = renderer->create_texture({16, 16}, kin::TextureFormat::Rgba8, red.data());
        } // to the pool
        renderer->present();
        const auto before = renderer->backend_stats().texture_uploads;
        const kin::Texture empty = renderer->create_texture({16, 16}, kin::TextureFormat::Rgba8, nullptr);
        assert(renderer->backend_stats().texture_uploads == before); // nothing sent
        kin::RenderTarget target = renderer->create_render_target({16, 16}, kin::ScaleMode::Nearest);
        std::vector<kin::u8> px;
        kin::Vec2i size{};
        {
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgb(0, 0, 255));
            renderer->draw_texture(empty, kin::Rectf{0.0f, 0.0f, 16.0f, 16.0f});
            assert(renderer->read_rgba({0.0f, 0.0f, 16.0f, 16.0f}, px, size));
        }
        assert(pixel_near(px, size, 8, 8, kin::Color::rgb(0, 0, 255), 2));
        // A data texture made empty reads zeros too (the data_formats shader
        // shows 0 as black where it would show the value).
        const kin::Texture data = renderer->create_texture({4, 4}, kin::TextureFormat::R32Float, nullptr);
        assert(data.valid());
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// Uploads past the shared staging buffer's size (it grows), copied on workers
// when a job system is set, land where they should.
void test_gpu_big_uploads() {
    constexpr std::string_view test_name = "test_gpu_big_uploads";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-big-upload-test", .width = 64, .height = 64, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        constexpr int n = 2560; // 25 MB of RGBA: more than the 16 MB staging buffer
        std::vector<kin::u8> texels(static_cast<std::size_t>(n) * n * 4);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                kin::u8* t = &texels[(static_cast<std::size_t>(y) * n + x) * 4];
                t[0] = static_cast<kin::u8>(x * 255 / n);
                t[1] = static_cast<kin::u8>(y * 255 / n);
                t[2] = 0;
                t[3] = 255;
            }
        }
        kin::JobSystem jobs{{.workers = 3}};
        for (kin::JobSystem* with : {static_cast<kin::JobSystem*>(nullptr), &jobs}) {
            renderer->set_job_system(with);
            const kin::Texture big = renderer->create_texture_from_rgba(texels.data(), {n, n});
            kin::RenderTarget target = renderer->create_render_target({64, 64}, kin::ScaleMode::Nearest);
            std::vector<kin::u8> px;
            kin::Vec2i size{};
            {
                const auto bind = renderer->scoped_render_target(target);
                renderer->clear(kin::Color::rgb(0, 0, 255));
                // Three texels far apart, each drawn over a 16 x 16 square.
                const auto sample = [&](int tx, int ty, float at) {
                    renderer->draw_texture(big, kin::Rectf{static_cast<float>(tx), static_cast<float>(ty), 1.0f, 1.0f},
                                           kin::Rectf{at, 0.0f, 16.0f, 16.0f});
                };
                sample(0, 0, 0.0f);
                sample(n / 2, n - 1, 16.0f);
                sample(n - 1, n / 4, 32.0f);
                assert(renderer->read_rgba({0.0f, 0.0f, 64.0f, 64.0f}, px, size));
            }
            assert(pixel_near(px, size, 8, 8, kin::Color::rgb(0, 0, 0), 2));
            assert(pixel_near(px, size, 24, 8, kin::Color::rgb(127, 254, 0), 2));
            assert(pixel_near(px, size, 40, 8, kin::Color::rgb(254, 63, 0), 2));
        }
        renderer->set_job_system(nullptr);
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// gpu_scope() times named parts of a frame on the GPU, while timing is on.
void test_gpu_scopes() {
    constexpr std::string_view test_name = "test_gpu_scopes";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-scope-test", .width = 256, .height = 256, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        const std::array<kin::u8, 4> white{255, 255, 255, 255};
        const kin::Texture dot = renderer->create_texture_from_rgba(white.data(), {1, 1});
        const auto frame = [&] {
            renderer->clear(kin::Color::rgb(0, 0, 0));
            {
                const auto heavy = renderer->gpu_scope("heavy");
                const auto ignored = renderer->gpu_scope("nested"); // inside another: ignored
                for (int i = 0; i < 600; ++i) {
                    renderer->draw_texture(dot, kin::Rectf{0.0f, 0.0f, 1.0f, 1.0f}, kin::Rectf{0.0f, 0.0f, 256.0f, 256.0f},
                                           kin::Color::rgba(255, 255, 255, 3));
                }
            }
            {
                const auto light = renderer->gpu_scope("light");
                renderer->fill_rect(kin::Rectf{0.0f, 0.0f, 4.0f, 4.0f}, kin::Color::rgb(255, 0, 0));
            }
            renderer->present();
        };

        // Off: nothing is timed.
        frame();
        frame();
        assert(renderer->take_gpu_scope_timings().empty());

        renderer->set_gpu_timing_enabled(true);
        std::vector<kin::GpuScopeTiming> timings;
        for (int i = 0; i < 30; ++i) {
            frame();
            for (kin::GpuScopeTiming& t : renderer->take_gpu_scope_timings()) {
                timings.push_back(std::move(t));
            }
        }
        // Let the GPU finish, then one more present collects the last of them.
        std::vector<kin::u8> px;
        kin::Vec2i size{};
        assert(renderer->read_rgba({0.0f, 0.0f, 1.0f, 1.0f}, px, size));
        renderer->present();
        for (kin::GpuScopeTiming& t : renderer->take_gpu_scope_timings()) {
            timings.push_back(std::move(t));
        }
        renderer->set_gpu_timing_enabled(false);
        std::vector<double> heavy, light;
        for (const kin::GpuScopeTiming& t : timings) {
            assert(t.name == "heavy" || t.name == "light");
            (t.name == "heavy" ? heavy : light).push_back(t.ms);
            // Each scope's pixels: 600 window-sized quads, or one 4 x 4 rect.
            const double pixels = t.name == "heavy" ? 600.0 * 256 * 256 : 16.0;
            assert(std::abs(t.pixels - pixels) < 1.0 && std::abs(t.overdraw - pixels / (256.0 * 256.0)) < 1e-6);
        }
        if (heavy.size() < 10 || light.size() < 10) { // with the GPU behind, some go untimed
            throw std::runtime_error(std::string(test_name) + ": " + std::to_string(heavy.size()) + " heavy and " +
                                     std::to_string(light.size()) + " light timings of 30 frames");
        }
        std::sort(heavy.begin(), heavy.end());
        std::sort(light.begin(), light.end());
        const double heavy_median = heavy[heavy.size() / 2], light_median = light[light.size() / 2];
        if (!(heavy_median > 0.1 && heavy_median > 5.0 * light_median)) {
            throw std::runtime_error(std::string(test_name) + ": heavy " + std::to_string(heavy_median) +
                                     " ms, light " + std::to_string(light_median) + " ms");
        }
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// A run's pipeline record makes the next run's pipelines while it loads: a
// second renderer handed the first's record has the same pipelines as soon as
// its shader exists, before drawing anything.
void test_gpu_pipeline_record() {
    constexpr std::string_view test_name = "test_gpu_pipeline_record";
    const std::filesystem::path spv = std::filesystem::path{KIN_TEST_SHADER_DIR} / "custom_vertex.frag.spv";
    if (!std::filesystem::exists(spv)) {
        skip_or_require_gpu_test(test_name, "test shader not compiled (glslc not found)");
        return;
    }
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-pipeline-record-test", .width = 32, .height = 32, .hidden = true});
        const std::vector<kin::u8> code = read_spirv(spv);
        kin::ShaderDesc desc{};
        desc.spirv = {code.data(), static_cast<kin::u32>(code.size())};
        const auto lines = [](const std::string& record) {
            std::vector<std::string> out;
            std::istringstream in{record};
            for (std::string line; std::getline(in, line);) {
                out.push_back(line);
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        std::string record;
        {
            std::string unavailable_reason;
            std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
            if (!renderer) {
                skip_or_require_gpu_test(test_name, unavailable_reason);
                return;
            }
            gpu_ready = true;
            const kin::ShaderHandle shader = renderer->create_shader(desc);
            const std::array<kin::ShaderVertex, 3> triangle{{{.position = {0, 0}}, {.position = {32, 0}}, {.position = {0, 32}}}};
            renderer->fill_rect(kin::Rectf{0.0f, 0.0f, 4.0f, 4.0f}, kin::Color::rgb(255, 0, 0));
            {
                const auto blend = renderer->scoped_blend_mode(kin::BlendMode::Max);
                renderer->draw_shader_geometry(triangle, {}, shader, {});
            }
            renderer->present();
            record = renderer->pipeline_record();
            assert(record.starts_with("kin.pipelines/1\n") && lines(record).size() >= 4);
        }
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        renderer->prewarm_pipelines("not a record"); // ignored
        renderer->prewarm_pipelines(record);
        renderer->create_shader(desc); // nothing drawn yet
        if (lines(renderer->pipeline_record()) != lines(record)) {
            throw std::runtime_error(std::string(test_name) + ": the second run did not make the first run's pipelines");
        }
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// Storage buffers: a shader reads entries a draw's vertices pick, updates and
// writes land, and a draw missing its buffer is skipped rather than drawn.
void test_gpu_data_buffers() {
    constexpr std::string_view test_name = "test_gpu_data_buffers";
    const std::filesystem::path spv = std::filesystem::path{KIN_TEST_SHADER_DIR} / "storage_read.frag.spv";
    if (!std::filesystem::exists(spv)) {
        skip_or_require_gpu_test(test_name, "test shader not compiled (glslc not found)");
        return;
    }
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-data-buffer-test", .width = 32, .height = 8, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        assert(renderer->capabilities().data_buffers);
        const std::vector<kin::u8> code = read_spirv(spv);
        kin::ShaderDesc desc{};
        desc.spirv = {code.data(), static_cast<kin::u32>(code.size())};
        const kin::ShaderHandle shader = renderer->create_shader(desc); // counts from the SPIR-V
        assert(shader && renderer->shader_layout(shader)->storage_buffers == 1);

        std::array<float, 16> colors{1, 0, 0, 1,  0, 1, 0, 1,  0, 0, 1, 1,  1, 1, 1, 1};
        const kin::DataBuffer items = renderer->create_data_buffer(sizeof(colors), colors.data());
        assert(items.valid() && items.size() == sizeof(colors));
        // Four 8 x 8 squares, square i reading entry i.
        std::vector<kin::ShaderVertex> vertices;
        for (int i = 0; i < 4; ++i) {
            const float x = static_cast<float>(i * 8);
            const std::array<float, 4> custom{static_cast<float>(i), 0.0f, 0.0f, 0.0f};
            for (const kin::Vec2f p : {kin::Vec2f{x, 0}, kin::Vec2f{x + 8, 0}, kin::Vec2f{x + 8, 8},
                                       kin::Vec2f{x, 0}, kin::Vec2f{x + 8, 8}, kin::Vec2f{x, 8}}) {
                vertices.push_back({.position = p, .custom = custom});
            }
        }
        kin::RenderTarget target = renderer->create_render_target({32, 8}, kin::ScaleMode::Nearest);
        std::vector<kin::u8> px;
        kin::Vec2i size{};
        const auto draw = [&](std::span<const kin::DataBuffer> buffers) {
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgba(0, 0, 0, 0));
            renderer->draw_shader_geometry(vertices, {}, shader, {}, {}, buffers);
            assert(renderer->read_rgba({0.0f, 0.0f, 32.0f, 8.0f}, px, size));
        };
        draw(std::span<const kin::DataBuffer>{&items, 1});
        assert(pixel_near(px, size, 4, 4, kin::Color::rgb(255, 0, 0), 2));
        assert(pixel_near(px, size, 12, 4, kin::Color::rgb(0, 255, 0), 2));
        assert(pixel_near(px, size, 20, 4, kin::Color::rgb(0, 0, 255), 2));
        assert(pixel_near(px, size, 28, 4, kin::Color::rgb(255, 255, 255), 2));

        // One entry updated, another written in place.
        const std::array<float, 4> yellow{1, 1, 0, 1};
        assert(renderer->update_data_buffer(items, 16, 16, yellow.data()));
        assert(renderer->write_data_buffer(items, 32, 16, [](std::span<kin::u8> bytes) {
            const std::array<float, 4> magenta{1, 0, 1, 1};
            std::memcpy(bytes.data(), magenta.data(), bytes.size());
        }));
        assert(!renderer->update_data_buffer(items, 60, 16, yellow.data())); // past the end
        draw(std::span<const kin::DataBuffer>{&items, 1});
        assert(pixel_near(px, size, 12, 4, kin::Color::rgb(255, 255, 0), 2));
        assert(pixel_near(px, size, 20, 4, kin::Color::rgb(255, 0, 255), 2));
        assert(pixel_near(px, size, 4, 4, kin::Color::rgb(255, 0, 0), 2)); // untouched

        // Without its buffer the draw is skipped (an unbound one is a GPU error).
        draw({});
        assert(pixel_near(px, size, 4, 4, kin::Color::rgba(0, 0, 0, 0), 0));
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// A smooth effect drawn at half resolution and stretched back looks the same
// as drawn at full resolution; at 1 it is the plain draw.
void test_gpu_shader_surface_scaled() {
    constexpr std::string_view test_name = "test_gpu_shader_surface_scaled";
    const std::filesystem::path spv = std::filesystem::path{KIN_TEST_SHADER_DIR} / "bench_heavy.frag.spv";
    if (!std::filesystem::exists(spv)) {
        skip_or_require_gpu_test(test_name, "test shader not compiled (glslc not found)");
        return;
    }
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-scaled-test", .width = 64, .height = 64, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        const std::vector<kin::u8> code = read_spirv(spv);
        kin::ShaderDesc desc{};
        desc.spirv = {code.data(), static_cast<kin::u32>(code.size())};
        const kin::ShaderHandle shader = renderer->create_shader(desc);
        kin::RenderTarget target = renderer->create_render_target({64, 64}, kin::ScaleMode::Nearest);
        const auto draw = [&](float resolution) {
            std::vector<kin::u8> px;
            kin::Vec2i size{};
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgb(0, 0, 0));
            renderer->draw_shader_surface_scaled(resolution, {0.0f, 0.0f, 64.0f, 64.0f}, shader, {});
            assert(renderer->read_rgba({0.0f, 0.0f, 64.0f, 64.0f}, px, size));
            return px;
        };
        const std::vector<kin::u8> full = draw(1.0f);
        const std::vector<kin::u8> half = draw(0.5f);
        double error = 0.0;
        for (std::size_t i = 0; i < full.size(); ++i) {
            error += std::abs(static_cast<int>(full[i]) - static_cast<int>(half[i]));
        }
        error /= static_cast<double>(full.size());
        if (error > 3.0) {
            throw std::runtime_error(std::string(test_name) + ": half resolution strays " + std::to_string(error));
        }
        assert(full != std::vector<kin::u8>(full.size(), 0)); // something was drawn
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// Compute: a shader writes a storage texture (reading a uniform and a buffer),
// ordered with the draws around it; layouts and workgroup size from the SPIR-V.
void test_gpu_compute() {
    constexpr std::string_view test_name = "test_gpu_compute";
    const std::filesystem::path spv = std::filesystem::path{KIN_TEST_SHADER_DIR} / "compute_gradient.comp.spv";
    if (!std::filesystem::exists(spv)) {
        skip_or_require_gpu_test(test_name, "test shader not compiled (glslc not found)");
        return;
    }
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-compute-test", .width = 64, .height = 64, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        assert(renderer->capabilities().compute);
        const std::vector<kin::u8> code = read_spirv(spv);
        const kin::ShaderBlob blob{code.data(), static_cast<kin::u32>(code.size())};
        const kin::ShaderLayout layout = *kin::reflect_spirv(blob);
        assert(layout.local_size[0] == 8 && layout.local_size[1] == 8);
        assert(layout.readwrite_storage_textures == 1 && layout.storage_buffers == 1 && layout.uniform_buffers == 1);
        const kin::ComputeShaderHandle shader = renderer->create_compute_shader(blob);
        assert(shader);

        // 60 x 36: not a multiple of the workgroup, so the edges are covered too.
        const kin::Texture image = renderer->create_storage_texture({60, 36});
        assert(image.valid());
        const std::array<float, 4> extra{0, 0, 0, 1};
        const kin::DataBuffer buffer = renderer->create_data_buffer(sizeof(extra), extra.data());
        kin::ShaderParams params;
        params.uniforms[0] = 0.5f;
        kin::RenderTarget target = renderer->create_render_target({60, 36}, kin::ScaleMode::Nearest);
        std::vector<kin::u8> px;
        kin::Vec2i size{};
        {
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgb(0, 0, 0));
            const kin::ComputeBindings bindings{.buffers = std::span<const kin::DataBuffer>{&buffer, 1},
                                                .outputs = std::span<const kin::Texture>{&image, 1},
                                                .params = &params};
            assert(renderer->dispatch_compute(shader, {60, 36}, bindings));
            renderer->draw_texture(image, kin::Rectf{0.0f, 0.0f, 60.0f, 36.0f}); // after the dispatch
            assert(renderer->read_rgba({0.0f, 0.0f, 60.0f, 36.0f}, px, size));
        }
        assert(pixel_near(px, size, 30, 18, kin::Color::rgb(128, 128, 128), 3));
        assert(pixel_near(px, size, 59, 35, kin::Color::rgb(251, 249, 128), 3)); // the far corner
        // Missing the output: refused, nothing run.
        assert(!renderer->dispatch_compute(shader, {60, 36}, kin::ComputeBindings{.params = &params}));
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// Hot reload: a GLSL file compiled at runtime, edited, recompiled and reloaded
// under the same handle; a broken edit keeps the last good shader.
void test_gpu_shader_hot_reload() {
    constexpr std::string_view test_name = "test_gpu_shader_hot_reload";
    if (!kin::shader_compiler_available()) {
        skip_or_require_gpu_test(test_name, "glslc not found");
        return;
    }
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-hot-reload-test", .width = 16, .height = 16, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        const std::filesystem::path source = std::filesystem::temp_directory_path() / "kin-hot-reload-test.frag.glsl";
        const auto write = [&](std::string_view colour, int seconds) {
            std::ofstream{source} << "#version 450 core\n"
                                     "layout(location = 0) out vec4 fColor;\n"
                                     "layout(location = 0) in struct { vec4 Color; vec2 UV; } In;\n"
                                     "void main() { fColor = "
                                  << colour << "; }\n";
            // Edits land in the same second in a test: make the change visible.
            std::filesystem::last_write_time(source, std::filesystem::file_time_type::clock::now() +
                                                         std::chrono::seconds{seconds});
        };
        write("vec4(1.0, 0.0, 0.0, 1.0)", 0);
        kin::ShaderFile shader{*renderer, source};
        assert(shader.handle() && shader.error().empty());
        kin::RenderTarget target = renderer->create_render_target({16, 16}, kin::ScaleMode::Nearest);
        const auto draw = [&] {
            std::vector<kin::u8> px;
            kin::Vec2i size{};
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgb(0, 0, 0));
            renderer->draw_shader_surface({0.0f, 0.0f, 16.0f, 16.0f}, shader.handle(), {});
            assert(renderer->read_rgba({0.0f, 0.0f, 16.0f, 16.0f}, px, size));
            return kin::Color::rgba(px[0], px[1], px[2], px[3]);
        };
        assert(color_near(draw(), kin::Color::rgb(255, 0, 0)));
        assert(!shader.poll()); // unchanged
        const kin::ShaderHandle handle = shader.handle();
        write("vec4(0.0, 1.0, 0.0, 1.0)", 2);
        assert(shader.poll() && shader.handle() == handle);
        assert(color_near(draw(), kin::Color::rgb(0, 255, 0)));
        write("vec4(0.0, 0.0, 1.0 1.0)", 4); // a typo
        assert(!shader.poll() && !shader.error().empty());
        assert(color_near(draw(), kin::Color::rgb(0, 255, 0))); // the last good one
        std::filesystem::remove(source);
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// Mipmaps: a 1-pixel checkerboard drawn 16 times smaller is an even grey with
// them, and aliases (pixels far from grey) without; an update remakes them.
void test_gpu_mipmaps() {
    constexpr std::string_view test_name = "test_gpu_mipmaps";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-mipmap-test", .width = 16, .height = 16, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        constexpr int n = 256;
        std::vector<kin::u8> checker(static_cast<std::size_t>(n) * n * 4);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const kin::u8 v = (x + y) % 2 ? 255 : 0;
                kin::u8* t = &checker[(static_cast<std::size_t>(y) * n + x) * 4];
                t[0] = t[1] = t[2] = v;
                t[3] = 255;
            }
        }
        kin::RenderTarget target = renderer->create_render_target({16, 16}, kin::ScaleMode::Nearest);
        const auto drawn_small = [&](const kin::Texture& texture) {
            std::vector<kin::u8> px;
            kin::Vec2i size{};
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgb(0, 0, 0));
            // Offset a little, so plain filtering lands between texels unevenly.
            renderer->draw_texture(texture, kin::Rectf{0.3f, 0.3f, 15.7f, 15.7f});
            assert(renderer->read_rgba({0.0f, 0.0f, 16.0f, 16.0f}, px, size));
            double off_grey = 0.0;
            int counted = 0;
            for (int y = 2; y < 14; ++y) {
                for (int x = 2; x < 14; ++x) {
                    off_grey += std::abs(static_cast<int>(px[(static_cast<std::size_t>(y) * 16 + x) * 4]) - 128);
                    ++counted;
                }
            }
            return std::pair{off_grey / counted, kin::Color::rgba(px[8 * 64 + 32], px[8 * 64 + 33], px[8 * 64 + 34], 255)};
        };
        const kin::Texture plain = renderer->create_texture_from_rgba(checker.data(), {n, n});
        renderer->set_scale_mode(plain, kin::ScaleMode::Linear);
        const kin::Texture smooth = renderer->create_texture_from_rgba(checker.data(), {n, n});
        renderer->set_scale_mode(smooth, kin::ScaleMode::Mipmapped);
        const double aliased = drawn_small(plain).first;
        const double mipmapped = drawn_small(smooth).first;
        if (!(mipmapped < 8.0 && aliased > 3.0 * mipmapped)) {
            throw std::runtime_error(std::string(test_name) + ": off grey " + std::to_string(mipmapped) +
                                     " with mipmaps, " + std::to_string(aliased) + " without");
        }
        // Updating it remakes the smaller levels.
        std::vector<kin::u8> red(static_cast<std::size_t>(n) * n * 4);
        for (std::size_t i = 0; i < red.size(); i += 4) {
            red[i] = 255;
            red[i + 3] = 255;
        }
        assert(renderer->update_texture(smooth, {0, 0}, {n, n}, red.data()));
        assert(color_near(drawn_small(smooth).second, kin::Color::rgb(255, 0, 0), 2));
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// Overdraw: the area a frame's draws cover over the screen's.
void test_gpu_overdraw() {
    constexpr std::string_view test_name = "test_gpu_overdraw";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-overdraw-test", .width = 64, .height = 32, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        renderer->present();
        renderer->clear(kin::Color::rgb(0, 0, 0)); // a clear covers nothing
        renderer->fill_rect(kin::Rectf{0.0f, 0.0f, 64.0f, 32.0f}, kin::Color::rgb(255, 0, 0));
        renderer->fill_rect(kin::Rectf{0.0f, 0.0f, 64.0f, 32.0f}, kin::Color::rgba(0, 255, 0, 128));
        const std::array<kin::u8, 4> white{255, 255, 255, 255};
        const kin::Texture dot = renderer->create_texture_from_rgba(white.data(), {1, 1});
        const std::array<kin::SpriteInstance, 1> sprite{{{.dest = {0.0f, 0.0f, 32.0f, 32.0f}}}};
        renderer->draw_sprites(dot, sprite); // half the screen
        renderer->present();
        const kin::RendererBackendStats stats = renderer->backend_stats();
        if (std::abs(stats.last_overdraw - 2.5) > 0.01 || std::abs(stats.last_pixels_drawn - 2.5 * 64 * 32) > 1.0) {
            throw std::runtime_error(std::string(test_name) + ": overdraw " + std::to_string(stats.last_overdraw));
        }
        // At a logical size the scene is shaded at its native size: one fill
        // of the logical screen is still once over every pixel.
        renderer->set_logical_size({32, 16});
        renderer->fill_rect(kin::Rectf{0.0f, 0.0f, 32.0f, 16.0f}, kin::Color::rgb(255, 0, 0));
        renderer->present();
        if (std::abs(renderer->backend_stats().last_overdraw - 1.0) > 0.01) {
            throw std::runtime_error(std::string(test_name) + ": logical overdraw " +
                                     std::to_string(renderer->backend_stats().last_overdraw));
        }

        // The overdraw view: each draw adds one layer (8 in red) where it covers,
        // whatever it draws; the presented frame shows them as colours.
        renderer->set_overdraw_view(true);
        renderer->clear(kin::Color::rgb(200, 200, 200)); // counted from black regardless
        renderer->fill_rect(kin::Rectf{0.0f, 0.0f, 16.0f, 16.0f}, kin::Color::rgb(0, 0, 255));
        renderer->draw_texture(dot, kin::Rectf{0.0f, 0.0f, 1.0f, 1.0f}, kin::Rectf{8.0f, 0.0f, 16.0f, 16.0f},
                               kin::Color::rgba(0, 255, 0, 10));
        std::vector<kin::u8> px;
        kin::Vec2i size{};
        assert(renderer->read_rgba({0.0f, 0.0f, 32.0f, 16.0f}, px, size)); // the counts
        const auto layers = [&](int x) { return (px[(static_cast<std::size_t>(8) * size.x + x * size.x / 32) * 4] + 4) / 8; };
        assert(layers(4) == 1 && layers(12) == 2 && layers(20) == 1 && layers(28) == 0);
        renderer->present();
        renderer->set_overdraw_view(false);
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// Layers: drawn into a target in the same coordinates and laid over once, at
// one opacity (so overlaps inside darken once), over only what was drawn.
void test_gpu_layers() {
    constexpr std::string_view test_name = "test_gpu_layers";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-layer-test", .width = 64, .height = 32, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        const kin::Color black = kin::Color::rgb(0, 0, 0);
        const auto frame = [&](float resolution, bool layered) {
            renderer->clear(kin::Color::rgb(255, 255, 255));
            {
                std::optional<kin::Renderer2D::LayerGuard> layer;
                if (layered) {
                    layer.emplace(renderer->begin_layer({.opacity = 0.5f, .resolution = resolution}));
                }
                const kin::Color c = layered ? black : kin::Color::rgba(0, 0, 0, 128);
                renderer->fill_rect(kin::Rectf{8.0f, 8.0f, 16.0f, 16.0f}, c);
                renderer->fill_rect(kin::Rectf{16.0f, 8.0f, 16.0f, 16.0f}, c); // overlaps x 16..24
            }
            std::vector<kin::u8> px;
            kin::Vec2i size{};
            assert(renderer->read_rgba({0.0f, 0.0f, 64.0f, 32.0f}, px, size));
            renderer->present();
            const auto at = [&](int x, int y) {
                const std::size_t i = (static_cast<std::size_t>(y * size.y / 32) * size.x + x * size.x / 64) * 4;
                return kin::Color::rgb(px[i], px[i + 1], px[i + 2]);
            };
            return std::array{at(12, 16), at(20, 16), at(40, 16)};
        };
        // Without a layer the overlap darkens twice; with one, once.
        const auto plain = frame(1.0f, false);
        assert(color_near(plain[0], kin::Color::rgb(127, 127, 127), 3) && color_near(plain[1], kin::Color::rgb(63, 63, 63), 4));
        const auto layered = frame(1.0f, true);
        assert(color_near(layered[0], kin::Color::rgb(127, 127, 127), 3));
        assert(color_near(layered[1], kin::Color::rgb(127, 127, 127), 3));
        assert(color_near(layered[2], kin::Color::rgb(255, 255, 255), 0));
        // Only what was drawn is laid over: 24 x 16 drawn into it twice-ish
        // (two 16 x 16 squares), and that box once over the screen.
        const double pixels = renderer->backend_stats().last_pixels_drawn;
        if (std::abs(pixels - (2 * 16 * 16 + 24 * 16)) > 1.0) {
            throw std::runtime_error(std::string(test_name) + ": " + std::to_string(pixels) + " pixels drawn");
        }
        // At half resolution, under a logical size, things stay where they are.
        renderer->set_logical_size({64, 32});
        const auto half = frame(0.5f, true);
        assert(color_near(half[0], kin::Color::rgb(127, 127, 127), 4) && color_near(half[1], kin::Color::rgb(127, 127, 127), 4));
        assert(color_near(half[2], kin::Color::rgb(255, 255, 255), 0));
        // A clip set outside holds inside; layers nest.
        renderer->clear(kin::Color::rgb(255, 255, 255));
        renderer->push_clip({0.0f, 0.0f, 16.0f, 32.0f});
        {
            const auto outer = renderer->begin_layer({.opacity = 1.0f});
            const auto inner = renderer->begin_layer({.opacity = 0.5f});
            renderer->fill_rect(kin::Rectf{8.0f, 8.0f, 16.0f, 16.0f}, black);
        }
        renderer->pop_clip();
        std::vector<kin::u8> px;
        kin::Vec2i size{};
        assert(renderer->read_rgba({0.0f, 0.0f, 64.0f, 32.0f}, px, size));
        const auto red_at = [&](int x) { return px[(static_cast<std::size_t>(16 * size.y / 32) * size.x + x * size.x / 64) * 4]; };
        assert(std::abs(red_at(12) - 127) <= 4); // inside the clip
        assert(red_at(20) == 255);               // outside it
        renderer->present();
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// Texture uploads are batched: many in a frame go in one command buffer ahead
// of it, so an update applies to the whole frame it is made in. A whole-texture
// update of a texture the frame hasn't drawn yet cycles its storage (no wait on
// earlier frames still reading it) and must still show.
void test_gpu_uploads_batch_in_order() {
    constexpr std::string_view test_name = "test_gpu_uploads_batch_in_order";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-upload-test", .width = 32, .height = 16, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        renderer->present(); // whatever setup uploaded is out of the way

        const auto before = renderer->backend_stats();
        std::vector<kin::Texture> textures;
        std::vector<kin::u8> texels(64u * 64u * 4u, 77);
        for (int i = 0; i < 50; ++i) {
            textures.push_back(renderer->create_texture_from_rgba(texels.data(), {64, 64}));
            assert(renderer->update_texture(textures.back(), {8, 8}, {4, 4}, texels.data()));
        }
        const std::array<kin::u8, 4> red{255, 0, 0, 255};
        const std::array<kin::u8, 4> green{0, 255, 0, 255};
        kin::Texture swatch = renderer->create_texture_from_rgba(red.data(), {1, 1});
        kin::RenderTarget target = renderer->create_render_target({32, 16}, kin::ScaleMode::Nearest);
        std::vector<kin::u8> px;
        kin::Vec2i size{};
        {
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgb(0, 0, 0));
            renderer->draw_texture(swatch, kin::Rectf{0.0f, 0.0f, 16.0f, 16.0f});
            assert(renderer->update_texture(swatch, {0, 0}, {1, 1}, green.data())); // the whole texture
            renderer->draw_texture(swatch, kin::Rectf{16.0f, 0.0f, 16.0f, 16.0f});
            assert(renderer->read_rgba({0.0f, 0.0f, 32.0f, 16.0f}, px, size));
        }
        const auto after = renderer->backend_stats();
        assert(after.texture_uploads - before.texture_uploads == 102);
        assert(after.texture_upload_submits - before.texture_upload_submits == 1);
        assert(pixel_near(px, size, 8, 8, kin::Color::rgb(0, 255, 0), 2)); // drawn before the update, in its frame
        assert(pixel_near(px, size, 24, 8, kin::Color::rgb(0, 255, 0), 2));

        // Next frame: replaced whole before it is drawn (cycled), then drawn.
        renderer->present();
        assert(renderer->update_texture(swatch, {0, 0}, {1, 1}, red.data()));
        {
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgb(0, 0, 0));
            renderer->draw_texture(swatch, kin::Rectf{0.0f, 0.0f, 32.0f, 16.0f});
            assert(renderer->read_rgba({0.0f, 0.0f, 32.0f, 16.0f}, px, size));
        }
        assert(pixel_near(px, size, 16, 8, kin::Color::rgb(255, 0, 0), 2));
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// Triangles with a material shader: only what they cover is drawn, each vertex
// carries its own `custom`, indices and plain lists both work, and with Max
// overlapping shapes combine by the larger.
void test_gpu_shader_geometry() {
    constexpr std::string_view test_name = "test_gpu_shader_geometry";
    const std::filesystem::path spv = std::filesystem::path{KIN_TEST_SHADER_DIR} / "custom_vertex.frag.spv";
    if (!std::filesystem::exists(spv)) {
        skip_or_require_gpu_test(test_name, "test shader not compiled (glslc not found)");
        return;
    }
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-shader-geometry-test", .width = 32, .height = 16, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        assert(renderer->capabilities().shader_geometry);

        std::ifstream file(spv, std::ios::binary);
        const std::vector<kin::u8> code{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        kin::ShaderDesc desc{};
        desc.spirv = {code.data(), static_cast<kin::u32>(code.size())};
        desc.num_samplers = 1;
        const kin::ShaderHandle shader = renderer->create_shader(desc);
        assert(shader);

        const auto vertex = [](float x, float y, std::array<float, 4> custom) {
            return kin::ShaderVertex{.position = {x, y}, .custom = custom};
        };
        constexpr std::array<float, 4> red{1.0f, 0.0f, 0.0f, 1.0f};
        constexpr std::array<float, 4> green{0.0f, 0.5f, 0.0f, 1.0f};
        // A quad from four vertices and six indices (left half), and one triangle
        // as a plain list (right half, below its diagonal).
        const std::array<kin::ShaderVertex, 4> quad{vertex(0, 0, red), vertex(16, 0, red), vertex(16, 16, red),
                                                    vertex(0, 16, red)};
        const std::array<kin::u32, 6> quad_indices{0, 1, 2, 0, 2, 3};
        const std::array<kin::ShaderVertex, 3> triangle{vertex(16, 0, green), vertex(32, 16, green),
                                                        vertex(16, 16, green)};
        kin::RenderTarget target = renderer->create_render_target({32, 16}, kin::ScaleMode::Nearest);
        std::vector<kin::u8> px;
        kin::Vec2i size{};
        const auto draw = [&](auto&& body) {
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgba(0, 0, 0, 0));
            body();
            assert(renderer->read_rgba({0.0f, 0.0f, 32.0f, 16.0f}, px, size));
        };
        draw([&] {
            renderer->draw_shader_geometry(quad, quad_indices, shader, {});
            renderer->draw_shader_geometry(triangle, {}, shader, {});
        });
        assert(pixel_near(px, size, 8, 8, kin::Color::rgb(255, 0, 0), 2));
        assert(pixel_near(px, size, 20, 13, kin::Color::rgb(0, 128, 0), 3));  // below the diagonal
        assert(pixel_near(px, size, 28, 3, kin::Color::rgba(0, 0, 0, 0), 0)); // above it: not covered

        // Bad geometry is refused whole.
        const std::array<kin::u32, 3> past_the_end{0, 1, 9};
        draw([&] { renderer->draw_shader_geometry(quad, past_the_end, shader, {}); });
        assert(pixel_near(px, size, 8, 8, kin::Color::rgba(0, 0, 0, 0), 0));

        // Two overlapping shapes with Max: the overlap takes the larger.
        constexpr std::array<float, 4> dim{0.4f, 0.4f, 0.4f, 1.0f};
        constexpr std::array<float, 4> bright{0.8f, 0.2f, 0.8f, 1.0f};
        const std::array<kin::ShaderVertex, 6> left{vertex(0, 0, dim), vertex(20, 0, dim), vertex(20, 16, dim),
                                                    vertex(0, 0, dim), vertex(20, 16, dim), vertex(0, 16, dim)};
        const std::array<kin::ShaderVertex, 6> right{vertex(12, 0, bright), vertex(32, 0, bright),
                                                     vertex(32, 16, bright), vertex(12, 0, bright),
                                                     vertex(32, 16, bright), vertex(12, 16, bright)};
        draw([&] {
            const auto blend = renderer->scoped_blend_mode(kin::BlendMode::Max);
            renderer->draw_shader_geometry(left, {}, shader, {});
            renderer->draw_shader_geometry(right, {}, shader, {});
        });
        assert(pixel_near(px, size, 4, 8, kin::Color::rgb(102, 102, 102), 2));
        assert(pixel_near(px, size, 16, 8, kin::Color::rgb(204, 102, 204), 2)); // the overlap
        assert(pixel_near(px, size, 28, 8, kin::Color::rgb(204, 51, 204), 2));
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// Shader layouts read from SPIR-V: the test shaders' and the engine's.
void test_shader_reflection() {
    const std::filesystem::path tests{KIN_TEST_SHADER_DIR};
    if (!std::filesystem::exists(tests / "four_sources.frag.spv")) {
        return; // glslc not found: nothing compiled to read
    }
    const auto reflect = [](const std::filesystem::path& path) {
        const std::vector<kin::u8> code = read_spirv(path);
        std::string error;
        const std::optional<kin::ShaderLayout> layout =
            kin::reflect_spirv({code.data(), static_cast<kin::u32>(code.size())}, &error);
        if (!layout) {
            throw std::runtime_error(path.string() + ": " + error);
        }
        return *layout;
    };
    const kin::ShaderLayout four = reflect(tests / "four_sources.frag.spv");
    assert(four.samplers == 4 && four.uniform_buffers == 0 && four.storage_buffers == 0);
    const kin::ShaderLayout formats = reflect(tests / "data_formats.frag.spv");
    assert(formats.samplers == 3 && formats.uniform_buffers == 1);
    assert(formats.uniform_bytes == 128);
    const kin::ShaderParamInfo* u = formats.find("u");
    assert(u && u->offset == 0 && u->size == 128);
    assert(reflect(tests / "custom_vertex.frag.spv").samplers == 1);
    // Every engine shader reads.
    for (const auto& entry : std::filesystem::directory_iterator{KIN_GPU_SHADER_DIR_FOR_TESTS}) {
        if (entry.path().extension() == ".spv") {
            reflect(entry.path());
        }
    }
    // Not SPIR-V: refused, with a reason.
    const std::array<kin::u8, 8> junk{1, 2, 3, 4, 5, 6, 7, 8};
    std::string error;
    assert(!kin::reflect_spirv({junk.data(), 8}, &error) && !error.empty());

    // Params by name write where the shader reads.
    kin::ShaderParams params;
    params.layout = std::make_shared<const kin::ShaderLayout>(formats);
    const std::array<float, 4> fifth{0.0f, 0.25f, 0.0f, 0.0f};
    std::vector<float> u_values(24, 0.0f);
    std::copy(fifth.begin(), fifth.end(), u_values.begin() + 20);
    assert(params.set("u", u_values));
    assert(params.uniforms[21] == 0.25f);
    assert(!params.set("nope", 1.0f));
    assert(!params.set("u", std::vector<float>(33, 0.0f))); // past the member
}

// Max and Min blending: a fill and a texture over a cleared target. Returns
// the pixels at (8, 8) (the fill) and (24, 8) (the texture) for each mode.
std::array<kin::Color, 4> min_max_blended(kin::Renderer2D& renderer) {
    const std::array<kin::u8, 4> texel{50, 220, 120, 255};
    const kin::Texture texture = renderer.create_texture_from_rgba(texel.data(), {1, 1});
    std::array<kin::Color, 4> out{};
    std::size_t i = 0;
    for (const kin::BlendMode mode : {kin::BlendMode::Max, kin::BlendMode::Min}) {
        kin::RenderTarget target = renderer.create_render_target({32, 16}, kin::ScaleMode::Nearest);
        assert(target.valid());
        const auto bind = renderer.scoped_render_target(target);
        renderer.clear(kin::Color::rgb(100, 50, 200));
        {
            const auto blend = renderer.scoped_blend_mode(mode);
            renderer.fill_rect(kin::Rectf{0.0f, 0.0f, 16.0f, 16.0f}, kin::Color::rgb(200, 20, 100));
            renderer.draw_texture(texture, kin::Rectf{16.0f, 0.0f, 16.0f, 16.0f});
        }
        std::vector<kin::u8> px;
        kin::Vec2i size{};
        assert(renderer.read_rgba({0.0f, 0.0f, 32.0f, 16.0f}, px, size));
        const auto at = [&](int x, int y) {
            const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.x) + static_cast<std::size_t>(x)) * 4u;
            return kin::Color::rgba(px[idx], px[idx + 1], px[idx + 2], px[idx + 3]);
        };
        out[i++] = at(8, 8);
        out[i++] = at(24, 8);
    }
    return out;
}

void test_min_max_blend_on_software_backend() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "min-max-test", .width = 32, .height = 16, .hidden = true});
    kin::Renderer2D renderer{window};
    const std::array<kin::Color, 4> px = min_max_blended(renderer);
    if (renderer.capabilities().min_max_blend) {
        assert(color_near(px[0], kin::Color::rgb(200, 50, 200)));
        assert(color_near(px[3], kin::Color::rgb(50, 50, 120)));
    } else {
        // Drawn as Alpha (and a warning logged): plain opaque draws.
        assert(color_near(px[0], kin::Color::rgb(200, 20, 100)));
        assert(color_near(px[1], kin::Color::rgb(50, 220, 120)));
    }
}

void test_min_max_blend_on_gpu_backend() {
    constexpr std::string_view test_name = "test_min_max_blend_on_gpu_backend";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-min-max-test", .width = 32, .height = 16, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        assert(renderer->capabilities().min_max_blend);
        const std::array<kin::Color, 4> px = min_max_blended(*renderer);
        const std::array<kin::Color, 4> want{kin::Color::rgb(200, 50, 200), kin::Color::rgb(100, 220, 200),
                                             kin::Color::rgb(100, 20, 100), kin::Color::rgb(50, 50, 120)};
        for (std::size_t i = 0; i < px.size(); ++i) {
            if (!color_near(px[i], want[i])) {
                throw std::runtime_error(std::string(test_name) + ": pixel " + std::to_string(i) + " is " +
                                         std::to_string(px[i].r) + "," + std::to_string(px[i].g) + "," +
                                         std::to_string(px[i].b) + "," + std::to_string(px[i].a));
            }
        }
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// A cached target is drawn again only when its key or size changes (or it
// is invalidated), and keeps what it was drawn with in between.
void check_cached_target(kin::Renderer2D& renderer) {
    kin::CachedTarget cache{kin::ScaleMode::Nearest};
    const auto draw_in = [&](kin::Color color) {
        const auto bind = renderer.scoped_render_target(cache.target());
        renderer.clear(kin::Color::rgba(0, 0, 0, 0));
        renderer.fill_rect(kin::Rectf{0.0f, 0.0f, 16.0f, 16.0f}, color);
    };
    const kin::u64 key = kin::cache_key(1, 2.5f);
    assert(key == kin::cache_key(1, 2.5f) && key != kin::cache_key(1, 2.6f));
    assert(cache.stale(renderer, {16, 16}, key));
    draw_in(kin::Color::rgb(255, 0, 0));
    for (int frame = 0; frame < 3; ++frame) {
        assert(!cache.stale(renderer, {16, 16}, key)); // reused
        kin::RenderTarget screen = renderer.create_render_target({16, 16}, kin::ScaleMode::Nearest);
        std::vector<kin::u8> px;
        kin::Vec2i size{};
        {
            const auto bind = renderer.scoped_render_target(screen);
            renderer.clear(kin::Color::rgb(0, 0, 0));
            renderer.draw_texture(cache.texture(), kin::Rectf{0.0f, 0.0f, 16.0f, 16.0f});
            assert(renderer.read_rgba({0.0f, 0.0f, 16.0f, 16.0f}, px, size));
        }
        assert(pixel_near(px, size, 8, 8, kin::Color::rgb(255, 0, 0), 2)); // what it was drawn with
        renderer.present();
    }
    assert(cache.stale(renderer, {16, 16}, kin::cache_key(2)));   // a new key
    assert(!cache.stale(renderer, {16, 16}, kin::cache_key(2)));
    assert(cache.stale(renderer, {32, 16}, kin::cache_key(2)));   // a new size
    cache.invalidate();
    assert(cache.stale(renderer, {32, 16}, kin::cache_key(2)));   // invalidated
    assert(cache.reuses() == 4 && cache.redraws() == 4);
}

// Without layer targets (SDL's software renderer) a layer draws straight
// through: what is drawn in it still lands, and the guard ends cleanly.
void test_layers_on_software_backend() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "layer-fallback-test", .width = 32, .height = 16, .hidden = true});
    kin::Renderer2D renderer{window};
    kin::RenderTarget target = renderer.create_render_target({32, 16}, kin::ScaleMode::Nearest);
    std::vector<kin::u8> px;
    kin::Vec2i size{};
    {
        const auto bind = renderer.scoped_render_target(target);
        renderer.clear(kin::Color::rgb(255, 255, 255));
        {
            const auto outer = renderer.begin_layer({.opacity = 0.5f});
            const auto inner = renderer.begin_layer();
            renderer.fill_rect(kin::Rectf{0.0f, 0.0f, 16.0f, 16.0f}, kin::Color::rgb(0, 0, 0));
        }
        renderer.fill_rect(kin::Rectf{16.0f, 0.0f, 16.0f, 16.0f}, kin::Color::rgb(255, 0, 0)); // after: not in a layer
        assert(renderer.read_rgba({0.0f, 0.0f, 32.0f, 16.0f}, px, size));
    }
    // SDL's renderer has layers too: half of the black over the white.
    assert(pixel_near(px, size, 8, 8, kin::Color::rgb(128, 128, 128), 3));
    assert(pixel_near(px, size, 24, 8, kin::Color::rgb(255, 0, 0), 2));
}

// Shared by the software (SDL) and GPU backend tests: transforms map shapes,
// textures, sprite batches and lines the same way on both.
void check_transforms(kin::Renderer2D& renderer) {
    using kin::Affine2;
    const kin::Color white = kin::Color::rgb(255, 255, 255);
    const kin::Color red = kin::Color::rgb(255, 0, 0);
    const kin::Color green = kin::Color::rgb(0, 255, 0);
    const kin::Color blue = kin::Color::rgb(0, 0, 255);
    const kin::Color black = kin::Color::rgb(0, 0, 0);
    std::vector<kin::u8> pixels(4 * 4 * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 2] = 255;
        pixels[i + 3] = 255;
    }
    const kin::Texture texture = renderer.create_texture_from_rgba(pixels.data(), {4, 4});
    kin::RenderTarget target = renderer.create_render_target({64, 64}, kin::ScaleMode::Nearest);

    // Outside the target a transform is set; inside it starts untransformed.
    const auto outside = renderer.scoped_transform(Affine2::translation({5.0f, 5.0f}));
    std::vector<kin::u8> px;
    kin::Vec2i size{};
    {
        const auto bind = renderer.scoped_render_target(target);
        assert(renderer.transform().is_identity());
        renderer.clear(white);
        {
            // Moved and scaled: a 4 x 4 square at (8, 8), 8 x 8.
            const auto t = renderer.scoped_transform(Affine2::translation({8.0f, 8.0f}));
            const auto s = renderer.scoped_transform(Affine2::scaling({2.0f, 2.0f}));
            renderer.fill_rect({0.0f, 0.0f, 4.0f, 4.0f}, red);
        }
        {
            // Turned a quarter about (40, 16): the 12 x 4 bar stands up, x 36..40, y 16..28.
            const auto t = renderer.scoped_transform(Affine2::translation({40.0f, 16.0f}) * Affine2::rotation(90.0f) *
                                                     Affine2::translation({-40.0f, -16.0f}));
            renderer.fill_rect({40.0f, 16.0f, 12.0f, 4.0f}, green);
        }
        {
            // Sprites turned and scaled evenly (still sprites): x 12..16, y 40..48.
            const auto t = renderer.scoped_transform(Affine2::translation({16.0f, 40.0f}) * Affine2::rotation(90.0f) *
                                                     Affine2::scaling({2.0f, 2.0f}));
            const std::array<kin::SpriteInstance, 2> sprites{{
                {.dest = {0.0f, 0.0f, 4.0f, 2.0f}},
                {.dest = {0.0f, 0.0f, 4.0f, 2.0f}},
            }};
            renderer.draw_sprites(texture, sprites);
        }
        {
            // Squashed, then turned (drawn quad by quad): x 44..48, y 40..48.
            const auto t = renderer.scoped_transform(Affine2::translation({48.0f, 40.0f}) * Affine2::rotation(90.0f) *
                                                     Affine2::scaling({2.0f, 1.0f}));
            const std::array<kin::SpriteInstance, 2> sprites{{
                {.dest = {0.0f, 0.0f, 4.0f, 4.0f}},
                {.dest = {0.0f, 0.0f, 4.0f, 4.0f}},
            }};
            renderer.draw_sprites(texture, sprites);
        }
        {
            // A line widens with the scale: x 2..18, y 56..60.
            const auto t = renderer.scoped_transform(Affine2::translation({0.0f, 56.0f}) * Affine2::scaling({4.0f, 4.0f}));
            renderer.draw_line({0.0f, 0.0f}, {4.0f, 0.0f}, black);
            // Nested targets start untransformed too, and give it back.
            const kin::Affine2 before = renderer.transform();
            {
                kin::RenderTarget inner = renderer.create_render_target({4, 4}, kin::ScaleMode::Nearest);
                const auto bind_inner = renderer.scoped_render_target(inner);
                assert(renderer.transform().is_identity());
            }
            assert(renderer.transform() == before);
        }
        assert(renderer.read_rgba({0.0f, 0.0f, 64.0f, 64.0f}, px, size));
    }
    assert((renderer.transform() == Affine2::translation({5.0f, 5.0f})));
    const auto expect = [&](int x, int y, kin::Color color, const char* what) {
        if (!pixel_near(px, size, x, y, color, 2)) {
            const std::size_t i = (static_cast<std::size_t>(y) * size.x + x) * 4;
            throw std::runtime_error(std::string("check_transforms (") + std::string(renderer.backend_name()) + "): " +
                                     what + " at " + std::to_string(x) + "," + std::to_string(y) + " is " +
                                     std::to_string(px[i]) + "," + std::to_string(px[i + 1]) + "," + std::to_string(px[i + 2]));
        }
    };
    expect(12, 12, red, "scaled square");
    expect(20, 12, white, "past the scaled square");
    expect(6, 6, white, "before the scaled square");
    expect(38, 24, green, "turned bar");
    expect(46, 18, white, "where the bar was");
    expect(14, 44, blue, "turned sprites");
    expect(18, 44, white, "past the turned sprites");
    expect(46, 46, blue, "squashed sprites");
    expect(50, 46, white, "past the squashed sprites");
    expect(10, 58, black, "scaled line");
    expect(10, 62, white, "under the scaled line");
}

// Shared by the software (SDL) and GPU backend tests: shapes land where they
// should on both, with soft edges.
void check_shapes(kin::Renderer2D& renderer) {
    const kin::Color white = kin::Color::rgb(255, 255, 255);
    const kin::Color red = kin::Color::rgb(255, 0, 0);
    const kin::Color green = kin::Color::rgb(0, 255, 0);
    const kin::Color blue = kin::Color::rgb(0, 0, 255);
    const kin::Color black = kin::Color::rgb(0, 0, 0);
    assert(renderer.capabilities().shapes);
    kin::RenderTarget target = renderer.create_render_target({64, 64}, kin::ScaleMode::Nearest);
    kin::Shape square;
    square.fill(kin::Path::rect({0, 0, 1, 1}), kin::colors::white);
    const kin::ShapeMesh unit = square.mesh();
    std::vector<kin::u8> px;
    kin::Vec2i size{};
    {
        const auto bind = renderer.scoped_render_target(target);
        renderer.clear(white);
        renderer.fill_circle({16.0f, 16.0f}, 10.0f, red);
        renderer.draw_line({32.0f, 8.0f}, {60.0f, 8.0f}, blue, 4.0f);
        renderer.draw_circle({48.0f, 40.0f}, 8.0f, green, 2.0f);
        // A unit square made once, drawn 6 x 6 at (10, 44) and 10 x 10 at (30, 50).
        renderer.draw_shape(unit, kin::Affine2::translation({10.0f, 44.0f}) * kin::Affine2::scaling({6.0f, 6.0f}), black);
        renderer.draw_shape(unit, kin::Affine2::translation({30.0f, 50.0f}) * kin::Affine2::scaling({10.0f, 10.0f}),
                            black);
        assert(renderer.read_rgba({0.0f, 0.0f, 64.0f, 64.0f}, px, size));
    }
    const auto at = [&](int x, int y) {
        const std::size_t i = (static_cast<std::size_t>(y) * size.x + x) * 4;
        return kin::Color::rgb(px[i], px[i + 1], px[i + 2]);
    };
    const auto expect = [&](int x, int y, kin::Color color, const char* what) {
        if (!pixel_near(px, size, x, y, color, 3)) {
            const kin::Color c = at(x, y);
            throw std::runtime_error(std::string("check_shapes (") + std::string(renderer.backend_name()) + "): " + what +
                                     " at " + std::to_string(x) + "," + std::to_string(y) + " is " + std::to_string(c.r) +
                                     "," + std::to_string(c.g) + "," + std::to_string(c.b));
        }
    };
    expect(16, 16, red, "circle");
    expect(16, 29, white, "past the circle");
    expect(46, 7, blue, "thick line");
    expect(46, 12, white, "past the thick line");
    expect(48, 32, green, "ring");
    expect(48, 40, white, "inside the ring");
    expect(12, 46, black, "scaled mesh");
    expect(15, 49, black, "scaled mesh's far corner");
    expect(18, 46, white, "past the scaled mesh");
    expect(35, 55, black, "the bigger mesh");
    // The circle's edge is soft: half way between red and white where the
    // outline crosses the pixel (below the centre, 10.5 out at y = 26).
    const kin::Color edge = at(16, 26);
    if (!(edge.r > 240 && edge.g > 40 && edge.g < 215)) {
        throw std::runtime_error(std::string("check_shapes (") + std::string(renderer.backend_name()) +
                                 "): the circle's edge pixel is " + std::to_string(edge.g) + " green, not soft");
    }
    // Ten times bigger, the edge is still one pixel soft: 1.5 pixels out is clear.
    expect(41, 55, white, "1.5 px past the bigger mesh");
}

// Primitives (drawn whole on SDL_GPU, tessellated on SDL_Renderer): the same
// pixels either way.
void check_primitives(kin::Renderer2D& renderer) {
    const kin::Color white = kin::Color::rgb(255, 255, 255);
    const kin::Color red = kin::Color::rgb(255, 0, 0);
    const kin::Color green = kin::Color::rgb(0, 255, 0);
    const kin::Color blue = kin::Color::rgb(0, 0, 255);
    const kin::Color black = kin::Color::rgb(0, 0, 0);
    kin::RenderTarget target = renderer.create_render_target({64, 64}, kin::ScaleMode::Nearest);
    kin::Shape ring;
    ring.fill_and_stroke(kin::Path::circle({0, 0}, 8), red, black, {.width = 2});
    kin::Shape dot;
    dot.fill(kin::Path::circle({0, 0}, 1), kin::colors::white);
    const kin::ShapeMesh ring_mesh = ring.mesh(), dot_mesh = dot.mesh();
    assert(ring_mesh.primitives.size() == 1 && dot_mesh.primitives.size() == 1);
    std::vector<kin::u8> px;
    kin::Vec2i size{};
    {
        const auto bind = renderer.scoped_render_target(target);
        renderer.clear(white);
        renderer.fill_ellipse({16.0f, 16.0f}, {12.0f, 6.0f}, red);
        renderer.draw_rounded_rect({36.0f, 4.0f, 24.0f, 24.0f}, 6.0f, blue, 3.0f);
        renderer.draw_line({8.0f, 40.0f}, {28.0f, 40.0f}, green, 8.0f, kin::LineCap::Round);
        renderer.draw_shape(ring_mesh, kin::Affine2::translation({48.0f, 48.0f}));
        renderer.draw_shape(dot_mesh, kin::Affine2::translation({16.0f, 54.0f}) * kin::Affine2::scaling({8.0f, 8.0f}),
                            black);
        assert(renderer.read_rgba({0.0f, 0.0f, 64.0f, 64.0f}, px, size));
    }
    const auto expect = [&](int x, int y, kin::Color color, const char* what) {
        if (!pixel_near(px, size, x, y, color, 3)) {
            const std::size_t i = (static_cast<std::size_t>(y) * size.x + x) * 4;
            throw std::runtime_error(std::string("check_primitives (") + std::string(renderer.backend_name()) + "): " +
                                     what + " at " + std::to_string(x) + "," + std::to_string(y) + " is " +
                                     std::to_string(px[i]) + "," + std::to_string(px[i + 1]) + "," + std::to_string(px[i + 2]));
        }
    };
    expect(16, 16, red, "ellipse");
    expect(26, 16, red, "ellipse, along its long axis");
    expect(16, 24, white, "past the ellipse's short axis");
    expect(37, 16, blue, "rounded border");
    expect(48, 16, white, "inside the border");
    expect(34, 16, white, "outside the border");
    expect(5, 40, green, "capsule's round cap");
    expect(18, 43, green, "capsule");
    expect(34, 40, white, "past the capsule");
    expect(48, 48, red, "ring's fill");
    expect(48, 40, black, "ring's stroke over its fill");
    expect(16, 54, black, "dot drawn 8x");
    expect(19, 54, black, "inside the dot drawn 8x");
    expect(25, 54, white, "1.5 px past the dot drawn 8x");
}

// Sdf text: one atlas, sharp at any zoom (SDL_GPU). Zoomed 8x, an "I"'s stem
// has edges a pixel or two soft, not a texel's blur, and an outline rings it.
// Backends without distance fields draw it as Bitmap text.
void check_sdf_text(kin::Renderer2D& renderer) {
    if (!kin::ui2::system_ui_font_available()) {
        return;
    }
    const kin::ui2::Font font = kin::ui2::system_ui_font(16, kin::ui2::TextRendering::Sdf);
    const bool fields = renderer.capabilities().distance_fields;
    kin::RenderTarget target = renderer.create_render_target({64, 64}, kin::ScaleMode::Nearest);
    const auto row = [&](bool outline) {
        std::vector<kin::u8> px;
        kin::Vec2i size{};
        const auto bind = renderer.scoped_render_target(target);
        renderer.clear(kin::Color::rgb(255, 255, 255));
        {
            const auto zoom = renderer.scoped_transform(kin::Affine2::scaling({8.0f, 8.0f}));
            if (outline) {
                kin::ui2::draw_text_outlined(renderer, font, "I", {1.0f, -4.0f}, 1.0f, kin::Color::rgb(0, 0, 0), 1.0f,
                                             kin::Color::rgb(255, 0, 0));
            } else {
                kin::ui2::draw_text(renderer, font, "I", {1.0f, -4.0f}, 1.0f, kin::Color::rgb(0, 0, 0));
            }
        }
        assert(renderer.read_rgba({0.0f, 0.0f, 64.0f, 64.0f}, px, size));
        std::vector<kin::Color> out;
        for (int x = 0; x < size.x; ++x) {
            const std::size_t i = (static_cast<std::size_t>(32) * size.x + x) * 4;
            out.push_back(kin::Color::rgb(px[i], px[i + 1], px[i + 2]));
        }
        return out;
    };
    const std::vector<kin::Color> plain = row(false);
    int dark = 0, ramp = 0;
    for (const kin::Color c : plain) {
        dark += c.g < 40 ? 1 : 0;
        ramp += c.g >= 40 && c.g <= 215 ? 1 : 0;
    }
    // Bitmap text magnified 8x is as soft as its texels: a thin stem (Segoe UI's)
    // may have no texel fully inked, so only ask that it was drawn.
    if (fields ? dark < 6 || ramp > 6 : dark + ramp < 6) {
        throw std::runtime_error(std::string("check_sdf_text (") + std::string(renderer.backend_name()) + "): " +
                                 std::to_string(dark) + " dark and " + std::to_string(ramp) + " soft pixels in the stem");
    }
    if (fields) {
        // The outline: red beside the stem, on both sides.
        const std::vector<kin::Color> ringed = row(true);
        int red = 0;
        for (const kin::Color c : ringed) {
            red += c.r > 200 && c.g < 60 ? 1 : 0;
        }
        if (red < 6) {
            throw std::runtime_error("check_sdf_text: " + std::to_string(red) + " outline pixels");
        }
    }
}

// Clips and masks (capabilities().masks): paths, inverted, from a texture's
// alpha, as a stencil, by luminance, nested, transformed, empty. White is drawn
// through each over black; a pixel's grey is how much the mask let through.
void check_masks(kin::Renderer2D& renderer) {
    assert(renderer.capabilities().masks);
    const kin::Color white = kin::Color::rgb(255, 255, 255);
    kin::RenderTarget target = renderer.create_render_target({64, 64}, kin::ScaleMode::Nearest);
    const std::array<kin::u8, 16> ramp_texels{255, 255, 255, 0,   255, 255, 255, 85,
                                              255, 255, 255, 170, 255, 255, 255, 255};
    const kin::Texture ramp = renderer.create_texture_from_rgba(ramp_texels.data(), {4, 1});
    renderer.set_scale_mode(ramp, kin::ScaleMode::Nearest);
    const kin::Path disc = kin::Path::circle({32.0f, 32.0f}, 16.0f);
    const auto fill_disc = [&](kin::Renderer2D& r) { r.fill_path(disc, white); };
    const auto whole = [&](kin::Renderer2D& r) { r.fill_rect({-200.0f, -200.0f, 400.0f, 400.0f}, white); };

    std::vector<kin::u8> px;
    kin::Vec2i size{};
    std::string scene;
    const auto draw = [&](std::string name, const std::function<void()>& body) {
        scene = std::move(name);
        const auto bind = renderer.scoped_render_target(target);
        renderer.clear(kin::Color::rgb(0, 0, 0));
        body();
        assert(renderer.read_rgba({0.0f, 0.0f, 64.0f, 64.0f}, px, size));
    };
    const auto expect = [&](int x, int y, int grey) {
        const kin::Color want = kin::Color::rgb(static_cast<kin::u8>(grey), static_cast<kin::u8>(grey), static_cast<kin::u8>(grey));
        if (!pixel_near(px, size, x, y, want, 4)) {
            const std::size_t i = (static_cast<std::size_t>(y) * size.x + x) * 4;
            throw std::runtime_error("check_masks (" + std::string(renderer.backend_name()) + "): " + scene + " at " +
                                     std::to_string(x) + "," + std::to_string(y) + " is " + std::to_string(px[i]) + "," +
                                     std::to_string(px[i + 1]) + "," + std::to_string(px[i + 2]) + ", not " +
                                     std::to_string(grey));
        }
    };

    draw("a circle", [&] {
        auto clip = renderer.scoped_clip(disc);
        whole(renderer);
    });
    expect(32, 32, 255);
    expect(32, 21, 255);
    expect(32, 51, 0);
    expect(4, 4, 0);

    draw("a rectangle, moved and scaled", [&] {
        auto move = renderer.scoped_transform(kin::Affine2::translation({8.0f, 8.0f}) * kin::Affine2::scaling({2.0f, 2.0f}));
        auto clip = renderer.scoped_clip(kin::Path::rect({0.0f, 0.0f, 8.0f, 8.0f}));
        whole(renderer);
    });
    expect(9, 9, 255);
    expect(22, 22, 255);
    expect(26, 26, 0);
    expect(6, 6, 0);

    draw("a turned square", [&] {
        auto turn = renderer.scoped_transform(kin::Affine2::translation({32.0f, 32.0f}) * kin::Affine2::rotation(45.0f));
        auto clip = renderer.scoped_clip(kin::Path::rect({-12.0f, -12.0f, 24.0f, 24.0f}));
        whole(renderer);
    });
    expect(32, 20, 255); // towards a corner turned straight up, 17 px from the centre
    expect(20, 20, 0);   // where the unturned square's corner was

    draw("outside a circle", [&] {
        auto mask = renderer.scoped_mask(fill_disc, {.invert = true});
        whole(renderer);
    });
    expect(32, 32, 0);
    expect(4, 4, 255);

    draw("a texture's alpha", [&] {
        auto mask = renderer.scoped_mask(ramp, {0.0f, 0.0f, 64.0f, 64.0f});
        whole(renderer);
    });
    expect(8, 32, 0);
    expect(24, 32, 85);
    expect(40, 32, 170);
    expect(56, 32, 255);

    draw("a stencil from a texture's alpha", [&] {
        auto mask = renderer.scoped_mask(ramp, {0.0f, 0.0f, 64.0f, 64.0f}, {.mode = kin::MaskMode::Stencil, .threshold = 0.5f});
        whole(renderer);
    });
    expect(24, 32, 0);
    expect(40, 32, 255);

    draw("luminance", [&] {
        auto mask = renderer.scoped_mask(
            [&](kin::Renderer2D& r) {
                r.fill_rect({0.0f, 0.0f, 32.0f, 64.0f}, white);
                r.fill_rect({32.0f, 0.0f, 32.0f, 64.0f}, kin::Color::rgb(128, 128, 128));
            },
            {.source = kin::MaskSource::Luminance});
        whole(renderer);
    });
    expect(16, 32, 255);
    expect(48, 32, 128);

    draw("nested: the circle's left half", [&] {
        auto clip = renderer.scoped_clip(disc);
        auto mask = renderer.scoped_mask([&](kin::Renderer2D& r) { r.fill_rect({0.0f, 0.0f, 32.0f, 64.0f}, white); });
        whole(renderer);
    });
    expect(24, 32, 255);
    expect(40, 32, 0);
    expect(4, 32, 0);

    draw("a mask drawn under the transform", [&] {
        auto move = renderer.scoped_transform(kin::Affine2::translation({16.0f, 0.0f}));
        auto clip = renderer.scoped_clip(kin::Path::circle({16.0f, 32.0f}, 8.0f));
        whole(renderer);
    });
    expect(32, 32, 255);
    expect(16, 32, 0);

    draw("half-transparent red through a circle", [&] {
        auto clip = renderer.scoped_clip(disc);
        renderer.fill_rect({0.0f, 0.0f, 64.0f, 64.0f}, kin::Color::rgba(255, 0, 0, 128));
    });
    if (!pixel_near(px, size, 32, 32, kin::Color::rgb(128, 0, 0), 4) || !pixel_near(px, size, 4, 4, kin::Color::rgb(0, 0, 0), 4)) {
        throw std::runtime_error("check_masks (" + std::string(renderer.backend_name()) + "): " + scene);
    }

    draw("an empty mask", [&] {
        auto mask = renderer.scoped_mask([](kin::Renderer2D&) {});
        whole(renderer);
    });
    expect(32, 32, 0);
    draw("outside an empty mask", [&] {
        auto mask = renderer.scoped_mask([](kin::Renderer2D&) {}, {.invert = true});
        whole(renderer);
    });
    expect(32, 32, 255);

    draw("a hard circle", [&] {
        auto clip = renderer.scoped_clip(disc, kin::FillRule::NonZero, kin::ClipEdge::Hard);
        whole(renderer);
    });
    expect(32, 32, 255);
    expect(32, 47, 255); // its centre 15.5 from the circle's: in, wholly
    expect(32, 48, 0);   // 16.5: out, wholly
    expect(4, 4, 0);

    const kin::Path wedge = kin::Path::polygon(std::array<kin::Vec2f, 3>{{{0.0f, 0.0f}, {64.0f, 0.0f}, {0.0f, 64.0f}}});
    draw("hard clips nested, in a soft one, popped in turn", [&] {
        renderer.push_clip(disc, kin::FillRule::NonZero, kin::ClipEdge::Hard);
        renderer.push_clip(wedge, kin::FillRule::NonZero, kin::ClipEdge::Hard);
        renderer.push_mask([&](kin::Renderer2D& r) { r.fill_rect({0.0f, 0.0f, 64.0f, 30.0f}, white); });
        whole(renderer); // the circle, above the diagonal, above y 30
        renderer.pop_clip();
        renderer.fill_rect({0.0f, 40.0f, 64.0f, 1.0f}, white); // the circle and the diagonal still
        renderer.pop_clip();
        renderer.fill_rect({0.0f, 44.0f, 64.0f, 1.0f}, white); // the circle still
        renderer.pop_clip();
        renderer.fill_rect({0.0f, 62.0f, 64.0f, 2.0f}, white); // free
    });
    expect(24, 24, 255);
    expect(40, 24, 0);  // below the diagonal
    expect(24, 34, 0);  // below y 30, inside the rest
    expect(20, 40, 255);
    expect(28, 40, 0);  // the diagonal
    expect(32, 44, 255);
    expect(4, 44, 0);   // outside the circle
    expect(32, 63, 255);

    draw("a layer inside a hard clip", [&] {
        auto clip = renderer.scoped_clip(disc, kin::FillRule::NonZero, kin::ClipEdge::Hard);
        auto layer = renderer.begin_layer();
        whole(renderer);
    });
    expect(32, 32, 255);
    expect(4, 4, 0);

    draw("a queued group, under a zooming camera", [&] {
        auto content = std::make_shared<kin::RenderQueue>(kin::RenderSortMode::LayerThenOrder);
        content->fill_rect({.order = -100}, {-200.0f, -200.0f, 400.0f, 400.0f}, white);
        kin::RenderQueue queue{kin::RenderSortMode::LayerThenOrder};
        queue.draw_group({.order = 5}, content, kin::ClipRegion::to_path(kin::Path::circle({16.0f, 16.0f}, 8.0f)));
        queue.fill_rect({.order = 0}, {0.0f, 0.0f, 32.0f, 32.0f}, kin::Color::rgb(0, 0, 0)); // under the group
        kin::Camera2D camera;
        camera.viewport = {64.0f, 64.0f};
        camera.zoom = 2.0f;
        camera.look_at({16.0f, 16.0f});
        kin::RenderView view;
        view.camera = &camera;
        queue.flush(renderer, view);
    });
    expect(32, 32, 255);
    expect(32, 21, 255);
    expect(32, 51, 0);
    expect(4, 4, 0);

    draw("a ui2 clip with two rounded corners", [&] {
        kin::Input input;
        input.begin_frame();
        kin::ui2::Context ui;
        ui.begin(input, renderer);
        ui.push_clip({8.0f, 8.0f, 48.0f, 48.0f}, {16.0f, 16.0f, 0.0f, 0.0f});
        ui.fill_rect({0.0f, 0.0f, 64.0f, 64.0f}, white);
        ui.pop_clip();
        ui.end();
    });
    expect(10, 10, 0);   // cut off by the rounded top-left corner
    expect(53, 53, 255); // the square bottom-right corner
    expect(32, 32, 255);
    expect(4, 32, 0);

    draw("a rectangle clip, then a mask, popped in turn", [&] {
        renderer.push_clip(kin::Rectf{0.0f, 0.0f, 32.0f, 64.0f});
        renderer.push_mask(fill_disc);
        whole(renderer);
        renderer.pop_clip();
        renderer.fill_rect({40.0f, 0.0f, 8.0f, 8.0f}, white); // still within the rectangle
        renderer.pop_clip();
        renderer.fill_rect({56.0f, 0.0f, 8.0f, 8.0f}, white); // free
    });
    expect(24, 32, 255);
    expect(40, 32, 0);
    expect(44, 4, 0);
    expect(60, 4, 255);
}

// Masks straight to a window drawn at twice its logical size.
void check_masks_on_window(kin::Renderer2D& renderer) {
    renderer.set_logical_size({32, 32});
    renderer.clear(kin::Color::rgb(0, 0, 0));
    {
        auto clip = renderer.scoped_clip(kin::Path::circle({16.0f, 16.0f}, 8.0f));
        renderer.fill_rect({0.0f, 0.0f, 32.0f, 32.0f}, kin::Color::rgb(255, 255, 255));
    }
    {   // the window's own stencil: a hard clip, then drawing free of it
        auto clip = renderer.scoped_clip(kin::Path::rect({0.0f, 0.0f, 4.0f, 4.0f}).append(kin::Path::circle({28.0f, 28.0f}, 3.0f)),
                                         kin::FillRule::NonZero, kin::ClipEdge::Hard);
        renderer.fill_rect({0.0f, 0.0f, 32.0f, 32.0f}, kin::Color::rgb(0, 0, 255));
    }
    renderer.fill_rect({30.0f, 0.0f, 2.0f, 2.0f}, kin::Color::rgb(0, 255, 0));
    std::vector<kin::u8> px;
    kin::Vec2i size{};
    assert(renderer.read_rgba({0.0f, 0.0f, 32.0f, 32.0f}, px, size));
    const auto at = [&](float x, float y) {
        return kin::Vec2i{static_cast<int>(x * static_cast<float>(size.x) / 32.0f),
                          static_cast<int>(y * static_cast<float>(size.y) / 32.0f)};
    };
    const kin::Vec2i centre = at(16.0f, 16.0f), corner = at(2.0f, 2.0f), outside = at(16.0f, 27.0f);
    const kin::Vec2i dot = at(28.0f, 28.0f), past = at(31.0f, 20.0f), free = at(31.0f, 1.0f);
    if (!pixel_near(px, size, centre.x, centre.y, kin::Color::rgb(255, 255, 255)) ||
        !pixel_near(px, size, corner.x, corner.y, kin::Color::rgb(0, 0, 255)) ||
        !pixel_near(px, size, outside.x, outside.y, kin::Color::rgb(0, 0, 0)) ||
        !pixel_near(px, size, dot.x, dot.y, kin::Color::rgb(0, 0, 255)) ||
        !pixel_near(px, size, past.x, past.y, kin::Color::rgb(0, 0, 0)) ||
        !pixel_near(px, size, free.x, free.y, kin::Color::rgb(0, 255, 0))) {
        throw std::runtime_error("check_masks_on_window (" + std::string(renderer.backend_name()) + ")");
    }
}

// sRGB and linear light, OKLab, and mixing in each.
void test_color_math() {
    for (int v = 0; v < 256; ++v) {
        const kin::Color c = kin::Color::rgba(static_cast<kin::u8>(v), 0, static_cast<kin::u8>(255 - v), 77);
        assert(kin::to_srgb(kin::to_linear(c)) == c);
    }
    assert(std::abs(kin::srgb_to_linear(0.5f) - 0.2140f) < 1e-3f);
    assert(std::abs(kin::linear_to_srgb(0.5f) - 0.7354f) < 1e-3f);
    const kin::Oklab red = kin::to_oklab(kin::to_linear(kin::Color::rgb(255, 0, 0)));
    assert(std::abs(red.l - 0.62796f) < 1e-3f && std::abs(red.a - 0.22486f) < 1e-3f && std::abs(red.b - 0.12585f) < 1e-3f);
    const kin::Oklab white = kin::to_oklab({1.0f, 1.0f, 1.0f, 1.0f});
    assert(std::abs(white.l - 1.0f) < 1e-3f && std::abs(white.a) < 1e-3f && std::abs(white.b) < 1e-3f);
    const kin::LinearColor back = kin::from_oklab(red);
    assert(std::abs(back.r - 1.0f) < 1e-3f && std::abs(back.g) < 1e-3f && std::abs(back.b) < 1e-3f);
    // Halfway from black to white: the sRGB value, half the light, half the lightness.
    const kin::Color black = kin::colors::black, full = kin::colors::white;
    assert(kin::mix(black, full, 0.5f, kin::ColorMix::Srgb).r == 128);
    assert(kin::mix(black, full, 0.5f, kin::ColorMix::Linear).r == 188);
    assert(std::abs(int(kin::mix(black, full, 0.5f, kin::ColorMix::Oklab).r) - 99) <= 1);
    // Red to green: the sRGB middle is dark, the others are not.
    const kin::Color r = kin::Color::rgb(255, 0, 0), g = kin::Color::rgb(0, 255, 0);
    const auto light = [](kin::Color c) { return kin::to_oklab(kin::to_linear(c)).l; };
    assert(light(kin::mix(r, g, 0.5f, kin::ColorMix::Srgb)) < light(kin::mix(r, g, 0.5f, kin::ColorMix::Oklab)) - 0.05f);
}

// LUTs: neutral, from a strip or a grid image, from .cube text, through PNG.
void test_color_luts() {
    const kin::ColorLut neutral = kin::ColorLut::neutral(17);
    assert(neutral.size() == 17 && neutral.strip().size() == 17u * 17u * 17u * 4u);
    for (const kin::Color c : {kin::Color::rgb(0, 0, 0), kin::Color::rgb(255, 255, 255), kin::Color::rgb(12, 200, 99),
                               kin::Color::rgba(128, 64, 250, 9)}) {
        const kin::Color out = neutral.apply(c);
        assert(std::abs(int(out.r) - c.r) <= 1 && std::abs(int(out.g) - c.g) <= 1 && std::abs(int(out.b) - c.b) <= 1 &&
               out.a == c.a);
    }
    // A strip image; and the same table as a square grid of 4 x 4 tiles.
    const kin::ColorLut n16 = kin::ColorLut::neutral(16);
    const std::optional<kin::ColorLut> strip = kin::ColorLut::from_image(n16.strip(), {256, 16});
    assert(strip && strip->size() == 16 && std::ranges::equal(strip->strip(), n16.strip()));
    std::vector<kin::u8> grid(64u * 64u * 4u);
    for (int b = 0; b < 16; ++b) {
        for (int g = 0; g < 16; ++g) {
            for (int r = 0; r < 16; ++r) {
                const std::size_t from = (static_cast<std::size_t>(g) * 256u + static_cast<std::size_t>(b * 16 + r)) * 4u;
                const std::size_t to = (static_cast<std::size_t>((b / 4) * 16 + g) * 64u + static_cast<std::size_t>((b % 4) * 16 + r)) * 4u;
                std::copy_n(n16.strip().begin() + static_cast<std::ptrdiff_t>(from), 4, grid.begin() + static_cast<std::ptrdiff_t>(to));
            }
        }
    }
    const std::optional<kin::ColorLut> tiled = kin::ColorLut::from_image(grid, {64, 64});
    assert(tiled && std::ranges::equal(tiled->strip(), n16.strip()));
    std::string error;
    assert(!kin::ColorLut::from_image(grid, {64, 32}, &error) && !error.empty());

    // .cube: an inverting table, red fastest, over a domain of 0 to 2.
    const std::string cube = "TITLE \"invert\"\n# a comment\nLUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n"
                             "2 2 2\n0 2 2\n2 0 2\n0 0 2\n2 2 0\n0 2 0\n2 0 0\n0 0 0\n";
    const std::optional<kin::ColorLut> invert = kin::ColorLut::parse_cube(cube, &error);
    assert(invert && invert->size() == 2);
    assert((invert->apply(kin::Color::rgb(0, 0, 0)) == kin::Color::rgb(255, 255, 255)));
    assert((invert->apply(kin::Color::rgb(255, 0, 255)) == kin::Color::rgb(0, 255, 0)));
    assert(!kin::ColorLut::parse_cube("LUT_3D_SIZE 2\n0 0 0\n", &error) && error.find("entries") != std::string::npos);
    assert(!kin::ColorLut::parse_cube("LUT_1D_SIZE 4\n", &error));

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "kin_lut_test.png";
    assert(invert->save_png(path));
    const std::optional<kin::ColorLut> loaded = kin::ColorLut::load(path, &error);
    assert(loaded && std::ranges::equal(loaded->strip(), invert->strip()));
    std::filesystem::remove(path);
}

// Linear light on SDL_GPU: blending, textures, gradients, HDR and tonemapping,
// LUTs and dithering, pixel by pixel.
void check_linear_color(kin::Renderer2D& renderer, kin::Window& window) {
    const auto fail = [&](const std::string& what) {
        throw std::runtime_error("check_linear_color: " + what);
    };
    kin::RenderTarget target; // made after each change of colour space: targets keep their format
    std::vector<kin::u8> px;
    kin::Vec2i size{};
    const auto grey_at = [&](int x, int y) { return int(px[(static_cast<std::size_t>(y) * size.x + x) * 4]); };
    const auto in_target = [&](const std::function<void()>& draw) {
        const auto bind = renderer.scoped_render_target(target);
        renderer.clear(kin::Color::rgb(0, 0, 0));
        draw();
        assert(renderer.read_rgba({0.0f, 0.0f, 64.0f, 64.0f}, px, size));
    };
    // What the screen shows (through the output pass): a screenshot's pixel.
    kin::Image shot;
    const auto on_screen = [&](const std::function<void()>& draw) {
        renderer.clear(kin::Color::rgb(0, 0, 0));
        draw();
        renderer.present();
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "kin_color_test.png";
        assert(renderer.save_png(path.string().c_str()));
        shot = kin::load_image(path);
        std::filesystem::remove(path);
    };
    const auto shot_at = [&](int x, int y) {
        const std::size_t i = (static_cast<std::size_t>(y) * shot.size.x + x) * 4;
        return kin::Color::rgb(shot.rgba[i], shot.rgba[i + 1], shot.rgba[i + 2]);
    };
    (void)window;
    const std::array<kin::u8, 4> texel{128, 64, 200, 255};
    const kin::Gradient ramp{.start = kin::colors::black, .end = kin::colors::white, .direction = kin::GradientDirection::Horizontal};

    for (const bool hdr : {false, true}) {
        const std::string mode = hdr ? "hdr: " : "srgb: ";
        if (!renderer.set_color_space(kin::ColorSpace::Linear, hdr)) {
            fail(mode + "not set");
        }
        target = renderer.create_render_target({64, 64}, kin::ScaleMode::Nearest);
        // Half white over black: half the light, not half the value.
        in_target([&] { renderer.fill_rect({0.0f, 0.0f, 64.0f, 64.0f}, kin::Color::rgba(255, 255, 255, 128)); });
        if (std::abs(grey_at(8, 8) - 188) > 2) {
            fail(mode + "half white over black is " + std::to_string(grey_at(8, 8)));
        }
        // A colour texture drawn as it is comes back as it was.
        const kin::Texture texture = renderer.create_texture_from_rgba(texel.data(), {1, 1});
        in_target([&] { renderer.draw_texture(texture, {0.0f, 0.0f, 64.0f, 64.0f}); });
        const std::size_t i = (8u * static_cast<std::size_t>(size.x) + 8u) * 4u;
        if (std::abs(int(px[i]) - 128) > 1 || std::abs(int(px[i + 1]) - 64) > 1 || std::abs(int(px[i + 2]) - 200) > 1) {
            fail(mode + "a texture's colour changed");
        }
        // Gradients: as the pipeline blends, or as asked.
        in_target([&] { renderer.fill_gradient_rect({0.0f, 0.0f, 64.0f, 64.0f}, ramp); });
        if (std::abs(grey_at(32, 8) - 188) > 6) {
            fail(mode + "a linear gradient's middle is " + std::to_string(grey_at(32, 8)));
        }
        kin::Gradient srgb = ramp;
        srgb.mix = kin::ColorMix::Srgb;
        in_target([&] { renderer.fill_gradient_rect({0.0f, 0.0f, 64.0f, 64.0f}, srgb); });
        if (std::abs(grey_at(32, 8) - 128) > 6) {
            fail(mode + "an sRGB gradient's middle is " + std::to_string(grey_at(32, 8)));
        }
        // Layers and masks lay over in linear light too.
        in_target([&] {
            auto layer = renderer.begin_layer({.opacity = 0.5f});
            renderer.fill_rect({0.0f, 0.0f, 64.0f, 64.0f}, kin::colors::white);
        });
        if (std::abs(grey_at(8, 8) - 188) > 2) {
            fail(mode + "a half-opaque layer is " + std::to_string(grey_at(8, 8)));
        }
        in_target([&] {
            auto clip = renderer.scoped_clip(kin::Path::circle({32.0f, 32.0f}, 16.0f));
            renderer.fill_rect({0.0f, 0.0f, 64.0f, 64.0f}, kin::Color::rgba(255, 255, 255, 128));
        });
        if (std::abs(grey_at(32, 32) - 188) > 2 || grey_at(4, 4) != 0) {
            fail(mode + "a clipped half white is " + std::to_string(grey_at(32, 32)));
        }
        // The screen: encoded on the way out.
        on_screen([&] { renderer.fill_rect({0.0f, 0.0f, 64.0f, 64.0f}, kin::Color::rgba(255, 255, 255, 128)); });
        if (std::abs(int(shot_at(8, 8).r) - 188) > 2) {
            fail(mode + "the screen shows " + std::to_string(shot_at(8, 8).r));
        }
    }

    // HDR: twice white, added, brought down by each tonemapper and by exposure.
    assert(renderer.set_color_space(kin::ColorSpace::Linear, true));
    const auto twice_white = [&] {
        const auto add = renderer.scoped_blend_mode(kin::BlendMode::Additive);
        renderer.fill_rect({0.0f, 0.0f, 64.0f, 64.0f}, kin::colors::white);
        renderer.fill_rect({0.0f, 0.0f, 64.0f, 64.0f}, kin::colors::white);
    };
    const auto expect_shot = [&](kin::ColorOutput output, int grey, const char* what) {
        renderer.set_color_output(std::move(output));
        on_screen(twice_white);
        if (std::abs(int(shot_at(8, 8).r) - grey) > 3) {
            fail(std::string(what) + " shows " + std::to_string(shot_at(8, 8).r) + ", not " + std::to_string(grey));
        }
    };
    expect_shot({}, 255, "clipped at white");
    expect_shot({.tonemap = kin::Tonemap::Reinhard}, 214, "Reinhard");  // 2/3 light
    expect_shot({.tonemap = kin::Tonemap::Aces}, 245, "ACES");          // 0.915 light
    expect_shot({.exposure = 0.25f}, 188, "a quarter exposure");         // half the light

    // Grading: an inverting LUT, applied, cross-faded away, half strength.
    auto invert = std::make_shared<const kin::ColorLut>(*kin::ColorLut::parse_cube(
        "LUT_3D_SIZE 2\n1 1 1\n0 1 1\n1 0 1\n0 0 1\n1 1 0\n0 1 0\n1 0 0\n0 0 0\n"));
    auto neutral = std::make_shared<const kin::ColorLut>(kin::ColorLut::neutral(16));
    const auto expect_graded = [&](kin::ColorOutput output, int grey, const char* what) {
        renderer.set_color_output(std::move(output));
        on_screen([&] { renderer.fill_rect({0.0f, 0.0f, 64.0f, 64.0f}, kin::Color::rgb(40, 40, 40)); });
        if (std::abs(int(shot_at(8, 8).r) - grey) > 3) {
            fail(std::string(what) + " shows " + std::to_string(shot_at(8, 8).r) + ", not " + std::to_string(grey));
        }
    };
    expect_graded({.lut = invert}, 215, "an inverting LUT");
    expect_graded({.lut = invert, .lut_to = neutral, .lut_mix = 1.0f}, 40, "cross-faded to neutral");
    expect_graded({.lut = invert, .lut_strength = 0.5f}, 128, "half an inverting LUT");
    // In a gamma pipeline too.
    assert(renderer.set_color_space(kin::ColorSpace::Gamma));
    expect_graded({.lut = invert}, 215, "an inverting LUT, gamma");

    // Dithering: a dark grey between two 8-bit steps comes out as both.
    assert(renderer.set_color_space(kin::ColorSpace::Linear));
    const auto faint = [&] { renderer.fill_rect({0.0f, 0.0f, 64.0f, 64.0f}, kin::Color::rgba(255, 255, 255, 1)); };
    const auto values = [&] {
        std::array<int, 256> seen{};
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 32; ++x) {
                ++seen[shot_at(x, y).r];
            }
        }
        return static_cast<int>(std::ranges::count_if(seen, [](int n) { return n > 0; }));
    };
    renderer.set_color_output({});
    on_screen(faint);
    if (values() != 1) {
        fail("undithered, a flat colour has " + std::to_string(values()) + " values");
    }
    renderer.set_color_output({.dither = true});
    on_screen(faint);
    if (values() < 2) {
        fail("dithered, a colour between steps stays one value");
    }
    renderer.set_color_output({});
    assert(renderer.set_color_space(kin::ColorSpace::Gamma));
}

void test_linear_color_on_gpu_backend() {
    constexpr std::string_view test_name = "test_linear_color_on_gpu_backend";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-color-test", .width = 64, .height = 64, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        assert(renderer->capabilities().linear_color && renderer->capabilities().color_output);
        check_linear_color(*renderer, window);
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// SDL's renderer keeps its gamma pipeline: a linear one is refused.
void test_linear_color_refused_on_software_backend() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "color-test", .width = 32, .height = 32, .hidden = true});
    kin::Renderer2D renderer{window};
    assert(!renderer.capabilities().linear_color);
    assert(!renderer.set_color_space(kin::ColorSpace::Linear));
    assert(renderer.color_space() == kin::ColorSpace::Gamma);
    assert(renderer.set_color_space(kin::ColorSpace::Gamma));
}

void test_masks_on_software_backend() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "mask-test", .width = 64, .height = 64, .hidden = true});
    kin::Renderer2D renderer{window};
    check_masks(renderer);
    check_masks_on_window(renderer);
}

// SDL's renderer on a GPU (its "gpu" driver) lays alpha masks over by
// blending, without reading them back.
void test_masks_on_accelerated_sdl_renderer() {
    constexpr std::string_view test_name = "test_masks_on_accelerated_sdl_renderer";
    bool ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "sdl-mask-test", .width = 64, .height = 64, .hidden = true});
        kin::Renderer2D renderer{kin::make_render_backend(window, false, /*allow_gpu=*/false)};
        if (renderer.backend_name() == "software") {
            skip_or_require_gpu_test(test_name, "SDL chose its software renderer");
            return;
        }
        ready = true;
        check_masks(renderer);
        check_masks_on_window(renderer);
    } catch (const std::exception& e) {
        if (ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

void test_masks_on_gpu_backend() {
    constexpr std::string_view test_name = "test_masks_on_gpu_backend";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-mask-test", .width = 64, .height = 64, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        check_masks(*renderer);
        check_masks_on_window(*renderer);
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

void test_shapes_on_software_backend() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "shape-test", .width = 64, .height = 64, .hidden = true});
    kin::Renderer2D renderer{window};
    check_shapes(renderer);
    check_primitives(renderer);
    check_sdf_text(renderer);
}

void test_shapes_on_gpu_backend() {
    constexpr std::string_view test_name = "test_shapes_on_gpu_backend";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-shape-test", .width = 64, .height = 64, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        assert(renderer->capabilities().shape_primitives);
        check_shapes(*renderer);
        check_primitives(*renderer);
        assert(renderer->capabilities().distance_fields);
        check_sdf_text(*renderer);
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

void test_transforms_on_software_backend() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "transform-test", .width = 64, .height = 64, .hidden = true});
    kin::Renderer2D renderer{window};
    assert(renderer.capabilities().transforms);
    check_transforms(renderer);
}

void test_transforms_on_gpu_backend() {
    constexpr std::string_view test_name = "test_transforms_on_gpu_backend";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-transform-test", .width = 64, .height = 64, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        assert(renderer->capabilities().transforms);
        check_transforms(*renderer);
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

void test_camera_zoom_and_rotation() {
    const auto near = [](kin::Vec2f a, kin::Vec2f b) { return std::abs(a.x - b.x) < 1e-3f && std::abs(a.y - b.y) < 1e-3f; };
    kin::Camera2D camera;
    camera.viewport = {100.0f, 50.0f};
    camera.offset = {10.0f, 20.0f};
    // Unzoomed and unturned, as before: the offset is the top-left corner.
    assert(camera.translation_only());
    assert((camera.world_to_screen({10.0f, 20.0f}) == kin::Vec2f{0.0f, 0.0f}));
    assert((camera.view_transform() == kin::Affine2::translation({-10.0f, -20.0f})));
    assert((camera.visible_rect(1.0f) == kin::Rectf{9.0f, 19.0f, 102.0f, 52.0f}));
    // Zoom works about the viewport's centre, which stays put.
    camera.zoom = 2.0f;
    assert(near(camera.center(), {60.0f, 45.0f}) && near(camera.world_to_screen({60.0f, 45.0f}), {50.0f, 25.0f}));
    assert(near(camera.world_to_screen({70.0f, 45.0f}), {70.0f, 25.0f}));
    assert(near(camera.screen_to_world({70.0f, 25.0f}), {70.0f, 45.0f}));
    const kin::Rectf zoomed = camera.visible_rect();
    assert(near({zoomed.x, zoomed.y}, {35.0f, 32.5f}) && near({zoomed.w, zoomed.h}, {50.0f, 25.0f}));
    // Turned clockwise, the world turns the other way: what lies right of the
    // centre shows above it.
    camera.zoom = 1.0f;
    camera.rotation = 90.0f;
    assert(near(camera.world_to_screen({70.0f, 45.0f}), {50.0f, 15.0f}));
    assert(near(camera.screen_to_world(camera.world_to_screen({3.0f, -7.0f})), {3.0f, -7.0f}));
    const kin::Rectf turned = camera.visible_rect();
    assert(near({turned.w, turned.h}, {50.0f, 100.0f}));
    camera.look_at({0.0f, 0.0f});
    assert(near(camera.world_to_screen({0.0f, 0.0f}), {50.0f, 25.0f}));
}

void test_cached_target_on_software_backend() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "cached-target-test", .width = 16, .height = 16, .hidden = true});
    kin::Renderer2D renderer{window};
    check_cached_target(renderer);
}

void test_cached_target_on_gpu_backend() {
    constexpr std::string_view test_name = "test_cached_target_on_gpu_backend";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-cached-target-test", .width = 16, .height = 16, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        check_cached_target(*renderer);
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

// Shared by the software (SDL) and GPU backend tests: both must light the same way.
void check_lighting(kin::Renderer2D& renderer) {
    constexpr kin::Vec2i size{64, 64};
    kin::LightLayer layer;
    const kin::Color white = kin::Color::rgb(255, 255, 255);
    const kin::Color ambient = kin::Color::rgb(40, 40, 40);

    // Ambient alone multiplies the scene.
    std::vector<kin::u8> px = lit_scene(renderer, layer, kin::Color::rgb(200, 100, 50), kin::Color::rgb(128, 128, 128), {});
    assert(pixel_near(px, size, 32, 32, kin::Color::rgb(100, 50, 25), 3));

    // A red light: full at its centre, smoothly less towards its edge, none beyond.
    const std::array<kin::Light2D, 1> red{{{.position = {16.0f, 32.0f}, .radius = 16.0f, .color = kin::Color::rgb(255, 0, 0)}}};
    px = lit_scene(renderer, layer, white, ambient, red);
    assert(pixel_near(px, size, 16, 32, kin::Color::rgb(255, 40, 40), 4));
    // Pixel 24's centre is 8.5 from the light: alpha (1 - (8.5 / 16)^2)^2 = 0.515.
    assert(pixel_near(px, size, 24, 32, kin::Color::rgb(40 + 131, 40, 40), 10));
    assert(pixel_near(px, size, 48, 32, ambient, 2));
    assert(pixel_near(px, size, 16, 4, ambient, 2));

    // Intensity above 1 adds the colour again.
    const std::array<kin::Light2D, 1> bright{{{.position = {32.0f, 32.0f}, .radius = 16.0f,
                                               .color = kin::Color::rgb(100, 0, 0), .intensity = 2.0f}}};
    px = lit_scene(renderer, layer, white, ambient, bright);
    assert(pixel_near(px, size, 32, 32, kin::Color::rgb(240, 40, 40), 6));

    // A shape replaces the round falloff: here only its left half lets light through.
    const std::array<kin::u8, 16> half_open{255, 255, 255, 255, 255, 255, 255, 255,
                                            255, 255, 255, 0, 255, 255, 255, 0};
    kin::Light2D shaped{.position = {32.0f, 32.0f}, .radius = 16.0f, .color = kin::Color::rgb(0, 200, 0),
                        .shape = renderer.create_texture_from_rgba(half_open.data(), {4, 1})};
    px = lit_scene(renderer, layer, white, ambient, std::span<const kin::Light2D>{&shaped, 1});
    assert(pixel_near(px, size, 24, 32, kin::Color::rgb(40, 240, 40), 6));
    assert(pixel_near(px, size, 40, 32, ambient, 6));
    shaped.rotation = 180.0f; // now the right half is lit
    px = lit_scene(renderer, layer, white, ambient, std::span<const kin::Light2D>{&shaped, 1});
    assert(pixel_near(px, size, 24, 32, ambient, 6));
    assert(pixel_near(px, size, 40, 32, kin::Color::rgb(40, 240, 40), 6));

    // Lights entirely outside the area change nothing; ones partly inside still count.
    const std::array<kin::Light2D, 2> edges{{
        {.position = {-100.0f, 32.0f}, .radius = 20.0f, .color = kin::Color::rgb(0, 0, 255)},
        {.position = {70.0f, 32.0f}, .radius = 16.0f, .color = kin::Color::rgb(0, 0, 255)},
    }};
    px = lit_scene(renderer, layer, white, ambient, edges);
    assert(pixel_near(px, size, 2, 32, ambient, 2));
    // Pixel 63's centre is 6.5 from the light at x = 70: alpha 0.697.
    assert(pixel_near(px, size, 63, 32, kin::Color::rgb(40, 40, 40 + 178), 10));
}

void test_lighting_on_software_backend() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "lighting-test", .width = 64, .height = 64, .hidden = true});
    kin::Renderer2D renderer{window};
    check_lighting(renderer);
}

void test_lighting_on_gpu_backend() {
    constexpr std::string_view test_name = "test_lighting_on_gpu_backend";
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-lighting-test", .width = 64, .height = 64, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        check_lighting(*renderer);
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

void test_shader_params_and_formats_on_software_backends() {
    std::vector<kin::LogEvent> log_events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
        .memory_events = &log_events,
    });

    // 16 floats by default; more than MaxShaderUniformFloats is refused.
    assert(kin::ShaderParams{}.uniforms.size() == 16);
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D fake{std::move(backend)};
    kin::ShaderParams big;
    big.uniforms.resize(kin::MaxShaderUniformFloats);
    fake.draw_shader_surface({0.0f, 0.0f, 8.0f, 8.0f}, kin::ShaderHandle{1}, big);
    assert(raw->shader_draw_sources.size() == 1);
    big.uniforms.resize(kin::MaxShaderUniformFloats + 1);
    fake.draw_shader_surface({0.0f, 0.0f, 8.0f, 8.0f}, kin::ShaderHandle{1}, big);
    assert(raw->shader_draw_sources.size() == 1);
    assert(has_log_event(log_events, "render", "shader params too large"));

    // Rgba8 textures work everywhere; the software backend has no data formats.
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "formats-test", .width = 16, .height = 16, .hidden = true});
    kin::Renderer2D renderer{window};
    assert(!renderer.capabilities().data_textures);
    const kin::Texture rgba = renderer.create_texture({3, 2}, kin::TextureFormat::Rgba8);
    assert(rgba.valid() && (rgba.size() == kin::Vec2i{3, 2}) && rgba.format() == kin::TextureFormat::Rgba8);
    assert(!renderer.create_texture({3, 2}, kin::TextureFormat::R16Uint).valid());
    assert(has_log_event(log_events, "render", "create_texture: this backend has no data texture formats"));
    assert(!renderer.create_texture({0, 2}, kin::TextureFormat::Rgba8).valid());
    static_assert(kin::texture_format_bytes(kin::TextureFormat::R16Uint) == 2);
    static_assert(kin::texture_format_bytes(kin::TextureFormat::Rg16Uint) == 4);
    kin::set_logger_config({.sdl_sink = false});
}

void test_gpu_data_textures_and_large_uniforms() {
    constexpr std::string_view test_name = "test_gpu_data_textures_and_large_uniforms";
    const std::filesystem::path spv = std::filesystem::path{KIN_TEST_SHADER_DIR} / "data_formats.frag.spv";
    if (!std::filesystem::exists(spv)) {
        skip_or_require_gpu_test(test_name, "test shader not compiled (glslc not found)");
        return;
    }
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "gpu-formats-test", .width = 64, .height = 64, .hidden = true});
        std::string unavailable_reason;
        std::unique_ptr<kin::Renderer2D> renderer = try_create_gpu_renderer(window, unavailable_reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, unavailable_reason);
            return;
        }
        gpu_ready = true;
        assert(renderer->capabilities().data_textures);

        std::ifstream file(spv, std::ios::binary);
        const std::vector<kin::u8> code{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        kin::ShaderDesc desc{};
        desc.spirv = {code.data(), static_cast<kin::u32>(code.size())};
        desc.num_samplers = 3;
        desc.num_uniform_buffers = 1;
        const kin::ShaderHandle shader = renderer->create_shader(desc);
        assert(shader);

        const std::array<std::uint16_t, 1> r16{32768};
        const std::array<std::uint16_t, 4> rg16{65535, 0, 0, 65535}; // texel (1, 0) has green 65535
        const std::array<float, 1> r32f{0.25f};
        std::array<kin::Texture, 3> sources{
            renderer->create_texture({1, 1}, kin::TextureFormat::R16Uint, r16.data()),
            renderer->create_texture({2, 1}, kin::TextureFormat::Rg16Uint, rg16.data()),
            renderer->create_texture({1, 1}, kin::TextureFormat::R32Float, r32f.data()),
        };
        for (const kin::Texture& t : sources) {
            assert(t.valid());
        }
        assert(sources[1].format() == kin::TextureFormat::Rg16Uint);
        kin::ShaderParams params;
        params.uniforms.resize(32);
        params.uniforms[21] = 0.25f; // u[5].y: past the old 16-float limit

        kin::RenderTarget target = renderer->create_render_target({8, 8}, kin::ScaleMode::Nearest);
        const auto draw = [&] {
            std::vector<kin::u8> pixels;
            kin::Vec2i size{};
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgb(0, 0, 0));
            renderer->draw_shader_surface({0.0f, 0.0f, 8.0f, 8.0f}, shader, params, std::span<const kin::Texture>{sources});
            // A data texture is not an image: draw_texture refuses it.
            renderer->draw_texture(sources[2], kin::Rectf{0.0f, 0.0f, 8.0f, 8.0f});
            assert(renderer->read_rgba({0.0f, 0.0f, 8.0f, 8.0f}, pixels, size));
            return pixels;
        };
        assert(pixel_near(draw(), {8, 8}, 4, 4, kin::Color::rgb(128, 255, 128), 2));

        // update_texture writes in the texture's own format.
        const std::array<float, 1> half{0.5f};
        assert(renderer->update_texture(sources[2], {0, 0}, {1, 1}, reinterpret_cast<const kin::u8*>(half.data())));
        assert(pixel_near(draw(), {8, 8}, 4, 4, kin::Color::rgb(128, 255, 191), 2));

        // Made empty (cleared on the GPU where the format can be a render
        // target, else sent zeros), every format reads 0: only the uniform shows.
        sources = {
            renderer->create_texture({1, 1}, kin::TextureFormat::R16Uint, nullptr),
            renderer->create_texture({2, 1}, kin::TextureFormat::Rg16Uint, nullptr),
            renderer->create_texture({1, 1}, kin::TextureFormat::R32Float, nullptr),
        };
        assert(pixel_near(draw(), {8, 8}, 4, 4, kin::Color::rgb(0, 0, 64), 2));

        // Shader draws with the same params share one draw; different params
        // each keep theirs (the blue channel shows u[5].y).
        {
            std::vector<kin::u8> pixels;
            kin::Vec2i size{};
            kin::ShaderParams second = params;
            second.uniforms[21] = 0.75f;
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgb(0, 0, 0));
            renderer->draw_shader_surface({0.0f, 0.0f, 2.0f, 8.0f}, shader, params, std::span<const kin::Texture>{sources});
            renderer->draw_shader_surface({2.0f, 0.0f, 2.0f, 8.0f}, shader, params, std::span<const kin::Texture>{sources});
            renderer->draw_shader_surface({4.0f, 0.0f, 4.0f, 8.0f}, shader, second, std::span<const kin::Texture>{sources});
            assert(renderer->read_rgba({0.0f, 0.0f, 8.0f, 8.0f}, pixels, size));
            assert(pixel_near(pixels, {8, 8}, 1, 4, kin::Color::rgb(0, 0, 64), 2));
            assert(pixel_near(pixels, {8, 8}, 3, 4, kin::Color::rgb(0, 0, 64), 2));
            assert(pixel_near(pixels, {8, 8}, 6, 4, kin::Color::rgb(0, 0, 191), 2));
        }
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

void test_lighting_declines_without_render_targets() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::LightLayer layer;
    const std::array<kin::Light2D, 1> light{{{.position = {8.0f, 8.0f}}}};
    const int draws_before = raw->texture_draws;
    assert(!layer.apply(renderer, {0.0f, 0.0f, 64.0f, 64.0f}, kin::Color::rgb(20, 20, 20), light));
    assert(raw->texture_draws == draws_before);
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
    test_sdl_logical_size_follows_window_size();
    test_sdl_render_targets_and_gradients();
    test_render_target_pool_reuse();
    test_render_target_move_assign_releases();
    test_blur_smooths_hard_edge();
    test_blur_degrades_on_fake_backend();
    test_capture_backdrop_round_trips();
    test_gpu_save_png_captures_bound_render_target();
    test_gpu_texture_released_before_flush();
    test_gpu_timing_survives_a_gpu_behind();
    test_gpu_native_coordinates_disable_logical_presentation();
    test_gpu_frame_timing();
    test_gpu_logical_transforms_are_immediate();
    test_gpu_native_pixel_size_and_pointer_mapping();
    test_gpu_shader_surface_binds_every_source();
    test_shader_params_and_formats_on_software_backends();
    test_gpu_data_textures_and_large_uniforms();
    test_gpu_resources_outlive_renderer();
    test_shader_reflection();
    test_min_max_blend_on_software_backend();
    test_min_max_blend_on_gpu_backend();
    test_gpu_shader_geometry();
    test_gpu_uploads_batch_in_order();
    test_gpu_pipeline_record();
    test_gpu_data_buffers();
    test_gpu_shader_surface_scaled();
    test_gpu_compute();
    test_gpu_shader_hot_reload();
    test_gpu_mipmaps();
    test_gpu_overdraw();
    test_gpu_layers();
    test_layers_on_software_backend();
    test_cached_target_on_software_backend();
    test_cached_target_on_gpu_backend();
    test_transforms_on_software_backend();
    test_shapes_on_software_backend();
    test_shapes_on_gpu_backend();
    test_masks_on_software_backend();
    test_masks_on_gpu_backend();
    test_masks_on_accelerated_sdl_renderer();
    test_color_math();
    test_color_luts();
    test_linear_color_refused_on_software_backend();
    test_linear_color_on_gpu_backend();
    test_transforms_on_gpu_backend();
    test_camera_zoom_and_rotation();
    test_gpu_scopes();
    test_gpu_big_uploads();
    test_gpu_empty_textures_are_clear();
    test_write_texture_on_software_backend();
    test_gpu_write_texture();
    test_lighting_on_software_backend();
    test_lighting_on_gpu_backend();
    test_lighting_declines_without_render_targets();
    test_sprite_batches_on_software_backend();
    test_textures_outliving_their_renderer();
    test_flipped_sprites_on_software_backend();
    test_sprite_batches_on_gpu_backend();
    return 0;
}
