#include <kin/core/json.hpp>
#include <kin/core/instrumentation.hpp>
#include <kin/core/jobs.hpp>
#include <kin/core/profile.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/ui2.hpp>
#include <kin/platform/input.hpp>
#include <kin/prefab/prefab_asset.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/render_queue.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/sprite.hpp>
#include <kin/renderer/texture.hpp>
#include <kin/tilemap/tilemap.hpp>
#include <kin/ui2/theme.hpp>

#include "input_latency_bench.hpp"
#if defined(KIN_BENCH_HAS_EXAMPLES)
#include "workloads.hpp"
#endif
#include <optional>
#include <stdexcept>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

thread_local kin::TimingCollector* current_bench_timings = nullptr;

struct BenchOptions {
    bool list = false;
    bool profile_lines = false;
    bool fail_on_allocation = false;
    int iterations = 5;
    int warmup = 1;
    int input_sim_hz = 60;
    int input_render_hz = 144;
    std::string suite;
    std::string case_name;
    std::string json_path;
};

struct BenchSample {
    double ms = 0.0;
    kin::u64 total = 0;
    kin::u64 allocations = 0;
    kin::u64 allocated_bytes = 0;
};

struct BenchResult {
    std::string suite;
    std::string name;
    std::vector<BenchSample> samples;
    kin::u64 total = 0;
    double min_ms = 0.0;
    double mean_ms = 0.0;
    double median_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    kin::u64 allocations = 0;
    kin::u64 allocated_bytes = 0;
    std::vector<kin::TimingSummary> timing_scopes;
    std::optional<InputLatencyResult> input_latency;
};

using BenchFn = kin::u64 (*)(int);

struct BenchCase {
    std::string_view suite;
    std::string_view name;
    BenchFn fn = nullptr;
    int sim_hz = 0;
    int render_hz = 0;
};

kin::TileMap make_bench_map(kin::i32 cols, kin::i32 rows) {
    kin::TileMap map;
    map.resize(cols, rows);
    map.tile_w = 8;
    map.tile_h = 8;
    map.tileset.resize(4);
    map.tileset.tiles[1] = {
        .id = "floor",
        .solid = false,
        .cost = 10,
        .color = kin::Color::rgb(30, 40, 50),
        .top_edge_color = kin::Color::rgb(45, 55, 65),
    };
    map.tileset.tiles[2] = {
        .id = "wall",
        .solid = true,
        .walkable = false,
        .cost = 100,
        .color = kin::Color::rgb(100, 110, 120),
        .top_edge_color = kin::Color::rgb(140, 150, 160),
    };
    map.tileset.tiles[3] = {
        .id = "mud",
        .solid = false,
        .cost = 30,
        .color = kin::Color::rgb(70, 60, 45),
        .top_edge_color = kin::Color::rgb(90, 80, 60),
    };

    map.fill_rect({0, 0, cols, rows}, 1);
    kin::TileLayer& dense = map.add_layer("dense", 1);
    dense.participates_in_navigation = false;
    kin::TileLayer& sparse = map.add_layer("sparse", 2);
    sparse.participates_in_navigation = false;
    kin::TileLayer& nav = map.add_layer("nav", 3);
    nav.participates_in_collision = false;
    nav.participates_in_navigation = true;

    for (kin::i32 row = 0; row < rows; ++row) {
        for (kin::i32 col = 0; col < cols; ++col) {
            if ((row + col) % 3 == 0) {
                map.set("dense", col, row, 2);
            }
            if (row % 17 == 0 && col % 11 == 0) {
                map.set("sparse", col, row, 2);
            }
            if ((row + col) % 9 == 0) {
                map.set("nav", col, row, 3);
            }
        }
    }
    map.clear_dirty();
    return map;
}

struct TilemapFixture {
    static constexpr kin::i32 cols = 192;
    static constexpr kin::i32 rows = 128;

    kin::TileMap map = make_bench_map(cols, rows);
    kin::RenderView full_view{
        .cull_rect = {0.0f, 0.0f, static_cast<kin::f32>(cols * map.tile_w), static_cast<kin::f32>(rows * map.tile_h)},
        .culling_enabled = true,
    };
    kin::RenderView partial_view{
        .cull_rect = {160.0f, 120.0f, 640.0f, 360.0f},
        .culling_enabled = true,
    };

    TilemapFixture() {
        map.set_render_chunk_size(16, 16);
        kin::RenderQueue warmup{kin::RenderSortMode::Submission};
        map.submit(warmup, full_view, {.draw_top_edge = true});
    }
};

TilemapFixture& tilemap_fixture() {
    static TilemapFixture fixture;
    return fixture;
}

kin::u64 cached_chunk_rebuild_submit(int) {
    KIN_PROFILE_LINE("tilemap.cached_chunk_rebuild_submit");
    TilemapFixture& fixture = tilemap_fixture();
    fixture.map.invalidate_render_cache();
    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    fixture.map.submit(queue, fixture.full_view, {.draw_top_edge = true});
    return queue.size() + fixture.map.cached_render_chunk_count();
}

kin::u64 cached_partial_submit(int) {
    KIN_PROFILE_LINE("tilemap.cached_partial_submit");
    TilemapFixture& fixture = tilemap_fixture();
    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    fixture.map.submit(queue, fixture.partial_view, {.draw_top_edge = true});
    return queue.size();
}

