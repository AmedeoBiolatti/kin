#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <kin/assets/content.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/scene_session3d.hpp>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
using namespace kin;
void check(const auto& ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Scratch {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("kin-particle-assets-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() {
        std::error_code e;
        std::filesystem::remove_all(path, e);
    }
};
void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream(path, std::ios::binary) << text;
    static int revision = 0;
    std::filesystem::last_write_time(path,
                                     std::filesystem::file_time_type::clock::now() + std::chrono::seconds(++revision));
}
std::string read(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
constexpr auto prefab = R"({"schema":"kin.prefab/1","id":"effect","root":"root","entities":[
 {"id":"root","components":{"Transform3D":{}}},
 {"id":"puff","parent":"root","components":{"Transform3D":{},"ParticleEmitter3D":{
  "seed":7,"rate":40,"max_particles":16,"lifetime":[0.2,0.3],"size":[0.4,0.5],"speed":[0.5,1],"blend":"additive","receives_shadow":true,"casts_shadow":true,"shadow_cutoff":0.3}}}]})";
constexpr auto scene = R"({"schema":"kin.scene/1","prefabs":[
 {"id":"left","asset":"effect.kinprefab","overrides":{"root":{"Transform3D":{"position":[-1,0,0]}}}},
 {"id":"right","asset":"effect.kinprefab","overrides":{"root":{"Transform3D":{"position":[1,0,0]}}}}
]})";
kin::EcsEntity emitter(const SceneInstance& instance, std::string_view name) {
    return instance.prefab_instances.at(std::string(name)).by_authored_id.at("puff");
}

