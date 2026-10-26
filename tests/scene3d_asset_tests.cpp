#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <kin/assets/content.hpp>
#include <kin/ecs/component_schema.hpp>
#include <kin/ecs/editor.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/scene3d.hpp>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/scene_asset_runtime.hpp>
#include <kin/scripting/script_scene.hpp>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
using namespace kin;
void check(const auto& value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
bool near(Vec3f a, Vec3f b) {
    return length(a - b) < .0001f;
}
struct Scratch {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("kin-scene3d-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

constexpr auto prefab_text = R"json({
  "schema":"kin.prefab/1", "id":"lit-prop", "root":"root", "entities":[
    {"id":"root", "components":{
      "Transform3D":{"position":[1,2,3],"rotation":[0,0,0,1],"scale":[1,1,1]},
      "ModelRenderer3D":{"asset":"parts.glb","id":71},
      "Collider3D":{"kind":"box","half_extents":[0.5,1,0.5]},
      "RigidBody3D":{"motion":"kinematic"}}},
    {"id":"lamp","parent":"root","components":{
      "Transform3D":{"position":{"x":0,"y":2,"z":0}},
      "PointLight3D":{"color":[2,1,0.5],"shadow_enabled":true}}}
  ]})json";
constexpr auto scene_text = R"json({
  "schema":"kin.scene/1", "name":"Authored 3D", "entities":[], "prefabs":[
    {"id":"left","asset":"prop.kinprefab","overrides":{
      "root":{"Transform3D":{"position":[-4,0,0]}}}},
    {"id":"right","asset":"prop.kinprefab","overrides":{
      "root":{"Transform3D":{"position":[3,0,0]},"ModelRenderer3D":{"id":82}}}}
  ]})json";