kin::u64 uncached_full_submit(int) {
    KIN_PROFILE_LINE("tilemap.uncached_full_submit");
    TilemapFixture& fixture = tilemap_fixture();
    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    fixture.map.submit(queue, fixture.full_view, {.draw_top_edge = true, .static_renderable = false});
    return queue.size();
}

kin::u64 collision_rects_dense(int) {
    KIN_PROFILE_LINE("tilemap.collision_rects_dense");
    return tilemap_fixture().map.collision_rects("dense").size();
}

kin::u64 collision_rects_sparse(int) {
    KIN_PROFILE_LINE("tilemap.collision_rects_sparse");
    return tilemap_fixture().map.collision_rects("sparse").size();
}

kin::u64 blocks_rect_sweep(int i) {
    KIN_PROFILE_LINE("tilemap.blocks_rect_sweep");
    TilemapFixture& fixture = tilemap_fixture();
    const kin::f32 x = static_cast<kin::f32>((i * 19) % (TilemapFixture::cols * fixture.map.tile_w - 64));
    const kin::f32 y = static_cast<kin::f32>((i * 13) % (TilemapFixture::rows * fixture.map.tile_h - 64));
    return fixture.map.blocks_rect({x, y, 64.0f, 64.0f}) ? 1u : 0u;
}

kin::u64 aggregate_nav_samples(int i) {
    KIN_PROFILE_LINE("tilemap.aggregate_nav_samples");
    TilemapFixture& fixture = tilemap_fixture();
    const kin::i32 col = (i * 17) % TilemapFixture::cols;
    const kin::i32 row = (i * 23) % TilemapFixture::rows;
    return static_cast<kin::u64>(fixture.map.walkable(col, row) ? 1 : 0) +
           static_cast<kin::u64>(fixture.map.movement_cost(col, row));
}

// ---------------------------------------------------------------------------
// Render-queue / sprite-pipeline benches.
//
// These isolate the CPU-side rendering machinery — RenderCommand construction,
// sort, and execute_render_command dispatch — from rasterization, so we can see
// how the queue cost scales with primitive count (the "Tier 1" question). A
// no-op backend stands in for a real renderer: every draw resolves through the
// same Renderer2D -> IRenderer2DBackend virtual path a game pays, but does no
// pixel work, so the timings reflect engine overhead only.
// ---------------------------------------------------------------------------

class NullTextureBackend final : public kin::ITextureBackend {
public:
    explicit NullTextureBackend(kin::Vec2i size) : _size(size) {}
    kin::Vec2i size() const override { return _size; }

private:
    kin::Vec2i _size;
};

class NullBackend final : public kin::IRenderer2DBackend {
public:
    std::string_view name() const override { return "null"; }
    void clear(kin::Color) override {}
    void present() override {}
    void set_logical_size(kin::Vec2i) override {}
    void set_integer_logical_size(kin::Vec2i) override {}
    kin::Vec2i output_size() const override { return {1280, 720}; }
    kin::Vec2f window_to_logical(kin::Vec2f p) const override { return p; }
    kin::Vec2f logical_to_window(kin::Vec2f p) const override { return p; }
    kin::Texture create_texture_from_rgba(const kin::u8*, kin::Vec2i s) override {
        return kin::Texture{std::make_shared<NullTextureBackend>(s)};
    }
    void draw_texture(const kin::Texture&, kin::Rectf) override { ++_draws; }
    void draw_texture(const kin::Texture&, kin::Rectf, kin::Rectf) override { ++_draws; }
    void draw_sprites(const kin::Texture&, std::span<const kin::SpriteInstance> sprites) override { _draws += sprites.size(); }
    void fill_rect(kin::Rectf, kin::Color) override { ++_draws; }
    void draw_rect(kin::Rectf, kin::Color) override { ++_draws; }
    void draw_line(kin::Vec2f, kin::Vec2f, kin::Color) override { ++_draws; }
    void set_viewport(kin::Rectf) override {}
    void reset_viewport() override {}
    void push_viewport(kin::Rectf) override {}
    void pop_viewport() override {}

    kin::u64 draws() const { return _draws; }

private:
    kin::u64 _draws = 0;
};

struct RenderFixture {
    static constexpr int texture_count = 16;
    std::vector<kin::Sprite> sprites;
    kin::RenderQueue queue{kin::RenderSortMode::LayerThenY};
    kin::Renderer2D renderer{std::make_unique<NullBackend>()};

    RenderFixture() {
        sprites.reserve(texture_count);
        for (int i = 0; i < texture_count; ++i) {
            kin::Texture tex{std::make_shared<NullTextureBackend>(kin::Vec2i{32, 32})};
            sprites.push_back(kin::Sprite{.texture = std::move(tex), .source = {0.0f, 0.0f, 32.0f, 32.0f}});
        }
    }
};

RenderFixture& render_fixture() {
    static RenderFixture fixture;
    return fixture;
}

// Deterministic but scattered key so sort_commands does representative work
// (mixed layers/orders/y) rather than sorting an already-ordered list.
kin::RenderKey scattered_key(int n) {
    const kin::u32 h = static_cast<kin::u32>(n) * 2654435761u;
    kin::RenderKey key;
    key.layer = static_cast<kin::i32>((h >> 5) % 8) * 100;
    key.order = static_cast<kin::i32>((h >> 11) % 32);
    key.y = static_cast<kin::f32>((h >> 3) % 4096);
    key.use_y = true;
    return key;
}

