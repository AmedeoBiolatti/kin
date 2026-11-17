#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <kin/assets/content.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/backend.hpp>
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
    check(model && !model->parts.empty(), "particle/glTF sharing fixture failed to load");
    const auto nearest = assets.load_texture("shared pixel.png", true, true).value_or(Texture{});
    const auto mipmapped = assets.load_texture("shared pixel.png", false, true).value_or(Texture{});
    const auto& material = model->parts[0].material;
    check(texture && texture == b.get<ParticleEmitter3D>()->system.desc.look.texture && texture == nearest &&
              texture != mipmapped && mipmapped == material.texture && texture != material.normal &&
              mipmapped != material.normal,
          "particle/glTF texture sharing ignored colour role or legacy filtering");
    check(material.texture_samplers[u32(MaterialMap3D::BaseColor)] &&
              material.texture_samplers[u32(MaterialMap3D::BaseColor)]->mag == TextureFilter3D::Nearest,
          "imported per-map nearest sampling was lost");
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
    check(!session.reload_if_changed() && session.world() == active &&
              a.get<ParticleEmitter3D>()->system.state_hash() == hash,
          "bad texture reload replaced simulation");
    ok(session.step(1.f / 60), "last good effect cannot continue");
    write(scratch.path / "shared pixel.png", png);
    check(session.reload_if_changed().value == SceneSession3DReload::Activated && session.world() != active,
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
    check(!session.reload_if_changed(), "oversized prefab capacities accepted");
    write(scratch.path / "effect.kinprefab", config);
    check(session.reload_if_changed().value == SceneSession3DReload::Activated, "valid prefab edit did not recover");
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
    assert(session.unload());
    assets.trim();
    check(assets.stats().texture_entries == 0, "particle texture retained after unload");
}
// Exercise the persistent simulation through its public API and actual pixels.
void simulated_gpu(Renderer2D& renderer) {
    Renderer3D r(renderer);
    check(r.capabilities().gpu_particles, "GPU particle simulation unavailable");
    ParticleDesc3D d;
    d.max_particles = 65; // partial final compute group
    d.lifetime = {.5f, .5f};
    d.speed = {0, 0};
    d.size = {1, 1};
    d.alpha_start = d.alpha_end = 1;
    d.look.fogged = false;
    d.look.softness = 0;
    d.colour_start = d.colour_end = {1, 0, 0};
    auto make = r.create_gpu_particles(d, 7);
    check(make, "GPU emitter creation failed: " + diagnostics3d_text(make.diagnostics));
    auto system = *make;
    auto copy = system;
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 0, 0}, {0, 0, -1}, 60);
    scene.cull = false;
    check(scene.draw_particles(copy), "GPU emitter submission failed");
    auto view = r.create_view({160, 120}, {.samples = 1}).value_or({});
    const auto picture = [&](View3D& v) {
        renderer.clear(colors::black);
        check(r.render(v, scene), "GPU particle render failed: " + diagnostics3d_text(r.diagnostics()));
        renderer.draw_texture(v.texture(), {0, 0, 160, 120});
        std::vector<u8> pixels;
        Vec2i size;
        check(renderer.read_rgba({0, 0, 160, 120}, pixels, size), "GPU particle readback failed");
        renderer.present();
        return pixels;
    };
    const auto centre = [](const auto& pixels) { return pixels[(60 * 160 + 80) * 4]; };
    check(centre(picture(view)) == 0, "new GPU emitter is not empty");
    GpuParticleUpdate3D u;
    u.transform = Mat4f::translation({0, 0, -4});
    u.burst = 1;
    check(r.update_gpu_particles(system, u), "GPU burst failed");
    const auto first = picture(view);
    check(centre(first) > 200, "GPU burst did not draw");
    check(picture(view) == first, "rendering advanced the GPU simulation");
    auto second = r.create_view({160, 120}, {.samples = 1}).value_or({});
    check(picture(second) == first, "second view changed simulation");
    auto bad = u;
    bad.dt = std::numeric_limits<f64>::quiet_NaN();
    check(!r.update_gpu_particles(system, bad), "GPU NaN timestep accepted");
    bad.dt = -1;
    check(!r.update_gpu_particles(system, bad), "GPU negative timestep accepted");
    bad.dt = .251;
    check(!r.update_gpu_particles(system, bad), "GPU catch-up bound ignored");
    check(!r.update_gpu_particles({}, u), "empty GPU handle accepted");
    check(picture(view) == first, "invalid update mutated GPU state");
    u.burst = 0;
    u.dt = .25;
    u.emitting = false;
    check(r.update_gpu_particles(copy, u) && r.update_gpu_particles(copy, u), "GPU expiry update failed");
    check(centre(picture(view)) == 0, "GPU particles did not expire");
    u.dt = 0;
    u.burst = 1000000;
    check(r.update_gpu_particles(system, u), "bounded oversized GPU burst failed");
    check(centre(picture(view)) > 200 && r.debug_stats().particles_drawn == 65,
          "GPU ring capacity or partial group failed");
    u.burst = 0;
    u.clear = true;
    check(r.update_gpu_particles(system, u) && centre(picture(view)) == 0, "GPU clear failed");
    // Rate emission, world-space motion, lifetime curves and TAA/soft variants.
    d.rate = 8;
    d.velocity_min = d.velocity_max = {4, 0, 0};
    d.look.soft = true;
    system = r.create_gpu_particles(d, 3).value_or({});
    scene.clear();
    check(scene.draw_particles(system), "GPU rate emitter submission failed");
    scene.taa.enabled = true;
    u.clear = false;
    u.emitting = true;
    u.dt = .125;
    check(r.update_gpu_particles(system, u) && centre(picture(view)) > 200, "GPU rate/soft/TAA path failed");
    u.emitting = false;
    u.dt = .25;
    check(r.update_gpu_particles(system, u), "GPU motion update failed");
    check(centre(picture(view)) < 20, "GPU particles did not move in world space");
    // Scene and recorded commands keep resources alive after the caller drops handles.
    copy = {};
    system = {};
    make = {};
    (void)picture(view);
    scene.clear();
    check(scene.gpu_particle_draws().empty(), "scene clear retained GPU submissions");
    // Compare analytic gravity/drag against the existing fixed-step CPU model.
    d = ParticleDesc3D{};
    d.max_particles = 1;
    d.speed = {0, 0};
    d.lifetime = {2, 2};
    d.size = {.2f, .2f};
    d.gravity = {0, -8, 0};
    d.drag = 2;
    d.alpha_start = d.alpha_end = 1;
    d.colour_start = d.colour_end = {1, 0, 0};
    d.look.fogged = false;
    d.look.softness = 0;
    system = r.create_gpu_particles(d).value_or({});
    scene.taa.enabled = false;
    check(scene.draw_particles(system), "GPU force emitter submission failed");
    u = {};
    u.transform = Mat4f::translation({0, 0, -4});
    u.burst = 1;
    check(r.update_gpu_particles(system, u), "GPU force burst failed");
    u.burst = 0;
    u.dt = .25;
    u.wind = {2, 0, 0};
    u.emitting = false;
    check(r.update_gpu_particles(system, u), "GPU force update failed");
    ParticleSystem3D control;
    control.desc = d;
    control.transform = u.transform;
    control.burst(1);
    Wind3D wind;
    wind.speed = 2;
    control.update(.25, &wind);
    const auto projected = scene.camera.project(control.particles()[0].position, {0, 0, 160, 120});
    const auto forced = picture(view);
    check(projected && forced[(int(projected->pos.y) * 160 + int(projected->pos.x)) * 4] > 200 && centre(forced) < 20,
          "GPU gravity/drag differs from CPU reference");
    const auto allocations = renderer.backend_stats().gpu_memory.allocations;
    u.dt = 0;
    for (int i = 0; i < 4; ++i) {
        check(r.update_gpu_particles(system, u) && r.render(view, scene), "warmed GPU update/render failed");
        renderer.present();
    }
    check(renderer.backend_stats().gpu_memory.allocations == allocations && r.debug_stats().device_waits == 0,
          "warmed GPU simulation allocated resources or waited");
    auto invalid = d;
    invalid.max_particles = 0;
    check(!r.create_gpu_particles(invalid), "zero GPU capacity accepted");
    invalid = d;
    invalid.max_particles = 1048577;
    check(!r.create_gpu_particles(invalid), "oversized GPU capacity accepted");
    invalid = d;
    invalid.look.casts_shadow = true;
    check(r.create_gpu_particles(invalid), "GPU shadow casting rejected");
    invalid.look.shadow_cutoff = std::numeric_limits<f32>::quiet_NaN();
    check(!r.create_gpu_particles(invalid), "invalid GPU shadow cutoff accepted");
    invalid = d;
    invalid.lifetime = {0, 1};
    check(!r.create_gpu_particles(invalid), "zero GPU lifetime accepted");
}
void gpu_depth_order(Renderer2D& renderer) {
    Renderer3D r(renderer);
    for (u32 capacity : {3u, 257u, 1025u, 20000u})
        for (bool sorted : {false, true}) {
            ParticleDesc3D d;
            d.max_particles = capacity;
            d.speed = {0, 0};
            d.lifetime = {1, 1};
            d.size = {2, 2};
            d.colour_start = {1, 0, 0};
            d.colour_end = {0, 0, 1};
            d.alpha_start = d.alpha_end = .5f;
            d.look.fogged = false;
            d.look.softness = 0;
            d.look.sort = sorted;
            auto system = r.create_gpu_particles(d).value_or({});
            check(r.update_gpu_particles(system, {.transform = Mat4f::translation({0, 0, -4}), .burst = 1}),
                  "sorted front birth failed");
            if (capacity > 3)
                check(r.update_gpu_particles(system,
                                             {.transform = Mat4f::translation({1000, 0, -4}), .burst = capacity - 2}),
                      "sorted offscreen births failed");
            check(r.update_gpu_particles(system, {.dt = .25, .emitting = false}) &&
                      r.update_gpu_particles(system, {.dt = .25, .emitting = false}),
                  "sorted aging failed");
            check(r.update_gpu_particles(system, {.transform = Mat4f::translation({0, 0, -6}), .burst = 1}),
                  "sorted back birth failed");
            auto gpu_view = r.create_view({160, 120}, {.samples = 1}).value_or({});
            auto cpu_view = r.create_view({160, 120}, {.samples = 1}).value_or({});
            auto other_view = r.create_view({160, 120}, {.samples = 1}).value_or({});
            std::vector<u8> first;
            for (int camera = 0; camera < 3; ++camera) {
                Scene3D gpu, cpu;
                gpu.camera = cpu.camera = camera == 1 ? Camera3D::look_at({0, 0, -10}, {0, 0, 0}, 60)
                                                      : Camera3D::look_at({0, 0, 0}, {0, 0, -1}, 60);
                check(gpu.draw_particles(system), "sorted GPU submission failed");
                ParticleSprite3D sprites[2];
                sprites[0].position = {0, 0, -4};
                sprites[0].colour = {.5f, 0, .5f};
                sprites[1].position = {0, 0, -6};
                sprites[1].colour = {1, 0, 0};
                for (auto& p : sprites) {
                    p.size = 2;
                    p.alpha = .5f;
                }
                check(cpu.draw_particles(d.look, sprites), "sorted CPU oracle rejected");
                const auto shoot = [&](View3D& v, const Scene3D& scene) {
                    renderer.clear(colors::black);
                    check(r.render(v, scene), "sorted render failed");
                    renderer.draw_texture(v.texture(), {0, 0, 160, 120});
                    std::vector<u8> pixels;
                    Vec2i size;
                    check(renderer.read_rgba({0, 0, 160, 120}, pixels, size), "sorted readback failed");
                    renderer.present();
                    return pixels;
                };
                const auto expected = shoot(cpu_view, cpu);
                const auto actual = shoot(camera == 1 ? other_view : gpu_view, gpu);
                int worst = 0;
                for (std::size_t i = 0; i < actual.size(); ++i)
                    worst = std::max(worst, std::abs(int(actual[i]) - int(expected[i])));
                check(worst <= 1, "GPU depth order/culling differs from CPU sorted sprites");
                if (camera == 0)
                    first = actual;
                if (camera == 2)
                    check(actual == first, "second view left a stale GPU sort");
            }
        }
}