void codecs_and_shared_tools() {
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    const auto count = world.components().descriptors().size();
    register_builtin_editor_components(world);
    check(world.components().descriptors().size() == count, "registration is not idempotent");
    for (auto name : {"Transform3D", "ModelRenderer3D", "SkinnedMeshRenderer", "Animator3D", "PointLight3D",
                      "SpotLight3D", "Collider3D", "RigidBody3D"})
        check(world.components().find(name) != nullptr, "standard registry missing 3D component");

    const auto decode = [](ComponentFieldKind kind, std::string_view text) {
        auto json = parse_json(text);
        std::string error;
        check(json.ok(), "test JSON invalid");
        return component_field_value_from_json(kind, *json.value, error);
    };
    check(decode(ComponentFieldKind::Vec3f, "[1,2,3]").has_value(), "vec3 array rejected");
    check(decode(ComponentFieldKind::Quatf, R"({"x":0,"y":0,"z":0,"w":1})").has_value(), "quat object rejected");
    check(!decode(ComponentFieldKind::Vec3f, "[1,2]"), "short vector accepted");
    check(!decode(ComponentFieldKind::Vec3f, "[1,2,3,4]"), "long vector accepted");
    check(!decode(ComponentFieldKind::Vec3f, "[1,2,1e100]"), "float overflow accepted");
    check(!decode(ComponentFieldKind::Quatf, "[0,0,0,0]"), "zero quaternion accepted");
    check(!decode(ComponentFieldKind::Quatf, "[0,0,0,2]"), "nonunit quaternion accepted");

    const auto schema = parse_component_schema_asset(R"({"schema":"kin.components/1","components":[
      {"name":"Spawn3D","fields":[{"name":"position","kind":"vec3f","default":[1,2,3]},
        {"name":"rotation","kind":"quatf","default":[0,0,0,1]}]}]})");
    check(schema.ok(), "data schema does not understand 3D fields");
    std::vector<std::string> errors;
    check(apply_component_schema_asset(world.components(), *schema.document, errors),
          "3D data schema registration failed");
    auto data = world.entities().create({.name = "spawn", .authored_id = "spawn"});
    check(world.components().add(data, "Spawn3D"), "3D data component add failed");
    check(world.components().patch_field(data, "Spawn3D", "position", Vec3f{7, 8, 9}), "3D data patch failed");

    const auto loaded = parse_prefab_asset(prefab_text, world.components());
    check(loaded.ok(), "standard prefab parser rejected 3D components");
    auto first = instantiate_prefab(world, *loaded.prefab, {}, world.components());
    auto second = instantiate_prefab(world, *loaded.prefab, {}, world.components());
    check(first.ok() && second.ok(), "standard prefab instantiate failed");
    check(world.components().patch_field(first.root, "Transform3D", "position", Vec3f{8, 0, 0}),
          "native vec3 patch failed");
    check(near(second.root.get<Transform3D>()->position, {1, 2, 3}), "prefab patch mutated another instance");
    check(!world.components().patch_field(first.root, "Transform3D", "rotation", Quatf{0, 0, 0, 0}),
          "patch bypassed quaternion validation");
    check(!world.components().patch_field(first.root, "Transform3D", "position",
                                          Vec3f{0, std::numeric_limits<float>::infinity(), 0}),
          "patch accepted infinite position");
    check(!world.components().patch_field(first.root, "Collider3D", "kind", std::string("convex")),
          "enum adapter accepted unsupported shape");
    check(first.root.get<Collider3D>()->kind == Collider3D::Kind::Box, "failed enum patch mutated collider");
    check(first.by_authored_id.at("lamp").get<PointLight3D>()->shadow_enabled, "nested property not authored");

    const auto document = scene_document_from_prefab_asset(*loaded.prefab);
    auto rebuilt =
        prefab_asset_from_scene_document(document, {.prefab_id = "roundtrip", .root = "root"}, world.components());
    check(rebuilt.ok(), "shared scene-to-prefab utility lost new field kinds");
    std::ostringstream text;
    write_prefab_asset(text, *rebuilt.prefab);
    auto roundtrip = parse_prefab_asset(text.str(), world.components());
    check(roundtrip.ok(), "prefab JSON roundtrip failed");
    auto third = instantiate_prefab(world, *roundtrip.prefab, {}, world.components());
    check(third.ok() && near(third.root.get<Transform3D>()->position, {1, 2, 3}), "prefab roundtrip changed transform");
    check(third.root.get<ModelRenderer3D>()->asset.id == "parts.glb", "prefab asset reference lost");
    PrefabOverrideSet bad;
    bad.entities["root"] = {{"Collider3D", {{"kind", std::string("unsupported")}}}};
    const auto rejected = instantiate_prefab(world, *loaded.prefab, {}, world.components(), bad);
    check(!rejected.ok() && !rejected.root.alive() && !rejected.diagnostics.empty(),
          "invalid adapted property did not roll back prefab");

    kin::EcsWorld edit;
    register_builtin_editor_components(edit);
    edit.entities()
        .create({.name = "actor", .authored_id = "actor"})
        .set(Transform3D{.position = {5, 6, 7}})
        .set(ModelRenderer3D{.asset = {"parts.glb"}});
    PlayWorldSession play;
    check(play.start(edit, {.configure_play_world = register_builtin_editor_components}).ok,
          "editor play clone rejected authored 3D");
    const auto actor = play.play_world()->entities().find_by_authored_id("actor");
    check(actor.get<ModelRenderer3D>()->asset.id == "parts.glb" && !actor.get<ModelRenderer3D>()->model,
          "editor clone serialized runtime handles");
    check(near(actor.get<Transform3D>()->position, {5, 6, 7}), "editor clone lost 3D values");

    Scratch scratch;
    std::ofstream(scratch.path / "scene.lua") << R"lua(
function on_load(scene)
    assert(scene:count("Transform3D")==1)
    assert(scene:count("Transform3D","UnknownComponent")==0)
    local entity=scene:with("Transform3D","ModelRenderer3D")[1]
    local transform=entity:get("Transform3D")
    assert(transform.position.z==7 and transform.rotation.w==1)
    entity:patch("Transform3D", {position={x=9,y=8,z=7},rotation={x=0,y=1,z=0,w=0}})
    entity:patch("ModelRenderer3D", {asset="changed.glb"})
    assert(entity:get("ModelRenderer3D").asset=="changed.glb")
end
)lua";
    ScriptScene script{{.asset_root = scratch.path, .script_path = "scene.lua", .name = "3D Lua", .hot_reload = false},
                       &edit};
    check(script.script_loaded(), "shared Lua get/patch rejected 3D fields");
    const auto changed = edit.entities().find_by_authored_id("actor");
    check(near(changed.get<Transform3D>()->position, {9, 8, 7}) && changed.get<Transform3D>()->rotation.y == 1 &&
              changed.get<ModelRenderer3D>()->asset.id == "changed.glb",
          "Lua patches did not reach native 3D components");
}

