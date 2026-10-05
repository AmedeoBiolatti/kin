#include <kin/core/jobs.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/particles.hpp>
#include <kin/platform/log.hpp>

#include <cassert>
#include <cmath>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include <memory>
#include <span>
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
        events.push_back("fill " + std::to_string(static_cast<int>(rect.x)));
    }
    // No masks: a path clips to its bounds, which this records.
    void push_clip(kin::Rectf rect) override {
        events.push_back("clip " + std::to_string(static_cast<int>(rect.x)) + " " + std::to_string(static_cast<int>(rect.y)) +
                         " " + std::to_string(static_cast<int>(rect.w)) + " " + std::to_string(static_cast<int>(rect.h)));
    }
    void pop_clip() override { events.push_back("pop"); }
    std::vector<std::string> events;
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

    void set_transform(const kin::Affine2& transform) override { transforms.push_back(transform); }
    void draw_shape_mesh(std::span<const kin::ShapeVertex> vertices, std::span<const kin::u32>, kin::u32,
                         kin::Color tint) override {
        shape_vertex_counts.push_back(vertices.size());
        shape_tints.push_back(tint);
        // Where the mesh's origin lands, under the transform set now.
        shape_origins.push_back(transforms.empty() ? kin::Vec2f{} : transforms.back().apply({0.0f, 0.0f}));
    }
    std::vector<std::size_t> shape_vertex_counts;
    std::vector<kin::Color> shape_tints;
    std::vector<kin::Vec2f> shape_origins;
    std::vector<kin::Affine2> transforms;
};

bool near(kin::f32 a, kin::f32 b) {
    return std::abs(a - b) < 1e-3f;
}

bool near(kin::Vec2f a, kin::Vec2f b) {
    return near(a.x, b.x) && near(a.y, b.y);
}

bool near(kin::Rectf a, kin::Rectf b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.w, b.w) && near(a.h, b.h);
}

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

// Parents are updated before their children however the entities were created:
// here the deepest one first, so storage order is the reverse of the hierarchy.
void test_world_render_state_propagates_hierarchy_created_bottom_up() {
    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::WorldTransform>("WorldTransform");

    kin::EcsEntity leaf = world.entity("leaf").set(kin::Transform2D{{1.0f, 0.0f}, 5.0f});
    kin::EcsEntity middle = world.entity("middle").set(kin::Transform2D{{10.0f, 0.0f}, 0.0f, {2.0f, 2.0f}});
    kin::EcsEntity root = world.entity("root").set(kin::Transform2D{{100.0f, 100.0f}, 90.0f});
    leaf.raw().child_of(middle.raw());
    middle.raw().child_of(root.raw());

    // Each child's position goes through its parents' turn and scale.
    kin::WorldRenderState state{world};
    state.propagate_transforms();
    const kin::WorldTransform* m = middle.get<kin::WorldTransform>();
    const kin::WorldTransform* l = leaf.get<kin::WorldTransform>();
    assert(near(m->pos, {100.0f, 110.0f}) && m->rotation == 90.0f && m->scale == (kin::Vec2f{2.0f, 2.0f}));
    assert(near(l->pos, {100.0f, 112.0f}) && l->rotation == 95.0f && l->scale == (kin::Vec2f{2.0f, 2.0f}));
    // The same, composed on demand.
    assert(near(kin::world_position(leaf), {100.0f, 112.0f}));

    root.set(kin::Transform2D{{200.0f, 0.0f}, 0.0f});
    state.propagate_transforms();
    assert((middle.get<kin::WorldTransform>()->pos == kin::Vec2f{210.0f, 0.0f}));
    assert((leaf.get<kin::WorldTransform>()->pos == kin::Vec2f{212.0f, 0.0f}));
}

