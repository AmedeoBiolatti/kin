#include <kin/assets/asset_manager.hpp>
#include <kin/anim/events.hpp>
#include <kin/ecs/particles.hpp>
#include <kin/ecs/render.hpp>
#include <kin/particles/animation_particles.hpp>
#include <kin/particles/particle_catalog.hpp>
#include <kin/particles/particle_field.hpp>
#include <kin/particles/particle_system.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include <filesystem>
#include <fstream>
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

    void clear(kin::Color) override {}
    void present() override {}
    void set_logical_size(kin::Vec2i) override {}
    void set_integer_logical_size(kin::Vec2i) override {}
    kin::Vec2i output_size() const override { return {320, 180}; }
    kin::Vec2f window_to_logical(kin::Vec2f value) const override { return {value.x * 0.5f, value.y * 0.5f}; }
    kin::Vec2f logical_to_window(kin::Vec2f value) const override { return {value.x * 2.0f, value.y * 2.0f}; }
    kin::Texture create_texture_from_rgba(const kin::u8*, kin::Vec2i size) override {
        return kin::Texture{std::make_shared<FakeTextureBackend>(size)};
    }
    void draw_texture(const kin::Texture&, kin::Rectf dest) override {
        draws.push_back(dest);
        commands.push_back("texture");
    }
    void draw_texture(const kin::Texture&, kin::Rectf source, kin::Rectf dest) override {
        sources.push_back(source);
        draws.push_back(dest);
        commands.push_back("texture");
    }
    void draw_texture(const kin::Texture&, kin::Rectf source, kin::Rectf dest, kin::Color tint) override {
        sources.push_back(source);
        draws.push_back(dest);
        tints.push_back(tint);
        commands.push_back("texture");
    }
    void fill_rect(kin::Rectf rect, kin::Color color) override {
        rects.push_back(rect);
        colors.push_back(color);
        commands.push_back("rect");
    }
    void draw_rect(kin::Rectf, kin::Color) override {}
    void draw_line(kin::Vec2f, kin::Vec2f, kin::Color) override {}
    void set_viewport(kin::Rectf) override {}
    void reset_viewport() override {}
    void push_viewport(kin::Rectf) override {}
    void pop_viewport() override {}

    std::vector<kin::Rectf> draws;
    std::vector<kin::Rectf> sources;
    std::vector<kin::Rectf> rects;
    std::vector<kin::Color> colors;
    std::vector<kin::Color> tints;
    std::vector<std::string> commands;
};

kin::ParticleCatalog test_catalog() {
    kin::ParticleCatalog catalog;
    catalog.set_effect("spark", {.burst = {
        .count = 3,
        .speed = {10.0f, 10.0f},
        .lifetime = {1.0f, 1.0f},
        .start_size = 2.0f,
        .end_size = 0.0f,
        .start_color = {200, 100, 50, 255},
        .end_color = {200, 100, 50, 0},
        .render = {.layer = 2, .order = 1},
    }});
    return catalog;
}

void test_deterministic_bursts_and_update() {
    kin::ParticleSystem a{kin::make_key(123)};
    kin::ParticleSystem b{kin::make_key(123)};
    const kin::ParticleBurst burst{
        .position = {10.0f, 20.0f},
        .count = 4,
        .speed = {8.0f, 8.0f},
        .lifetime = {0.5f, 0.5f},
    };

    a.burst(burst);
    b.burst(burst);
    assert(a.active_count() == 4);
    assert(b.active_count() == 4);
    for (std::size_t i = 0; i < a.particles().size(); ++i) {
        assert(a.particles()[i].position == b.particles()[i].position);
        assert(a.particles()[i].velocity == b.particles()[i].velocity);
        assert(a.particles()[i].lifetime == b.particles()[i].lifetime);
    }

    kin::ParticleSystem c{kin::make_key(124)};
    c.burst(burst);
    assert(c.particles()[0].velocity != a.particles()[0].velocity);

    a.update(0.25f);
    assert(a.active_count() == 4);
    a.update(0.25f);
    assert(a.empty());
}