void authorable_api_settings() {
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    auto asset = parse_scene_asset(R"({"entities":[
      {"id":"env","components":{"SceneEnvironment3D":{
        "sun_color":[4,2,1],"ground_ambient_enabled":true,"ground_ambient":[0.1,0.2,0.3],
        "sky_ground_color":[0.4,0.3,0.2],"sky_night_color":[0.01,0.02,0.03],"sky_mode":"procedural",
        "sky_sun_size":1.2,"sky_sun_disc":12,"sky_moon":true,"sky_moon_direction":[0,1,0],
        "sky_moon_size":2,"sky_moon_light":0.1,"sky_moon_color":[0.6,0.7,0.8],
        "sky_light_threshold":0.02,"sky_light_interval":3,"fog_color":[2,3,4],
        "lod_enabled":true,"lod_error_pixels":2,"lod_bias":1.5,"lod_max_cluster_triangles":12345,"anisotropy":8,"texture_lod_bias":-0.5,
        "gi_enabled":true,"gi_min":[-8,-1,-8],"gi_max":[8,6,8],"gi_cell":0.2,"gi_levels":2,"gi_side":96,"gi_height":48,"gi_probe_spacing":3,"gi_probe_rays":96,
        "gi_probes_per_render":512,"gi_hysteresis":0.9,"gi_intensity":1.5}}},
      {"id":"lamp","components":{"Transform3D":{},"PointLight3D":{
        "color":[6,3,1],"shadow_enabled":true,"shadow_resolution":512,"shadow_bias":0.7,
        "shadow_normal_offset":0.8,"shadow_near_plane":0.1,"shadow_distance":4,"shadow_softness":1.2}}},
      {"id":"cone","components":{"Transform3D":{},"SpotLight3D":{
        "shadow_enabled":true,"shadow_resolution":256,"shadow_bias":0.4,"shadow_normal_offset":0.9,
        "shadow_near_plane":0.2,"shadow_distance":5,"shadow_softness":1.3}}}
    ]})",
                                   world.components());
    check(asset.ok(), "extended light/sky settings rejected");
    auto instance = instantiate_scene_asset(world, *asset.scene, {}, world.components());
    check(instance.ok(), "settings did not instantiate");
    WorldRenderState3D state{world};
    state.propagate_transforms();
    Scene3D scene;
    ViewOptions3D view;
    view.pick = true;
    for (int frame = 0; frame < 2; ++frame) {
        scene.clear();
        check(apply_scene3d_settings(world, scene, &view).diagnostics.empty(), "settings failed to apply");
        state.collect(scene);
        check(scene.lights.sun.color.r == 4 && scene.lights.ground_ambient.enabled &&
                  scene.lights.ground_ambient.color.b == .3f,
              "HDR/ground light lost on collect");
        check(scene.sky.moon && scene.sky.moon_color.g == .7f && scene.sky.night_color.b == .03f &&
                  scene.sky.light_interval == 3 && scene.fog.colour.b == 4,
              "typed sky/fog settings lost");
        check(scene.lod.enabled && scene.lod.max_cluster_triangles == 12345 && scene.lod.bias == 1.5f &&
                  scene.anisotropy == 8 && scene.texture_lod_bias == -0.5f && view.pick,
              "authored quality or game-owned picking lost");
        const kin::GlobalIllumination3D& gi = scene.global_illumination;
        check(gi.enabled && gi.area.min.x == -8 && gi.area.max.y == 6 && gi.cell == .2f && gi.levels == 2 &&
                  gi.side == 96 && gi.height == 48 && gi.probes.enabled &&
                  gi.probes.spacing == 3 && gi.probes.rays == 96 && gi.probes.per_render == 512 &&
                  gi.probes.hysteresis == .9f && gi.probes.intensity == 1.5f,
              "authored global illumination lost");
        check(scene.lights.points.size() == 1 && scene.lights.points[0].shadow_resolution == 512 &&
                  scene.lights.points[0].shadow_bias == .7f && scene.lights.points[0].shadow_near_plane == .1f,
              "point shadow settings lost");
        check(scene.lights.spots.size() == 1 && scene.lights.spots[0].shadow_resolution == 256 &&
                  scene.lights.spots[0].shadow_softness == 1.3f,
              "spot shadow settings lost");
    }
    const auto legacy = parse_scene_asset(
        R"({"entities":[{"id":"legacy","components":{"PointLight3D":{"shadows":true,"shadow_near":0.1}}}]})",
        world.components());
    check(!legacy.ok(), "removed local-light schema was silently accepted");
    const auto lamp_entity = instance.by_authored_id.at("lamp");
    check(!world.components().patch_field(lamp_entity, "PointLight3D", "shadows", true),
          "removed light field remained writable");
    check(!world.components().patch_field(lamp_entity, "PointLight3D", "shadow_near", f32{.1f}),
          "removed near-plane field remained writable");
    auto saved = build_scene_asset(world);
    check(saved.ok(), "extended settings save failed");
    std::ostringstream text;
    write_scene_asset(text, *saved.scene);
    kin::EcsWorld restored;
    register_builtin_editor_components(restored);
    auto parsed = parse_scene_asset(text.str(), restored.components());
    check(parsed.ok(), "extended settings round trip failed");
    auto loaded = instantiate_scene_asset(restored, *parsed.scene, {}, restored.components());
    check(loaded.ok(), "saved settings did not instantiate");
    const auto* env = loaded.by_authored_id.at("env").get<SceneEnvironment3D>();
    check(env->lights.ground_ambient.enabled && env->lights.ground_ambient.color.b == .3f &&
              env->sky.moon_color.g == .7f && env->anisotropy == 8 && env->texture_lod_bias == -0.5f &&
              env->global_illumination.probes.rays == 96 && env->global_illumination.area.max.z == 8,
          "ground/sky colour, anisotropy or global illumination changed in round trip");
    auto environment = instance.by_authored_id.at("env");
    check(world.components().patch_field(environment, "SceneEnvironment3D", "ground_ambient_enabled", false),
          "ground light disable failed");
    check(world.components().patch_field(environment, "SceneEnvironment3D", "ground_ambient", Vec3f{.4f, .5f, .6f}),
          "disabled ground colour edit failed");
    check(!environment.get<SceneEnvironment3D>()->lights.ground_ambient.enabled,
          "editing an inactive colour enabled the feature");
    auto inactive = build_scene_asset(world);
    check(inactive.ok(), "inactive RGB save failed");
    kin::EcsWorld idle;
    register_builtin_editor_components(idle);
    auto inactive_instance = instantiate_scene_asset(idle, *inactive.scene, {}, idle.components());
    check(inactive_instance.ok(), "inactive RGB reload failed");
    const auto& ground = inactive_instance.by_authored_id.at("env").get<SceneEnvironment3D>()->lights.ground_ambient;
    check(!ground.enabled && ground.color.b == .6f, "inactive RGB value or enable flag changed on reload");

    check(!world.components().patch_field(instance.by_authored_id.at("lamp"), "PointLight3D", "shadow_near_plane", f32{0}),
          "invalid shadow near plane accepted");
    check(world.components().patch_field(environment, "SceneEnvironment3D", "anisotropy", u32{17}),
          "anisotropy edit failed");
    check(!apply_scene3d_settings(world, scene).diagnostics.empty() && scene.anisotropy == 8,
          "anisotropy past 16 applied");
    check(world.components().patch_field(environment, "SceneEnvironment3D", "anisotropy", u32{8}),
          "anisotropy restore failed");
    check(world.components().patch_field(environment, "SceneEnvironment3D", "gi_probe_rays", u32{4}),
          "probe rays edit failed");
    check(!apply_scene3d_settings(world, scene).diagnostics.empty() && scene.global_illumination.probes.rays == 96,
          "too few probe rays applied");
    check(world.components().patch_field(environment, "SceneEnvironment3D", "gi_probe_rays", u32{96}),
          "probe rays restore failed");
    check(world.components().patch_field(environment, "SceneEnvironment3D", "gi_height", u32{512}),
          "GI height edit failed");
    check(!apply_scene3d_settings(world, scene).diagnostics.empty() && scene.global_illumination.height == 48,
          "too many GI voxels applied");
    check(world.components().patch_field(environment, "SceneEnvironment3D", "gi_height", u32{48}),
          "GI height restore failed");
    const auto old = scene.sky;
    environment.get_mut<SceneEnvironment3D>()->sky.moon_color.r = std::numeric_limits<f32>::quiet_NaN();
    check(!apply_scene3d_settings(world, scene).diagnostics.empty() && scene.sky.moon_color == old.moon_color,
          "invalid direct RGB edit partially applied");
}