void build_sprite_queue(kin::RenderQueue& queue, int count) {
    const RenderFixture& fixture = render_fixture();
    queue.clear();
    queue.reserve(static_cast<std::size_t>(count));
    for (int n = 0; n < count; ++n) {
        const kin::Sprite& sprite = fixture.sprites[static_cast<std::size_t>(n) % fixture.sprites.size()];
        const kin::u32 h = static_cast<kin::u32>(n) * 40503u;
        const kin::Rectf dest{
            static_cast<kin::f32>(h % 1280u), static_cast<kin::f32>((h >> 4) % 720u), 32.0f, 32.0f};
        queue.draw_sprite(scattered_key(n), sprite, dest);
    }
}

// Build only: RenderCommand construction (incl. per-sprite Texture shared_ptr churn).
template <int N>
kin::u64 render_submit(int) {
    kin::RenderQueue& queue = render_fixture().queue;
    queue.set_sort(kin::RenderSortMode::LayerThenY);
    build_sprite_queue(queue, N);
    return queue.size();
}

// Build + sort: subtract render_submit to isolate sort_commands.
template <int N>
kin::u64 render_submit_sort(int) {
    kin::RenderQueue& queue = render_fixture().queue;
    queue.set_sort(kin::RenderSortMode::LayerThenY);
    build_sprite_queue(queue, N);
    queue.sort_commands();
    return queue.size();
}

// Build + sort + flush. The flush's internal sort is neutralized (set to
// Submission after the real sort) so the delta vs render_submit_sort is pure
// dispatch (execute_render_command + the Renderer2D->backend virtual calls).
template <int N>
kin::u64 render_submit_sort_flush(int) {
    RenderFixture& fixture = render_fixture();
    fixture.queue.set_sort(kin::RenderSortMode::LayerThenY);
    build_sprite_queue(fixture.queue, N);
    fixture.queue.sort_commands();
    fixture.queue.set_sort(kin::RenderSortMode::Submission); // make flush's re-sort a no-op
    fixture.queue.flush(fixture.renderer);
    return fixture.queue.size();
}

// Build + flush, the everyday path: flush sorts and draws in one call. Compare
// to render_submit_sort_flush, which sorts the commands physically first.
template <int N>
kin::u64 render_submit_flush(int) {
    RenderFixture& fixture = render_fixture();
    fixture.queue.set_sort(kin::RenderSortMode::LayerThenY);
    build_sprite_queue(fixture.queue, N);
    fixture.queue.flush(fixture.renderer);
    return fixture.queue.size();
}

// Rect-only build (no Texture/shared_ptr) — compare to render_submit at the same
// N to quantify the per-command shared_ptr refcount cost the Texture-handle
// proposal would remove.
template <int N>
kin::u64 render_submit_rect(int) {
    kin::RenderQueue& queue = render_fixture().queue;
    queue.set_sort(kin::RenderSortMode::LayerThenY);
    queue.clear();
    queue.reserve(static_cast<std::size_t>(N));
    for (int n = 0; n < N; ++n) {
        const kin::u32 h = static_cast<kin::u32>(n) * 40503u;
        const kin::Rectf dest{
            static_cast<kin::f32>(h % 1280u), static_cast<kin::f32>((h >> 4) % 720u), 32.0f, 32.0f};
        queue.fill_rect(scattered_key(n), dest, kin::colors::white);
    }
    return queue.size();
}

// ---------------------------------------------------------------------------
// UI theme benches. Games/scenes (e.g. MenuScene) rebuild the whole Theme via
// game_theme() every frame even though the inputs never change. These measure
// that rebuild cost and the win from memoizing the factory: with a per-(preset)
// memo, `same_preset` becomes a cache hit (cheap) while `cycle_preset` keeps
// missing (full rebuild) — the gap is the saved per-frame cost.
// ---------------------------------------------------------------------------
template <bool same_preset>
kin::u64 bench_game_theme(int i) {
    const auto preset = same_preset
        ? kin::ui2::PalettePreset::Default
        : static_cast<kin::ui2::PalettePreset>(i % 6);
    const kin::ui2::Theme theme = kin::ui2::game_theme(preset); // default (null) font
    return static_cast<kin::u64>(theme.palette.accent.r);       // touch a field
}

struct BenchPosition {
    kin::Vec2f pos{};
    kin::i32 value = 0;
};

struct BenchExcluded {
    kin::i32 marker = 1;
};

void register_bench_components(kin::EcsWorld& world) {
    world.components().native<BenchPosition>("BenchPosition")
        .field("pos", &BenchPosition::pos)
        .field("value", &BenchPosition::value);
    world.components().native<BenchExcluded>("BenchExcluded");
}

struct EcsQueryFixture {
    kin::EcsWorld world;
    kin::EcsQueryPlan plan;

    EcsQueryFixture() {
        register_bench_components(world);
        for (int i = 0; i < 10000; ++i) {
            kin::EcsEntity entity = world.entity("bench_entity_" + std::to_string(i));
            if ((i % 2) == 0) {
                entity.set(BenchPosition{.value = i});
            }
            if ((i % 10) == 0) {
                entity.set(BenchExcluded{});
            }
        }
        plan = world.build_query_plan({.all = {"BenchPosition"}, .none = {"BenchExcluded"}});
    }
};