void test_catalog_asset_and_defaults() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-particle-tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto path = dir / "effects.kinparticles";
    std::ofstream{path}
        << "# effect name count speed_min speed_max life_min life_max start_size end_size "
           "start_rgba end_rgba accel layer order sprite\n"
        << "effect player.step 3 8 25 0.15 0.3 2 0 180 150 100 200 180 150 100 0 0 40 300 2 -\n"
        << "field background.stars 5 777 0 0 320 180 0 0 8 40 1 2 55 195 255 255 255 255 1 1 -1000 0 -\n";

    kin::AssetManager assets{dir};
    assets.discover();
    const kin::AssetMetadata* meta = assets.metadata("effects.kinparticles");
    assert(meta != nullptr);
    assert(meta->type == kin::AssetType::Particles);
    assert(kin::asset_type_name(kin::AssetType::Particles) == "Particles");

    const auto catalog = assets.load<kin::ParticleCatalog>("effects.kinparticles");
    assert(catalog);
    const kin::ParticleEffect* effect = catalog->effect("player.step");
    assert(effect != nullptr);
    assert(effect->burst.count == 3);
    assert(effect->burst.acceleration.y == 40.0f);
    const kin::ParticleField* field = catalog->field("background.stars");
    assert(field != nullptr);
    assert(field->count == 5);
    assert(field->wrap);

    const kin::ParticleCatalog defaults = kin::default_particle_catalog();
    assert(defaults.has_effect("hit_warm"));
    assert(defaults.has_effect("step_dust"));
    assert(defaults.has_field("simple_stars"));
}

void test_particle_catalog_seed_roundtrip() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-particle-seed-tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto first_path = dir / "first.kinparticles";
    const auto second_path = dir / "second.kinparticles";

    kin::ParticleCatalog catalog;
    catalog.set_field("stars", {
        .count = 2,
        .seed = 777,
        .area = {0.0f, 0.0f, 10.0f, 10.0f},
        .velocity_y = {1.0f, 1.0f},
        .size = {1.0f, 1.0f},
    });
    assert(kin::save_particle_catalog(catalog, first_path));
    const kin::ParticleCatalog loaded = kin::load_particle_catalog(first_path);
    const kin::ParticleField* field = loaded.field("stars");
    assert(field != nullptr);
    assert(field->seed == 777);
    assert(kin::save_particle_catalog(loaded, second_path));
    const kin::ParticleCatalog reloaded = kin::load_particle_catalog(second_path);
    const kin::ParticleField* reloaded_field = reloaded.field("stars");
    assert(reloaded_field != nullptr);
    assert(reloaded_field->seed == 777);

    const kin::ParticleFieldState first = kin::init_particle_field(*field);
    const kin::ParticleFieldState second = kin::init_particle_field(*reloaded_field);
    assert(first.particles.size() == second.particles.size());
    // Seed 777 consumes brightness, then x/y. Preserve this stream across
    // compiler versions and LTO, not just equality between two wrong streams.
    assert((first.particles[0].position == kin::Vec2f{0x1.31d9ccp+2f, 0x1.27c698p+3f}));

    assert(first.particles[0].position == second.particles[0].position);
    assert(first.particles[1].velocity == second.particles[1].velocity);
}

void test_fields_wrap_and_render_pixel_policies() {
    kin::ParticleField field{
        .count = 2,
        .seed = 42,
        .area = {0.0f, 0.0f, 10.0f, 20.0f},
        .velocity_y = {5.0f, 5.0f},
        .size = {1.0f, 1.0f},
        .brightness = {100.0f, 100.0f},
        .wrap = true,
    };
    kin::ParticleFieldState state = kin::init_particle_field(field);
    assert(state.particles.size() == 2);
    assert(state.particles[0].position.x >= 0.0f);
    assert(state.particles[0].position.x <= 10.0f);
    assert(state.particles[0].velocity.y == 5.0f);
    assert(state.particles[0].color.r == 100);

    state.particles[0].position.y = 21.0f;
    kin::update_particle_field(state, 0.0f);
    assert(state.particles[0].position.y == 21.0f);
    kin::update_particle_field(state, 0.01f);
    assert(state.particles[0].position.y >= 0.0f);
    assert(state.particles[0].position.y <= 20.0f);

    kin::Particle particle{.position = {1.24f, 2.76f}, .start_size = 1.24f, .end_size = 1.24f};
    assert((kin::particle_rect(particle, kin::ParticlePixelPolicy::LogicalPixel, 0.5f) == kin::Rectf{1.0f, 2.0f, 1.0f, 1.0f}));
    assert((kin::particle_rect(particle, kin::ParticlePixelPolicy::Subpixel, 0.5f) == kin::Rectf{0.5f, 2.0f, 1.0f, 1.0f}));
    const kin::Rectf third = kin::particle_rect(particle, kin::ParticlePixelPolicy::Subpixel, 1.0f / 3.0f);
    assert(third.w > 1.2f && third.w < 1.4f);

    kin::ParticleFieldState degenerate = kin::init_particle_field({
        .count = 1,
        .seed = 99,
        .area = {0.0f, 0.0f, 0.0f, 0.0f},
        .velocity_x = {1.0f, 1.0f},
        .velocity_y = {1.0f, 1.0f},
        .size = {1.0f, 1.0f},
        .wrap = true,
    });
    kin::update_particle_field(degenerate, 0.25f);
    assert(degenerate.particles.size() == 1);
}

