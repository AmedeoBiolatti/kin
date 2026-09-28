#include <kin/ecs/render.hpp>
#include <kin/ecs/particles.hpp>
#include <kin/platform/log.hpp>

#include <cassert>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include <memory>
#include <string_view>
#include <vector>

namespace {

struct HiddenTag {
};

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

    void clear(kin::Color) override {}
    void present() override {}
    void set_logical_size(kin::Vec2i) override {}
    void set_integer_logical_size(kin::Vec2i) override {}
    kin::Vec2i output_size() const override { return {320, 180}; }
    kin::Vec2f window_to_logical(kin::Vec2f value) const override { return value; }
    kin::Vec2f logical_to_window(kin::Vec2f value) const override { return value; }
    kin::Texture create_texture_from_rgba(const kin::u8*, kin::Vec2i size) override {
        return kin::Texture{std::make_shared<FakeTextureBackend>(size)};
    }
    void fill_rect(kin::Rectf rect, kin::Color color) override {
        rects.push_back(rect);
        colors.push_back(color);
    }
    void draw_rect(kin::Rectf rect, kin::Color color) override {
        outlines.push_back(rect);
        colors.push_back(color);
    }
    void draw_line(kin::Vec2f a, kin::Vec2f b, kin::Color color) override {
        lines_a.push_back(a);
        lines_b.push_back(b);
        colors.push_back(color);
    }
    void set_viewport(kin::Rectf) override {}
    void reset_viewport() override {}
    void push_viewport(kin::Rectf) override {}
    void pop_viewport() override {}

    void draw_texture(const kin::Texture&, kin::Rectf dest) override {
        draws.push_back(dest);
    }

    void draw_texture(const kin::Texture&, kin::Rectf source, kin::Rectf dest) override {
        sources.push_back(source);
        draws.push_back(dest);
    }
    void draw_texture(const kin::Texture&, kin::Rectf source, kin::Rectf dest, kin::Color tint) override {
        sources.push_back(source);
        draws.push_back(dest);
        colors.push_back(tint);
    }
    void draw_texture(const kin::Texture&, kin::Rectf source, kin::Rectf dest, kin::Color tint, kin::f32 rotation, kin::Vec2f pivot) override {
        sources.push_back(source);
        draws.push_back(dest);
        colors.push_back(tint);
        rotations.push_back(rotation);
        pivots.push_back(pivot);
    }

    std::vector<kin::Rectf> sources;
    std::vector<kin::Rectf> draws;
    std::vector<kin::Rectf> rects;
    std::vector<kin::Rectf> outlines;
    std::vector<kin::Vec2f> lines_a;
    std::vector<kin::Vec2f> lines_b;
    std::vector<kin::Color> colors;
    std::vector<kin::f32> rotations;
    std::vector<kin::Vec2f> pivots;
};

void test_render_world_sorts_and_draws_sprites() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{16, 16})};
    kin::SpriteCatalog catalog;
    catalog.set_texture("main", texture);
    catalog.add({.id = "child.sprite", .texture_id = "main", .source = {0.0f, 0.0f, 8.0f, 9.0f}});
    catalog.add({.id = "first.sprite", .texture_id = "main", .source = {0.0f, 0.0f, 5.0f, 6.0f}});
    catalog.add({.id = "last.sprite", .texture_id = "main", .source = {0.0f, 0.0f, 8.0f, 9.0f}});
    catalog.add({.id = "hidden.sprite", .texture_id = "main", .source = {0.0f, 0.0f, 8.0f, 9.0f}});

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::SpriteRenderer>("SpriteRenderer");
    world.component<HiddenTag>("HiddenTag");

    kin::EcsEntity parent = world.entity("parent").set(kin::Transform2D{{100.0f, 50.0f}});
    kin::EcsEntity child = world.entity("child")
                               .set(kin::Transform2D{{4.0f, 5.0f}})
                               .set(kin::SpriteRenderer{
                                   .sprite = catalog.ref("child.sprite"),
                                   .offset = {1.0f, 2.0f},
                                   .layer = 1,
                                   .order = 0,
                               });
    child.raw().child_of(parent.raw());

    world.entity("first")
        .set(kin::Transform2D{{10.0f, 20.0f}})
        .set(kin::SpriteRenderer{
            .sprite = catalog.ref("first.sprite"),
            .layer = 0,
            .order = 1,
        });

    world.entity("last")
        .set(kin::Transform2D{{30.0f, 40.0f}})
        .set(kin::SpriteRenderer{
            .sprite = catalog.ref("last.sprite"),
            .size = {3.0f, 4.0f},
            .layer = 2,
            .order = 0,
        });

    world.entity("hidden")
        .set(kin::Transform2D{{70.0f, 80.0f}})
        .set(kin::SpriteRenderer{
            .sprite = catalog.ref("hidden.sprite"),
            .visible = false,
        });

    const kin::Vec2f child_world_pos = kin::world_position(child);
    const kin::Vec2f expected_child_world_pos{104.0f, 55.0f};
    assert(child_world_pos == expected_child_world_pos);

    kin::render_world(world, renderer);
    assert(raw->draws.size() == 3);

    const kin::Rectf first_draw{10.0f, 20.0f, 5.0f, 6.0f};
    const kin::Rectf child_draw{105.0f, 57.0f, 8.0f, 9.0f};
    const kin::Rectf last_draw{30.0f, 40.0f, 3.0f, 4.0f};
    assert(raw->draws[0] == first_draw);
    assert(raw->draws[1] == child_draw);
    assert(raw->draws[2] == last_draw);

    raw->draws.clear();
    kin::render_filtered(world, renderer, [](flecs::entity entity) {
        return entity.name() != "child";
    });

    assert(raw->draws.size() == 2);
    assert(raw->draws[0] == first_draw);
    assert(raw->draws[1] == last_draw);
}