struct ComponentReadFixture {
    kin::EcsWorld world;
    kin::EcsEntity entity;

    ComponentReadFixture() {
        register_bench_components(world);
        entity = world.entity("component-read");
        entity.set(BenchPosition{{12.0F, 34.0F}, 56});
    }
};

ComponentReadFixture& component_read_fixture() {
    static ComponentReadFixture fixture;
    return fixture;
}

kin::u64 reflected_component_read(int) {
    ComponentReadFixture& fixture = component_read_fixture();
    kin::u64 fields = 0;
    fixture.world.components().visit_fields(
        fixture.entity, "BenchPosition",
        [&](std::string_view, kin::ComponentFieldKind, const kin::ComponentFieldValue&) {
            ++fields;
        });
    return fields;
}

kin::u64 reflected_component_read_snapshot(int) {
    ComponentReadFixture& fixture = component_read_fixture();
    const auto snapshot = fixture.world.components().snapshot(fixture.entity, "BenchPosition");
    return snapshot ? snapshot->fields.size() : 0U;
}

EcsQueryFixture& ecs_query_fixture() {
    static EcsQueryFixture fixture;
    return fixture;
}

kin::u64 ecs_query_10k(int) {
    const std::vector<kin::EcsEntity> entities = ecs_query_fixture().world.query_entities(ecs_query_fixture().plan);
    return entities.size();
}

struct UiWorldFixture {
    kin::EcsWorld world;
    kin::ui2::Context ui;
    kin::Input input;
    kin::Renderer2D renderer{std::make_unique<NullBackend>()};
    kin::Ui2WorldRenderScratch scratch;

    UiWorldFixture() {
        kin::register_ui2_components(world);
        kin::EcsEntity root = world.entity("bench_ui_root");
        root.set(kin::Ui2Root{.bounds = {0.0f, 0.0f, 1280.0f, 720.0f}});
        for (int i = 0; i < 500; ++i) {
            kin::EcsEntity child = world.entity("bench_ui_" + std::to_string(i));
            child.child_of(root);
            child.set(kin::Ui2Layout{.style = {.height = kin::ui2::fixed(20.0f)}, .sequence = static_cast<kin::u32>(i)});
            child.set(kin::ui2::Label{.text = "row " + std::to_string(i)});
        }
        scratch.reserve(512);
        ui.begin(input, renderer);
        kin::update_ui2_world(world, ui, scratch);
        ui.end();
    }
};

UiWorldFixture& ui_world_fixture() {
    static UiWorldFixture fixture;
    return fixture;
}

kin::u64 ui_idle_500(int) {
    UiWorldFixture& fixture = ui_world_fixture();
    fixture.ui.begin(fixture.input, fixture.renderer);
    kin::update_ui2_world(fixture.world, fixture.ui, fixture.scratch);
    fixture.ui.end();
    return fixture.scratch.size();
}

struct PrefabFixture {
    kin::EcsWorld world;
    kin::PrefabAsset prefab;

    PrefabFixture() {
        register_bench_components(world);
        prefab.id = "bench_prefab";
        prefab.root = "root";
        for (int i = 0; i < 4; ++i) {
            kin::PrefabEntityAsset entity{
                .id = i == 0 ? "root" : "child_" + std::to_string(i),
                .name = i == 0 ? "BenchRoot" : "BenchChild" + std::to_string(i),
                .parent = i == 0 ? std::string{} : std::string{"root"},
            };
            entity.components.push_back({
                .name = "BenchPosition",
                .fields = {{.name = "value", .value = kin::i32{i}}},
            });
            prefab.entities.push_back(std::move(entity));
        }
    }
};

PrefabFixture& prefab_fixture() {
    static PrefabFixture fixture;
    return fixture;
}

kin::u64 prefab_spawn_destroy(int) {
    PrefabFixture& fixture = prefab_fixture();
    kin::PrefabInstance instance = kin::instantiate_prefab(
        fixture.world,
        fixture.prefab,
        {},
        fixture.world.components());
    const kin::u64 count = instance.entities.size();
    for (auto it = instance.entities.rbegin(); it != instance.entities.rend(); ++it) {
        it->destroy();
    }
    return count;
}

kin::u64 prefab_spawn_destroy_uncached(int) {
    PrefabFixture& fixture = prefab_fixture();
    static kin::u64 generation = 0;
    kin::PrefabAsset prefab = fixture.prefab;
    prefab.id += "_uncached_" + std::to_string(++generation);
    kin::PrefabInstance instance = kin::instantiate_prefab(
        fixture.world, prefab, {}, fixture.world.components());
    const kin::u64 count = instance.entities.size();
    for (auto it = instance.entities.rbegin(); it != instance.entities.rend(); ++it) {
        it->destroy();
    }
    return count;
}

struct CullFixture {
    kin::EcsWorld world;
    kin::WorldRenderState render{world};
    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    kin::RenderView view{
        .cull_rect = {0.0f, 0.0f, 640.0f, 360.0f},
        .culling_enabled = true,
    };

    CullFixture() {
        for (int i = 0; i < 10000; ++i) {
            const bool visible = (i % 10) == 0;
            world.entity().set(kin::Transform2D{.pos = visible
                ? kin::Vec2f{static_cast<kin::f32>(i % 600), static_cast<kin::f32>(i % 320)}
                : kin::Vec2f{10000.0f + static_cast<kin::f32>(i), 10000.0f}})
                .set(kin::RectRenderer{.size = {16.0f, 16.0f}});
        }
        render.propagate_transforms();
        queue.reserve(10000);
    }
};