// Without a WorldRenderState (no WorldTransform yet), world_transform() composes
// up the ChildOf chain.
void test_world_transform_composes_without_propagation() {
    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    kin::EcsEntity root = world.entity("root").set(kin::Transform2D{{10.0f, 0.0f}, -90.0f, {3.0f, 1.0f}});
    kin::EcsEntity child = world.entity("child").set(kin::Transform2D{{2.0f, 0.0f}, 10.0f, {0.5f, 0.5f}});
    child.raw().child_of(root.raw());
    const kin::WorldTransform t = kin::world_transform(child);
    assert(near(t.pos, {10.0f, -6.0f}) && near(t.rotation, -80.0f) && near(t.scale, {1.5f, 0.5f}));
    const kin::WorldTransform same = kin::compose(kin::compose({}, *root.get<kin::Transform2D>()), *child.get<kin::Transform2D>());
    assert(near(same.pos, t.pos) && same.rotation == t.rotation);
}

// to_local() undoes compose().
void test_to_local_inverts_compose() {
    const kin::WorldTransform parent{{50.0f, -20.0f}, 30.0f, {2.0f, 0.5f}};
    const kin::Transform2D child{{3.0f, 4.0f}, -10.0f, {1.5f, 1.5f}};
    const kin::WorldTransform world = kin::compose(parent, child);
    const kin::Transform2D back = kin::to_local(parent, world);
    assert(near(back.pos, child.pos) && near(back.rotation, child.rotation) && near(back.scale, child.scale));
    // A parent squashed flat along an axis keeps the child at its origin along it.
    const kin::Transform2D flat = kin::to_local({{0.0f, 0.0f}, 0.0f, {0.0f, 1.0f}}, {{5.0f, 5.0f}});
    assert(flat.pos == (kin::Vec2f{0.0f, 5.0f}));
}

// Renderers under a turned, scaled parent: offsets, sizes and line ends go
// through it, and the drawing turns with it.
void test_renderers_follow_parent_rotation_and_scale() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{16, 16})};

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::TextureRenderer>("TextureRenderer");
    world.component<kin::RectRenderer>("RectRenderer");
    world.component<kin::LineRenderer>("LineRenderer");
    kin::EcsEntity parent = world.entity("parent").set(kin::Transform2D{{100.0f, 100.0f}, 90.0f, {2.0f, 2.0f}});
    const auto child = [&](const char* name) {
        kin::EcsEntity e = world.entity(name).set(kin::Transform2D{{10.0f, 0.0f}});
        e.raw().child_of(parent.raw());
        return e; // at (100, 120), turned 90, scaled 2
    };
    child("texture").set(kin::TextureRenderer{.texture = texture, .size = {4.0f, 4.0f}, .pivot = {0.5f, 0.5f}, .order = 0});
    child("rect").set(kin::RectRenderer{.offset = {0.0f, 0.0f}, .size = {4.0f, 2.0f}, .color = kin::colors::white, .order = 1});
    child("line").set(kin::LineRenderer{.a = {0.0f, 0.0f}, .b = {5.0f, 0.0f}, .color = kin::colors::white, .order = 2});

    kin::render_world(world, renderer);
    assert(raw->draws.size() == 1 && near(raw->draws[0], {96.0f, 116.0f, 8.0f, 8.0f}));
    assert(raw->rotations[0] == 90.0f && raw->pivots[0] == (kin::Vec2f{0.5f, 0.5f}));
    // The rectangle is scaled, then turned about the entity's origin by the transform.
    assert(raw->rects.size() == 1 && near(raw->rects[0], {100.0f, 120.0f, 8.0f, 4.0f}));
    assert(raw->transforms.size() == 2 && near(raw->transforms[0].apply({108.0f, 120.0f}), {100.0f, 128.0f}));
    assert(raw->transforms[1].is_identity());
    assert(raw->lines_a.size() == 1 && near(raw->lines_a[0], {100.0f, 120.0f}) && near(raw->lines_b[0], {100.0f, 130.0f}));
}

