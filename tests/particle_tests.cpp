#include <kin/assets/asset_manager.hpp>
#include <kin/anim/events.hpp>
#include <kin/ecs/particles.hpp>
#include <kin/ecs/render.hpp>
#include <kin/particles/animation_particles.hpp>
#include <kin/particles/particle_catalog.hpp>
#include <kin/particles/particle_field.hpp>
#include <kin/particles/particle_system.hpp>
#include <kin/platform/log.hpp>

#include <cassert>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include <filesystem>
#include <fstream>
#include <memory>
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

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    test_deterministic_bursts_and_update();
    test_catalog_asset_and_defaults();
    test_particle_catalog_seed_roundtrip();
    test_fields_wrap_and_render_pixel_policies();
    test_rendering_rects_sprites_sort_and_output_policy();
    test_animation_event_and_ecs_bridge();
    test_ecs_render_world_interleaves_particles_with_sprites();
    test_ecs_render_world_converts_output_policy_particles();
    return 0;
}