void test_world_render_state_propagates_hierarchy() {
    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::WorldTransform>("WorldTransform");

    kin::EcsEntity root = world.entity("root").set(kin::Transform2D{{10.0f, 20.0f}});
    kin::EcsEntity child = world.entity("child").set(kin::Transform2D{{3.0f, 4.0f}});
    kin::EcsEntity grandchild = world.entity("grandchild").set(kin::Transform2D{{1.0f, 2.0f}});
    child.raw().child_of(root.raw());
    grandchild.raw().child_of(child.raw());

    assert((kin::world_position(grandchild) == kin::Vec2f{14.0f, 26.0f}));

    kin::WorldRenderState state{world};
    state.propagate_transforms();

    const auto* root_world = root.get<kin::WorldTransform>();
    const auto* child_world = child.get<kin::WorldTransform>();
    const auto* grandchild_world = grandchild.get<kin::WorldTransform>();
    assert((root_world && root_world->pos == kin::Vec2f{10.0f, 20.0f}));
    assert((child_world && child_world->pos == kin::Vec2f{13.0f, 24.0f}));
    assert((grandchild_world && grandchild_world->pos == kin::Vec2f{14.0f, 26.0f}));
    assert((kin::world_position(grandchild) == kin::Vec2f{14.0f, 26.0f}));

    child.set(kin::Transform2D{{30.0f, 40.0f}});
    state.propagate_transforms();
    assert((child_world->pos == kin::Vec2f{40.0f, 60.0f}));
    assert((grandchild_world->pos == kin::Vec2f{41.0f, 62.0f}));
    assert((kin::world_position(grandchild) == kin::Vec2f{41.0f, 62.0f}));
}

void test_render_world_draws_primitives() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::RectRenderer>("RectRenderer");
    world.component<kin::LineRenderer>("LineRenderer");

    world.entity("fill")
        .set(kin::Transform2D{{4.0f, 5.0f}})
        .set(kin::RectRenderer{
            .offset = {1.0f, 2.0f},
            .size = {10.0f, 12.0f},
            .color = kin::Color::rgb(20, 30, 40),
            .layer = 1,
        });

    world.entity("outline")
        .set(kin::Transform2D{{20.0f, 30.0f}})
        .set(kin::RectRenderer{
            .size = {14.0f, 16.0f},
            .color = kin::Color::rgb(50, 60, 70),
            .layer = 2,
            .outline = true,
        });

    world.entity("line")
        .set(kin::Transform2D{{100.0f, 200.0f}})
        .set(kin::LineRenderer{
            .a = {1.0f, 2.0f},
            .b = {3.0f, 4.0f},
            .color = kin::Color::rgb(80, 90, 100),
            .layer = 3,
        });

    kin::render_world(world, renderer);

    assert(raw->rects.size() == 1);
    const kin::Rectf fill_rect{5.0f, 7.0f, 10.0f, 12.0f};
    assert(raw->rects[0] == fill_rect);

    assert(raw->outlines.size() == 1);
    const kin::Rectf outline_rect{20.0f, 30.0f, 14.0f, 16.0f};
    assert(raw->outlines[0] == outline_rect);

    assert(raw->lines_a.size() == 1);
    const kin::Vec2f line_a{101.0f, 202.0f};
    const kin::Vec2f line_b{103.0f, 204.0f};
    assert(raw->lines_a[0] == line_a);
    assert(raw->lines_b[0] == line_b);
}