CullFixture& cull_fixture() {
    static CullFixture fixture;
    return fixture;
}

kin::u64 render_collect_culled_10k(int) {
    CullFixture& fixture = cull_fixture();
    fixture.queue.clear();
    fixture.render.collect_dynamic(fixture.queue, {.sort = false, .view = &fixture.view});
    return fixture.queue.size();
}

// 100,000 y-sorted texture sprites, all in view (the Signal Siege stress case):
// what the ECS render path costs per visible sprite, backend work excluded.
struct EcsSpriteFixture {
    kin::EcsWorld world;
    kin::WorldRenderState render{world};
    kin::RenderQueue queue{kin::RenderSortMode::LayerThenY};
    kin::Renderer2D renderer{std::make_unique<NullBackend>()};
    kin::Texture atlas{std::make_shared<NullTextureBackend>(kin::Vec2i{384, 128})};
    kin::RenderView view{.cull_rect = {0.0f, 0.0f, 1280.0f, 800.0f}, .culling_enabled = true};

    EcsSpriteFixture() {
        for (int i = 0; i < 100000; ++i) {
            const kin::u32 h = static_cast<kin::u32>(i) * 2654435761u;
            world.entity()
                .set(kin::Transform2D{.pos = {static_cast<kin::f32>(h % 1240u) + 20.0f, static_cast<kin::f32>((h >> 12) % 760u) + 20.0f}})
                .set(kin::TextureRenderer{.texture = atlas, .source = {static_cast<kin::f32>((i % 3) * 128), 0.0f, 128.0f, 128.0f},
                                          .offset = {-16.0f, -16.0f}, .size = {32.0f, 32.0f}, .layer = 1, .y_sort = true});
        }
        render.propagate_transforms();
        queue.reserve(100000);
    }
};

EcsSpriteFixture& ecs_sprite_fixture() {
    static EcsSpriteFixture fixture;
    return fixture;
}

kin::u64 ecs_sprites_collect_100k(int) {
    EcsSpriteFixture& fixture = ecs_sprite_fixture();
    fixture.queue.clear();
    fixture.render.collect_dynamic(fixture.queue, {.view = &fixture.view});
    return fixture.queue.size();
}

// The fixed cost of a parallel_for over idle workers: what a frame pays per call.
kin::u64 jobs_parallel_for_empty(int) {
    static kin::JobSystem jobs;
    static std::array<kin::u64, 64 * 8> sink{};
    kin::u64 total = 0;
    for (int round = 0; round < 10; ++round) {
        jobs.parallel_for(64, [&](kin::i32 i) { sink[static_cast<std::size_t>(i) * 8] += static_cast<kin::u64>(round); });
        total += sink[0];
    }
    return total;
}

kin::u64 ecs_sprites_collect_100k_jobs(int) {
    static kin::JobSystem jobs{{.workers = std::getenv("KIN_BENCH_WORKERS") ? std::atoi(std::getenv("KIN_BENCH_WORKERS")) : 0}};
    EcsSpriteFixture& fixture = ecs_sprite_fixture();
    fixture.queue.clear();
    fixture.render.collect_dynamic(fixture.queue, {.view = &fixture.view, .jobs = &jobs});
    return fixture.queue.size();
}

kin::u64 ecs_sprites_collect_flush_100k(int) {
    EcsSpriteFixture& fixture = ecs_sprite_fixture();
    fixture.queue.clear();
    fixture.render.collect_dynamic(fixture.queue, {.view = &fixture.view});
    fixture.queue.flush(fixture.renderer, fixture.view);
    return fixture.queue.size();
}

#if defined(KIN_BENCH_HAS_EXAMPLES)
template<int Enemies>
kin::u64 arena_workload(int) {
    static examples::Arena arena(Enemies, 7);
    static kin::RenderQueue queue;
    // A short deterministic encounter per sample keeps the workload from
    // changing as benchmark warmup/iteration counts change.
    arena.reset(7);
    for (int i = 0; i < 120; ++i) arena.step(1.0f / 120, arena.autopilot(), true);
    kin::Camera2D camera; camera.viewport = {1280, 800}; camera.offset = {arena.player.x - 640, arena.player.y - 400};
    arena.collect(queue, {.camera = &camera, .culling_enabled = true});
    queue.sort_commands();
    return arena.checksum() + queue.size();
}

template<int Scenario>
kin::u64 tracker_workload(int) {
    static examples::Tracker model(10000, 7);
    static examples::TrackerDashboard dashboard;
    static kin::Input input;
    static kin::Renderer2D renderer{std::make_unique<NullBackend>()};
    if constexpr (Scenario == 1) dashboard.scripted_frame(model, model.ticks, "scroll");
    // Force a filter/sort/selection change per measured churn call.
    if constexpr (Scenario == 2) dashboard.scripted_frame(model, model.ticks * 60, "churn");
    model.streaming = Scenario != 0;
    model.step(1.0f / 120);
    input.begin_frame();
    dashboard.render(model, input, renderer, {1280, 800}, 1.0f / 120);
    return dashboard.drawn_rows();
}
#endif