void gpu_motion_history(Renderer2D& renderer) {
    Renderer3D r(renderer);
    int accumulated = 0;
    for (const auto facing :
         {ParticleFacing3D::Camera, ParticleFacing3D::Stretched, ParticleFacing3D::Axis, ParticleFacing3D::Flat}) {
        ParticleDesc3D d;
        d.max_particles = 1;
        d.lifetime = {10, 10};
        d.speed = {0, 0};
        d.size = {.7f, .7f};
        d.velocity_min = d.velocity_max = {.25f, .1f, 0};
        d.rotation = {15, 15};
        d.spin = {30, 30};
        d.alpha_start = d.alpha_end = .65f;
        d.colour_start = d.colour_end = {1, .3f, .1f};
        d.look.fogged = false;
        d.look.softness = .25f;
        d.look.facing = facing;
        d.look.stretch = .6f;
        if (facing == ParticleFacing3D::Flat)
            d.look.axis = {0, 0, 1};
        auto system = r.create_gpu_particles(d).value_or({});
        auto temporal = r.create_view({160, 120}, {.samples = 1}).value_or({});
        auto oracle = r.create_view({160, 120}, {.samples = 1}).value_or({});
        auto spatial = r.create_view({160, 120}, {.samples = 1}).value_or({});
        auto other = r.create_view({160, 120}, {.samples = 1}).value_or({});
        Vec3f origin{-.3f, .1f, -4};
        GpuParticleUpdate3D u;
        u.transform = Mat4f::translation(origin);
        u.burst = 1;
        check(r.update_gpu_particles(system, u), "temporal GPU burst failed");
        f32 age = 0;
        u64 birth = 1;
        for (int frame = 0; frame < 26; ++frame) {
            u = {};
            u.emitting = false;
            u.transform = Mat4f::translation(origin);
            if (frame == 10 || frame == 14) {
                u.clear = frame == 10;
                u.burst = 1;
                age = 0;
                ++birth;
            } else if (frame > 0 && frame < 16) {
                u.dt = 1. / 16;
                age += 1.f / 16;
            }
            check(r.update_gpu_particles(system, u), "temporal GPU update failed");
            Scene3D gpu, cpu;
            const f32 camera_x = frame < 16 ? .008f * frame : .12f;
            gpu.camera = cpu.camera = Camera3D::look_at({camera_x, 0, 0}, {camera_x, 0, -1}, 60);
            gpu.taa = cpu.taa = {.enabled = true, .blend = .98f, .sharpness = 0};
            const bool submitted = frame != 22 && frame != 23;
            if (submitted) {
                check(gpu.draw_particles(system), "temporal GPU submission failed");
                ParticleSprite3D p;
                p.position = origin + d.velocity_min * age;
                p.velocity = d.velocity_min;
                p.size = .7f;
                p.alpha = .65f;
                p.colour = d.colour_start;
                p.rotation = (15.f + age * 30.f) * (3.14159265358979323846f / 180);
                p.id = birth;
                check(cpu.draw_particles(d.look, std::span{&p, 1}, 17), "temporal CPU oracle submission failed");
            }
            if (frame == 18) {
                temporal.reset_history();
                oracle.reset_history();
            }
            const auto shoot = [&](View3D& view, const Scene3D& scene) {
                renderer.clear(colors::black);
                check(r.render(view, scene), "temporal GPU picture failed");
                renderer.draw_texture(view.texture(), {0, 0, 160, 120});
                std::vector<u8> pixels;
                Vec2i size;
                check(renderer.read_rgba({0, 0, 160, 120}, pixels, size), "temporal GPU readback failed");
                renderer.present();
                return pixels;
            };
            const auto expected = shoot(oracle, cpu);
            const auto actual = shoot(temporal, gpu);
            int worst = 0;
            for (std::size_t i = 0; i < actual.size(); ++i)
                worst = std::max(worst, std::abs(int(actual[i]) - int(expected[i])));
            if (worst > 3)
                std::cerr << "GPU history facing " << int(facing) << " frame " << frame << ": " << worst << '\n';
            check(worst <= 3, "GPU particle TAA differs from CPU motion history");
            gpu.taa.blend = 0;
            const auto fresh = shoot(spatial, gpu);
            if (frame == 21)
                for (std::size_t i = 0; i < actual.size(); ++i)
                    accumulated += std::abs(int(actual[i]) - int(fresh[i])) > 2;
            // A second camera's render must not replace the first view's past.
            gpu.taa.blend = .98f;
            gpu.camera = Camera3D::look_at({.4f, .2f, .1f}, {0, 0, -4}, 60);
            check(r.render(other, gpu), "second GPU history view failed");
            renderer.present();
        }
    }
    check(accumulated > 0, "GPU particle TAA still rejects every temporal sample");
}
void gpu_global_composition(Renderer2D& renderer) {
    Renderer3D r(renderer);
    const auto plane = r.create_mesh(meshes::box({1, 1, .02f})).value_or({});
    const auto read = [&](const Texture& texture) {
        renderer.clear(colors::black);
        renderer.draw_texture(texture, {0, 0, 160, 120});
        std::vector<u8> pixels;
        Vec2i size;
        check(renderer.read_rgba({0, 0, 160, 120}, pixels, size), "composition readback failed");
        renderer.present();
        return pixels;
    };
    const auto equal = [&](const auto& a, const auto& b, const std::string& label, int tolerance = 3) {
        int worst = 0;
        std::size_t at = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::abs(int(a[i]) - int(b[i])) > worst) {
                worst = std::abs(int(a[i]) - int(b[i]));
                at = i;
            }
        if (worst > tolerance)
            std::cerr << label << " max=" << worst << " pixel=" << at / 4 << " channel=" << at % 4
                      << " actual=" << int(a[at]) << " expected=" << int(b[at]) << '\n';
        check(worst <= tolerance, label);
    };
    for (bool linear : {false, true}) {
        check(renderer.set_color_space(linear ? ColorSpace::Linear : ColorSpace::Gamma),
              "composition colour space failed");
        for (int mode = 0; mode < 4; ++mode)
            for (bool taa : {false, true}) {
                std::vector<u8> pixels(37 * 19 * 4);
                for (int y = 0; y < 19; ++y)
                    for (int x = 0; x < 37; ++x) {
                        auto i = (y * 37 + x) * 4;
                        pixels[i] = u8(40 + x * 5);
                        pixels[i + 1] = u8(30 + y * 9);
                        pixels[i + 2] = u8((x + y) % 2 ? 210 : 50);
                        pixels[i + 3] = u8(mode == 2 ? 0 : 80 + (x % 9) * 18);
                    }
                auto texture = renderer.create_texture_from_rgba(pixels.data(), {37, 19});
                renderer.set_scale_mode(texture, mode == 1   ? ScaleMode::Nearest
                                                 : mode == 2 ? ScaleMode::Linear
                                                             : ScaleMode::Mipmapped);
                ParticleDesc3D a;
                a.max_particles = 3;
                a.speed = {0, 0};
                a.lifetime = {10, 10};
                a.size = mode == 3 ? ParticleRange3D{.4f, .4f} : ParticleRange3D{2.6f, 2.6f};
                a.colour_start = a.colour_end = {1, .15f, .05f};
                a.alpha_start = a.alpha_end = .45f;
                a.look.softness = .25f;
                a.look.sort = true;
                if (mode)
                    a.look.texture = texture;
                a.look.blend = mode == 1   ? ParticleBlend3D::Additive
                               : mode == 2 ? ParticleBlend3D::Premultiplied
                                           : ParticleBlend3D::Alpha;
                ParticleDesc3D b = a;
                b.colour_start = b.colour_end = {.05f, .6f, 1};
                b.alpha_start = b.alpha_end = .6f;
                b.look.texture = {};
                b.look.blend = ParticleBlend3D::Alpha;
                b.look.facing = ParticleFacing3D::Axis;
                if (mode == 3) {
                    a.look.lit = b.look.lit = 1;
                    a.look.receives_shadow = b.look.receives_shadow = true;
                    a.look.soft = true;
                    a.look.soft_distance = .5f;
                    b.look.fogged = false;
                }
                auto ga = r.create_gpu_particles(a, 7).value_or({}), gb = r.create_gpu_particles(b, 9).value_or({});
                std::array<Vec3f, 2> pa{{{0, 0, mode == 3 ? -11.9f : -8.f}, {0, 0, -3}}};
                // Exercise stable source ties and mesh-before-sprite ties as well as
                // distinct depths. The unsorted one-sprite source below is a
                // CPU-owned anchor in the premultiplied case.
                const std::array<Vec3f, 2> pb{{{0, 0, mode == 1 ? -8.f : -7.f}, {0, 0, mode == 1 ? -5.f : -4.f}}};
                const auto birth = [&](const GpuParticleSystem3D& system, Vec3f p, bool clear = false) {
                    check(r.update_gpu_particles(system,
                                                 {.transform = Mat4f::translation(p), .burst = 1, .clear = clear}),
                          "composition GPU birth failed");
                };
                birth(ga, pa[0]);
                birth(ga, pa[1]);
                birth(gb, pb[0]);
                birth(gb, pb[1]);
                std::vector<Texture> extra_textures;
                std::vector<GpuParticleSystem3D> extra;
                if (mode == 1)
                    for (int i = 0; i < 15; ++i) {
                        const u8 colour[]{u8(40 + i * 11), u8(220 - i * 8), u8(60 + i * 7), u8(50 + i * 9)};
                        extra_textures.push_back(renderer.create_texture_from_rgba(colour, {1, 1}));
                        auto desc = b;
                        desc.max_particles = 1;
                        desc.look.texture = extra_textures.back();
                        extra.push_back(r.create_gpu_particles(desc, u32(i + 21)).value_or({}));
                        birth(extra.back(), {0, 0, -3.2f - f32(i) * .32f});
                    }
                const auto options = ViewOptions3D{.samples = mode % 2 ? 4u : 1u};
                auto gpu_view = r.create_view({160, 120}, options).value_or({});
                auto cpu_view = r.create_view({160, 120}, options).value_or({});
                auto target_gpu = renderer.create_render_target({160, 120}),
                     target_cpu = renderer.create_render_target({160, 120});
                auto depth = renderer.create_render_attachment({160, 120}, TextureFormat::Depth32Float);
                auto soft = renderer.create_render_attachment({160, 120}, TextureFormat::Depth32Float);
                auto normals = renderer.create_render_attachment({160, 120}, TextureFormat::Rg16Float);
                const auto build = [&](bool gpu, int frame) {
                    Scene3D s;
                    s.camera = Camera3D::look_at({.015f * frame, 0, 0}, {.015f * frame, 0, -1}, 60);
                    s.taa = {.enabled = taa, .blend = .95f, .sharpness = 0};
                    s.lights.sun.color = {};
                    (void)s.draw(plane, Mat4f::translation({0, 0, -12}) * Mat4f::scaling({14, 12, 1}),
                                 {.color = Color::rgb(30, 45, 60), .unlit = true, .casts_shadow = false});
                    Material3D glass{.color = Color::rgba(80, 210, 130, 125), .unlit = true, .blend = Blend3D::Alpha};
                    const std::array copies{Mat4f::translation({0, 0, -5}) * Mat4f::scaling({4, 4, 1}),
                                            Mat4f::translation({0, 0, -9}) * Mat4f::scaling({5, 5, 1})};
                    (void)s.draw_instanced(plane, copies, glass);
                    if (mode == 3) {
                        s.lights.sun = {.direction = normalize(Vec3f{-.5f, 1, 1}), .color = {1.4f, .8f, .5f}};
                        s.lights.shadow_area = {{-5, -4, -14}, {5, 5, 2}};
                        s.lights.shadow_resolution = 128;
                        s.lights.points.push_back({.position = {1, 1, -3}, .color = {5, 2, 7}, .radius = 15});
                        s.fog = {.enabled = true, .density = .02f, .colour = {.2f, .3f, .4f}};
                        (void)s.draw(plane, Mat4f::translation({.8f, .2f, -2}) * Mat4f::scaling({.1f, 1, 1}),
                                     {.color = colors::white});
                    }
                    const auto sprites = [&](const ParticleDesc3D& d, auto positions, u64 history) {
                        std::vector<ParticleSprite3D> sprites;
                        for (u32 i = 0; i < positions.size(); ++i) {
                            ParticleSprite3D p;
                            p.position = positions[i];
                            p.size = d.size.min;
                            p.colour = d.colour_start;
                            p.alpha = d.alpha_start;
                            p.id = history * 10 + i + 1;
                            sprites.push_back(p);
                        }
                        check(s.draw_particles(d.look, sprites, history), "composition CPU oracle rejected");
                    };
                    if (frame != 3) {
                        if (gpu)
                            check(s.draw_particles(ga), "composition emitter A rejected");
                        else
                            sprites(a, pa, frame >= 2 ? 101 : 1);
                    }
                    if (gpu)
                        check(s.draw_particles(gb), "composition emitter B rejected");
                    else
                        sprites(b, pb, 2);
                    for (u32 i = 0; i < extra.size(); ++i) {
                        if (gpu)
                            check(s.draw_particles(extra[i]), "composition extra texture emitter rejected");
                        else {
                            auto desc = b;
                            desc.look.texture = extra_textures[i];
                            sprites(desc, std::array{Vec3f{0, 0, -3.2f - f32(i) * .32f}}, u64(i + 10));
                        }
                    }
                    ParticleSprite3D cpu;
                    cpu.position = {0, 0, -6};
                    cpu.size = 3;
                    cpu.colour = {.2f, .1f, 1};
                    cpu.alpha = .35f;
                    cpu.id = 900;
                    check(s.draw_particles({.softness = .25f, .sort = mode != 2}, std::span{&cpu, 1}, 900),
                          "composition CPU particle rejected");
                    return s;
                };
                for (int frame = 0; frame < 5; ++frame) {
                    if (frame == 1 && mode) {
                        pixels[0] = 180;
                        pixels[1] = 40;
                        check(renderer.update_texture(texture, {}, {37, 19}, pixels.data()),
                              "composition texture update failed");
                    }
                    if (frame == 2) {
                        pa = {Vec3f{0, 0, -2.5f}, Vec3f{0, 0, -8.5f}};
                        birth(ga, pa[0], true);
                        birth(ga, pa[1]);
                    }
                    const auto expected_scene = build(false, frame), actual_scene = build(true, frame);
                    renderer.clear(colors::black);
                    check(r.render(cpu_view, expected_scene), "composition CPU view failed");
                    const auto expected = read(cpu_view.texture());
                    renderer.clear(colors::black);
                    check(r.render(gpu_view, actual_scene), "composition GPU view failed");
                    check(r.debug_stats().particle_composition_segments >= 3 && r.debug_stats().device_waits == 0,
                          "composition did not split around transparent mesh copies");
                    equal(read(gpu_view.texture()), expected,
                          "global GPU composition mode " + std::to_string(mode) + " linear " + std::to_string(linear) +
                              " TAA " + std::to_string(taa) + " frame " + std::to_string(frame));
                    if (!taa && frame == 0) {
                        const auto surface = [&](const Scene3D& scene, RenderTarget& target) {
                            renderer.clear(colors::black);
                            const auto prepared = r.prepare(scene).value_or({});
                            if (mode == 3)
                                check(r.render_geometry_prepared({soft, normals}, prepared),
                                      "composition soft depth failed");
                            check(r.render_surface({.colour = target, .depth = depth}, prepared,
                                                   {.stage = SurfaceStage3D::Opaque}),
                                  "composition opaque surface failed");
                            check(r.render_surface({.colour = target, .depth = depth}, prepared,
                                                   {.stage = SurfaceStage3D::Transparent,
                                                    .particles = true,
                                                    .soft_depth = mode == 3 ? soft : Texture{}}),
                                  "composition transparent surface failed");
                            check(prepared.valid() && r.debug_stats().device_waits == 0,
                                  "composition expired prepared inputs");
                            return read(target.texture());
                        };
                        const auto ref = surface(expected_scene, target_cpu);
                        equal(surface(actual_scene, target_gpu), ref,
                              "global GPU surface mode " + std::to_string(mode));
                    }
                }
                auto stable = build(true, 4);
                renderer.clear(colors::black);
                check(r.render(gpu_view, stable), "composition warmup failed");
                renderer.present();
                const auto allocations = renderer.backend_stats().gpu_memory.allocations;
                for (int i = 0; i < 3; ++i) {
                    renderer.clear(colors::black);
                    check(r.render(gpu_view, stable), "composition warmed render failed");
                    renderer.present();
                }
                check(renderer.backend_stats().gpu_memory.allocations == allocations,
                      "stable global composition allocated GPU handles");
            }
    }
    check(renderer.set_color_space(ColorSpace::Gamma), "composition colour space restore failed");
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
        simulated_gpu(renderer);
        gpu_motion_history(renderer);
        gpu_depth_order(renderer);
        gpu_global_composition(renderer);
        particle_gpu(renderer);
        std::cout << "Particle authoring, isolation, budgets, texture sharing and reload checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        const char* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
        return ready || (required && std::string_view(required) != "0") ? 1 : 77;
    }
}
