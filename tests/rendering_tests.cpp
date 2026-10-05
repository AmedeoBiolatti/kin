#include <kin/renderer/material.hpp>
#include <kin/renderer/render_graph.hpp>
#include <kin/renderer/render_profile.hpp>
#include <kin/renderer/static_texture_layer.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <algorithm>
#include <array>
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
    void draw_texture(const kin::Texture&, kin::Rectf source, kin::Rectf dest) override {
        rects.push_back(dest);
        sources.push_back(source);
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
    void draw_sprites(const kin::Texture& texture, std::span<const kin::SpriteInstance> sprites) override {
        sprite_batches.push_back(sprites.size());
        IRenderer2DBackend::draw_sprites(texture, sprites); // one draw_texture each, recorded as usual
    }

    std::vector<std::size_t> sprite_batches;
    std::vector<kin::Rectf> sources;
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

// A ranged cull leaves the commands outside [first, last) alone.
void test_render_queue_culls_a_range() {
    kin::RenderQueue queue;
    for (const float x : {-100.0f, 10.0f, -100.0f, 10.0f, -100.0f}) {
        queue.fill_rect({}, {x, 10.0f, 4.0f, 4.0f}, kin::colors::white);
    }
    kin::RenderView view{.cull_rect = {0.0f, 0.0f, 40.0f, 40.0f}, .culling_enabled = true};
    queue.cull(view, 1, 3);
    assert(queue.size() == 4);
    assert(queue.commands()[0].rect.x == -100.0f && queue.commands()[1].rect.x == 10.0f);
    assert(queue.commands()[2].rect.x == 10.0f && queue.commands()[3].rect.x == -100.0f);
    queue.cull(view, 3);
    assert(queue.size() == 3);
    queue.cull(view);
    assert(queue.size() == 2);
}

// Plain sprites are queued compactly beside the other commands; every view of
// the queue (flush order, commands(), culls by submission number, sorting) must
// be as if they were ordinary commands.
void test_render_queue_sprite_lane_keeps_order() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    const std::array<kin::u8, 4> pixel{255, 255, 255, 255};
    const kin::Texture texture = renderer.create_texture_from_rgba(pixel.data(), {1, 1});
    const kin::Sprite sprite{.texture = texture, .source = {0.0f, 0.0f, 1.0f, 1.0f}};
    const kin::Material2D tinted{.tint = kin::Color::rgb(255, 0, 0)};

    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    queue.fill_rect({}, {0.0f, 0.0f, 1.0f, 1.0f}, kin::colors::white);                   // 0
    queue.draw_texture({}, texture, {1.0f, 0.0f, 1.0f, 1.0f});                            // 1: queued sprite
    queue.draw_sprite({}, sprite, {2.0f, 0.0f, 1.0f, 1.0f});                              // 2: queued sprite
    queue.fill_rect({}, {3.0f, 0.0f, 1.0f, 1.0f}, kin::colors::white);                   // 3
    queue.draw_texture({}, texture, {4.0f, 0.0f, 1.0f, 1.0f}, kin::colors::white, {&tinted}); // 4: a material, so a command
    queue.submit({.type = kin::RenderCommandType::Sprite, .rect = {5.0f, 0.0f, 1.0f, 1.0f},
                  .source = sprite.source, .texture = texture});                           // 5: queued sprite
    assert(queue.size() == 6 && queue.submitted() == 6);

    queue.flush(renderer);
    const std::vector<std::string> expected{"fill", "sprite", "sprite", "fill", "sprite", "sprite"};
    assert(raw->commands == expected);
    // Runs: sprites 1-2, then 4 (a command, with its material's tint) and 5.
    assert((raw->sprite_batches == std::vector<std::size_t>{2, 2}));
    for (std::size_t i = 0; i < raw->rects.size(); ++i) {
        assert(raw->rects[i].x == static_cast<float>(i));
    }

    // commands() converts the queued sprites, in submission order.
    const auto commands = queue.commands();
    const std::array<kin::RenderCommandType, 6> types{
        kin::RenderCommandType::FillRect, kin::RenderCommandType::Texture, kin::RenderCommandType::Sprite,
        kin::RenderCommandType::FillRect, kin::RenderCommandType::Texture, kin::RenderCommandType::Sprite};
    assert(commands.size() == 6);
    for (std::size_t i = 0; i < commands.size(); ++i) {
        assert(commands[i].type == types[i] && commands[i].sequence == i && commands[i].rect.x == static_cast<float>(i));
    }
    assert(commands[1].texture == texture && commands[1].source.w == 0.0f && commands[2].source.w == 1.0f);
    raw->commands.clear();
    raw->rects.clear();
    queue.flush(renderer);
    assert(raw->commands == expected);

    // Y order across both kinds; ties keep submission order.
    queue.clear();
    queue.set_sort(kin::RenderSortMode::LayerThenY);
    queue.draw_texture({.y = 30.0f, .use_y = true}, texture, {0.0f, 30.0f, 1.0f, 1.0f});
    queue.fill_rect({.y = 10.0f, .use_y = true}, {1.0f, 10.0f, 1.0f, 1.0f}, kin::colors::white);
    queue.draw_texture({.y = 20.0f, .use_y = true}, texture, {2.0f, 20.0f, 1.0f, 1.0f});
    queue.fill_rect({.y = 20.0f, .use_y = true}, {3.0f, 20.0f, 1.0f, 1.0f}, kin::colors::white);
    queue.draw_texture({.y = 20.0f, .use_y = true}, texture, {4.0f, 20.0f, 1.0f, 1.0f});
    raw->commands.clear();
    raw->rects.clear();
    queue.flush(renderer);
    const std::array<float, 5> by_y{1.0f, 2.0f, 3.0f, 4.0f, 0.0f};
    for (std::size_t i = 0; i < by_y.size(); ++i) {
        assert(raw->rects[i].x == by_y[i]);
    }
    queue.sort_commands();
    for (std::size_t i = 0; i < by_y.size(); ++i) {
        assert(queue.commands()[i].rect.x == by_y[i]);
    }

    // Ranged culls count submissions, whichever kind each command is.
    queue.clear();
    queue.set_sort(kin::RenderSortMode::Submission);
    for (int i = 0; i < 6; ++i) {
        const float x = i % 2 == 0 ? -100.0f : 10.0f;
        if (i < 3) {
            queue.draw_texture({}, texture, {x, 10.0f, 4.0f, 4.0f});
        } else {
            queue.fill_rect({}, {x, 10.0f, 4.0f, 4.0f}, kin::colors::white);
        }
    }
    const kin::RenderView view{.cull_rect = {0.0f, 0.0f, 40.0f, 40.0f}, .culling_enabled = true};
    queue.cull(view, 2, 5); // drops submissions 2 and 4
    assert(queue.size() == 4);
    const std::array<float, 4> kept{-100.0f, 10.0f, 10.0f, 10.0f};
    for (std::size_t i = 0; i < kept.size(); ++i) {
        assert(queue.commands()[i].rect.x == kept[i]);
    }
}