std::vector<u8> image(Renderer2D& renderer, Renderer3D& r3d, View3D& view, Scene3D& scene) {
    renderer.clear();
    check(r3d.render(view, scene), "scene render failed");
    renderer.draw_texture(view.texture(), {0, 0, 160, 120});
    std::vector<u8> rgba;
    Vec2i size;
    check(renderer.read_rgba({0, 0, 160, 120}, rgba, size) && size == Vec2i{160, 120}, "readback failed");
    renderer.present();
    return rgba;
}

void authored_settings_and_animation() {
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    auto parsed = parse_scene_asset(R"({"entities":[
      {"id":"rig","components":{"Transform3D":{"position":[4,0,0],"rotation":[0,0.70710678,0,0.70710678],"scale":[2,2,2]}}},
      {"id":"camera","parent":"rig","components":{"Transform3D":{"position":[0,1,5]},"SceneCamera3D":{"fov":75,"far":0}}},
      {"id":"environment","components":{"SceneEnvironment3D":{"sky_enabled":true,"fog_enabled":true,"fog_density":0.02,"hdr":true,"bloom":0.2},"TimeOfDay3D":{"hours":17}}}
    ]})",
                                    world.components());
    check(parsed.ok(), "camera/environment JSON rejected");
    WorldRenderState3D state{world};
    auto instance = instantiate_scene_asset(world, *parsed.scene, {}, world.components());
    check(instance.ok(), "camera/environment instantiation failed");
    state.propagate_transforms();
    Scene3D scene;
    ViewOptions3D view;
    check(apply_scene3d_settings(world, scene, &view).diagnostics.empty(), "camera/environment application failed");
    check(near(scene.camera.position, {14, 2, 0}) && near(scene.camera.forward(), {-1, 0, 0}) &&
              scene.camera.fov == 75 && std::isinf(scene.camera.z_far),
          "authored parented camera pose/projection lost");
    check(scene.sky.enabled && scene.fog.enabled && scene.fog.density == .02f && view.hdr && view.bloom == .2f,
          "environment/view settings lost");
    check(state.apply_time_of_day(scene), "authored time of day missing");
    auto saved = build_scene_asset(world);
    check(saved.ok(), "camera/environment save failed");
    std::ostringstream text;
    write_scene_asset(text, *saved.scene);
    check(parse_scene_asset(text.str(), world.components()).ok(), "infinite far-plane save is not valid JSON");
    auto camera = instance.by_authored_id.at("camera");
    check(world.components().patch_field(camera, "SceneCamera3D", "far", f32{.01f}), "far edit rejected prematurely");
    const auto previous = scene.camera;
    check(apply_scene3d_settings(world, scene).diagnostics.size() == 1 && near(scene.camera.position, previous.position) &&
              scene.camera.z_far == previous.z_far,
          "invalid projection partially applied");
    auto environment = instance.by_authored_id.at("environment");
    check(world.components().patch_field(environment, "SceneEnvironment3D", "samples", u32{64}), "sample edit failed");
    check(apply_scene3d_settings(world, scene, &view).diagnostics.size() == 2 && view.samples == 4,
          "invalid environment partially applied");
    camera.get_mut<SceneCamera3D>()->enabled = false;
    environment.get_mut<SceneEnvironment3D>()->enabled = false;
    check(apply_scene3d_settings(world, scene).diagnostics.empty(), "disabled settings still applied");

    kin::EcsWorld animation;
    register_builtin_editor_components(animation);
    const auto root = std::filesystem::path(KIN_TEST_SUMMIT);
    auto prefab = load_prefab_asset(root / "prefabs/survivor.kinprefab", animation.components());
    check(prefab.ok(), "authored animator prefab rejected");
    auto actor = instantiate_prefab(animation, *prefab.prefab, {}, animation.components());
    check(actor.ok() && resolve_animator3d_assets(animation, root).diagnostics.empty(), "CPU-only animator asset binding failed");
    const auto config = actor.root.get<AnimatorAsset3D>()->configuration;
    auto* animator = actor.root.get_mut<Animator3D>();
    check(config && animator->machine && animator->blend_spaces.at("ground").entries.size() == 3 &&
              animator->layers.empty() && animator->ik_constraints.size() == 3,
          "advanced animation configuration lost");
    animator->params.set_bool("airborne", true);
    const auto* transition = evaluate(*animator->machine, "ground", animator->params);
    check(transition && transition->to == "air", "shared kinanim machine transition failed");
    animator->state = "air";
    check(resolve_animator3d_assets(animation, root).diagnostics.empty() && actor.root.get<Animator3D>()->state == "air" &&
              actor.root.get<AnimatorAsset3D>()->configuration == config,
          "unchanged explicit resolution reset animation");
    check(animation.components().patch_field(actor.root, "AnimatorAsset3D", "asset",
                                             ComponentAssetRef{"missing.kinanimator"}),
          "animator ref patch failed");
    auto errors = resolve_animator3d_assets(animation, root).diagnostics;
    check(errors.size() == 1 && errors[0].find("missing.kinanimator") != std::string::npos &&
              actor.root.get<AnimatorAsset3D>()->configuration == config,
          "invalid animator replacement discarded last good data");
    Scratch scratch;
    const auto path = scratch.path / "layers.kinanimator";
    std::ofstream(path)
        << R"({"schema":"kin.animator3d/1","layers":[{"clip":"Idle","mask_joint":"spine","weight":0.4,"additive":true}],"fades":[{"seconds":0.3}]})";
    check(animation.components().patch_field(actor.root, "AnimatorAsset3D", "asset", ComponentAssetRef{path.string()}),
          "layer config ref failed");
    check(resolve_animator3d_assets(animation, root).diagnostics.empty(), "layer/fade asset rejected");
    animator = actor.root.get_mut<Animator3D>();
    check(!animator->machine && animator->layers.size() == 1 && animator->layers[0].mask_joint == "spine" &&
              animator->layers[0].additive && animator->fades.size() == 1,
          "layer/fade configuration lost");
    const auto last_good = actor.root.get<AnimatorAsset3D>()->configuration;
    std::ofstream(path) << R"({"schema":"kin.animator3d/1","layers":[{"clip":"Idle","weght":0.4}]})";
    const auto malformed = resolve_animator3d_assets(animation, root).diagnostics;
    check(malformed.size() == 1 && malformed[0].find("weght") != std::string::npos &&
              actor.root.get<AnimatorAsset3D>()->configuration == last_good,
          "malformed config did not preserve last good data or diagnose typo");
}