void cpu() {
    Scratch scratch;
    write(scratch.path / "effect.kinprefab", prefab);
    write(scratch.path / "scene.kinscene", scene);
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    WorldRenderState3D rendering(world);
    SceneAssetRuntime source;
    check(source.load(scratch.path / "scene.kinscene", scratch.path, world.components()) && source.instantiate(world),
          "particle scene/prefab rejected");
    auto a = emitter(source.instance(), "left"), b = emitter(source.instance(), "right");
    check(!ParticleLook3D{}.receives_shadow && a.get<ParticleEmitter3D>()->system.desc.look.receives_shadow &&
              b.get<ParticleEmitter3D>()->system.desc.look.receives_shadow,
          "shadow reception default or authoring lost");
    check(world.components().patch_field(a, "ParticleEmitter3D", "receives_shadow", false) &&
              !a.get<ParticleEmitter3D>()->system.desc.look.receives_shadow &&
              b.get<ParticleEmitter3D>()->system.desc.look.receives_shadow,
          "shadow edit leaked to sibling");
    check(!ParticleLook3D{}.casts_shadow && ParticleLook3D{}.shadow_cutoff == .5f &&
              a.get<ParticleEmitter3D>()->system.desc.look.casts_shadow &&
              b.get<ParticleEmitter3D>()->system.desc.look.shadow_cutoff == .3f,
          "shadow casting defaults or authored settings lost");
    check(world.components().patch_field(a, "ParticleEmitter3D", "casts_shadow", false) &&
              world.components().patch_field(a, "ParticleEmitter3D", "shadow_cutoff", .7f) &&
              !a.get<ParticleEmitter3D>()->system.desc.look.casts_shadow &&
              b.get<ParticleEmitter3D>()->system.desc.look.casts_shadow,
          "casting edit leaked to sibling");
    check(
        !world.components().patch_field(a, "ParticleEmitter3D", "shadow_cutoff", std::numeric_limits<f32>::infinity()),
        "nonfinite shadow cutoff accepted");
    for (f32 bad : {-1.f, 1.01f}) {
        a.get_mut<ParticleEmitter3D>()->system.desc.look.shadow_cutoff = bad;
        check(!validate_particles3d(world).diagnostics.empty(), "out of range shadow cutoff accepted");
    }
    a.get_mut<ParticleEmitter3D>()->system.desc.look.shadow_cutoff = .7f;
    const auto descriptor_count = world.components().descriptors().size();
    register_builtin_editor_components(world);
    check(world.components().descriptors().size() == descriptor_count, "particle registration not idempotent");
    check(validate_particles3d(world, {2, 16, 32}).diagnostics.empty(), "valid particle scene rejected");
    check(!validate_particles3d(world, {2, 16, 31}).diagnostics.empty() &&
              !validate_particles3d(world, {1, 16, 32}).diagnostics.empty(),
          "scene capacity/emitter limits ignored");
    check(!world.components().patch_field(a, "ParticleEmitter3D", "shape", std::string("unknown")),
          "unknown enum accepted");
    check(!world.components().patch_field(a, "ParticleEmitter3D", "lifetime", Vec2f{2, 1}), "reversed range accepted");
    check(!world.components().patch_field(a, "ParticleEmitter3D", "rate", std::numeric_limits<f32>::infinity()),
          "nonfinite rate accepted");
    rendering.propagate_transforms();
    for (int i = 0; i < 120; ++i)
        rendering.simulate_particles(1. / 60);
    check(a.get<ParticleEmitter3D>() != b.get<ParticleEmitter3D>(), "prefabs share live particle state");
    check(a.get<ParticleEmitter3D>()->system.count() > 0 && b.get<ParticleEmitter3D>()->system.count() > 0,
          "authored systems did not simulate");
    check(std::abs(a.get<ParticleEmitter3D>()->system.time() - 2) < 1e-6 &&
              std::abs(b.get<ParticleEmitter3D>()->system.time() - 2) < 1e-6,
          "inherited system advanced more than once");
    check(std::abs(a.get<ParticleEmitter3D>()->system.particles()[0].position.x + 1) < .001f &&
              std::abs(b.get<ParticleEmitter3D>()->system.particles()[0].position.x - 1) < .001f,
          "parent transform missing from emission");
    const auto base = a.raw().target(flecs::IsA);
    check(base && base.get<ParticleEmitter3D>()->system.count() == 0 &&
              base.get<ParticleEmitter3D>()->system.time() == 0,
          "prefab definition simulated");
    a.get_mut<ParticleEmitter3D>()->emitting = false;
    for (int i = 0; i < 60; ++i)
        rendering.simulate_particles(1. / 60);
    check(a.get<ParticleEmitter3D>()->system.count() == 0 && b.get<ParticleEmitter3D>()->system.count() > 0,
          "stop-emitting did not drain independently");
    b.get_mut<ParticleEmitter3D>()->system.emitting = false;
    for (int i = 0; i < 60; ++i)
        rendering.simulate_particles(1. / 60);
    check(!b.get<ParticleEmitter3D>()->system.emitting && b.get<ParticleEmitter3D>()->system.count() == 0,
          "bridge overwrote native emission stop");
    b.get_mut<ParticleEmitter3D>()->system.emitting = true;
    for (int i = 0; i < 10; ++i)
        rendering.simulate_particles(1. / 60);
    a.get_mut<ParticleEmitter3D>()->system.burst(1000);
    check(a.get<ParticleEmitter3D>()->system.count() == 16, "burst exceeded authored capacity");
    Scene3D draws;
    rendering.collect(draws);
    check(draws.particle_draws().size() == 2, "stopped emitter hides live bursts");
    a.get_mut<ParticleEmitter3D>()->enabled = false;
    draws.clear();
    rendering.collect(draws);
    check(draws.particle_draws().size() == 1 && !validate_particles3d(world, {2, 16, 31}).diagnostics.empty(),
          "disabled emitter bypasses capacity budget");
    a.get_mut<ParticleEmitter3D>()->system.clear();
    check(world.components().patch_field(a, "ParticleEmitter3D", "atlas_columns", u32(0)), "atlas edit failed");
    check(!validate_particles3d(world).diagnostics.empty(), "invalid atlas accepted");
    check(world.components().patch_field(a, "ParticleEmitter3D", "atlas_columns", u32(1)), "atlas restore failed");
    auto saved = build_scene_asset(world);
    check(saved.ok(), "particle scene save failed");
    std::ostringstream text;
    write_scene_asset(text, *saved.scene);
    check(text.str().find("bound_texture") == std::string::npos && text.str().find("_particles") == std::string::npos,
          "runtime particle state serialized");
    kin::EcsWorld restored;
    register_builtin_editor_components(restored);
    auto copy = instantiate_scene_asset(restored, *saved.scene, scratch.path, restored.components());
    check(copy.ok() && emitter(copy, "left").get<ParticleEmitter3D>()->system.count() == 0 &&
              !emitter(copy, "left").get<ParticleEmitter3D>()->emitting,
          "particle overrides or fresh simulation lost on save");
    check(!emitter(copy, "left").get<ParticleEmitter3D>()->system.desc.look.receives_shadow &&
              emitter(copy, "right").get<ParticleEmitter3D>()->system.desc.look.receives_shadow,
          "shadow reception overrides lost on save/load");
    check(!emitter(copy, "left").get<ParticleEmitter3D>()->system.desc.look.casts_shadow &&
              emitter(copy, "left").get<ParticleEmitter3D>()->system.desc.look.shadow_cutoff == .7f &&
              emitter(copy, "right").get<ParticleEmitter3D>()->system.desc.look.casts_shadow &&
              emitter(copy, "right").get<ParticleEmitter3D>()->system.desc.look.shadow_cutoff == .3f,
          "casting settings lost on save/load");
    auto& desc = b.get_mut<ParticleEmitter3D>()->system.desc;
    desc.max_particles = UINT32_MAX;
    check(!validate_particles3d(world).diagnostics.empty(), "huge capacity accepted");
    desc.max_particles = 16;
    desc.lifetime = {0, 1};
    check(!validate_particles3d(world).diagnostics.empty(), "zero lifetime accepted");
    desc.lifetime = {.2f, .3f};
    desc.look.facing = ParticleFacing3D::Axis;
    desc.look.axis = {};
    check(!validate_particles3d(world).diagnostics.empty(), "zero axis accepted");
}