const std::vector<BenchCase>& bench_cases() {
    static const std::vector<BenchCase> cases{
        {"tilemap", "cached_chunk_rebuild_submit", cached_chunk_rebuild_submit},
        {"tilemap", "cached_partial_submit", cached_partial_submit},
        {"tilemap", "uncached_full_submit", uncached_full_submit},
        {"tilemap", "collision_rects_dense", collision_rects_dense},
        {"tilemap", "collision_rects_sparse", collision_rects_sparse},
        {"tilemap", "blocks_rect_sweep", blocks_rect_sweep},
        {"tilemap", "aggregate_nav_samples", aggregate_nav_samples},

        {"render", "submit_sprites_2k", render_submit<2000>},
        {"render", "submit_sort_sprites_2k", render_submit_sort<2000>},
        {"render", "submit_sort_flush_sprites_2k", render_submit_sort_flush<2000>},
        {"render", "submit_flush_sprites_2k", render_submit_flush<2000>},
        {"render", "submit_sprites_8k", render_submit<8000>},
        {"render", "submit_sort_sprites_8k", render_submit_sort<8000>},
        {"render", "submit_sort_flush_sprites_8k", render_submit_sort_flush<8000>},
        {"render", "submit_flush_sprites_8k", render_submit_flush<8000>},
        {"render", "submit_sprites_32k", render_submit<32000>},
        {"render", "submit_sort_sprites_32k", render_submit_sort<32000>},
        {"render", "submit_sort_flush_sprites_32k", render_submit_sort_flush<32000>},
        {"render", "submit_flush_sprites_32k", render_submit_flush<32000>},
        {"render", "submit_rect_8k", render_submit_rect<8000>},
        {"render", "submit_rect_32k", render_submit_rect<32000>},

        {"ui", "game_theme_same_preset", bench_game_theme<true>},
        {"ui", "game_theme_cycle_preset", bench_game_theme<false>},
        {"ui", "idle_world_500", ui_idle_500},

        {"ecs", "query_10k", ecs_query_10k},
        {"component", "reflected_read", reflected_component_read},
        {"component", "reflected_read_snapshot", reflected_component_read_snapshot},
        {"prefab", "spawn_destroy", prefab_spawn_destroy},
        {"prefab", "spawn_destroy_uncached", prefab_spawn_destroy_uncached},
        {"render", "collect_culled_10k", render_collect_culled_10k},
        {"render", "ecs_sprites_collect_100k", ecs_sprites_collect_100k},
        {"render", "ecs_sprites_collect_flush_100k", ecs_sprites_collect_flush_100k},
        {"render", "ecs_sprites_collect_100k_jobs", ecs_sprites_collect_100k_jobs},
        {"jobs", "parallel_for_empty_x10", jobs_parallel_for_empty},
        {"input", "latency_60_60", nullptr, 60, 60},
        {"input", "latency_60_144", nullptr, 60, 144},
        {"input", "latency_120_144", nullptr, 120, 144},
        {"input", "latency_60_240", nullptr, 60, 240},
        {"input", "latency", nullptr}, // Configurable rates.
#if defined(KIN_BENCH_HAS_EXAMPLES)
        {"examples", "arena_1200_encounter", arena_workload<1200>},
        {"examples", "arena_5000_encounter", arena_workload<5000>},
        {"examples", "arena_100000_encounter", arena_workload<100000>},
        {"examples", "tracker_idle_10k", tracker_workload<0>},
        {"examples", "tracker_scroll_10k", tracker_workload<1>},
        {"examples", "tracker_churn_10k", tracker_workload<2>},
#endif

    };
    return cases;
}

bool parse_int(std::string_view text, int& value) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

BenchOptions parse_options(int argc, char** argv) {
    BenchOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--list") {
            options.list = true;
        } else if (arg == "--profile-lines") {
            options.profile_lines = true;
        } else if (arg == "--fail-on-allocation") {
            options.fail_on_allocation = true;
        } else if (arg.starts_with("--suite=")) {
            options.suite = arg.substr(8);
        } else if (arg == "--suite" && i + 1 < argc) {
            options.suite = argv[++i];
        } else if (arg.starts_with("--case=")) {
            options.case_name = arg.substr(7);
        } else if (arg == "--case" && i + 1 < argc) {
            options.case_name = argv[++i];
        } else if (arg.starts_with("--iterations=")) {
            parse_int(arg.substr(13), options.iterations);
        } else if (arg == "--iterations" && i + 1 < argc) {
            parse_int(argv[++i], options.iterations);
        } else if (arg.starts_with("--warmup=")) {
            parse_int(arg.substr(9), options.warmup);
        } else if (arg == "--warmup" && i + 1 < argc) {
            parse_int(argv[++i], options.warmup);
        } else if (arg.starts_with("--json=")) {
            options.json_path = arg.substr(7);
        } else if (arg == "--json" && i + 1 < argc) {
            options.json_path = argv[++i];
        } else if (arg.starts_with("--input-sim-hz=") || arg.starts_with("--input-render-hz=") ||
                   arg == "--input-sim-hz" || arg == "--input-render-hz") {
            const bool simulation = arg.starts_with("--input-sim-hz");
            const auto equals = arg.find('=');
            const std::string_view value = equals != std::string_view::npos ? arg.substr(equals + 1)
                : i + 1 < argc ? std::string_view{argv[++i]} : std::string_view{};
            int rate = 0;
            if (!parse_int(value, rate) || rate < 10 || rate > 1000) {
                throw std::invalid_argument("input rates must be integers from 10 to 1000 Hz");
            }
            (simulation ? options.input_sim_hz : options.input_render_hz) = rate;
        }
    }
    options.iterations = std::max(1, options.iterations);
    options.warmup = std::max(0, options.warmup);
    return options;
}