void test_render_world_draws_texture_renderer() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{64, 32})};

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::TextureRenderer>("TextureRenderer");

    world.entity("layer")
        .set(kin::Transform2D{{10.0f, 20.0f}})
        .set(kin::TextureRenderer{
            .texture = texture,
            .source = {4.0f, 5.0f, 20.0f, 10.0f},
            .offset = {2.0f, 3.0f},
            .size = {40.0f, 20.0f},
            .pivot = {0.5f, 0.0f},
            .layer = 1,
            .order = 0,
        });

    kin::render_world(world, renderer);

    assert(raw->draws.size() == 1);
    assert(raw->sources.size() == 1);
    assert((raw->sources[0] == kin::Rectf{4.0f, 5.0f, 20.0f, 10.0f}));
    assert((raw->draws[0] == kin::Rectf{-8.0f, 23.0f, 40.0f, 20.0f}));
}

void test_sprite_pivot_offsets_and_y_sort() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{32, 32})};

    kin::SpriteCatalog catalog;
    catalog.set_texture("main", texture);
    catalog.add({
        .id = "actor",
        .texture_id = "main",
        .source = {0.0f, 0.0f, 16.0f, 16.0f},
        .offset = {2.0f, -4.0f},
        .size = {20.0f, 30.0f},
        .pivot = {0.5f, 1.0f},
    });
    catalog.add({
        .id = "front",
        .texture_id = "main",
        .source = {16.0f, 0.0f, 16.0f, 16.0f},
        .size = {4.0f, 4.0f},
    });

    assert(kin::layer_value(kin::RenderLayer::Actors, 5) == 105);

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::SpriteRenderer>("SpriteRenderer");

    world.entity("actor")
        .set(kin::Transform2D{{100.0f, 200.0f}})
        .set(kin::SpriteRenderer{
            .sprite = catalog.ref("actor"),
            .offset = {3.0f, 5.0f},
            .layer = kin::layer_value(kin::RenderLayer::Actors),
            .order = 1,
            .y_sort = true,
            .sort_y_offset = 12.0f,
        });

    world.entity("front")
        .set(kin::Transform2D{{10.0f, 100.0f}})
        .set(kin::SpriteRenderer{
            .sprite = catalog.ref("front"),
            .layer = kin::layer_value(kin::RenderLayer::Actors),
            .order = 1,
            .y_sort = true,
        });

    kin::render_world(world, renderer);
    assert(raw->draws.size() == 2);

    const kin::Rectf front_draw{10.0f, 100.0f, 4.0f, 4.0f};
    const kin::Rectf actor_draw{95.0f, 171.0f, 20.0f, 30.0f};
    assert(raw->draws[0] == front_draw);
    assert(raw->draws[1] == actor_draw);
}

void test_sprite_renderer_tint_rotation_and_pivot_reach_commands() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{32, 32})};

    kin::SpriteCatalog catalog;
    catalog.set_texture("main", texture);
    catalog.add({
        .id = "catalog-pivot",
        .texture_id = "main",
        .source = {0.0f, 0.0f, 16.0f, 16.0f},
        .size = {20.0f, 10.0f},
        .pivot = {0.25f, 0.75f},
    });
    catalog.add({
        .id = "override-pivot",
        .texture_id = "main",
        .source = {16.0f, 0.0f, 16.0f, 16.0f},
        .size = {12.0f, 8.0f},
        .pivot = {0.5f, 0.5f},
    });

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::SpriteRenderer>("SpriteRenderer");

    world.entity("catalog-pivot")
        .set(kin::Transform2D{{10.0f, 20.0f}})
        .set(kin::SpriteRenderer{
            .sprite = catalog.ref("catalog-pivot"),
            .tint = kin::Color::rgba(200, 180, 160, 140),
            .rotation = 30.0f,
            .order = 0,
        });

    world.entity("override-pivot")
        .set(kin::Transform2D{{30.0f, 40.0f}})
        .set(kin::SpriteRenderer{
            .sprite = catalog.ref("override-pivot"),
            .pivot = {1.0f, 0.0f},
            .tint = kin::Color::rgba(10, 20, 30, 40),
            .rotation = 90.0f,
            .order = 1,
        });

    kin::render_world(world, renderer);

    assert(raw->draws.size() == 2);
    assert(raw->colors[0] == kin::Color::rgba(200, 180, 160, 140));
    assert(raw->colors[1] == kin::Color::rgba(10, 20, 30, 40));
    assert(raw->rotations[0] == 30.0f);
    assert(raw->rotations[1] == 90.0f);
    assert((raw->pivots[0] == kin::Vec2f{0.25f, 0.75f}));
    assert((raw->pivots[1] == kin::Vec2f{1.0f, 0.0f}));
}