void particle_gpu(Renderer2D& renderer) {
    Scratch scratch;
    write(scratch.path / "effect.kinprefab", prefab);
    std::string config = prefab;
    config.insert(config.find("\"seed\""), "\"texture\":\"shared pixel.png\",\"texture_nearest\":true,");
    write(scratch.path / "effect.kinprefab", config);
    write(scratch.path / "scene.kinscene", scene);
    std::filesystem::copy_file(std::filesystem::path(KIN_TEST_MODELS) / "shared pixel.png",
                               scratch.path / "shared pixel.png");
    std::filesystem::copy_file(std::filesystem::path(KIN_TEST_MODELS) / "shared-a.gltf",
                               scratch.path / "shared-a.gltf");
    const auto png = read(scratch.path / "shared pixel.png");
    Renderer3D r3d(renderer);
    Assets3D assets(r3d, scratch.path);
    SceneSession3DOptions options;
    options.viewport = {160, 120};
    options.view.samples = 1;
    options.particle_limits = {2, 16, 32};
    SceneSession3D session(r3d, assets, options);
    const auto ok = [&](const auto& value, const char* message) {
        if (!value)
            for (const auto& e : session.diagnostics())
                std::cerr << e << '\n';
        check(value, message);
    };
    ok(session.load("scene.kinscene"), "particle scene binding failed");
    auto a = emitter(*session.instance(), "left"), b = emitter(*session.instance(), "right");
    auto texture = a.get<ParticleEmitter3D>()->system.desc.look.texture;
    auto model = assets.load_model("shared-a.gltf").value_or({}).shared();
    check(texture && texture == b.get<ParticleEmitter3D>()->system.desc.look.texture &&
              texture == model->parts[0].material.texture && texture != model->parts[0].material.normal,
          "particle/glTF texture sharing ignored role/filter");
    for (int i = 0; i < 60; ++i)
        ok(session.step(1.f / 60), "particle stepping failed");
    const auto time = a.get<ParticleEmitter3D>()->system.time();
    check(resolve_particles3d_assets(*session.world(), assets).diagnostics.empty() &&
              a.get<ParticleEmitter3D>()->system.time() == time,
          "rebinding reset simulation");
    renderer.clear();
    ok(session.render(), "particle render failed");
    renderer.draw_texture(session.view()->texture(), {0, 0, 160, 120});
    std::vector<u8> pixels;
    Vec2i size;
    check(renderer.read_rgba({0, 0, 160, 120}, pixels, size), "particle readback failed");
    renderer.present();
    check(session.scene()->particle_draws().size() == 2 && session.scene()->particle_sprites().size() > 0,
          "particle collection empty");
    size_t coloured = 0;
    for (size_t i = 0; i < pixels.size(); i += 4)
        if (pixels[i] > 100 || pixels[i + 1] > 100 || pixels[i + 2] > 100)
            ++coloured;
    check(coloured > 8, "rendered particle sprites invisible");
    b.get_mut<ParticleEmitter3D>()->texture_nearest = false;
    check(resolve_particles3d_assets(*session.world(), assets).diagnostics.empty() &&
              b.get<ParticleEmitter3D>()->system.desc.look.texture != texture,
          "particle filter aliases texture");
    b.get_mut<ParticleEmitter3D>()->texture_asset.id = "missing.png";
    auto last = b.get<ParticleEmitter3D>()->system.desc.look.texture;
    check(!resolve_particles3d_assets(*session.world(), assets).diagnostics.empty() &&
              b.get<ParticleEmitter3D>()->system.desc.look.texture == last,
          "bad binding discarded previous texture");
    b.get_mut<ParticleEmitter3D>()->texture_asset.id.clear();
    check(resolve_particles3d_assets(*session.world(), assets).diagnostics.empty() &&
              !b.get<ParticleEmitter3D>()->system.desc.look.texture,
          "empty texture did not restore procedural particles");
    b.get_mut<ParticleEmitter3D>()->system.desc.look.texture = texture;
    check(resolve_particles3d_assets(*session.world(), assets).diagnostics.empty() &&
              b.get<ParticleEmitter3D>()->system.desc.look.texture == texture,
          "empty authored ref cleared a C++ texture");
    auto* active = session.world();
    const auto hash = a.get<ParticleEmitter3D>()->system.state_hash();
    write(scratch.path / "shared pixel.png", "broken");
    check(session.reload_if_changed().value_or(kin::SceneSession3DReload::Failed) == SceneSession3DReload::Failed &&
              session.world() == active && a.get<ParticleEmitter3D>()->system.state_hash() == hash,
          "bad texture reload replaced simulation");
    ok(session.step(1.f / 60), "last good effect cannot continue");
    write(scratch.path / "shared pixel.png", png);
    check(session.reload_if_changed().value_or(kin::SceneSession3DReload::Failed) == SceneSession3DReload::Activated &&
              session.world() != active,
          "particle texture reload failed to recover");
    a = emitter(*session.instance(), "left");
    check(a.get<ParticleEmitter3D>()->system.time() == 0 &&
              a.get<ParticleEmitter3D>()->system.desc.look.texture != texture,
          "reload did not refresh texture/fresh world");
    a.get_mut<ParticleEmitter3D>()->system.desc.max_particles = 17;
    check(!session.step(1.f / 60) && !session.collect() && a.get<ParticleEmitter3D>()->system.count() == 0,
          "runtime edit bypassed particle budget");
    a.get_mut<ParticleEmitter3D>()->system.desc.max_particles = 16;
    ok(session.step(1.f / 60), "budget edit did not recover");
    const std::string capacity_field = "\"max_particles\":16";
    write(
        scratch.path / "effect.kinprefab",
        std::string(config).replace(config.find(capacity_field), capacity_field.size(), "\"max_particles\":999999999"));
    check(session.reload_if_changed().value_or(kin::SceneSession3DReload::Failed) == SceneSession3DReload::Failed,
          "oversized prefab capacities accepted");
    write(scratch.path / "effect.kinprefab", config);
    check(session.reload_if_changed().value_or(kin::SceneSession3DReload::Failed) == SceneSession3DReload::Activated,
          "valid prefab edit did not recover");
    Scratch archive;
    check(write_content_pack(scratch.path, archive.path / "effects.kinpak").ok, "effect pack failed");
    const auto mount = archive.path / "virtual";
    mount_content_pack(mount, ContentPack::open(archive.path / "effects.kinpak"));
    {
        Assets3D packed(r3d, mount);
        SceneSession3D scene(r3d, packed, options);
        check(scene.load("scene.kinscene") && scene.step(1.f / 60), "packed particle references failed");
    }
    unmount_content_pack(mount);
    {
        Assets3D camp_assets(r3d, KIN_TEST_SUMMIT);
        SceneSession3DOptions camp_options;
        camp_options.viewport = {160, 120};
        camp_options.view.samples = 1;
        camp_options.particle_limits = {4, 128, 208};
        SceneSession3D camp(r3d, camp_assets, camp_options);
        check(camp.load("scenes/camp-play.kinscene"), "campfire prefab failed");
        for (int i = 0; i < 120; ++i)
            check(camp.step(1.f / 60), "campfire stepping failed");
        check(camp.collect() && camp.scene()->particle_draws().size() == 3 && camp.scene()->lights.points.size() == 1,
              "campfire particle/light composition incomplete");
    }
    texture = {};
    last = {};
    model.reset();
    session.unload();
    assets.trim();
    check(assets.stats().texture_entries == 0, "particle texture retained after unload");
}
} // namespace
int main(int argc, char** argv) {
    try {
        cpu();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "cpu")
        return 0;
    bool ready = false;
    try {
        App app{};
        auto& window = app.create_window({.title = "particle-assets", .width = 160, .height = 120, .hidden = true});
        auto backend = make_render_backend(window, false, true);
        if (!backend || backend->name() != "SDL_GPU")
            throw std::runtime_error("GPU backend required");
        ready = true;
        Renderer2D renderer(std::move(backend));
        particle_gpu(renderer);
        std::cout << "Particle authoring, isolation, budgets, texture sharing and reload checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        const char* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
        return ready || (required && std::string_view(required) != "0") ? 1 : 77;
    }
}