bool selected(const BenchOptions& options, const BenchCase& bench) {
    if (!options.suite.empty() && options.suite != bench.suite) {
        return false;
    }
    if (!options.case_name.empty() && options.case_name != bench.name) {
        return false;
    }
    return true;
}

double percentile(std::vector<double> sorted, double percent) {
    if (sorted.empty()) {
        return 0.0;
    }
    std::ranges::sort(sorted);
    const double raw = static_cast<double>(sorted.size() - 1) * percent;
    const auto index = static_cast<std::size_t>(std::clamp(std::ceil(raw), 0.0, static_cast<double>(sorted.size() - 1)));
    return sorted[index];
}

BenchResult run_case(const BenchCase& bench, const BenchOptions& options, kin::ProfileSession* profile) {
    BenchResult result{
        .suite = std::string{bench.suite},
        .name = std::string{bench.name},
    };

    if (bench.suite == "input") {
        if (options.fail_on_allocation) {
            throw std::invalid_argument("--fail-on-allocation is not supported for paced input-latency cases");
        }
        result.input_latency = benchmark_input_latency(
            bench.sim_hz ? bench.sim_hz : options.input_sim_hz,
            bench.render_hz ? bench.render_hz : options.input_render_hz,
            options.iterations, options.warmup);
        std::vector<double> latencies;
        for (const auto& sample : result.input_latency->samples) {
            if (sample.ms[1] < 0.0) continue;
            latencies.push_back(sample.ms[1]);
            result.samples.push_back({.ms = sample.ms[1], .total = 1});
            result.mean_ms += sample.ms[1];
        }
        result.total = latencies.size();
        result.min_ms = percentile(latencies, 0.0);
        result.median_ms = percentile(latencies, 0.5);
        result.p95_ms = percentile(latencies, 0.95);
        result.p99_ms = percentile(latencies, 0.99);
        if (!latencies.empty()) result.mean_ms /= latencies.size();
        return result;
    }

    kin::AllocationScope allocations;
    kin::TimingCollector timings;
    current_bench_timings = &timings;
    for (int i = 0; i < options.warmup; ++i) {
        result.total += bench.fn(i);
    }
    allocations.reset_after_warmup();
    timings.reset_stats();

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(options.iterations));
    result.samples.reserve(static_cast<std::size_t>(options.iterations));
    for (int i = 0; i < options.iterations; ++i) {
        if (profile) {
            profile->set_frame(i + 1);
        }
        const kin::AllocationSnapshot alloc_start = kin::allocation_snapshot();
        const auto begin = Clock::now();
        if (options.fail_on_allocation) {
            allocations.set_fail_on_allocation(true);
        }
        const kin::u64 total = bench.fn(i);
        if (options.fail_on_allocation) {
            allocations.set_fail_on_allocation(false);
        }
        const auto end = Clock::now();
        const kin::AllocationSnapshot alloc_end = kin::allocation_snapshot();
        const kin::AllocationSnapshot alloc_delta = kin::allocation_delta(alloc_start, alloc_end);
        const double ms = std::chrono::duration<double, std::milli>(end - begin).count();
        const kin::u64 ns = static_cast<kin::u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
        if (profile) {
            profile->record(bench.name, "benchmark", ns);
        }
        result.samples.push_back({
            .ms = ms,
            .total = total,
            .allocations = alloc_delta.count,
            .allocated_bytes = alloc_delta.bytes,
        });
        result.total += total;
        result.allocations += alloc_delta.count;
        result.allocated_bytes += alloc_delta.bytes;
        samples.push_back(ms);
    }
    current_bench_timings = nullptr;
    result.timing_scopes = timings.summaries();

    result.min_ms = percentile(samples, 0.0);
    result.median_ms = percentile(samples, 0.50);
    result.p95_ms = percentile(samples, 0.95);
    result.p99_ms = percentile(samples, 0.99);
    for (double sample : samples) {
        result.mean_ms += sample;
    }
    result.mean_ms = samples.empty() ? 0.0 : result.mean_ms / static_cast<double>(samples.size());
    return result;
}

void list_cases(std::ostream& out) {
    out << "benchmarks\n";
    for (const BenchCase& bench : bench_cases()) {
        out << "- " << bench.suite << "." << bench.name << '\n';
    }
}