void test_rotated_render_command_bounds_expand() {
    kin::RenderCommand command{
        .type = kin::RenderCommandType::Sprite,
        .rect = {0.0f, 0.0f, 10.0f, 20.0f},
        .rotation = 90.0f,
        .pivot = {0.5f, 0.5f},
    };

    const kin::Rectf bounds = kin::render_command_bounds(command);
    assert(bounds.x < 0.0f);
    assert(bounds.y > 0.0f);
    assert(bounds.w > command.rect.w);
    assert(bounds.h < command.rect.h);
}

void test_top_down_render_applies_camera_and_culling() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{32, 32})};
    kin::SpriteCatalog catalog;
    catalog.set_texture("main", texture);
    catalog.add({
        .id = "actor",
        .texture_id = "main",
        .source = {0.0f, 0.0f, 8.0f, 8.0f},
        .size = {8.0f, 8.0f},
    });

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::SpriteRenderer>("SpriteRenderer");

    world.entity("visible")
        .set(kin::Transform2D{{20.0f, 30.0f}})
        .set(kin::SpriteRenderer{.sprite = catalog.ref("actor")});
    world.entity("offscreen")
        .set(kin::Transform2D{{500.0f, 500.0f}})
        .set(kin::SpriteRenderer{.sprite = catalog.ref("actor")});

    kin::Camera2D camera;
    camera.offset = {10.0f, 20.0f};
    camera.set_viewport({100.0f, 100.0f});
    kin::render_top_down_world(world,
                               renderer,
                               {
                                   .camera = &camera,
                                   .cull_padding = 0.0f,
                                   .culling_enabled = true,
                               });

    assert(raw->draws.size() == 1);
    const kin::Rectf expected_draw{10.0f, 10.0f, 8.0f, 8.0f};
    assert(raw->draws[0] == expected_draw);
}

void test_static_render_cache_merges_with_dynamic_queue() {
    std::vector<kin::LogEvent> events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .sdl_sink = false,
        .memory_events = &events,
    });

    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::RectRenderer>("RectRenderer");
    world.component<kin::StaticRenderable>("StaticRenderable");

    const kin::Color static_low = kin::Color::rgb(10, 20, 30);
    const kin::Color dynamic_mid = kin::Color::rgb(40, 50, 60);
    const kin::Color static_high = kin::Color::rgb(70, 80, 90);

    world.entity("static-low")
        .set(kin::Transform2D{{0.0f, 10.0f}})
        .set(kin::RectRenderer{.size = {4.0f, 4.0f}, .color = static_low, .y_sort = true, .static_renderable = true});
    world.entity("dynamic-mid")
        .set(kin::Transform2D{{0.0f, 20.0f}})
        .set(kin::RectRenderer{.size = {4.0f, 4.0f}, .color = dynamic_mid, .y_sort = true});
    world.entity("static-high")
        .set(kin::Transform2D{{0.0f, 30.0f}})
        .set(kin::RectRenderer{.size = {4.0f, 4.0f}, .color = static_high, .y_sort = true, .static_renderable = true});

    kin::StaticRenderCache cache;
    cache.rebuild(world, [](kin::EcsWorld& cache_world, kin::RenderQueue& queue) {
        kin::collect_static_world(cache_world, queue, {.sort = true, .sort_mode = kin::RenderSortMode::LayerThenY});
    });
    assert(!cache.dirty());
    assert(cache.size() == 2);

    kin::RenderQueue dynamic{kin::RenderSortMode::LayerThenY};
    kin::collect_dynamic_world(world, dynamic, {.sort = true, .sort_mode = kin::RenderSortMode::LayerThenY});
    dynamic.sort_commands();

    kin::Camera2D camera;
    camera.offset = {0.0f, 5.0f};
    camera.set_viewport({100.0f, 100.0f});
    kin::RenderView view{.camera = &camera, .cull_rect = camera.visible_rect(0.0f), .culling_enabled = true};
    cache.flush_merged(renderer, dynamic, view);

    assert(raw->rects.size() == 3);
    assert(raw->colors[0] == static_low);
    assert(raw->colors[1] == dynamic_mid);
    assert(raw->colors[2] == static_high);
    assert((raw->rects[0] == kin::Rectf{0.0f, 5.0f, 4.0f, 4.0f}));

    cache.mark_dirty();
    assert(cache.dirty());

    bool saw_rebuild = false;
    bool saw_dirty = false;
    for (const kin::LogEvent& event : events) {
        saw_rebuild = saw_rebuild || (event.category == "render" && event.message == "static render cache rebuilt");
        saw_dirty = saw_dirty || (event.category == "render" && event.message == "static render cache marked dirty");
    }
    assert(saw_rebuild);
    assert(saw_dirty);
    kin::set_logger_config({.sdl_sink = false});
}

