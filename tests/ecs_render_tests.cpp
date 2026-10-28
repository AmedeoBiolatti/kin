#include <kin/assets/image.hpp>
#include <kin/core/jobs.hpp>
#include <kin/anim/anim_format.hpp>
#include <kin/ecs/animator.hpp>
#include <kin/ecs/assets2d.hpp>
#include <kin/ecs/camera.hpp>
#include <kin/ecs/ecs_scene.hpp>
#include <kin/ecs/lighting.hpp>
#include <kin/l10n/localization.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/shader_effect.hpp>
#include <kin/ecs/text.hpp>
#include <kin/ecs/tilemap.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/tilemap/tilemap.hpp>
#include <kin/platform/app.hpp>
#include <kin/platform/window.hpp>
#include <kin/renderer/sprite_catalog.hpp>
#include <kin/scene/scene_manager.hpp>
#if KIN_ENABLE_AUDIO
#include <kin/audio/audio_catalog.hpp>
#include <kin/audio/backend.hpp>
#include <kin/ecs/audio.hpp>
#endif
#include <kin/ecs/particles.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include <memory>
#include <span>
#include <string>
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
    void set_blend_mode(kin::BlendMode mode) override { blends.push_back(mode); }
    std::vector<kin::BlendMode> blends;
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

// Flipped sprites mirror about their pivot, which stays put; a negative scale
// flips too, and cancels a flag on the same axis.
void test_sprite_and_texture_renderers_flip() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{32, 32})};

    kin::SpriteCatalog catalog;
    catalog.set_texture("main", texture);
    catalog.add({
        .id = "hero",
        .texture_id = "main",
        .source = {0.0f, 0.0f, 16.0f, 16.0f},
        .size = {20.0f, 10.0f},
        .pivot = {0.25f, 0.75f},
    });

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::SpriteRenderer>("SpriteRenderer");
    world.component<kin::TextureRenderer>("TextureRenderer");

    world.entity("plain").set(kin::Transform2D{{10.0f, 20.0f}}).set(kin::SpriteRenderer{.sprite = catalog.ref("hero"), .order = 0});
    world.entity("flag")
        .set(kin::Transform2D{{10.0f, 20.0f}})
        .set(kin::SpriteRenderer{.sprite = catalog.ref("hero"), .order = 1, .flip_x = true});
    world.entity("scale")
        .set(kin::Transform2D{{10.0f, 20.0f}, 0.0f, {-1.0f, 1.0f}})
        .set(kin::SpriteRenderer{.sprite = catalog.ref("hero"), .order = 2});
    world.entity("both")
        .set(kin::Transform2D{{10.0f, 20.0f}, 0.0f, {-2.0f, 1.0f}})
        .set(kin::SpriteRenderer{.sprite = catalog.ref("hero"), .order = 3, .flip_x = true});
    world.entity("texture")
        .set(kin::Transform2D{{0.0f, 0.0f}})
        .set(kin::TextureRenderer{.texture = texture, .source = {0.0f, 0.0f, 8.0f, 4.0f}, .order = 4, .flip_y = true});

    kin::render_world(world, renderer);

    assert(raw->sources.size() == 5);
    // Unflipped: the pivot (a quarter in, three quarters down) on (10, 20).
    assert((raw->draws[0] == kin::Rectf{5.0f, 12.5f, 20.0f, 10.0f}));
    assert((raw->sources[0] == kin::Rectf{0.0f, 0.0f, 16.0f, 16.0f}));
    assert((raw->pivots[0] == kin::Vec2f{0.25f, 0.75f}));
    // Flipped: the pivot is a quarter in from the right, still on (10, 20).
    for (std::size_t i : {std::size_t{1}, std::size_t{2}}) {
        assert((raw->draws[i] == kin::Rectf{-5.0f, 12.5f, 20.0f, 10.0f}));
        assert((raw->sources[i] == kin::Rectf{16.0f, 0.0f, -16.0f, 16.0f}));
        assert((raw->pivots[i] == kin::Vec2f{0.75f, 0.75f}));
    }
    // Flag and negative scale cancel out; the scale still stretches.
    assert((raw->draws[3] == kin::Rectf{0.0f, 12.5f, 40.0f, 10.0f}));
    assert((raw->sources[3] == kin::Rectf{0.0f, 0.0f, 16.0f, 16.0f}));
    // Upside down about its top-left pivot, which becomes the bottom-left.
    assert((raw->draws[4] == kin::Rectf{0.0f, -4.0f, 8.0f, 4.0f}));
    assert((raw->sources[4] == kin::Rectf{0.0f, 4.0f, 8.0f, -4.0f}));
    assert((raw->pivots[4] == kin::Vec2f{0.0f, 1.0f}));
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
    const auto previous = kin::logger_config();
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
    kin::set_logger_config(previous); // the later tests' logs show again
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