void test_rendering_rects_sprites_sort_and_output_policy() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};

    kin::ParticleSystem particles{kin::make_key(1)};
    particles.emit({
        .position = {20.0f, 20.0f},
        .lifetime = 1.0f,
        .start_size = 2.0f,
        .end_size = 2.0f,
        .start_color = kin::Color::rgb(20, 30, 40),
        .end_color = kin::Color::rgb(20, 30, 40),
        .render = {.layer = 2, .order = 0},
    });
    particles.emit({
        .position = {10.0f, 10.0f},
        .lifetime = 1.0f,
        .start_size = 4.0f,
        .end_size = 4.0f,
        .start_color = kin::Color::rgb(50, 60, 70),
        .end_color = kin::Color::rgb(50, 60, 70),
        .render = {.layer = 1, .order = 0},
    });
    particles.render(renderer);
    assert(raw->rects.size() == 2);
    assert((raw->rects[0] == kin::Rectf{8.0f, 8.0f, 4.0f, 4.0f}));
    assert((raw->rects[1] == kin::Rectf{19.0f, 19.0f, 2.0f, 2.0f}));

    raw->rects.clear();
    particles.set_pixel_policy(kin::ParticlePixelPolicy::Output);
    particles.render(renderer);
    assert((raw->rects[0] == kin::Rectf{4.0f, 4.0f, 2.0f, 2.0f}));

    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{16, 16})};
    kin::SpriteCatalog sprites;
    sprites.set_texture("main", texture);
    sprites.add({.id = "spark.sprite", .texture_id = "main", .source = {1.0f, 2.0f, 3.0f, 4.0f}});
    kin::ParticleSystem sprite_particles;
    sprite_particles.emit({
        .position = {5.0f, 5.0f},
        .lifetime = 1.0f,
        .start_size = 3.0f,
        .end_size = 3.0f,
        .start_color = kin::Color{120, 80, 40, 96},
        .end_color = kin::Color{120, 80, 40, 96},
        .render = {.sprite_id = "spark.sprite"},
    });
    sprite_particles.render(renderer, &sprites);
    assert(raw->draws.size() == 1);
    assert((raw->sources[0] == kin::Rectf{1.0f, 2.0f, 3.0f, 4.0f}));
    assert(raw->tints.size() == 1);
    assert((raw->tints[0] == kin::Color{120, 80, 40, 96}));
}