// A flush hands runs of same-texture sprites to draw_sprites() in one call; any
// other draw, or another texture, ends a run, and a lone sprite is drawn alone.
void test_render_queue_batches_sprites_by_texture() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    const std::array<kin::u8, 4> pixel{255, 255, 255, 255};
    const kin::Texture a = renderer.create_texture_from_rgba(pixel.data(), {1, 1});
    const kin::Texture b = renderer.create_texture_from_rgba(pixel.data(), {1, 1});
    assert(a == a && !(a == b));

    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    const auto sprite = [&](const kin::Texture& texture, float x) {
        queue.draw_texture({}, texture, {x, 0.0f, 4.0f, 4.0f});
    };
    sprite(a, 0.0f);
    sprite(a, 4.0f);
    sprite(a, 8.0f);
    queue.fill_rect({}, {0.0f, 8.0f, 4.0f, 4.0f}, kin::colors::white);
    sprite(a, 12.0f);
    sprite(b, 16.0f);
    sprite(b, 20.0f);
    sprite(a, 24.0f);
    queue.flush(renderer);

    assert((raw->sprite_batches == std::vector<std::size_t>{3, 2}));
    const std::vector<std::string> expected{"sprite", "sprite", "sprite", "fill", "sprite", "sprite", "sprite", "sprite"};
    assert(raw->commands == expected);
    for (std::size_t i = 0; i < 3; ++i) {
        assert(raw->rects[i].x == 4.0f * static_cast<float>(i));
    }
}