#if KIN_ENABLE_IMAGE_IO
// Components naming images get them; a file saved again is read again; one
// that could not be read is told, and read once it is there; a game's own
// binders run too; textures set from C++ are left alone.
void test_assets2d_binds_and_reloads_images() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "kin_assets2d_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "images");
    kin::Image image{.size = {2, 3}, .rgba = std::vector<kin::u8>(24, 255)};
    assert(kin::save_image(image, dir / "images" / "hero.png"));

    kin::Renderer2D renderer{std::make_unique<FakeBackend>()};
    kin::Assets2D assets{renderer, dir};
    kin::EcsWorld world;
    kin::EcsEntity hero = world.entity().set(kin::Transform2D{}).set(kin::TextureRenderer{.asset = {.id = "images/hero.png"}});
    kin::EcsEntity missing = world.entity().set(kin::Transform2D{}).set(kin::TextureRenderer{.asset = {.id = "images/later.png"}});
    const std::vector<kin::u8> pixel(4, 255);
    const kin::Texture own = renderer.create_texture_from_rgba(pixel.data(), {1, 1});
    kin::EcsEntity drawn = world.entity().set(kin::Transform2D{}).set(kin::TextureRenderer{.texture = own});

    assert(assets.bind(world));
    assert(hero.get<kin::TextureRenderer>()->texture.size() == (kin::Vec2i{2, 3}));
    assert(!missing.get<kin::TextureRenderer>()->texture && assets.problems().contains("images/later.png"));
    assert(drawn.get<kin::TextureRenderer>()->texture == own);
    assert(!assets.bind(world)); // nothing changed
    // One texture for every component naming the image.
    kin::EcsEntity twin = world.entity().set(kin::Transform2D{}).set(kin::TextureRenderer{.asset = {.id = "images/hero.png"}});
    assert(assets.bind(world) && twin.get<kin::TextureRenderer>()->texture == hero.get<kin::TextureRenderer>()->texture);

    // Saved again (another size), and the missing one made.
    image = {.size = {4, 4}, .rgba = std::vector<kin::u8>(64, 128)};
    assert(kin::save_image(image, dir / "images" / "hero.png"));
    fs::last_write_time(dir / "images" / "hero.png", fs::last_write_time(dir / "images" / "hero.png") + std::chrono::seconds(2));
    assert(kin::save_image(image, dir / "images" / "later.png"));
    std::vector<std::string> changed = assets.reload_changed();
    std::ranges::sort(changed);
    assert((changed == std::vector<std::string>{"images/hero.png", "images/later.png"}));
    assert(assets.bind(world));
    assert(hero.get<kin::TextureRenderer>()->texture.size() == (kin::Vec2i{4, 4}));
    assert(missing.get<kin::TextureRenderer>()->texture && assets.problems().empty());
    assert(assets.reload_changed().empty());

    // A game's own binder.
    int binds = 0;
    assets.add_binder("Counter", [&](kin::EcsWorld&, kin::Assets2D&) { ++binds; });
    assets.add_binder("Counter", [&](kin::EcsWorld&, kin::Assets2D&) { binds += 10; }); // replaced
    assets.bind(world);
    assert(binds == 10);
    fs::remove_all(dir);
}

// A sprite catalog named by SpriteRenderers: bound (each keeping its sprite's
// id), drawn, and bound again when one of the catalog's images changes.
void test_assets2d_binds_sprite_catalogs() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "kin_assets2d_sprites_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "images");
    fs::create_directories(dir / "sprites");
    assert(kin::save_image({.size = {8, 4}, .rgba = std::vector<kin::u8>(128, 255)}, dir / "images" / "hero.png"));
    {
        std::ofstream out(dir / "sprites" / "hero.kinsprites");
        out << "texture hero images/hero.png\nsprite idle hero 0 0 4 4\nsprite run hero 4 0 4 4\n";
    }
    kin::Renderer2D renderer{std::make_unique<FakeBackend>()};
    kin::Assets2D assets{renderer, dir};
    kin::EcsWorld world;
    kin::SpriteRenderer idle{.asset = {.id = "sprites/hero.kinsprites"}};
    idle.sprite.id = "idle";
    kin::SpriteRenderer run = idle;
    run.sprite.id = "run";
    kin::EcsEntity a = world.entity().set(kin::Transform2D{}).set(idle);
    kin::EcsEntity b = world.entity().set(kin::Transform2D{}).set(run);
    assert(assets.bind(world));
    const kin::SpriteCatalog* catalog = a.get<kin::SpriteRenderer>()->sprite.catalog;
    assert(catalog && catalog == b.get<kin::SpriteRenderer>()->sprite.catalog);
    assert(a.get<kin::SpriteRenderer>()->sprite.id == "idle" && b.get<kin::SpriteRenderer>()->sprite.valid());
    kin::ResolvedSprite resolved;
    assert(catalog->resolve("run", resolved) && resolved.sprite.source.x == 4.0f);

    // An image of the catalog saved again: the catalog is read again, and given out.
    assert(kin::save_image({.size = {8, 8}, .rgba = std::vector<kin::u8>(256, 9)}, dir / "images" / "hero.png"));
    fs::last_write_time(dir / "images" / "hero.png", fs::last_write_time(dir / "images" / "hero.png") + std::chrono::seconds(2));
    const std::vector<std::string> changed = assets.reload_changed();
    assert(std::ranges::find(changed, std::string("sprites/hero.kinsprites")) != changed.end());
    assert(assets.bind(world));
    assert(a.get<kin::SpriteRenderer>()->sprite.catalog != catalog && a.get<kin::SpriteRenderer>()->sprite.valid());
    // One that is not there: told, and none bound.
    kin::SpriteRenderer lost{.asset = {.id = "sprites/lost.kinsprites"}};
    kin::EcsEntity c = world.entity().set(kin::Transform2D{}).set(lost);
    assets.bind(world);
    assert(!c.get<kin::SpriteRenderer>()->sprite.catalog && assets.problems().contains("sprites/lost.kinsprites"));
    fs::remove_all(dir);
}