void test_animation_event_and_ecs_bridge() {
    std::vector<kin::LogEvent> events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .sdl_sink = false,
        .memory_events = &events,
    });

    kin::ParticleCatalog catalog = test_catalog();
    kin::ParticleSystem particles{kin::make_key(99)};
    particles.set_catalog(std::move(catalog));

    assert(kin::dispatch_particle_animation_event({
        .channel = "particle",
        .value = "spark",
        .offset = {2.0f, -3.0f},
    }, {10.0f, 20.0f}, particles));
    assert(particles.active_count() == 3);
    assert((particles.particles()[0].position == kin::Vec2f{12.0f, 17.0f}));
    assert(!kin::dispatch_particle_animation_event({
        .channel = "particle",
        .value = "missing",
    }, {}, particles));
    assert(particles.active_count() == 3);

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::AnimationEventQueue>("AnimationEventQueue");
    world.component<kin::ParticleSystemComponent>("ParticleSystemComponent");
    world.component<kin::ParticleFieldComponent>("ParticleFieldComponent");

    kin::ParticleCatalog ecs_catalog = test_catalog();
    kin::ParticleSystem ecs_particles{kin::make_key(5)};
    ecs_particles.set_catalog(std::move(ecs_catalog));
    world.entity("particles").set(kin::ParticleSystemComponent{.system = std::move(ecs_particles)});

    kin::ParticleFieldComponent field_component{
        .name = "field",
        .state = kin::init_particle_field({
            .count = 1,
            .seed = 7,
            .area = {0.0f, 0.0f, 5.0f, 5.0f},
            .velocity_y = {1.0f, 1.0f},
            .size = {1.0f, 1.0f},
        }),
    };
    world.entity("field").set(std::move(field_component));

    world.entity("actor")
        .set(kin::Transform2D{{4.0f, 8.0f}})
        .set(kin::AnimationEventQueue{.pending = {{
            .channel = "particle",
            .value = "spark",
            .offset = {1.0f, 1.0f},
        }}});

    kin::dispatch_particle_animation_events(world);
    const auto* system = world.raw().lookup("particles").get<kin::ParticleSystemComponent>();
    assert(system != nullptr);
    assert(system->system.active_count() == 3);

    kin::update_particles(world, 0.25f);
    const auto* updated_field = world.raw().lookup("field").get<kin::ParticleFieldComponent>();
    assert(updated_field != nullptr);
    assert(updated_field->state.particles[0].position.y > 0.0f);

    bool saw_catalog = false;
    bool saw_emit = false;
    bool saw_missing = false;
    for (const kin::LogEvent& event : events) {
        saw_catalog = saw_catalog || (event.category == "particle" && event.message == "particle catalog assigned");
        saw_emit = saw_emit || (event.category == "particle" && event.message == "particle effect emitted");
        saw_missing = saw_missing || (event.category == "particle" && event.message == "particle effect missing");
    }
    assert(saw_catalog);
    assert(saw_emit);
    assert(saw_missing);
    kin::set_logger_config({.sdl_sink = false});
}

void test_ecs_render_world_interleaves_particles_with_sprites() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{16, 16})};
    kin::SpriteCatalog sprites;
    sprites.set_texture("main", texture);
    sprites.add({.id = "actor", .texture_id = "main", .source = {0.0f, 0.0f, 4.0f, 4.0f}});

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::SpriteRenderer>("SpriteRenderer");
    world.component<kin::ParticleSystemComponent>("ParticleSystemComponent");

    kin::ParticleSystem behind;
    behind.emit({
        .position = {10.0f, 10.0f},
        .lifetime = 1.0f,
        .start_size = 2.0f,
        .end_size = 2.0f,
        .start_color = kin::Color::rgb(10, 20, 30),
        .end_color = kin::Color::rgb(10, 20, 30),
        .render = {.layer = 0},
    });
    world.entity("behind").set(kin::ParticleSystemComponent{.system = std::move(behind)});

    world.entity("actor")
        .set(kin::Transform2D{{20.0f, 20.0f}})
        .set(kin::SpriteRenderer{
            .sprite = sprites.ref("actor"),
            .layer = 1,
        });

    kin::ParticleSystem front;
    front.emit({
        .position = {30.0f, 30.0f},
        .lifetime = 1.0f,
        .start_size = 2.0f,
        .end_size = 2.0f,
        .start_color = kin::Color::rgb(40, 50, 60),
        .end_color = kin::Color::rgb(40, 50, 60),
        .render = {.layer = 2},
    });
    world.entity("front").set(kin::ParticleSystemComponent{.system = std::move(front)});

    kin::render_world(world, renderer);
    assert(raw->rects.size() == 2);
    assert(raw->draws.size() == 1);
    assert(raw->commands.size() == 3);
    assert(raw->commands[0] == "rect");
    assert(raw->commands[1] == "texture");
    assert(raw->commands[2] == "rect");
    assert(raw->colors[0] == kin::Color::rgb(10, 20, 30));
    const kin::Rectf actor_draw{20.0f, 20.0f, 4.0f, 4.0f};
    assert(raw->draws[0] == actor_draw);
    assert(raw->colors[1] == kin::Color::rgb(40, 50, 60));
}

void test_ecs_render_world_converts_output_policy_particles() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};

    kin::EcsWorld world;
    world.component<kin::ParticleSystemComponent>("ParticleSystemComponent");

    kin::ParticleSystem particles;
    particles.set_pixel_policy(kin::ParticlePixelPolicy::Output);
    particles.emit({
        .position = {10.0f, 10.0f},
        .lifetime = 1.0f,
        .start_size = 4.0f,
        .end_size = 4.0f,
        .start_color = kin::Color::rgb(20, 30, 40),
        .end_color = kin::Color::rgb(20, 30, 40),
    });
    world.entity("particles").set(kin::ParticleSystemComponent{.system = std::move(particles)});

    kin::render_world(world, renderer);
    assert(raw->rects.size() == 1);
    assert((raw->rects[0] == kin::Rectf{4.0f, 4.0f, 2.0f, 2.0f}));
}

} // namespace