// The queue sorts packed keys with a radix sort above a size threshold and a
// comparison sort below it or once commands are out of submission order. Every
// path must match a stable sort by (layer, y, order), i.e. ties keep submission
// order, with -0 and +0 equal.
void test_render_queue_sort_matches_reference() {
    struct Submitted {
        kin::RenderKey key;
        int id = 0;
    };
    kin::u32 seed = 12345;
    const auto next = [&](int range) {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<int>((seed >> 8) % static_cast<kin::u32>(range));
    };
    const std::array<float, 7> ys{-0.0f, 0.0f, -12.5f, 3.0f, 3.0f, 1.0e6f, -1.0e-3f};

    for (const kin::RenderSortMode mode : {kin::RenderSortMode::LayerThenOrder, kin::RenderSortMode::LayerThenY}) {
        for (const int count : {5, 300, 5000}) {
            auto backend = std::make_unique<FakeBackend>();
            FakeBackend* raw = backend.get();
            kin::Renderer2D renderer{std::move(backend)};
            kin::RenderSortMode current = mode;
            kin::RenderQueue queue{mode};
            std::vector<Submitted> submitted;
            const auto submit = [&](int n) {
                for (int i = 0; i < n; ++i) {
                    const kin::RenderKey key{.layer = next(7) - 3,
                                             .order = next(5) - 2 + (next(50) == 0 ? 1 << 20 : 0),
                                             .y = ys[static_cast<std::size_t>(next(7))],
                                             .use_y = next(4) != 0};
                    const int id = static_cast<int>(submitted.size());
                    submitted.push_back({key, id});
                    queue.fill_rect(key, {static_cast<float>(id), 0.0f, 1.0f, 1.0f}, kin::colors::white);
                }
            };
            const auto expected = [&] {
                std::vector<Submitted> sorted = submitted;
                std::ranges::stable_sort(sorted, [&](const Submitted& a, const Submitted& b) {
                    if (a.key.layer != b.key.layer) {
                        return a.key.layer < b.key.layer;
                    }
                    if (current == kin::RenderSortMode::LayerThenY) {
                        const float ay = a.key.use_y ? a.key.y : 0.0f;
                        const float by = b.key.use_y ? b.key.y : 0.0f;
                        if (ay != by) {
                            return ay < by;
                        }
                    }
                    return a.key.order < b.key.order;
                });
                std::vector<int> ids;
                for (const Submitted& entry : sorted) {
                    ids.push_back(entry.id);
                }
                return ids;
            };
            const auto flushed_ids = [&] {
                raw->rects.clear();
                queue.flush(renderer);
                std::vector<int> ids;
                for (const kin::Rectf& rect : raw->rects) {
                    ids.push_back(static_cast<int>(rect.x));
                }
                return ids;
            };
            const auto queued_ids = [&] {
                std::vector<int> ids;
                for (const kin::RenderCommand& command : queue.commands()) {
                    ids.push_back(static_cast<int>(command.rect.x));
                }
                return ids;
            };

            submit(count);
            assert(flushed_ids() == expected());
            // flush draws through the order without moving the commands.
            for (int i = 0; i < count; ++i) {
                assert(queue.commands()[static_cast<std::size_t>(i)].rect.x == static_cast<float>(i));
            }

            queue.sort_commands();
            assert(queued_ids() == expected());
            assert(flushed_ids() == expected());

            // Commands added after a sort are out of submission order as a whole.
            submit(count / 2 + 3);
            queue.sort_commands();
            assert(queued_ids() == expected());
            assert(flushed_ids() == expected());

            // A new mode must put back in submission order ties that the old mode
            // had separated, e.g. by y.
            current = mode == kin::RenderSortMode::LayerThenY ? kin::RenderSortMode::LayerThenOrder
                                                              : kin::RenderSortMode::LayerThenY;
            queue.set_sort(current);
            queue.sort_commands();
            assert(queued_ids() == expected());
            assert(flushed_ids() == expected());
        }
    }
}