void test_assets2d_binds_particles() {
    // A particle catalog named by an emitter and a field: the emitter bursts
    // its effect where its entity is (as it starts, then at its rate); the
    // field is made from the catalog's, its area from its entity.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "kin_assets2d_particles_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "particles");
    const auto write_catalog = [&](int sparks) {
        std::ofstream out(dir / "particles" / "fx.kinparticles");
        out << "effect spark " << sparks << " 10 20 1 1 2 0 255 255 255 255 255 255 255 0 0 0 100 0\n"
            << "field stars 5 7 0 0 100 50 -1 1 -1 1 1 2 100 255 255 255 255 255 1 1 100 0\n";
    };
    write_catalog(4);
    kin::Renderer2D renderer{std::make_unique<FakeBackend>()};
    kin::Assets2D assets{renderer, dir};
    kin::EcsWorld world;
    kin::ParticleSystemComponent emitter;
    emitter.asset = {.id = "particles/fx.kinparticles"};
    emitter.effect = "spark";
    emitter.rate = 10.0f;
    emitter.burst_at_start = true;
    emitter.offset = {5.0f, 0.0f};
    kin::EcsEntity a = world.entity().set(kin::Transform2D{.pos = {100.0f, 50.0f}}).set(std::move(emitter));
    kin::ParticleFieldComponent stars;
    stars.asset = {.id = "particles/fx.kinparticles"};
    stars.name = "stars";
    kin::EcsEntity b = world.entity().set(kin::Transform2D{.pos = {200.0f, 300.0f}}).set(std::move(stars));

    // Nothing bursts before its catalog is bound.
    kin::update_particles(world, 0.01f);
    assert(a.get<kin::ParticleSystemComponent>()->system.empty() && !a.get<kin::ParticleSystemComponent>()->started);
    assert(assets.bind(world));
    const kin::ParticleCatalog* catalog = a.get<kin::ParticleSystemComponent>()->system.catalog();
    assert(catalog && catalog->has_effect("spark"));
    const kin::ParticleFieldComponent* field = b.get<kin::ParticleFieldComponent>();
    assert(field->bound && field->state.particles.size() == 5);
    assert(field->state.definition.area.x == 200.0f && field->state.definition.area.y == 300.0f);
    assert(!assets.bind(world)); // as bound: nothing changes

    // As it starts, one burst where it is; the next a tenth of a second on.
    kin::update_particles(world, 0.01f);
    const kin::ParticleSystem& system = a.get<kin::ParticleSystemComponent>()->system;
    assert(system.active_count() == 4);
    assert((system.particles()[0].position == kin::Vec2f{105.0f, 50.0f}));
    kin::update_particles(world, 0.05f);
    assert(system.active_count() == 4);
    kin::update_particles(world, 0.05f);
    assert(system.active_count() == 8);
    // Not emitting: none more.
    a.get_mut<kin::ParticleSystemComponent>()->emitting = false;
    kin::update_particles(world, 0.3f);
    assert(a.get<kin::ParticleSystemComponent>()->system.active_count() == 8);

    // The catalog saved again: read again, given out; its particles kept, the field made again.
    const kin::ParticleField* first_field = field->bound;
    write_catalog(6);
    fs::last_write_time(dir / "particles" / "fx.kinparticles", fs::last_write_time(dir / "particles" / "fx.kinparticles") + std::chrono::seconds(2));
    assert(!assets.reload_changed().empty());
    assert(assets.bind(world));
    assert(a.get<kin::ParticleSystemComponent>()->system.catalog() != catalog);
    assert(a.get<kin::ParticleSystemComponent>()->system.active_count() == 8);
    assert(b.get<kin::ParticleFieldComponent>()->bound && b.get<kin::ParticleFieldComponent>()->bound != first_field);
    a.get_mut<kin::ParticleSystemComponent>()->emitting = true;
    kin::update_particles(world, 0.1f);
    assert(a.get<kin::ParticleSystemComponent>()->system.active_count() == 14);

    // An effect or a field the catalog has not: nothing burst, no field made.
    kin::ParticleSystemComponent missing;
    missing.asset = {.id = "particles/fx.kinparticles"};
    missing.effect = "nope";
    missing.rate = 10.0f;
    kin::EcsEntity c = world.entity().set(kin::Transform2D{}).set(std::move(missing));
    kin::ParticleFieldComponent nowhere;
    nowhere.asset = {.id = "particles/fx.kinparticles"};
    nowhere.name = "nope";
    kin::EcsEntity d = world.entity().set(kin::Transform2D{}).set(std::move(nowhere));
    assets.bind(world);
    kin::update_particles(world, 0.5f);
    assert(c.get<kin::ParticleSystemComponent>()->system.empty());
    assert(!d.get<kin::ParticleFieldComponent>()->bound && d.get<kin::ParticleFieldComponent>()->state.particles.empty());
    fs::remove_all(dir);
}