// Direction and spread, where they start, drag, spin and a mid-life point;
// written as name=value after an effect's first settings and read back.
void test_richer_bursts_and_their_text() {
    constexpr kin::f32 pi = 3.14159265f;
    kin::ParticleSystem system{kin::make_key(7)};
    kin::ParticleBurst burst{
        .position = {100.0f, 100.0f},
        .count = 64,
        .speed = {10.0f, 10.0f},
        .lifetime = {1.0f, 1.0f},
        .start_size = 2.0f,
        .end_size = 0.0f,
        .start_color = {255, 0, 0, 255},
        .end_color = {0, 0, 255, 0},
        .direction = 90.0f, // down
        .spread = 30.0f,
        .shape = kin::ParticleShape::Box,
        .shape_size = {20.0f, 4.0f},
        .drag = 1.0f,
        .rotation = {10.0f, 10.0f},
        .spin = {90.0f, 90.0f},
        .use_mid = true,
        .mid_at = 0.25f,
        .mid_size = 8.0f,
        .mid_color = {0, 255, 0, 255},
    };
    system.burst(burst);
    for (const kin::Particle& p : system.particles()) {
        const kin::f32 angle = std::atan2(p.velocity.y, p.velocity.x) * 180.0f / pi;
        assert(angle >= 75.0f - 1e-3f && angle <= 105.0f + 1e-3f);
        assert(std::abs(p.position.x - 100.0f) <= 10.0f && std::abs(p.position.y - 100.0f) <= 2.0f);
        assert(p.rotation == 10.0f);
    }
    system.update(0.25f);
    const kin::Particle& p = system.particles()[0];
    assert(std::abs(p.rotation - 32.5f) < 1e-3f);
    // Slowed by drag: e^-0.25 of its speed kept.
    assert(std::abs(std::hypot(p.velocity.x, p.velocity.y) - 10.0f * std::exp(-0.25f)) < 1e-3f);
    // A quarter through its life: at the mid point.
    assert(std::abs(kin::particle_size_at(p) - 8.0f) < 1e-3f);
    assert((kin::particle_color_at(p) == kin::Color{0, 255, 0, 255}));
    system.update(0.375f); // halfway from mid (0.25) to the end
    assert(std::abs(kin::particle_size_at(system.particles()[0]) - 4.0f) < 1e-2f);

    kin::ParticleCatalog catalog;
    catalog.set_effect("rain", {.burst = burst});
    catalog.set_effect("plain", {.burst = {.count = 3}});
    const std::string text = kin::serialize_particle_catalog(catalog);
    assert(text.find("direction=90 spread=30 shape=box shape_size=20,4 drag=1 rotation=10 spin=90 "
                     "mid_at=0.25 mid_size=8 mid_color=0,255,0,255\n") != std::string::npos);
    assert(text.find("effect plain 3 20 80 0.25 0.6 2 0 255 255 255 255 255 255 255 0 0 0 300 0 -\n") != std::string::npos);
    kin::ParticleCatalog read;
    std::string error;
    assert(kin::parse_particle_catalog(text, read, error) && error.empty());
    const kin::ParticleBurst& back = read.effect("rain")->burst;
    assert(back.direction == 90.0f && back.spread == 30.0f && back.shape == kin::ParticleShape::Box);
    assert(back.shape_size == (kin::Vec2f{20.0f, 4.0f}) && back.use_mid && back.mid_size == 8.0f);
    assert(!read.effect("plain")->burst.use_mid && read.effect("plain")->burst.spread == 360.0f);
    assert(kin::serialize_particle_catalog(read) == text);

    // Not a setting kin knows: the line says so.
    assert(!kin::parse_particle_catalog("effect x 1 0 0 1 1 1 1 255 255 255 255 0 0 0 0 0 0 0 0 - wobble=3\n", read, error));
    assert(error.find("particles:1") != std::string::npos && error.find("wobble") != std::string::npos);
    assert(read.has_effect("rain")); // left as it was
    assert(read.remove_effect("rain") && !read.has_effect("rain") && !read.remove_effect("rain"));
}