// Sprites keep their texture and region in the command's texture/source fields;
// a material is kept by pointer; bulk submits continue the sequence so sorting
// keeps them after earlier commands with equal keys, in their given order.
void test_render_queue_sprite_fields_and_bulk_submit() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    const kin::Sprite sprite{.texture = kin::Texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{32, 16})},
                             .source = {8.0f, 0.0f, 8.0f, 8.0f}};
    const kin::Material2D material{.id = "tinted", .tint = kin::Color::rgb(128, 255, 255)};

    kin::RenderQueue queue{kin::RenderSortMode::LayerThenOrder};
    queue.draw_sprite({}, sprite, {0.0f, 0.0f, 8.0f, 8.0f}, kin::colors::white, {.material = &material});
    queue.draw_sprite({}, kin::Sprite{.texture = sprite.texture}, {1.0f, 0.0f, 8.0f, 8.0f}); // empty region: skipped
    const kin::RenderCommand& command = queue.commands()[0];
    assert(command.type == kin::RenderCommandType::Sprite);
    assert(command.source == sprite.source);
    assert(command.texture.size() == (kin::Vec2i{32, 16}));
    assert(command.material == &material);

    std::vector<kin::RenderCommand> batch;
    for (int i = 0; i < 3; ++i) {
        batch.push_back({.type = kin::RenderCommandType::FillRect, .rect = {10.0f + static_cast<float>(i), 0.0f, 1.0f, 1.0f}});
    }
    queue.submit(batch);
    queue.fill_rect({.layer = -1}, {20.0f, 0.0f, 1.0f, 1.0f}, kin::colors::white);
    assert(queue.size() == 6);
    for (std::size_t i = 0; i < queue.size(); ++i) {
        assert(queue.commands()[i].sequence == i);
    }

    queue.flush(renderer);
    // Layer -1 first, then submission order: the sprite (the empty one draws
    // nothing), then the batch.
    assert((raw->commands == std::vector<std::string>{"fill", "sprite", "fill", "fill", "fill"}));
    assert(raw->rects[0].x == 20.0f);
    assert(raw->rects[1].x == 0.0f);
    assert(raw->rects[2].x == 10.0f && raw->rects[3].x == 11.0f && raw->rects[4].x == 12.0f);
}

// A group draws its content together at its own key, sorted among itself,
// through its clip; under a camera that only moves, the clip moves with it.
void test_render_queue_groups() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    auto content = std::make_shared<kin::RenderQueue>(kin::RenderSortMode::LayerThenOrder);
    content->fill_rect({.order = 9}, {11.0f, 0.0f, 1.0f, 1.0f}, kin::colors::white);
    content->fill_rect({.order = -9}, {10.0f, 0.0f, 1.0f, 1.0f}, kin::colors::white);
    kin::RenderQueue queue{kin::RenderSortMode::LayerThenOrder};
    queue.fill_rect({.order = 2}, {2.0f, 0.0f, 1.0f, 1.0f}, kin::colors::white);
    queue.draw_group({.order = 1}, content, kin::ClipRegion::to_rect({10.0f, 0.0f, 5.0f, 5.0f}));
    queue.fill_rect({.order = 0}, {0.0f, 0.0f, 1.0f, 1.0f}, kin::colors::white);
    queue.flush(renderer);
    // No masks here: the clip is its bounds, as the scissor (a viewport, to this backend).
    assert((raw->commands == std::vector<std::string>{"fill", "push", "fill", "fill", "pop", "fill"}));
    assert(raw->rects[0].x == 0.0f && raw->rects[1].x == 10.0f && raw->rects[2].x == 10.0f &&
           raw->rects[3].x == 11.0f && raw->rects[4].x == 2.0f);

    // Under a camera that only moves, the group culls by its clip and the clip moves.
    kin::Camera2D camera;
    camera.viewport = {100.0f, 100.0f};
    camera.offset = {5.0f, 0.0f};
    kin::RenderView view;
    view.camera = &camera;
    view.culling_enabled = true;
    raw->commands.clear();
    raw->rects.clear();
    queue.flush(renderer, view);
    // (The fills outside the group, at x 0 and 2, are now off screen.)
    assert((raw->commands == std::vector<std::string>{"push", "fill", "fill", "pop"}));
    assert(raw->rects[0].x == 5.0f && raw->rects[1].x == 5.0f && raw->rects[2].x == 6.0f);
    camera.offset = {500.0f, 0.0f}; // the clip far off screen: the group is not drawn at all
    raw->commands.clear();
    queue.flush(renderer, view);
    assert(std::ranges::count(raw->commands, "push") == 0);
}