void test_assets2d_binds_tilemaps() {
    // A map naming a tileset file cut from an image: bound with its tiles and
    // the image as a sheet, drawn with its top left where its entity is; read
    // again when its tileset changes.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "kin_assets2d_tilemaps_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "images");
    fs::create_directories(dir / "tilesets");
    fs::create_directories(dir / "maps");
    kin::Image image{.size = {32, 16}, .rgba = std::vector<kin::u8>(32 * 16 * 4, 255)};
    assert(kin::save_image(image, dir / "images" / "tiles.png"));
    kin::TileSet tiles;
    tiles.image = "images/tiles.png";
    tiles.sheet = {.tile_w = 16, .tile_h = 16, .spacing = 0, .margin = 0};
    const kin::u16 grass = tiles.ensure_tile("grass");
    tiles.tile(grass)->frame = 1;
    assert(kin::save_tileset(tiles, dir / "tilesets" / "ground.kintileset"));
    kin::TileMap map;
    map.resize(3, 2);
    map.tileset_source = "tilesets/ground.kintileset";
    map.tileset = tiles;
    map.set(0, 0, grass);
    map.set(2, 1, grass);
    assert(kin::save_tilemap(map, dir / "maps" / "level.kintilemap"));

    kin::Renderer2D renderer{std::make_unique<FakeBackend>()};
    kin::Assets2D assets{renderer, dir};
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world); // as a scene file reads it: described
    assert(world.components().find("TilemapComponent"));
    kin::EcsEntity level = world.entity()
                               .set(kin::Transform2D{.pos = {100.0f, 40.0f}})
                               .set(kin::TilemapComponent{.asset = {.id = "maps/level.kintilemap"}});
    assert(assets.bind(world));
    const kin::TilemapComponent* bound = level.get<kin::TilemapComponent>();
    assert(bound->tilemap && bound->tilemap->map.tile_id("grass") == grass && bound->tilemap->sheet.valid());
    assert(bound->tilemap->sheet.cols() == 2);
    assert(!assets.bind(world)); // as bound: nothing changes

    kin::WorldRenderState state{world};
    state.propagate_transforms();
    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    state.collect_all(queue);
    assert(queue.size() == 2);
    assert(queue.commands()[0].type == kin::RenderCommandType::Sprite && queue.commands()[0].rect.x == 100.0f &&
           queue.commands()[0].rect.y == 40.0f && queue.commands()[0].source.x == 16.0f);
    assert(queue.commands()[1].rect.x == 132.0f && queue.commands()[1].rect.y == 56.0f);

    // The tileset saved again (another frame): the map read again, given out.
    const std::shared_ptr<const kin::TilemapAsset> first = bound->tilemap;
    tiles.tile(grass)->frame = 0;
    assert(kin::save_tileset(tiles, dir / "tilesets" / "ground.kintileset"));
    fs::last_write_time(dir / "tilesets" / "ground.kintileset", fs::last_write_time(dir / "tilesets" / "ground.kintileset") + std::chrono::seconds(2));
    assert(!assets.reload_changed().empty());
    assert(assets.bind(world));
    assert(level.get<kin::TilemapComponent>()->tilemap != first);
    assert(level.get<kin::TilemapComponent>()->tilemap->map.tileset.tile(grass)->frame == 0);

    // A map that is not there: told, nothing drawn.
    kin::EcsEntity lost = world.entity().set(kin::Transform2D{}).set(kin::TilemapComponent{.asset = {.id = "maps/missing.kintilemap"}});
    assets.bind(world);
    assert(!lost.get<kin::TilemapComponent>()->tilemap && assets.problems().contains("maps/missing.kintilemap"));
    fs::remove_all(dir);
}

#if KIN_ENABLE_AUDIO
void write_test_wav(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    const auto u32 = [&](kin::u32 v) { for (int i = 0; i < 4; ++i) out.put(static_cast<char>((v >> (8 * i)) & 0xff)); };
    const auto u16 = [&](kin::u16 v) { for (int i = 0; i < 2; ++i) out.put(static_cast<char>((v >> (8 * i)) & 0xff)); };
    const kin::u32 frames = 4800; // a tenth of a second, mono 16-bit at 48 kHz
    out.write("RIFF", 4);
    u32(36 + frames * 2);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(48000);
    u32(96000);
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(frames * 2);
    for (kin::u32 i = 0; i < frames; ++i) {
        u16(static_cast<kin::u16>(i % 50 < 25 ? 6000 : -6000));
    }
}

// AudioEmitters naming a sound file, or a cue of a catalog, play from their own
// catalogs.
void test_assets2d_binds_audio_emitters() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "kin_assets2d_audio_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "audio");
    write_test_wav(dir / "audio" / "hum.wav");
    {
        std::ofstream out(dir / "audio" / "sounds.kinaudio");
        out << "bus master 1\nbus sfx 1 master\nclip hum hum.wav\ncue buzz sound sfx loop=true clips=hum\n";
    }
    kin::Renderer2D renderer{std::make_unique<FakeBackend>()};
    kin::Assets2D assets{renderer, dir};
    kin::EcsWorld world;
    kin::EcsEntity file = world.entity().set(kin::Transform2D{}).set(kin::AudioEmitter{.asset = {.id = "audio/hum.wav"}});
    kin::EcsEntity cue = world.entity().set(kin::Transform2D{}).set(kin::AudioEmitter{.cue = "buzz", .asset = {.id = "audio/sounds.kinaudio"}});
    kin::EcsEntity none = world.entity().set(kin::Transform2D{}).set(kin::AudioEmitter{.cue = "buzz"});
    assert(assets.bind(world));
    const kin::AudioCatalog* own = file.get<kin::AudioEmitter>()->catalog;
    assert(own && own->cue("audio/hum.wav") && cue.get<kin::AudioEmitter>()->catalog->cue("buzz"));
    assert(!none.get<kin::AudioEmitter>()->catalog);

    kin::AudioEngine audio{kin::create_null_audio_backend()};
    kin::update_audio_emitters(world, audio);
    assert(file.get<kin::AudioEmitter>()->playing && cue.get<kin::AudioEmitter>()->playing);
    assert(!none.get<kin::AudioEmitter>()->playing); // no catalog of its own, none given
    fs::remove_all(dir);
}
#endif