void write_results_text(std::ostream& out, const std::vector<BenchResult>& results) {
    out << "benchmark results\n";
    out << std::left << std::setw(38) << "case"
        << std::right << std::setw(10) << "median"
        << std::setw(10) << "p95"
        << std::setw(10) << "p99"
        << std::setw(10) << "mean"
        << std::setw(12) << "allocs"
        << std::setw(14) << "bytes"
        << "  total\n";
    for (const BenchResult& result : results) {
        const std::string name = result.suite + "." + result.name;
        out << std::left << std::setw(38) << name.substr(0, 37)
            << std::right << std::setw(9) << std::fixed << std::setprecision(3) << result.median_ms << " "
            << std::setw(9) << result.p95_ms << " "
            << std::setw(9) << result.p99_ms << " "
            << std::setw(9) << result.mean_ms << " "
            << std::setw(11) << (result.input_latency ? "n/a" : std::to_string(result.allocations)) << " "
            << std::setw(13) << (result.input_latency ? "n/a" : std::to_string(result.allocated_bytes)) << "  "
            << result.total << '\n';
        if (result.input_latency) write_input_latency_text(out, *result.input_latency);
        for (const kin::TimingSummary& timing : result.timing_scopes) {
            if (timing.stats.calls == 0) {
                continue;
            }
            out << "  timing " << timing.name
                << " calls=" << timing.stats.calls
                << " total_ms=" << std::fixed << std::setprecision(3)
                << static_cast<double>(timing.stats.total_ns) / 1'000'000.0
                << " avg_ms=" << static_cast<double>(timing.stats.average_ns()) / 1'000'000.0
                << " max_ms=" << static_cast<double>(timing.stats.max_ns) / 1'000'000.0
                << '\n';
        }
    }
}

void write_results_json(std::ostream& out,
                        const BenchOptions& options,
                        const std::vector<BenchResult>& results,
                        const kin::ProfileSession* profile) {
    kin::JsonWriter json(out);
    json.begin_object();
    json.field("schema", "kin.benchmark/1");
    json.field("iterations", options.iterations);
    json.field("warmup", options.warmup);
    json.key("cases").begin_array();
    for (const BenchResult& result : results) {
        json.begin_object();
        json.field("suite", result.suite);
        json.field("name", result.name);
        json.field("samples", static_cast<kin::u64>(result.samples.size()));
        json.field("total", result.total);
        json.field("min_ms", result.min_ms);
        json.field("mean_ms", result.mean_ms);
        json.field("median_ms", result.median_ms);
        json.field("p95_ms", result.p95_ms);
        json.field("p99_ms", result.p99_ms);
        json.field("allocations", result.allocations);
        json.field("allocated_bytes", result.allocated_bytes);
        if (result.input_latency) {
            json.field("allocation_tracking", false);
            json.field("primary_metric", "event_to_update");
            json.key("input_latency");
            write_input_latency_json(json, *result.input_latency);
        }
        json.key("sample_details").begin_array();
        for (const BenchSample& sample : result.samples) {
            json.begin_object();
            json.field("ms", sample.ms);
            json.field("total", sample.total);
            json.field("allocations", sample.allocations);
            json.field("allocated_bytes", sample.allocated_bytes);
            json.end_object();
        }
        json.end_array();
        json.key("timing_scopes").begin_array();
        for (const kin::TimingSummary& timing : result.timing_scopes) {
            if (timing.stats.calls == 0) {
                continue;
            }
            json.begin_object();
            json.field("name", timing.name);
            json.field("calls", timing.stats.calls);
            json.field("total_ns", timing.stats.total_ns);
            json.field("average_ns", timing.stats.average_ns());
            json.field("max_ns", timing.stats.max_ns);
            json.end_object();
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();
    if (profile) {
        json.key("profile_summary").begin_array();
        for (const kin::ProfileSummary& row : profile->summarize()) {
            json.begin_object();
            json.field("name", row.name);
            json.field("category", row.category);
            json.field("file", row.file);
            json.field("line", row.line);
            json.field("calls", row.calls);
            json.field("total_ns", row.total_ns);
            json.field("median_ms", row.median_ms);
            json.field("p95_ms", row.p95_ms);
            json.field("p99_ms", row.p99_ms);
            json.end_object();
        }
        json.end_array();
    }
    json.end_object();
    out << '\n';
}

} // namespace

int main(int argc, char** argv) {
    BenchOptions options;
    try {
        options = parse_options(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (options.list) {
        list_cases(std::cout);
        return 0;
    }

    kin::ProfileSession profile;
    kin::ProfileSession* profile_ptr = nullptr;
    if (options.profile_lines) {
        profile.start("kin_bench");
        kin::set_current_profile_session(&profile);
        profile_ptr = &profile;
    }

    std::vector<BenchResult> results;
    for (const BenchCase& bench : bench_cases()) {
        if (!selected(options, bench)) {
            continue;
        }
        try {
            results.push_back(run_case(bench, options, profile_ptr));
        } catch (const std::bad_alloc&) {
            std::cerr << "benchmark allocation guard tripped: " << bench.suite << "." << bench.name << '\n';
            return 2;
        } catch (const std::exception& error) {
            std::cerr << "benchmark failed: " << bench.suite << "." << bench.name << ": " << error.what() << '\n';
            return 1;
        }
    }

    if (profile_ptr) {
        profile.stop();
        kin::set_current_profile_session(nullptr);
    }

    if (results.empty()) {
        std::cerr << "no benchmarks selected\n";
        return 1;
    }

    write_results_text(std::cout, results);
    if (profile_ptr) {
        kin::write_profile_text(std::cout, profile, 24, "benchmark profile");
    }

    if (!options.json_path.empty()) {
        const std::filesystem::path path{options.json_path};
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream file(path);
        if (!file) {
            std::cerr << "failed to open json path: " << options.json_path << '\n';
            return 1;
        }
        write_results_json(file, options, results, profile_ptr);
    }

    return std::ranges::any_of(results, [](const BenchResult& result) {
        return result.input_latency && !result.input_latency->ok();
    }) ? 3 : 0;
}