bool near(kin::f32 a, kin::f32 b, kin::f32 within = 1e-3f) {
    return std::abs(a - b) <= within;
}

void test_curves_over_life() {
    const std::vector<kin::ParticleKey> curve{{0.0f, 2.0f}, {0.5f, 6.0f}, {1.0f, 0.0f}};
    assert(near(kin::particle_curve_at(curve, 0.25f, 9.0f), 4.0f));
    assert(near(kin::particle_curve_at(curve, 0.75f, 9.0f), 3.0f));
    assert(near(kin::particle_curve_at({}, 0.5f, 9.0f), 9.0f));
    const std::vector<kin::ParticleKey> late{{0.5f, 1.0f}};
    assert(near(kin::particle_curve_at(late, 0.0f, 9.0f), 1.0f) && near(kin::particle_curve_at(late, 1.0f, 9.0f), 1.0f));
    const std::vector<kin::ParticleColorKey> gradient{{0.0f, {255, 0, 0, 255}}, {1.0f, {0, 0, 255, 0}}};
    assert((kin::particle_gradient_at(gradient, 0.5f, {}) == kin::Color{128, 0, 128, 128}));

    kin::ParticleSystem system{kin::make_key(3)};
    system.burst({
        .count = 1,
        .speed = {100.0f, 100.0f},
        .lifetime = {1.0f, 1.0f},
        .direction = 0.0f,
        .spread = 0.0f,
        .size_curve = curve,
        .color_curve = gradient,
        .speed_curve = {{0.0f, 1.0f}, {0.5f, 0.0f}}, // stops halfway
    });
    system.update(0.25f);
    const kin::Particle& p = system.particles()[0];
    assert(near(kin::particle_size_at(p), 4.0f));
    assert((kin::particle_color_at(p) == kin::Color{191, 0, 64, 191}));
    assert(near(p.position.x, 12.5f)); // at half its speed, as at 0.25 of its life
    system.update(0.5f);
    assert(near(system.particles()[0].position.x, 12.5f)); // at 0 since halfway
    assert(near(kin::particle_size_at(system.particles()[0]), 3.0f));
}

kin::ParticleCatalog stream_catalog() {
    kin::ParticleCatalog catalog;
    const kin::ParticleBurst still{.count = 5, .speed = {0.0f, 0.0f}, .lifetime = {10.0f, 10.0f}};
    catalog.set_effect("smoke", {.burst = still, .emission = {.rate = 10.0f}});
    catalog.set_effect("trail", {.burst = still, .emission = {.per_distance = 0.1f}});
    catalog.set_effect("short", {.burst = still, .emission = {.rate = 10.0f, .duration = 0.5f}});
    catalog.set_effect("warm", {.burst = {.speed = {0.0f, 0.0f}, .lifetime = {1.0f, 1.0f}},
                                .emission = {.rate = 10.0f, .prewarm = 2.0f}});
    catalog.set_effect("held", {.burst = still, .emission = {.rate = 10.0f, .local = true}});
    catalog.set_effect("thrown", {.burst = still, .emission = {.rate = 100.0f, .inherit = 1.0f}});
    catalog.set_effect("jet", {.burst = {.speed = {10.0f, 10.0f}, .lifetime = {10.0f, 10.0f}, .spread = 0.0f},
                               .emission = {.rate = 10.0f}});
    return catalog;
}