// A camera that zooms or turns is the renderer's transform while the queue
// draws; callbacks draw without it, as under a camera that only moves.
// A ShapeRenderer takes the entity's whole transform (turn, uneven and
// negative scale) and its offset, sorts and culls like the other renderers, and
// moves with the camera.
void test_shape_renderer() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::Shape square;
    square.fill(kin::Path::rect({0, 0, 4, 4}), kin::colors::white);
    const auto mesh = std::make_shared<const kin::ShapeMesh>(square.mesh());

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::ShapeRenderer>("ShapeRenderer");
    world.entity("turned")
        .set(kin::Transform2D{{100.0f, 50.0f}, 90.0f, {2.0f, -1.0f}})
        .set(kin::ShapeRenderer{.mesh = mesh, .offset = {1.0f, 0.0f}, .tint = kin::Color::rgb(255, 0, 0)});
    world.entity("far").set(kin::Transform2D{{5000.0f, 5000.0f}}).set(kin::ShapeRenderer{.mesh = mesh});
    world.entity("hidden").set(kin::Transform2D{}).set(kin::ShapeRenderer{.mesh = mesh, .visible = false});

    kin::Camera2D camera;
    camera.viewport = {320.0f, 180.0f};
    camera.offset = {10.0f, 20.0f};
    kin::render_top_down_world(world, renderer, {.camera = &camera});
    // Only the one in view: (1, 0) scaled by (2, -1) is (2, 0), turned a quarter
    // is (0, 2), at (100, 50) is (100, 52); the camera moves it by (-10, -20).
    // (This backend draws no primitives: the square comes as triangles.)
    assert(raw->shape_vertex_counts.size() == 1 && raw->shape_vertex_counts[0] > 0);
    assert((raw->shape_tints[0] == kin::Color::rgb(255, 0, 0)));
    assert(near(raw->shape_origins[0], {90.0f, 32.0f}));
    assert(raw->transforms.back().is_identity());

    // Queued directly, the queue keeps the mesh alive until it is cleared.
    raw->shape_vertex_counts.clear();
    kin::RenderQueue queue;
    {
        kin::Shape dot;
        dot.fill(kin::Path::circle({0, 0}, 3), kin::colors::white);
        queue.draw_shape({}, std::make_shared<const kin::ShapeMesh>(dot.mesh()), kin::Affine2::translation({7, 8}));
    }
    queue.flush(renderer);
    assert(raw->shape_vertex_counts.size() == 1 && near(raw->shape_origins.back(), {7.0f, 8.0f}));
}

void test_queue_flush_applies_zooming_camera() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::Camera2D camera;
    camera.viewport = {100.0f, 50.0f};
    camera.offset = {10.0f, 20.0f};
    camera.zoom = 2.0f;

    kin::RenderQueue queue;
    queue.fill_rect({}, {60.0f, 45.0f, 4.0f, 4.0f}, kin::colors::white);
    queue.custom({}, [](kin::Renderer2D& r) { r.fill_rect({0.0f, 0.0f, 1.0f, 1.0f}, kin::colors::white); });
    kin::RenderView view;
    view.camera = &camera;
    queue.flush(renderer, view);

    assert(raw->rects.size() == 2 && raw->rects[0] == (kin::Rectf{60.0f, 45.0f, 4.0f, 4.0f}));
    assert(raw->transforms.size() == 4);
    assert(near(raw->transforms[0].apply({60.0f, 45.0f}), {50.0f, 25.0f})); // the view's centre stays put
    assert(near(raw->transforms[1].apply({3.0f, 4.0f}), {3.0f, 4.0f}));     // the callback's
    assert(raw->transforms[2] == raw->transforms[0]);
    assert(raw->transforms[3].is_identity());
    assert(renderer.transform().is_identity());
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