// An EcsScene binds what its components name before it draws, from its asset
// root: a scene file naming an image shows it, with no code of the game's.
void test_ecs_scene_binds_assets() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "kin_ecs_scene_assets_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const kin::Image image{.size = {5, 7}, .rgba = std::vector<kin::u8>(140, 200)};
    assert(kin::save_image(image, dir / "hero.png"));

    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "assets", .width = 32, .height = 32, .hidden = true});
    kin::Renderer2D renderer{window};
    kin::SceneManager scenes;
    kin::SceneContext ctx{.app = app, .window = window, .renderer = renderer, .input = app.input(), .scenes = scenes};
    kin::EcsScene scene;
    scene.render_config().enabled = true;
    scene.render_config().asset_root = dir;
    kin::EcsEntity hero = scene.ecs().entity().set(kin::Transform2D{}).set(kin::TextureRenderer{.asset = {.id = "hero.png"}});
    int binds = 0;
    scene.add_asset_binder("Counter", [&](kin::EcsWorld&, kin::Assets2D&) { ++binds; });
    assert(!scene.assets());
    scene.render(ctx);
    assert(scene.assets() && scene.assets()->root() == dir && binds == 1);
    assert(hero.get<kin::TextureRenderer>()->texture.size() == (kin::Vec2i{5, 7}));
    scene.render_config().assets = false; // off: left as they are
    scene.render(ctx);
    assert(binds == 1);
    fs::remove_all(dir);
}
#endif

// A scene's camera: where its entity is, zoomed, kept within its limits, and
// catching its entity up when it smooths.
void test_scene_cameras() {
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    assert(!kin::scene_camera(world, {320.0f, 180.0f}));
    kin::EcsEntity spare = world.entity().set(kin::Transform2D{.pos = {-50.0f, 0.0f}}).set(kin::CameraComponent{.current = false});
    kin::EcsEntity player = world.entity().set(kin::Transform2D{.pos = {500.0f, 300.0f}});
    kin::EcsEntity follow = world.entity().set(kin::Transform2D{.pos = {0.0f, -20.0f}, .rotation = 10.0f}).set(kin::CameraComponent{.zoom = 2.0f});
    follow.raw().child_of(player.raw());
    std::optional<kin::Camera2D> camera = kin::scene_camera(world, {320.0f, 180.0f});
    assert(camera && near(camera->center(), {500.0f, 280.0f}) && near(camera->zoom, 2.0f) && near(camera->rotation, 10.0f));
    assert(near(camera->world_to_screen({500.0f, 280.0f}), {160.0f, 90.0f}));
    (void)spare;

    // Limited to 0..400 x 0..200: a 320 x 180 view (zoom 1) stops 160 and 90 in.
    follow.set(kin::CameraComponent{.limited = true, .limit_min = {0.0f, 0.0f}, .limit_max = {400.0f, 200.0f}});
    camera = kin::scene_camera(world, {320.0f, 180.0f});
    assert(near(camera->center(), {240.0f, 110.0f}));
    // Narrower than the view: centred on them.
    follow.set(kin::CameraComponent{.limited = true, .limit_min = {0.0f, 0.0f}, .limit_max = {100.0f, 1000.0f}});
    assert(near(kin::scene_camera(world, {320.0f, 180.0f})->center().x, 50.0f));

    // Smoothing: placed at once, then most of the way (63%) in its time.
    follow.set(kin::Transform2D{}).set(kin::CameraComponent{.smoothing = 0.5f});
    kin::update_cameras(world, 0.016f);
    assert(near(kin::scene_camera(world, {320.0f, 180.0f})->center(), {500.0f, 300.0f}));
    player.set(kin::Transform2D{.pos = {600.0f, 300.0f}});
    kin::update_cameras(world, 0.5f);
    const kin::Vec2f centre = kin::scene_camera(world, {320.0f, 180.0f})->center();
    assert(std::abs(centre.x - (500.0f + 100.0f * (1.0f - std::exp(-1.0f)))) < 0.01f && near(centre.y, 300.0f));
}