// A flip reaches the backend as a source read from the far side, from the
// sprite lane and from the general one (a material) alike.
void test_render_queue_flips_sprites() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    const kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{32, 16})};
    const kin::Sprite sprite{.texture = texture, .source = {8.0f, 0.0f, 8.0f, 8.0f}};
    const kin::Material2D material{.id = "plain"};

    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    queue.draw_sprite({}, sprite, {0.0f, 0.0f, 8.0f, 8.0f}, kin::colors::white, {}, 0.0f, {0.5f, 0.5f}, kin::Flip::X);
    queue.draw_sprite({}, sprite, {0.0f, 0.0f, 8.0f, 8.0f}, kin::colors::white, {.material = &material}, 0.0f,
                      {0.5f, 0.5f}, kin::Flip::Y);
    queue.draw_texture({}, texture, {0.0f, 0.0f, 32.0f, 16.0f}, kin::colors::white, {}, 0.0f, {0.5f, 0.5f}, kin::Flip::XY);
    queue.draw_texture_region({}, texture, {0.0f, 4.0f, 8.0f, 4.0f}, {0.0f, 0.0f, 8.0f, 4.0f});
    assert(std::any_of(queue.commands().begin(), queue.commands().end(), [&](const kin::RenderCommand& c) {
        return c.material == &material && c.flip == kin::Flip::Y;
    }));
    queue.flush(renderer);

    assert(raw->sources.size() == 4);
    assert((raw->sources[0] == kin::Rectf{16.0f, 0.0f, -8.0f, 8.0f}));
    assert((raw->sources[1] == kin::Rectf{8.0f, 8.0f, 8.0f, -8.0f}));
    assert((raw->sources[2] == kin::Rectf{32.0f, 16.0f, -32.0f, -16.0f})); // the whole texture
    assert((raw->sources[3] == kin::Rectf{0.0f, 4.0f, 8.0f, 4.0f}));        // unflipped
    static_assert(kin::mirrored(kin::mirrored(kin::Rectf{1.0f, 2.0f, 3.0f, 4.0f}, kin::Flip::XY), kin::Flip::XY) ==
                  kin::Rectf{1.0f, 2.0f, 3.0f, 4.0f});
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
    test_render_queue_culls_a_range();
    test_render_queue_batches_sprites_by_texture();
    test_render_queue_sprite_lane_keeps_order();
    test_render_queue_sort_matches_reference();
    test_render_queue_sprite_fields_and_bulk_submit();
    test_render_queue_groups();
    test_render_queue_flips_sprites();
    test_render_queue_pass_masks_and_text_command();
    test_render_graph_pass_toggles_and_stats();
    test_default_render_graph_flushes_pass_masks();
    test_backend_capabilities_are_reported_by_facade();
    test_material_fallback_and_profiles();
    test_queue_flush_applies_camera_view();
    test_rgba_canvas_blits_sprite_from_catalog_pixels();
    return 0;
}