void test_world_render_state_collect_dynamic_excludes_static_renderables() {
    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::WorldTransform>("WorldTransform");
    world.component<kin::RectRenderer>("RectRenderer");
    world.component<kin::ParticleSystemComponent>("ParticleSystemComponent");

    world.entity("static")
        .set(kin::Transform2D{{0.0f, 10.0f}})
        .set(kin::RectRenderer{.size = {4.0f, 4.0f}, .static_renderable = true});
    world.entity("dynamic")
        .set(kin::Transform2D{{0.0f, 20.0f}})
        .set(kin::RectRenderer{.size = {4.0f, 4.0f}});

    kin::ParticleSystemComponent particles;
    particles.system.emit({.position = {8.0f, 9.0f},
                           .lifetime = 1.0f,
                           .start_size = 2.0f,
                           .end_size = 2.0f,
                           .start_color = kin::colors::white,
                           .end_color = kin::colors::white});
    world.entity("particles").set(std::move(particles));

    kin::WorldRenderState state{world};
    state.propagate_transforms();

    kin::RenderQueue static_queue{kin::RenderSortMode::LayerThenY};
    state.collect_static(static_queue, {.sort = true, .sort_mode = kin::RenderSortMode::LayerThenY});
    assert(static_queue.size() == 1);

    kin::RenderQueue dynamic_queue{kin::RenderSortMode::LayerThenY};
    state.collect_dynamic(dynamic_queue, {.sort = true, .sort_mode = kin::RenderSortMode::LayerThenY});
    assert(dynamic_queue.size() == 2);
}

void test_one_shot_submit_reuses_scratch_queue_output() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::RectRenderer>("RectRenderer");

    kin::EcsEntity rect = world.entity("rect")
                              .set(kin::Transform2D{{4.0f, 5.0f}})
                              .set(kin::RectRenderer{
                                  .offset = {1.0f, 2.0f},
                                  .size = {10.0f, 12.0f},
                                  .color = kin::Color::rgb(20, 30, 40),
                              });

    assert(kin::submit_rect(renderer, rect.raw()));
    assert(kin::submit_rect(renderer, rect.raw()));
    assert(raw->rects.size() == 2);
    assert((raw->rects[0] == kin::Rectf{5.0f, 7.0f, 10.0f, 12.0f}));
    assert((raw->rects[1] == kin::Rectf{5.0f, 7.0f, 10.0f, 12.0f}));
}

} // namespace

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    test_render_world_sorts_and_draws_sprites();
    test_world_render_state_propagates_hierarchy();
    test_render_world_draws_primitives();
    test_render_world_draws_texture_renderer();
    test_sprite_pivot_offsets_and_y_sort();
    test_sprite_renderer_tint_rotation_and_pivot_reach_commands();
    test_rotated_render_command_bounds_expand();
    test_top_down_render_applies_camera_and_culling();
    test_static_render_cache_merges_with_dynamic_queue();
    test_world_render_state_collect_dynamic_excludes_static_renderables();
    test_one_shot_submit_reuses_scratch_queue_output();
    return 0;
}