// Shapes, clips and text as a scene file gives them: described fields, made
// into meshes, clips and fonts; the text drawn through the camera.
void test_scene_shapes_clips_and_text() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "kin_scene_shapes_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream svg(dir / "star.svg");
        svg << R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 20"><rect x="0" y="0" width="10" height="20" fill="#ff0000"/></svg>)";
    }
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    const std::string scene = R"json({
  "schema": "kin.scene/1",
  "name": "Parts",
  "entities": [
    {"id": "ball", "components": {"Transform2D": {"pos": {"x": 100, "y": 50}},
      "ShapeRenderer": {"shape": "ellipse", "size": {"x": 40, "y": 20}, "stroke": {"r": 0, "g": 0, "b": 0, "a": 255}, "stroke_width": 2}}},
    {"id": "badge", "components": {"Transform2D": {},
      "ShapeRenderer": {"shape": "svg", "asset": "star.svg", "size": {"x": 20, "y": 20}}}},
    {"id": "window", "components": {"Transform2D": {"pos": {"x": 10, "y": 10}},
      "ClipGroup": {"shape": "rounded_rect", "size": {"x": 60, "y": 40}, "offset": {"x": 30, "y": 20}, "radius": 6}}},
    {"id": "sign", "components": {"Transform2D": {"pos": {"x": 200, "y": 100}},
      "TextRenderer": {"text": "Shop\nopen", "size": 12, "align": "left"}}}
  ]
})json";
    kin::SceneLoadResult loaded = kin::parse_scene_asset(scene, world.components());
    assert(loaded.ok());
    kin::SceneInstance instance = kin::instantiate_scene_asset(world, *loaded.scene, dir, world.components());
    assert(instance.ok());
    kin::EcsEntity ball = instance.by_authored_id.at("ball");
    kin::EcsEntity badge = instance.by_authored_id.at("badge");
    kin::EcsEntity window = instance.by_authored_id.at("window");
    kin::EcsEntity sign = instance.by_authored_id.at("sign");
    assert(ball.get<kin::ShapeRenderer>()->spec.kind == kin::ShapeKind::Ellipse && !ball.get<kin::ShapeRenderer>()->mesh);

    // The clip is made as its fields are set: a rounded box 60 x 40 from (0, 0).
    const kin::ClipGroup* clip = window.get<kin::ClipGroup>();
    assert(clip->shape == kin::ClipShape::RoundedRect && clip->clip.kind == kin::ClipRegion::Kind::Path);
    assert(clip->clip.bounds() && near(*clip->clip.bounds(), {0.0f, 0.0f, 60.0f, 40.0f}));

    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::Assets2D assets{renderer, dir};
    assert(assets.bind(world));
    const kin::ShapeRenderer* ellipse = ball.get<kin::ShapeRenderer>();
    assert(ellipse->mesh && !ellipse->mesh->empty());
    // 40 x 20 about the entity, its 2-wide outline half outside.
    assert(ellipse->mesh->bounds.w > 41.0f && ellipse->mesh->bounds.w < 48.0f);
    const kin::ShapeMesh* first = ellipse->mesh.get();
    // The SVG, 10 x 20, fitted in 20 x 20: 10 x 20 about the entity.
    const kin::ShapeRenderer* svg = badge.get<kin::ShapeRenderer>();
    assert(svg->mesh && !svg->mesh->empty() && svg->made_from);
    assert(std::abs(svg->mesh->bounds.h - 20.0f) < 4.0f && std::abs(svg->mesh->bounds.y + 10.0f) < 2.0f);
    assert(sign.get<kin::TextRenderer>()->face);
    assert(!assets.bind(world)); // made: nothing changes
    assert(ball.get<kin::ShapeRenderer>()->mesh.get() == first);
    // A field changed: made again.
    ball.get_mut<kin::ShapeRenderer>()->spec.size = {80.0f, 20.0f};
    assert(assets.bind(world));
    assert(ball.get<kin::ShapeRenderer>()->mesh->bounds.w > 80.0f);

    // Two lines, left-aligned, centred on the entity.
    const kin::TextLayout laid = kin::layout_text(*sign.get<kin::TextRenderer>());
    assert(laid.lines.size() == 2 && laid.lines[0] == "Shop" && laid.lines[1] == "open");
    assert(near(laid.at, {-laid.size.x * 0.5f, -laid.size.y * 0.5f}));

    // Drawn through a zooming camera: the text's transform puts its entity
    // where the camera shows it.
    kin::Camera2D camera;
    camera.viewport = {320.0f, 180.0f};
    camera.zoom = 2.0f;
    camera.look_at({200.0f, 100.0f});
    kin::WorldRenderState state{world};
    state.propagate_transforms();
    kin::RenderQueue queue;
    const kin::RenderView view{.projection = kin::RenderProjection::TopDown2D, .camera = &camera};
    state.collect_all(queue, [&](flecs::entity e) { return e == sign.raw(); }, {.view = &view});
    assert(queue.size() == 1);
    raw->transforms.clear();
    raw->draws.clear();
    queue.flush(renderer, view);
    assert(!raw->draws.empty()); // its glyphs
    const bool placed = std::ranges::any_of(raw->transforms, [](const kin::Affine2& t) { return near(t.apply({0.0f, 0.0f}), {160.0f, 90.0f}); });
    assert(placed);
    fs::remove_all(dir);
}