// A ClipGroup cuts its entity's subtree to its clip (in its own space) and
// keeps it together at its own key; groups nest; outside entities are untouched.
void test_clip_groups_cut_and_keep_children_together() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::RectRenderer>("RectRenderer");
    world.component<kin::ClipGroup>("ClipGroup");

    world.entity("before").set(kin::Transform2D{{0.0f, 0.0f}}).set(kin::RectRenderer{.size = {1, 1}, .order = 0});
    world.entity("after").set(kin::Transform2D{{2.0f, 0.0f}}).set(kin::RectRenderer{.size = {1, 1}, .order = 2});
    // At (50, 50), scaled twice: its clip, 20 x 20 about its origin, covers 40 x 40.
    kin::EcsEntity window = world.entity("window")
                                .set(kin::Transform2D{.pos = {50.0f, 50.0f}, .scale = {2.0f, 2.0f}})
                                .set(kin::ClipGroup{.clip = kin::ClipRegion::to_rect({-10.0f, -10.0f, 20.0f, 20.0f}), .order = 1});
    kin::EcsEntity late = world.entity("late").set(kin::Transform2D{}).set(kin::RectRenderer{.size = {1, 1}, .order = 10});
    late.raw().child_of(window.raw());
    kin::EcsEntity early = world.entity("early").set(kin::Transform2D{.pos = {-5.0f, 0.0f}}).set(kin::RectRenderer{.size = {1, 1}, .order = -5});
    early.raw().child_of(window.raw());
    // A group in the group: at (60, 50), its 2 x 2 clip doubled, at order 5 among the window's.
    kin::EcsEntity pane = world.entity("pane")
                              .set(kin::Transform2D{.pos = {5.0f, 0.0f}})
                              .set(kin::ClipGroup{.clip = kin::ClipRegion::to_rect({0.0f, 0.0f, 2.0f, 2.0f}), .order = 5});
    pane.raw().child_of(window.raw());
    kin::EcsEntity in_pane = world.entity("in_pane").set(kin::Transform2D{}).set(kin::RectRenderer{.size = {1, 1}, .order = -100});
    in_pane.raw().child_of(pane.raw());

    kin::RenderQueue queue;
    kin::collect_world(world, queue, {}, {.sort_mode = kin::RenderSortMode::LayerThenOrder});
    queue.flush(renderer);
    const std::vector<std::string> expected{"fill 0",     "clip 30 30 40 40", "fill 40", "clip 60 50 4 4",
                                            "fill 60",    "pop",              "fill 50", "pop",
                                            "fill 2"};
    if (raw->events != expected) {
        for (const std::string& e : raw->events) {
            std::fprintf(stderr, "%s\n", e.c_str());
        }
        assert(false);
    }

    // Static renderers inside a group are drawn with the dynamic ones.
    late.set(kin::RectRenderer{.size = {1, 1}, .order = 10, .static_renderable = true});
    kin::RenderQueue statics;
    kin::collect_static_world(world, statics, {.sort_mode = kin::RenderSortMode::LayerThenOrder});
    assert(statics.size() == 0);
    kin::RenderQueue dynamics;
    kin::collect_dynamic_world(world, dynamics, {.sort_mode = kin::RenderSortMode::LayerThenOrder});
    raw->events.clear();
    dynamics.flush(renderer);
    assert(raw->events == expected);
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

// Rotated sprites are culled as they are submitted too, by the circle they can
// turn within, so a queue built from a view holds only what may be seen.
void test_collect_culls_rotated_sprites() {
    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{32, 32})};
    kin::SpriteCatalog catalog;
    catalog.set_texture("main", texture);
    catalog.add({.id = "bar", .texture_id = "main", .source = {0.0f, 0.0f, 32.0f, 4.0f}, .size = {32.0f, 4.0f}});

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::SpriteRenderer>("SpriteRenderer");
    const auto bar = [&](kin::Vec2f pos) {
        world.entity().set(kin::Transform2D{pos, 0.8f}).set(kin::SpriteRenderer{.sprite = catalog.ref("bar")});
    };
    bar({20.0f, 20.0f});   // inside the view
    bar({-12.0f, 20.0f});  // centre outside, but its turned ends reach in
    bar({500.0f, 500.0f}); // far away

    kin::WorldRenderState state{world};
    state.propagate_transforms();
    kin::RenderQueue queue;
    queue.fill_rect({}, {-100.0f, -100.0f, 4.0f, 4.0f}, kin::colors::white); // queued before: culled too
    const kin::RenderView view{.cull_rect = {0.0f, 0.0f, 40.0f, 40.0f}, .culling_enabled = true};
    state.collect_dynamic(queue, {.view = &view});
    assert(queue.size() == 2);
}