void test_emitters_stream() {
    kin::ParticleSystem system{kin::make_key(4)};
    system.set_catalog(stream_catalog());

    // At its rate, its count not burst.
    const kin::ParticleEmitter smoke = system.start("smoke", {});
    assert(smoke && system.emitting(smoke) && system.emitter_count() == 1);
    assert(system.active_count() == 0);
    system.update(1.0f);
    assert(system.active_count() == 10);
    system.update(0.25f);
    system.update(0.25f);
    assert(system.active_count() == 15);
    system.stop(smoke);
    system.update(1.0f);
    assert(system.active_count() == 15 && !system.emitting(smoke) && system.emitter_count() == 0);
    system.clear();

    // Along the way it moved, evenly.
    const kin::ParticleEmitter trail = system.start("trail", {});
    system.move(trail, {100.0f, 0.0f});
    system.update(0.1f);
    assert(system.active_count() == 10);
    for (std::size_t i = 0; i < 10; ++i) {
        assert(near(system.particles()[i].position.x, 10.0f * static_cast<kin::f32>(i + 1)));
    }
    system.jump(trail, {500.0f, 0.0f}); // and none on a jump
    system.update(0.1f);
    assert(system.active_count() == 10);
    system.clear();

    // For its duration.
    system.start("short", {});
    system.update(0.25f);
    system.update(0.25f);
    system.update(0.25f);
    assert(system.active_count() == 5 && system.emitter_count() == 0);
    system.clear();

    // Prewarmed: as it would be, two seconds in (a second's particles alive).
    system.start("warm", {});
    assert(system.active_count() >= 9 && system.active_count() <= 11);
    kin::f32 oldest = 0.0f;
    for (const kin::Particle& p : system.particles()) {
        oldest = std::max(oldest, p.age);
    }
    assert(oldest > 0.8f);
    system.clear();

    // Local: its particles move with it.
    const kin::ParticleEmitter held = system.start("held", {});
    system.update(1.0f);
    system.move(held, {50.0f, 0.0f});
    system.update(0.0f);
    for (const kin::Particle& p : system.particles()) {
        assert(near(p.position.x, 50.0f));
    }
    system.stop(held);
    system.update(0.0f);
    assert(system.emitter_count() == 1); // kept while its particles follow it
    system.clear();

    // With the emitter's velocity.
    const kin::ParticleEmitter thrown = system.start("thrown", {});
    system.move(thrown, {10.0f, 0.0f});
    system.update(0.1f);
    assert(system.active_count() == 10);
    assert(near(system.particles().back().velocity.x, 100.0f));
    system.clear();

    // Turned with it.
    system.start("jet", {}, 90.0f);
    system.update(0.1f);
    assert(system.active_count() == 1);
    assert(near(system.particles()[0].velocity.x, 0.0f) && near(system.particles()[0].velocity.y, 10.0f));
    system.clear();

    // An effect that does not stream: burst once.
    kin::ParticleCatalog plain = test_catalog();
    system.set_catalog(std::move(plain));
    assert(!system.start("spark", {}) && system.active_count() == 3 && system.emitter_count() == 0);
}

void test_sub_effects() {
    kin::ParticleCatalog catalog;
    catalog.set_effect("rocket", {.burst = {
        .count = 1, .speed = {0.0f, 0.0f}, .lifetime = {0.5f, 0.5f},
        .start_color = {255, 0, 0, 255}, .end_color = {255, 0, 0, 255},
        .on_death = "pop", .trail = "puff", .trail_rate = 10.0f,
    }});
    catalog.set_effect("pop", {.burst = {.count = 5, .lifetime = {1.0f, 1.0f},
                                         .start_color = {0, 255, 0, 255}, .end_color = {0, 255, 0, 255}}});
    catalog.set_effect("puff", {.burst = {.count = 1, .lifetime = {2.0f, 2.0f},
                                          .start_color = {0, 0, 255, 255}, .end_color = {0, 0, 255, 255}}});
    catalog.set_effect("echo", {.burst = {.count = 1, .lifetime = {0.1f, 0.1f}, .on_death = "echo"}});
    kin::ParticleSystem system{kin::make_key(8)};
    system.set_catalog(catalog);
    const auto count_of = [&](kin::Color color) {
        return std::ranges::count_if(system.particles(), [&](const kin::Particle& p) { return p.start_color == color; });
    };
    assert(system.emit("rocket", {}));
    for (int i = 0; i < 5; ++i) {
        system.update(0.1f);
    }
    assert(count_of({255, 0, 0, 255}) == 0);
    assert(count_of({0, 255, 0, 255}) == 5);
    assert(count_of({0, 0, 255, 255}) >= 4 && count_of({0, 0, 255, 255}) <= 5);
    system.clear();

    // Three deep at most: an effect bursting itself as it ends ends.
    system.emit("echo", {});
    int generations = 0;
    for (int i = 0; i < 20 && !system.empty(); ++i) {
        system.update(0.1f);
        generations += system.empty() ? 0 : 1;
    }
    assert(system.empty() && generations == 3);

    // Held to the limit.
    system.set_limit(3);
    system.emit("rocket", {});
    for (int i = 0; i < 5; ++i) {
        system.update(0.1f);
    }
    assert(system.active_count() <= 3);
}