// Lights, a scene's lighting and colour grade, blend modes and shader effects
// as a scene file gives them: described, bound, placed and drawn.
void test_lights_blends_and_shader_effects() {
    namespace fs = std::filesystem;
    assert(kin::blend_mode_name(kin::BlendMode::Additive) == "additive" && kin::blend_mode_named("multiply") == kin::BlendMode::Multiply);
    assert(!kin::blend_mode_named("glow") && !kin::blend_material(kin::BlendMode::Alpha));
    assert(kin::blend_material(kin::BlendMode::Additive)->blend == kin::BlendMode::Additive);
    assert(kin::tonemap_named("aces") == kin::Tonemap::Aces && kin::tonemap_name(kin::Tonemap::Reinhard) == "reinhard");

    // Params: name=numbers words; others skipped; written back as read.
    const std::vector<kin::ShaderParamValue> params = kin::parse_shader_params("speed=1.5  tint=1,0.5,0.25,1 bad=x =3 alone");
    assert(params.size() == 2 && params[0].name == "speed" && params[0].values == std::vector<kin::f32>{1.5f});
    assert(params[1].name == "tint" && params[1].values.size() == 4 && near(params[1].values[2], 0.25f));
    assert(kin::format_shader_params(params) == "speed=1.5 tint=1,0.5,0.25,1");

    const fs::path dir = fs::temp_directory_path() / "kin_scene_lights_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        // A 2-point LUT that changes nothing.
        std::ofstream cube(dir / "look.cube");
        cube << "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    }
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    const std::string scene = R"json({
  "schema": "kin.scene/1",
  "name": "Night",
  "entities": [
    {"id": "sky", "components": {"SceneLighting": {"ambient": {"r": 20, "g": 20, "b": 40, "a": 255}}, "ColorGrade": {"tonemap": "aces", "look": "look.cube", "strength": 0.5}}},
    {"id": "lamp", "components": {"Transform2D": {"pos": {"x": 100, "y": 50}, "rotation": 30, "scale": {"x": 2, "y": 1}},
      "LightComponent": {"color": {"r": 255, "g": 200, "b": 100, "a": 255}, "radius": 40, "rotation": 15}}},
    {"id": "off", "components": {"Transform2D": {}, "LightComponent": {"enabled": false}}},
    {"id": "glow", "components": {"Transform2D": {"pos": {"x": 10, "y": 10}}, "RectRenderer": {"size": {"x": 8, "y": 8}, "blend": "additive"}}},
    {"id": "water", "components": {"Transform2D": {"pos": {"x": 60, "y": 60}},
      "ShaderEffect": {"asset": "missing.frag.glsl", "size": {"x": 30, "y": 20}, "params": "speed=2", "blend": "multiply"}}}
  ]
})json";
    kin::SceneLoadResult loaded = kin::parse_scene_asset(scene, world.components());
    assert(loaded.ok());
    kin::SceneInstance instance = kin::instantiate_scene_asset(world, *loaded.scene, dir, world.components());
    assert(instance.ok());
    kin::EcsEntity glow = instance.by_authored_id.at("glow");
    kin::EcsEntity water = instance.by_authored_id.at("water");
    assert(glow.get<kin::RectRenderer>()->blend == kin::BlendMode::Additive);
    assert(water.get<kin::ShaderEffect>()->params == "speed=2" && water.get<kin::ShaderEffect>()->blend == kin::BlendMode::Multiply);

    // The scene's lighting, and its lights placed through a camera that doubles the world.
    const kin::SceneLighting* lighting = kin::scene_lighting(world);
    assert(lighting && lighting->ambient.b == 40);
    const std::vector<kin::Light2D> lights = kin::scene_lights(world, kin::Affine2::scaling({2.0f, 2.0f}));
    assert(lights.size() == 1);
    assert(near(lights[0].position, {200.0f, 100.0f}) && near(lights[0].radius, 160.0f) && near(lights[0].rotation, 45.0f));
    assert(lights[0].color.g == 200);

    // Bound: the look read, the shader not (no such file: a problem, and nothing drawn).
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::Assets2D assets{renderer, dir};
    assets.bind(world);
    const std::optional<kin::ColorOutput> output = kin::scene_color_output(world);
    assert(output && output->tonemap == kin::Tonemap::Aces && output->lut && output->lut->size() == 2 && near(output->lut_strength, 0.5f));
    assert(!water.get<kin::ShaderEffect>()->shader && assets.problems().contains("missing.frag.glsl"));

    // The additive rect draws in its blend mode, and back to alpha after.
    kin::WorldRenderState state{world};
    state.propagate_transforms();
    kin::RenderQueue queue;
    state.collect_all(queue, {}, {});
    assert(queue.size() == 1); // the rect; the shader effect has no shader
    raw->blends.clear();
    queue.flush(renderer);
    assert(raw->blends.size() == 2 && raw->blends[0] == kin::BlendMode::Additive && raw->blends[1] == kin::BlendMode::Alpha);

    // Off: no lighting.
    instance.by_authored_id.at("sky").get_mut<kin::SceneLighting>()->enabled = false;
    assert(!kin::scene_lighting(world));
    fs::remove_all(dir);
}

// A scene's text translated under its own key, or an explicit one, while a
// language has it.
void test_scene_text_translated() {
    assert(kin::scene_text_key("Main Hall", "sign") == "scene.main_hall.sign");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    const kin::SceneLoadResult loaded = kin::parse_scene_asset(R"json({"schema": "kin.scene/1", "name": "Main", "entities": [
        {"id": "sign", "components": {"Transform2D": {}, "TextRenderer": {"text": "Shop"}}},
        {"id": "door", "components": {"Transform2D": {}, "TextRenderer": {"text": "Door", "key": "doors.front"}}}]})json", world.components());
    assert(loaded.ok());
    kin::SceneInstance instance = kin::instantiate_scene_asset(world, *loaded.scene, std::filesystem::temp_directory_path(), world.components());
    const kin::TextRenderer* sign = instance.by_authored_id.at("sign").get<kin::TextRenderer>();
    const kin::TextRenderer* door = instance.by_authored_id.at("door").get<kin::TextRenderer>();
    assert(sign->auto_key == "scene.main.sign" && kin::shown_text(*sign) == "Shop"); // no language: its text
    kin::Localization l10n;
    kin::LanguageFile fr{.locale = "fr", .name = "Français", .direction = {}, .strings = {}};
    fr.strings["scene.main.sign"] = "Boutique";
    fr.strings["doors.front"] = "Porte";
    l10n.add("test", fr);
    l10n.set_locale("fr");
    kin::set_active_localization(&l10n);
    assert(kin::shown_text(*sign) == "Boutique" && kin::shown_text(*door) == "Porte");
    l10n.set_locale("en");
    assert(kin::shown_text(*sign) == "Shop"); // English has no key: its text
    kin::set_active_localization(nullptr);
}