// With a job system, large texture tables are prepared on workers; the queue
// must come out exactly as a single-threaded collect makes it.
void test_parallel_collect_matches_serial() {
    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::TextureRenderer>("TextureRenderer");
    const kin::Texture a{std::make_shared<FakeTextureBackend>(kin::Vec2i{64, 32})};
    const kin::Texture b{std::make_shared<FakeTextureBackend>(kin::Vec2i{16, 16})};
    for (int i = 0; i < 20000; ++i) {
        const auto h = static_cast<unsigned>(i) * 2654435761u;
        world.entity()
            .set(kin::Transform2D{{static_cast<float>(h % 700u) - 50.0f, static_cast<float>((h >> 12) % 500u) - 50.0f}})
            .set(kin::TextureRenderer{.texture = i % 7 == 0 ? b : a, .source = {static_cast<float>(i % 2) * 32.0f, 0.0f, 32.0f, 32.0f},
                                      .size = {8.0f, 8.0f}, .tint = kin::Color::rgb(255, static_cast<kin::u8>(i % 256), 0),
                                      .y_sort = true, .static_renderable = i % 11 == 0});
    }
    kin::WorldRenderState state{world};
    state.propagate_transforms();
    const kin::RenderView view{.cull_rect = {0.0f, 0.0f, 600.0f, 400.0f}, .culling_enabled = true};
    kin::JobSystem jobs{{.workers = 3}};
    for (const bool statics : {false, true}) {
        kin::RenderQueue serial;
        kin::RenderQueue parallel;
        if (statics) {
            state.collect_static(serial, {.view = &view});
            state.collect_static(parallel, {.view = &view, .jobs = &jobs});
        } else {
            state.collect_dynamic(serial, {.view = &view});
            state.collect_dynamic(parallel, {.view = &view, .jobs = &jobs});
        }
        const auto expected = serial.commands();
        const auto actual = parallel.commands();
        assert(expected.size() == actual.size() && expected.size() > 1000);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            assert(expected[i].sequence == actual[i].sequence && expected[i].texture == actual[i].texture);
            assert(expected[i].rect == actual[i].rect && expected[i].source == actual[i].source);
            assert(expected[i].key.y == actual[i].key.y && expected[i].color == actual[i].color);
        }
    }
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
    test_world_render_state_propagates_hierarchy_created_bottom_up();
    test_world_transform_composes_without_propagation();
    test_to_local_inverts_compose();
    test_renderers_follow_parent_rotation_and_scale();
    test_queue_flush_applies_zooming_camera();
    test_shape_renderer();
    test_render_world_draws_primitives();
    test_render_world_draws_texture_renderer();
    test_sprite_pivot_offsets_and_y_sort();
    test_sprite_renderer_tint_rotation_and_pivot_reach_commands();
    test_clip_groups_cut_and_keep_children_together();
    test_rotated_render_command_bounds_expand();
    test_top_down_render_applies_camera_and_culling();
    test_collect_culls_rotated_sprites();
    test_parallel_collect_matches_serial();
    test_static_render_cache_merges_with_dynamic_queue();
    test_world_render_state_collect_dynamic_excludes_static_renderables();
    test_one_shot_submit_reuses_scratch_queue_output();
    return 0;
}