void test_streams_in_text() {
    kin::ParticleCatalog catalog;
    catalog.set_effect("fire", {
        .burst = {
            .count = 2,
            .size_curve = {{0.0f, 2.0f}, {0.5f, 6.0f}, {1.0f, 0.0f}},
            .color_curve = {{0.0f, {255, 200, 100, 255}}, {1.0f, {80, 20, 10, 0}}},
            .speed_curve = {{0.0f, 1.0f}, {1.0f, 0.25f}},
            .on_death = "smoke",
            .trail = "ember",
            .trail_rate = 4.0f,
        },
        .emission = {.rate = 30.0f, .per_distance = 0.5f, .duration = 2.0f, .prewarm = 1.0f, .inherit = 0.5f, .local = true},
    });
    const std::string text = kin::serialize_particle_catalog(catalog);
    assert(text.find(" size_curve=0:2;0.5:6;1:0 color_curve=0:255,200,100,255;1:80,20,10,0 speed_curve=0:1;1:0.25"
                     " on_death=smoke trail=ember trail_rate=4 rate=30 per_distance=0.5 duration=2 prewarm=1"
                     " inherit=0.5 local=1\n") != std::string::npos);
    kin::ParticleCatalog read;
    std::string error;
    assert(kin::parse_particle_catalog(text, read, error));
    const kin::ParticleEffect& back = *read.effect("fire");
    assert(back.burst.size_curve == catalog.effect("fire")->burst.size_curve);
    assert(back.burst.color_curve == catalog.effect("fire")->burst.color_curve);
    assert(back.burst.speed_curve == catalog.effect("fire")->burst.speed_curve);
    assert(back.burst.on_death == "smoke" && back.burst.trail == "ember" && back.burst.trail_rate == 4.0f);
    assert(back.emission == catalog.effect("fire")->emission && back.streams());
    assert(kin::serialize_particle_catalog(read) == text);

    // Keys read in any order come in order.
    assert(kin::parse_particle_catalog("effect x 1 0 0 1 1 1 1 255 255 255 255 0 0 0 0 0 0 0 0 - size_curve=1:0;0:4\n", read, error));
    assert((read.effect("x")->burst.size_curve == std::vector<kin::ParticleKey>{{0.0f, 4.0f}, {1.0f, 0.0f}}));
    assert(!kin::parse_particle_catalog("effect x 1 0 0 1 1 1 1 255 255 255 255 0 0 0 0 0 0 0 0 - size_curve=0;1\n", read, error));
    assert(error.find("size_curve") != std::string::npos);
    assert(!kin::parse_particle_catalog("effect x 1 0 0 1 1 1 1 255 255 255 255 0 0 0 0 0 0 0 0 - local=maybe\n", read, error));
}

void test_ecs_streams_follow_the_entity() {
    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::ParticleSystemComponent>("ParticleSystemComponent");
    kin::ParticleSystem system{kin::make_key(6)};
    system.set_catalog(stream_catalog());
    flecs::entity e = world.raw().entity("chimney")
                          .set(kin::Transform2D{{40.0f, 0.0f}})
                          .set(kin::ParticleSystemComponent{.system = std::move(system), .effect = "smoke", .rate = 50.0f});
    kin::update_particles(world, 1.0f);
    const auto* particles = e.get<kin::ParticleSystemComponent>();
    assert(particles->system.active_count() == 10); // its own rate, not the component's
    assert(near(particles->system.particles()[0].position.x, 40.0f));
    e.set(kin::Transform2D{{80.0f, 0.0f}});
    kin::update_particles(world, 0.5f);
    assert(particles->system.active_count() == 15);
    assert(near(particles->system.particles().back().position.x, 80.0f));
    e.get_mut<kin::ParticleSystemComponent>()->emitting = false;
    kin::update_particles(world, 1.0f);
    assert(particles->system.active_count() == 15 && particles->system.emitter_count() == 0);
    e.get_mut<kin::ParticleSystemComponent>()->emitting = true;
    kin::update_particles(world, 0.1f);
    assert(particles->system.active_count() == 16);
}

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    test_deterministic_bursts_and_update();
    test_catalog_asset_and_defaults();
    test_particle_catalog_seed_roundtrip();
    test_richer_bursts_and_their_text();
    test_curves_over_life();
    test_emitters_stream();
    test_sub_effects();
    test_streams_in_text();
    test_ecs_streams_follow_the_entity();
    test_fields_wrap_and_render_pixel_policies();
    test_rendering_rects_sprites_sort_and_output_policy();
    test_animation_event_and_ecs_bridge();
    test_ecs_render_world_interleaves_particles_with_sprites();
    test_ecs_render_world_converts_output_policy_particles();
    return 0;
}
