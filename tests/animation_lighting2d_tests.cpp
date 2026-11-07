#include <cassert>
#include <kin/renderer/renderer2d.hpp>
#include <cmath>
#include <kin/ecs/lighting.hpp>
#include <kin/ecs/normal_sprite2d.hpp>
#include <kin/ecs/skeleton2d.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/build.hpp>
#include <kin/scene/scene_asset.hpp>
#include <limits>

using namespace kin;
namespace {
std::shared_ptr<const Image> pixel(Color c) {
    return std::make_shared<Image>(Image{{1, 1}, {c.r, c.g, c.b, c.a}});
}
bool near(f32 a, f32 b) {
    return std::abs(a - b) < .001f;
}
void test_lighting() {
    SurfaceMaterial2D material{.albedo = pixel(colors::white), .normal = pixel({128, 128, 255, 255})};
    SurfaceGeometry2D geometry{.dest = {9, -.5f, 1, 1}};
    SurfaceLight2D light{.height = 1, .directional = true, .direction = {0, 0}};
    auto image = shade_normal_sprite2d(material, geometry, colors::black, std::span{&light, 1});
    assert(image.valid() && image.rgba[0] > 250);
    material.normal = pixel({255, 128, 128, 255});
    light.direction = {1, 0};
    light.height = .01f;
    image = shade_normal_sprite2d(material, geometry, colors::black, std::span{&light, 1});
    assert(image.rgba[0] > 250);
    geometry.flip = Flip::X;
    image = shade_normal_sprite2d(material, geometry, colors::black, std::span{&light, 1});
    assert(image.rgba[0] < 3);
    geometry.flip = Flip::None;
    material.light_mask = 2;
    light.mask = 1;
    image = shade_normal_sprite2d(material, geometry, colors::black, std::span{&light, 1});
    assert(image.rgba[0] == 0);
    material.light_mask = -1;
    material.normal = pixel({128, 128, 255, 255});
    light = {.position = {0, 0}, .height = 10, .radius = 20, .shadows = true};
    const LightOccluder2D wall{.points = {{4, -10}, {6, -10}, {6, 10}, {4, 10}}};
    const auto open = shade_normal_sprite2d(material, geometry, colors::black, std::span{&light, 1});
    const auto shadow =
        shade_normal_sprite2d(material, geometry, colors::black, std::span{&light, 1}, std::span{&wall, 1});
    assert(open.rgba[0] > 100 && shadow.rgba[0] == 0);
    light = {.height = 1, .directional = true, .direction = {0, 0}};
    material.albedo = pixel({0, 0, 0, 96});
    material.specular_strength = 1;
    material.specular = pixel(colors::white);
    image = shade_normal_sprite2d(material, geometry, colors::black, std::span{&light, 1});
    assert(image.rgba[0] > 250 && image.rgba[3] == 96);
    material.specular = pixel(colors::black);
    image = shade_normal_sprite2d(material, geometry, colors::black, std::span{&light, 1});
    assert(image.rgba[0] == 0);
    material.shininess = std::numeric_limits<float>::quiet_NaN();
    assert(!shade_normal_sprite2d(material, geometry, colors::white, {}).valid());
    material.shininess = 32;
    material.albedo = pixel(colors::white);
    material.specular_strength = 0;
    geometry.dest = {.75f, .75f, .1f, .1f};
    light = {.position = {0, 0}, .height = 1, .radius = 1, .shape = pixel(colors::white)};
    image = shade_normal_sprite2d(material, geometry, colors::black, std::span{&light, 1});
    assert(image.rgba[0] > 100); // shaped lights fill their rotated square, including corners
}
// A light that can't be used is skipped, not the sprite: values not finite,
// no radius; a negative softness is a sharp light, as in light maps. Past
// max_surface_lights, lights are skipped too. Intensity is capped as there.
void test_light_values() {
    const SurfaceMaterial2D material{.albedo = pixel(colors::white), .normal = pixel({128, 128, 255, 255})};
    const SurfaceGeometry2D geometry{.dest = {9, -.5f, 1, 1}};
    constexpr f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const LightOccluder2D wall{.points = {{100, 100}, {101, 100}, {101, 101}, {100, 101}}};
    const std::vector<SurfaceLight2D> lights{
        {.position = {nan, 0}},
        {.height = 1, .radius = 0, .directional = true},
        {.height = nan, .directional = true},
        {.height = 1, .directional = true, .direction = {0, 0}, .shadows = true, .shadow_softness = -4}};
    auto image = shade_normal_sprite2d(material, geometry, colors::black, lights, std::span{&wall, 1});
    assert(image.valid() && image.rgba[0] > 250);
    std::vector<SurfaceLight2D> many(max_surface_lights, SurfaceLight2D{.intensity = 0});
    many.push_back({.height = 1, .directional = true, .direction = {0, 0}});
    image = shade_normal_sprite2d(material, geometry, colors::black, many);
    assert(image.valid() && image.rgba[0] == 0); // the last is past the limit
    const SurfaceMaterial2D dim{.albedo = pixel(Color::rgb(20, 20, 20)), .normal = material.normal};
    SurfaceLight2D bright{.height = 1, .intensity = 100, .directional = true, .direction = {0, 0}};
    const auto capped = shade_normal_sprite2d(dim, geometry, colors::black, std::span{&bright, 1});
    bright.intensity = max_light_intensity;
    const auto most = shade_normal_sprite2d(dim, geometry, colors::black, std::span{&bright, 1});
    bright.intensity = 1;
    const auto once = shade_normal_sprite2d(dim, geometry, colors::black, std::span{&bright, 1});
    assert(capped.rgba == most.rgba && most.rgba[0] > once.rgba[0]);
}
// One SceneLighting rule: the first enabled one is the scene's, lighting the
// world by a light map or, with forward_sprites, normal-mapped sprites (so
// never both). The gathered lights are kept while nothing changes.
void test_forward_lighting_gather() {
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    auto map = world.entities().create();
    map.set(SceneLighting{.ambient = colors::black});
    auto forward = world.entities().create();
    forward.set(SceneLighting{.ambient = Color::rgb(10, 20, 30), .forward_sprites = true});
    auto sun = world.entities().create();
    sun.set(DirectionalLight2D{.direction = {0, 0}});
    auto lighting = gather_normal_sprite_lighting2d(world.raw());
    assert(scene_lighting(world) == map.get<SceneLighting>());
    assert(lighting->lights.empty() && lighting->ambient == colors::white); // unlit: the light map lights them
    map.get_mut<SceneLighting>()->enabled = false;
    lighting = gather_normal_sprite_lighting2d(world.raw(), lighting);
    assert(lighting->lights.size() == 1 && lighting->ambient == Color::rgb(10, 20, 30));
    assert(gather_normal_sprite_lighting2d(world.raw(), lighting) == lighting);
    sun.get_mut<DirectionalLight2D>()->direction = {1, 0};
    assert(gather_normal_sprite_lighting2d(world.raw(), lighting) != lighting);
    // A light with values that can't be used costs only itself.
    auto torch = world.entities().create();
    torch.set(Transform2D{});
    torch.set(LightComponent{.shadows = true, .shadow_softness = -5});
    lighting = gather_normal_sprite_lighting2d(world.raw(), lighting);
    assert(lighting->lights.size() == 2 && lighting->lights[0].shadow_softness == 0);
}
SkeletonAsset2D rig() {
    SkeletonAsset2D a;
    a.bones = {{"upper", -1, {}, 10}, {"lower", 0, {.position = {10, 0}}, 10}};
    a.vertices = {{{0, 0}, {0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}},
                  {{10, 0}, {1, 0}, {0, 1, 0, 0}, {.5f, .5f, 0, 0}},
                  {{10, 5}, {1, 1}, {1, 0, 0, 0}, {1, 0, 0, 0}},
                  {{0, 5}, {0, 1}, {0, 0, 0, 0}, {1, 0, 0, 0}}};
    a.indices = {0, 1, 2, 0, 2, 3};
    a.attachments = {{"hand", 1, {.position = {10, 0}}}};
    a.clips["wave"] = {1, {{0, {{0, {}}, {1, {.rotation = 90}}}}}};
    return a;
}
void test_skeleton() {
    auto a = rig();
    std::string error;
    assert(validate_skeleton2d(a, error));
    SkeletonPose2D pose;
    pose.reset(a);
    auto mesh = deform_skeleton2d(a, pose);
    assert(near(mesh[1].position.x, 10));
    assert(sample_skeleton2d(a, "wave", .5f, false, pose) && near(pose.local[0].rotation, 45));
    assert(sample_skeleton2d(a, "wave", 1.5f, true, pose) && near(pose.local[0].rotation, 45));
    pose.reset(a);
    assert(solve_two_bone_ik2d(a, pose, 0, 1, {10, 10}));
    const auto hand = skeleton_attachment2d(a, pose, "hand");
    assert(hand && near(hand->tx, 10) && near(hand->ty, 10));
    assert(near(std::hypot(pose.world[1].tx, pose.world[1].ty), 10));
    pose.reset(a);
    assert(solve_two_bone_ik2d(a, pose, 0, 1, {100, 0}));
    const auto end = pose.world[1].apply({10, 0});
    assert(near(end.x, 20) && near(end.y, 0));
    assert(!solve_two_bone_ik2d(a, pose, 1, 0, {0, 0}));
    SkeletonAsset2D compensated;
    compensated.bones = {{"parent", -1, {.scale = {2, 1}}, 0},
                         {"root", 0, {.scale = {.5f, 1}}, 10},
                         {"tip", 1, {.position = {10, 0}}, 10}};
    SkeletonPose2D unsupported;
    unsupported.reset(compensated);
    assert(!solve_two_bone_ik2d(compensated, unsupported, 1, 2, {10, 10}));
    a.bones[0].parent = 1;
    assert(!validate_skeleton2d(a, error));
    const auto parsed = parse_skeleton2d(R"({"schema":"kin.skeleton2d/1","bones":[{"name":"root","length":2}],
        "vertices":[{"position":{"x":1},"influences":[{"bone":0,"weight":2}]}],"indices":[0,0,0]})",
                                         error);
    assert(parsed && parsed->vertices[0].weights[0] == 1);
    assert(!parse_skeleton2d(R"({"schema":"kin.skeleton2d/1","bones":[{"name":"bad","parent":0}]})", error));
}
void test_runtime() {
    App app{{.mode = AppMode::Headless}};
    auto& window = app.create_window({.width = 48, .height = 32, .hidden = true});
    Renderer2D renderer{make_render_backend(window, false, false)};
    if (requested_renderer() == RendererRequest::Gpu)
        assert(renderer.capabilities().shader_geometry);
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    auto config = world.entities().create();
    config.set(SceneLighting{.ambient = colors::black, .forward_sprites = true});
    auto sun = world.entities().create();
    sun.set(DirectionalLight2D{.direction = {0, 0}});
    auto surface = world.entities().create();
    surface.set(Transform2D{.pos = {4, 4}});
    // A light that can't be used is skipped (not every sprite).
    auto broken = world.entities().create();
    broken.set(Transform2D{.pos = {40, 4}});
    broken.set(LightComponent{.height = std::numeric_limits<f32>::quiet_NaN(), .mask = 0});
    NormalSpriteRenderer2D normal;
    normal.albedo = pixel({255, 0, 0, 255});
    normal.normal = pixel({128, 128, 255, 255});
    normal.size = {8, 8};
    normal.pivot = {0, 0};
    surface.set(normal);
    auto actor = world.entities().create();
    actor.set(Transform2D{.pos = {24, 4}});
    auto a = std::make_shared<SkeletonAsset2D>(rig());
    SkeletonRenderer2D skeleton;
    skeleton.skeleton = a;
    skeleton.pose.reset(*a);
    const u8 blue[] = {0, 0, 255, 255};
    skeleton.texture = renderer.create_texture_from_rgba(blue, 1, 1);
    actor.set(skeleton);
    auto attached = world.entities().create({.parent = actor});
    attached.set(Transform2D{});
    attached.set(SkeletonAttachment2D{.attachment = "hand"});
    actor.set(SkeletonAnimator2D{.animation = "wave", .loop = false});
    update_skeletons2d(world, .5f);
    assert(near(actor.get<SkeletonRenderer2D>()->pose.local[0].rotation, 45));
    assert(near(attached.get<Transform2D>()->pos.x, std::sqrt(200.0f)));
    update_skeletons2d(world, .5f);
    assert(actor.get<SkeletonAnimator2D>()->finished);
    actor.get_mut<SkeletonAnimator2D>()->playing = false;
    actor.get_mut<SkeletonRenderer2D>()->pose.reset(*a); // show rest mesh for the pixel test
    renderer.clear(colors::black);
    render_world(world, renderer);
    std::vector<u8> pixels;
    Vec2i size;
    assert(renderer.read_rgba({0, 0, 48, 32}, pixels, size));
    auto at = [&](int x, int y, int channel) {
        return pixels[(static_cast<std::size_t>(y) * size.x + x) * 4 + channel];
    };
    assert(at(6, 6, 0) > 240 && at(6, 6, 2) == 0);
#if !KIN_HAS_COMPACT_RENDERER // draws no textured meshes
    assert(at(28, 6, 2) > 240);
#endif
    const auto& cache = *surface.get<NormalSpriteRenderer2D>()->cache;
    const bool gpu = normal_lighting2d_gpu_supported(renderer.capabilities());
    assert(gpu ? cache.gpu.used : cache.shaded.size() == 1 && cache.shaded[0].texture);
    const Texture previous = gpu ? Texture{} : cache.shaded[0].texture;
    render_world(world, renderer);
    assert(gpu || cache.shaded[0].texture == previous);
    sun.get_mut<DirectionalLight2D>()->mask = 0;
    renderer.clear(colors::black);
    render_world(world, renderer);
    assert(renderer.read_rgba({0, 0, 48, 32}, pixels, size));
    assert(at(6, 6, 0) == 0);
    surface.get_mut<NormalSpriteRenderer2D>()->layer = layer_value(RenderLayer::UI);
    renderer.clear(colors::black);
    render_world(world, renderer);
    assert(renderer.read_rgba({0, 0, 48, 32}, pixels, size) && at(6, 6, 0) > 240); // UI stays unlit
    assert(!renderer.draw_textured_geometry(skeleton.texture, {}, {}));
    if (renderer.capabilities().linear_color) {
        assert(renderer.set_color_space(ColorSpace::Linear));
        const u8 white[] = {255, 255, 255, 255};
        const auto texture = renderer.create_texture_from_rgba(white, 1, 1);
        const Color tint{128, 64, 32, 255};
        const std::array<ShaderVertex, 4> vertices{
            {{{1, 1}, {0, 0}, tint}, {{9, 1}, {1, 0}, tint}, {{9, 9}, {1, 1}, tint}, {{1, 9}, {0, 1}, tint}}};
        const std::array<u32, 6> indices{0, 1, 2, 0, 2, 3};
        renderer.clear(colors::black);
        assert(renderer.draw_textured_geometry(texture, vertices, indices));
        assert(renderer.read_rgba({0, 0, 48, 32}, pixels, size));
        assert(std::abs(at(4, 4, 0) - 128) <= 2 && std::abs(at(4, 4, 1) - 64) <= 2 && std::abs(at(4, 4, 2) - 32) <= 2);
    }
}
// Prefab instances inherit their prefab's renderers: each is drawn (where
// it is), and normal-mapped ones keep their shaded images per instance, so
// they are not shaded again each frame.
void test_prefab_instances() {
    App app{{.mode = AppMode::Headless}};
    auto& window = app.create_window({.width = 48, .height = 32, .hidden = true});
    Renderer2D renderer{make_render_backend(window, false, false)};
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    world.entities().create().set(SceneLighting{.ambient = colors::black, .forward_sprites = true});
    world.entities().create().set(DirectionalLight2D{.direction = {0, 0}});
    NormalSpriteRenderer2D sprite;
    sprite.albedo = pixel({255, 0, 0, 255});
    sprite.size = {8, 8};
    sprite.pivot = {0, 0};
    flecs::entity tile = world.raw().prefab("Tile");
    tile.set(sprite);
    flecs::entity box = world.raw().prefab("Box");
    box.set(RectRenderer{.size = {8, 8}, .color = Color::rgb(0, 255, 0)});
    std::vector<flecs::entity> tiles;
    for (const f32 x : {2.0f, 20.0f}) {
        tiles.push_back(world.raw().entity().is_a(tile).set(Transform2D{.pos = {x, 2}}));
        world.raw().entity().is_a(box).set(Transform2D{.pos = {x + 4, 22}});
    }
    assert(!tiles[0].owns<NormalSpriteRenderer2D>());
    std::vector<u8> pixels;
    Vec2i size;
    auto at = [&](int x, int y, int channel) {
        return pixels[(static_cast<std::size_t>(y) * size.x + x) * 4 + channel];
    };
    const auto& cache = *tiles[0].get<NormalSpriteRenderer2D>()->cache;
    std::vector<Texture> first;
    for (int frame = 0; frame < 3; ++frame) {
        renderer.clear(colors::black);
        render_world(world, renderer);
        assert(renderer.read_rgba({0, 0, 48, 32}, pixels, size));
        assert(at(4, 4, 0) > 240 && at(22, 4, 0) > 240);   // both tiles
        assert(at(6, 24, 1) > 240 && at(24, 24, 1) > 240); // both boxes
        if (normal_lighting2d_gpu_supported(renderer.capabilities())) {
            assert(cache.gpu.used);
            continue;
        }
        assert(cache.shaded.size() == 2);
        if (frame == 0) {
            first = {cache.shaded[0].texture, cache.shaded[1].texture};
        }
        assert(cache.shaded[0].texture == first[0] && cache.shaded[1].texture == first[1]);
    }
}
} // namespace
int main() {
    test_lighting();
    test_light_values();
    test_forward_lighting_gather();
    test_skeleton();
    test_runtime();
    test_prefab_instances();
}