void gpu_scene(Renderer2D& renderer) {
    Scratch scratch;
    std::ofstream(scratch.path / "prop.kinprefab") << prefab_text;
    std::ofstream(scratch.path / "test.kinscene") << scene_text;
    std::filesystem::copy_file(std::filesystem::path(KIN_TEST_MODELS) / "parts.glb", scratch.path / "parts.glb");
    Renderer3D r3d{renderer};
    Assets3D assets{r3d, scratch.path};
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    SceneAssetRuntime runtime;
    check(runtime.load(scratch.path / "test.kinscene", scratch.path, world.components()),
          "existing runtime load failed");
    check(runtime.instantiate(world), "existing runtime instantiate failed");
    auto left = runtime.instance().prefab_instances.at("left").root;
    auto right = runtime.instance().prefab_instances.at("right").root;
    check(!left.get<ModelRenderer3D>()->model, "instantiation did hidden GPU loading");
    check(resolve_render3d_assets(world, assets).diagnostics.empty(), "3D asset binding failed");
    auto model = left.get<ModelRenderer3D>()->model;
    check(model && model == right.get<ModelRenderer3D>()->model, "prefab instances did not share Assets3D models");
    check(assets.stats().model_loads == 1, "prefab prototypes loaded or duplicate models uploaded");
    const auto base = world.raw().prefab().set(ModelRenderer3D{.asset = {"parts.glb"}});
    auto inherited_a = kin::EcsEntity{world.raw().entity().is_a(base)};
    auto inherited_b = kin::EcsEntity{world.raw().entity().is_a(base)};
    check(resolve_render3d_assets(world, assets).diagnostics.empty(), "inherited renderer binding failed");
    check(inherited_a.raw().owns<ModelRenderer3D>() && inherited_b.raw().owns<ModelRenderer3D>() &&
              !base.get<ModelRenderer3D>()->model,
          "binding mutated shared prefab base");
    inherited_a.destroy();
    inherited_b.destroy();
    base.destruct();

    WorldRenderState3D state{world};
    state.propagate_transforms();
    const auto lamp = runtime.instance().prefab_instances.at("left").by_authored_id.at("lamp");
    check(near(lamp.get<WorldTransform3D>()->position(), {-4, 2, 0}), "prefab hierarchy transform wrong");
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 2, 12}, {0, 1, 0}, 60);
    (void)scene.draw(*model, Mat4f::translation({-4, 0, 0}), colors::white, 71);
    (void)scene.draw(*model, Mat4f::translation({3, 0, 0}), colors::white, 82);
    auto view = r3d.create_view({160, 120}, {.samples = 1}).value_or({});
    const auto reference = image(renderer, r3d, view, scene);
    scene.clear();
    state.collect(scene);
    check(scene.lights.points.size() == 2, "authored lights not collected");
    scene.lights.points.clear(); // Same lighting as the direct-model reference.
    const auto authored = image(renderer, r3d, view, scene);
    check(reference.size() == authored.size(), "scene readback sizes differ");
    int colored = 0;
    for (size_t i = 0; i < reference.size(); ++i) {
        check(std::abs(int(reference[i]) - int(authored[i])) <= 1, "authored static model changed pixels");
        if (i % 4 != 3 && reference[i] > 10)
            ++colored;
    }
    check(colored > 1000 && r3d.stats().instances == 6, "blank/incomplete authored model parity");

    auto saved = scene_asset_from_world(world);
    std::ostringstream text;
    write_scene_asset(text, saved);
    auto parsed = parse_scene_asset(text.str(), world.components());
    check(parsed.ok() && parsed.scene->entities.empty() && parsed.scene->prefabs.size() == 2,
          "scene save lost prefab links");
    kin::EcsWorld copy;
    register_builtin_editor_components(copy);
    auto copied = instantiate_scene_asset(copy, *parsed.scene, scratch.path, copy.components());
    check(copied.ok(), "saved 3D scene did not instantiate");
    const auto copy_left = copied.by_authored_id.at("left");
    check(!copy_left.get<ModelRenderer3D>()->model && near(copy_left.get<Transform3D>()->position, {-4, 0, 0}),
          "save serialized GPU handles or lost overrides");

    World3D physics;
    WorldPhysicsState3D bodies{world, physics};
    bodies.sync_bodies();
    check(bool(bodies.body_of(left.raw())) && bool(bodies.body_of(right.raw())),
          "authored primitive physics bodies absent");
    check(world.components().patch_field(left, "ModelRenderer3D", "asset", ComponentAssetRef{"late.glb"}),
          "asset ref edit failed");
    const auto errors = resolve_render3d_assets(world, assets).diagnostics;
    check(errors.size() == 1 && errors[0].find("late.glb") != std::string::npos &&
              errors[0].find("ModelRenderer3D") != std::string::npos,
          "asset failure diagnostic lacks context");
    check(left.get<ModelRenderer3D>()->model == model && right.get<ModelRenderer3D>()->asset.id == "parts.glb",
          "failed replacement destroyed previous model or sibling");
    std::filesystem::copy_file(scratch.path / "parts.glb", scratch.path / "late.glb");
    check(resolve_render3d_assets(world, assets).diagnostics.empty() && left.get<ModelRenderer3D>()->model != model,
          "explicit failed-asset retry did not work");

    std::weak_ptr<const Model3D> weak = model;
    std::weak_ptr<IMeshBackend3D> mesh_weak = model->parts[0].mesh.backend();
    model.reset();
    scene.clear();
    runtime.clear(world);
    state.collect(scene);
    assets.trim();
    check(weak.expired() && mesh_weak.expired() && assets.stats().model_entries == 0,
          "scene unloading/prototypes/buckets pinned GPU assets");

    const auto packed = write_content_pack(scratch.path, scratch.path / "scene.kinpak");
    check(packed.ok, "scene content pack write failed");
    auto pack = ContentPack::open(scratch.path / "scene.kinpak");
    check(bool(pack), "scene pack open failed");
    const auto mount = scratch.path / "virtual";
    mount_content_pack(mount, pack);
    kin::EcsWorld packed_world;
    register_builtin_editor_components(packed_world);
    auto loaded = load_scene_asset(mount / "test.kinscene", packed_world.components());
    check(loaded.ok(), "packed scene load failed");
    auto packed_instance = instantiate_scene_asset(packed_world, *loaded.scene, mount, packed_world.components());
    Assets3D packed_assets{r3d, mount};
    check(packed_instance.ok() && resolve_render3d_assets(packed_world, packed_assets).diagnostics.empty(),
          "packed prefab/model dependencies failed");
    unmount_content_pack(mount);

    if (r3d.capabilities().skinning) {
        kin::EcsWorld skin_world;
        register_builtin_editor_components(skin_world);
        Assets3D skin_assets{r3d, KIN_TEST_SUMMIT};
        const auto skin_prefab = load_prefab_asset(
            std::filesystem::path(KIN_TEST_SUMMIT) / "prefabs/survivor.kinprefab", skin_world.components());
        check(skin_prefab.ok(), "Summit survivor prefab invalid");
        auto survivor = instantiate_prefab(skin_world, *skin_prefab.prefab, {}, skin_world.components());
        const auto binding_errors = resolve_render3d_assets(skin_world, skin_assets).diagnostics;
        for (const auto& error : binding_errors)
            std::cerr << error << '\n';
        check(survivor.ok() && binding_errors.empty(), "authored skinned binding failed");
        WorldRenderState3D skin_state{skin_world};
        skin_state.animate(.1f);
        auto* animator = survivor.root.get_mut<Animator3D>();
        const auto old = survivor.root.get<SkinnedMeshRenderer3D>()->model;
        check(animator->bound == old.get(), "authored animator did not bind");
        const auto good_config = survivor.root.get<AnimatorAsset3D>()->configuration;
        const auto invalid_path = scratch.path / "bad-clip.kinanimator";
        std::ofstream(invalid_path) << R"({"schema":"kin.animator3d/1","layers":[{"clip":"AbsentClip"}]})";
        check(skin_world.components().patch_field(survivor.root, "AnimatorAsset3D", "asset",
                                                  ComponentAssetRef{invalid_path.string()}),
              "bad clip ref edit failed");
        const auto invalid_clip = resolve_render3d_assets(skin_world, skin_assets).diagnostics;
        check(invalid_clip.size() == 1 && invalid_clip[0].find("AbsentClip") != std::string::npos &&
                  survivor.root.get<AnimatorAsset3D>()->configuration == good_config,
              "bad clip accepted or discarded last good config");
        check(skin_world.components().patch_field(survivor.root, "AnimatorAsset3D", "asset",
                                                  ComponentAssetRef{"animations/survivor.kinanimator"}),
              "restore animator ref failed");
        check(resolve_render3d_assets(skin_world, skin_assets).diagnostics.empty(), "restore animator binding failed");
        skin_assets.clear();
        check(resolve_render3d_assets(skin_world, skin_assets).diagnostics.empty(), "skinned explicit refresh failed");
        check(!animator->bound && animator->clip == "Idle", "skinned refresh failed to reset runtime or erased config");
        skin_state.animate(.1f);
        check(animator->bound != old.get(), "skinned animator kept stale model pointers");
        auto inherited_base = skin_world.raw()
                                  .prefab()
                                  .set(SkinnedMeshRenderer3D{.asset = {"kenney-character/survivor.glb"}})
                                  .set(Animator3D{.clip = "Idle"});
        auto inherited = kin::EcsEntity{skin_world.raw().entity().is_a(inherited_base)};
        check(resolve_render3d_assets(skin_world, skin_assets).diagnostics.empty() &&
                  inherited.get<SkinnedMeshRenderer3D>()->model && inherited.raw().owns<Animator3D>() &&
                  !inherited_base.get<SkinnedMeshRenderer3D>()->model,
              "skinned binding failed inherited copy-on-write");
        skin_world.entity("bad-static").set(ModelRenderer3D{.asset = {"kenney-character/survivor.glb"}});
        skin_world.entity("bad-skin")
            .set(SkinnedMeshRenderer3D{.asset = {(std::filesystem::path(KIN_TEST_MODELS) / "parts.glb").string()}});
        const auto incompatible = resolve_render3d_assets(skin_world, skin_assets).diagnostics;
        check(incompatible.size() == 2 && incompatible[0].find("requires") != std::string::npos &&
                  incompatible[1].find("requires") != std::string::npos,
              "incompatible static/skinned asset was accepted");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        codecs_and_shared_tools();
        authored_settings_and_animation();
        authorable_api_settings();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "cpu")
        return 0;
    bool ready = false;
    try {
        App app{};
        auto& window = app.create_window({.title = "scene3d-asset-tests", .width = 160, .height = 120, .hidden = true});
        auto backend = make_render_backend(window, false, true);
        if (!backend || backend->name() != "SDL_GPU")
            throw std::runtime_error("GPU backend required for scene asset checks");
        ready = true;
        Renderer2D renderer{std::move(backend)};
        gpu_scene(renderer);
        std::cout << "Unified 3D scenes, prefabs, assets and ownership checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        const char* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
        return ready || (required && std::string_view(required) != "0") ? 1 : 77;
    }
}