// An animation file named by an Animator: its tracks set described fields
// (any, as "Component.field"), of the entity and of a child by name; it plays
// through once, or on when it repeats; a prefab's instances each their own.
void test_animators() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "kin_animator_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    kin::AnimationRegistryFragment fragment;
    kin::Clip slide{.duration = 1.0f};
    slide.properties.push_back({.property = "Transform2D.pos",
                                .keys = {{.time = 0.0f, .value = kin::Vec2f{0.0f, 0.0f}}, {.time = 1.0f, .value = kin::Vec2f{100.0f, 0.0f}}}});
    slide.properties.push_back({.property = "RectRenderer.color", .target = "lamp",
                                .keys = {{.time = 0.0f, .value = kin::Color{0, 0, 0, 255}}, {.time = 1.0f, .value = kin::Color{200, 100, 0, 255}}}});
    fragment.animations.push_back({.name = "slide", .root = kin::clip_node(std::move(slide))});
    kin::Clip pulse{.duration = 0.5f};
    pulse.properties.push_back({.property = "RectRenderer.size",
                                .keys = {{.time = 0.0f, .value = kin::Vec2f{10.0f, 10.0f}}, {.time = 0.5f, .value = kin::Vec2f{20.0f, 20.0f}}}});
    fragment.animations.push_back({.name = "pulse", .root = kin::repeat_node(kin::clip_node(std::move(pulse)), 0)});
    assert(kin::save_animation_registry_fragment(fragment, dir / "moves.kinanim"));

    kin::Renderer2D renderer{std::make_unique<FakeBackend>()};
    kin::Assets2D assets{renderer, dir};
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    assert(world.components().find("Animator"));
    kin::EcsEntity cart = world.entity("cart").set(kin::Transform2D{}).set(kin::Animator{.asset = {.id = "moves.kinanim"}, .animation = "slide"});
    kin::EcsEntity lamp = world.entity("lamp").set(kin::Transform2D{}).set(kin::RectRenderer{.size = {4, 4}});
    lamp.raw().child_of(cart.raw());
    kin::update_animators(world, 0.1f); // not bound yet: nothing moves
    assert(cart.get<kin::Transform2D>()->pos.x == 0.0f);
    assert(assets.bind(world) && cart.get<kin::Animator>()->library);
    kin::update_animators(world, 0.0f);
    kin::update_animators(world, 0.5f);
    assert(near(cart.get<kin::Transform2D>()->pos.x, 50.0f) && near(cart.get<kin::Animator>()->time, 0.5f));
    assert(lamp.get<kin::RectRenderer>()->color.r == 100); // the child's, by its name
    kin::update_animators(world, 0.75f);
    assert(near(cart.get<kin::Transform2D>()->pos.x, 100.0f) && cart.get<kin::Animator>()->finished);
    // Another named: from its start; one that repeats plays on.
    cart.get_mut<kin::Animator>()->animation = "pulse";
    cart.set(kin::RectRenderer{});
    kin::update_animators(world, 0.0f);
    kin::update_animators(world, 1.25f);
    assert(near(cart.get<kin::RectRenderer>()->size.x, 15.0f) && !cart.get<kin::Animator>()->finished);

    // A prefab's instances: each moved on its own (their Transform2Ds copied down).
    kin::EcsEntity prefab{world.raw().prefab("Cart")
                              .set(kin::Transform2D{})
                              .set(kin::RectRenderer{.size = {10, 10}})
                              .set(kin::Animator{.asset = {.id = "moves.kinanim"}, .animation = "pulse"})};
    kin::EcsEntity first{world.raw().entity().is_a(prefab.raw())};
    kin::EcsEntity second{world.raw().entity().is_a(prefab.raw())};
    assert(assets.bind(world) || true);
    kin::update_animators(world, 0.0f);
    first.get_mut<kin::Animator>()->speed = 0.0f; // stays at its start
    kin::update_animators(world, 0.25f);
    assert(near(second.get<kin::RectRenderer>()->size.x, 15.0f) && near(first.get<kin::RectRenderer>()->size.x, 10.0f));
    assert(near(prefab.get<kin::RectRenderer>()->size.x, 10.0f)); // the prefab as it was

    // A state machine of the file: from its initial state, on to another as
    // the game's parameters say (a trigger taken once), its state told.
    fragment.state_machines["moves"] = {.initial = "still",
                                        .states = {{"still", "pulse"}, {"going", "slide"}},
                                        .transitions = {{.from = "still", .to = "going", .when = {{.param = "go", .op = kin::CondOp::TriggerSet}}},
                                                        {.from = "going", .to = "still", .when = {{.op = kin::CondOp::Finished}}}}};
    assert(kin::save_animation_registry_fragment(fragment, dir / "moves.kinanim"));
    assets.reload_changed();
    kin::EcsEntity walker = world.entity("walker").set(kin::Transform2D{}).set(kin::RectRenderer{})
                                .set(kin::Animator{.asset = {.id = "moves.kinanim"}, .machine = "moves"});
    assets.bind(world);
    kin::update_animators(world, 0.0f);
    kin::update_animators(world, 0.1f);
    assert(walker.get<kin::Animator>()->state == "still");
    walker.get_mut<kin::Animator>()->params.set_trigger("go");
    kin::update_animators(world, 0.1f);
    assert(walker.get<kin::Animator>()->state == "going" && walker.get<kin::Animator>()->params.triggers.empty());
    kin::update_animators(world, 0.5f);
    assert(near(walker.get<kin::Transform2D>()->pos.x, 60.0f)); // 0.1 s on the step it went, and 0.5 s
    kin::update_animators(world, 0.6f); // slid through: back to still
    kin::update_animators(world, 0.0f);
    assert(walker.get<kin::Animator>()->state == "still");
    fs::remove_all(dir);
}

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
    test_sprite_and_texture_renderers_flip();
    test_rotated_render_command_bounds_expand();
    test_top_down_render_applies_camera_and_culling();
    test_collect_culls_rotated_sprites();
    test_parallel_collect_matches_serial();
    test_static_render_cache_merges_with_dynamic_queue();
    test_world_render_state_collect_dynamic_excludes_static_renderables();
    test_one_shot_submit_reuses_scratch_queue_output();
    test_scene_cameras();
#if KIN_ENABLE_IMAGE_IO
    test_assets2d_binds_and_reloads_images();
    test_ecs_scene_binds_assets();
    test_assets2d_binds_sprite_catalogs();
    test_assets2d_binds_particles();
    test_assets2d_binds_tilemaps();
    test_scene_shapes_clips_and_text();
    test_lights_blends_and_shader_effects();
    test_scene_text_translated();
    test_animators();
#if KIN_ENABLE_AUDIO
    test_assets2d_binds_audio_emitters();
#endif
#endif
    return 0;
}
