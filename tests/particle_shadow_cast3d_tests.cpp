#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <kin/assets/image.hpp>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace kin;
namespace {
constexpr Rectf screen{0, 0, 160, 120};
void check(bool ok, const char* what) {
    if (!ok)
        throw std::runtime_error(what);
}
struct Picture {
    std::vector<u8> rgba;
    Color at(Vec2f p) const {
        const auto i = (std::size_t(int(p.y)) * 160 + int(p.x)) * 4;
        return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
    }
};
struct Fixture {
    Renderer2D& renderer;
    Renderer3D r3d;
    Mesh3D wall;
    Texture white, transparent, split;
    explicit Fixture(Renderer2D& r) : renderer(r), r3d(r) {
        wall = r3d.create_mesh(meshes::box({12, 10, .1f}));
        const u8 w[]{255, 255, 255, 255}, t[]{255, 255, 255, 0}, s[]{255, 255, 255, 0, 255, 255, 255, 255};
        white = renderer.create_texture_from_rgba(w, {1, 1});
        transparent = renderer.create_texture_from_rgba(t, {1, 1});
        split = renderer.create_texture_from_rgba(s, {2, 1});
        renderer.set_scale_mode(split, ScaleMode::Nearest);
    }
    Scene3D scene(int kind, ParticleLook3D look, ParticleSprite3D sprite, bool offscreen = false, bool submit = true) {
        Scene3D s;
        s.camera = Camera3D::look_at({0, 0, 0}, {0, 0, -1}, 60);
        s.lights.sun.color = {};
        s.lights.ambient = {.025f, .025f, .025f};
        s.lights.shadow_area = {{-12, -5, -9}, {6, 5, -1}};
        s.lights.shadow_resolution = 512;
        s.lights.shadow_distance = 12;
        s.lights.cache_shadows = true;
        s.lights.local_shadow_resolution = 512;
        if (kind < 2) {
            s.lights.sun = {.direction = normalize(Vec3f{-1, 0, 2}), .color = {1.3f, 1.3f, 1.3f}};
            s.lights.shadow_cascades = kind == 1 ? 4 : 1;
        } else if (kind == 2) {
            s.lights.fill = {.direction = normalize(Vec3f{-1, 0, 2}), .color = {1.3f, 1.3f, 1.3f}};
            s.lights.fill_shadow = {.enabled = true, .cascades = 2, .resolution = 512};
        } else if (kind == 3) {
            SpotLightSource3D l;
            l.position = {offscreen ? -10.0f : -2.0f, 0, 0};
            l.direction = normalize(Vec3f{-l.position.x, 0, -6});
            l.color = offscreen ? LinearRgb{150, 150, 150} : LinearRgb{30, 30, 30};
            l.radius = offscreen ? 20 : 12;
            l.inner_angle = 35;
            l.outer_angle = 60;
            l.shadow.enabled = true;
            s.lights.spots.push_back(l);
        } else {
            PointLightSource3D l;
            l.position = {offscreen ? -10.0f : -2.0f, 0, 0};
            l.color = offscreen ? LinearRgb{150, 150, 150} : LinearRgb{30, 30, 30};
            l.radius = offscreen ? 20 : 12;
            l.shadow.enabled = true;
            s.lights.points.push_back(l);
        }
        Material3D surface;
        surface.color = colors::white;
        surface.roughness = 1;
        surface.casts_shadow = false;
        s.draw(wall, Mat4f::translation({0, 0, -6}), surface);
        if (submit)
            s.draw_particles(look, std::span{&sprite, 1});
        return s;
    }
    Picture shoot(View3D& view, const Scene3D& scene) {
        renderer.clear(colors::black);
        check(r3d.render(view, scene), "particle shadow render failed");
        renderer.draw_texture(view.texture(), screen);
        Picture p;
        Vec2i size;
        check(renderer.read_rgba(screen, p.rgba, size) && size == Vec2i{160, 120}, "readback failed");
        renderer.present();
        return p;
    }
    Color at(const Picture& p, Vec3f world) {
        const auto camera = Camera3D::look_at({0, 0, 0}, {0, 0, -1}, 60);
        const auto xy = camera.project(world, screen);
        check(bool(xy), "sample projection failed");
        return p.at(xy->pos);
    }
};
int diff(Color a, Color b) {
    return std::max({std::abs(int(a.r) - b.r), std::abs(int(a.g) - b.g), std::abs(int(a.b) - b.b)});
}
ParticleSprite3D sprite() {
    ParticleSprite3D p;
    p.position = {0, 0, -4};
    p.size = 1;
    p.colour = {0, 0, 0};
    p.alpha = 1;
    return p;
}
ParticleLook3D look(const Texture& t) {
    ParticleLook3D l;
    l.texture = t;
    l.blend = ParticleBlend3D::Additive;
    l.casts_shadow = true;
    return l;
}
void interactions(Fixture& f) {
    // Separate renderers keep the oracle from resetting the cache under test.
    Fixture oracle(f.renderer);
    // Paired views have the same temporal history. Redrawing every map is the
    // oracle for cached moving silhouettes, including removal and flipbooks.
    for (int kind = 0; kind < 5; ++kind)
        for (bool hdr : {false, true})
            for (u32 samples : {1u, 4u}) {
                auto cached = f.r3d.create_view({160, 120}, {.samples = samples, .hdr = hdr});
                auto fresh = oracle.r3d.create_view({160, 120}, {.samples = samples, .hdr = hdr});
                int worst = 0;
                for (int frame = 0; frame < 24; ++frame) {
                    auto p = sprite();
                    p.position.y = frame < 8 ? -.6f + .15f * frame : .45f;
                    p.alpha = frame == 9 ? .2f : 1;
                    p.frame = frame < 12 || frame >= 16 ? 1 : 0;
                    auto l = look(f.split);
                    l.atlas_columns = l.atlas_frames = 2;
                    l.receives_shadow = true;
                    l.lit = 1;
                    l.soft = true;
                    l.soft_distance = 3;
                    l.casts_shadow = frame < 18;
                    l.shadow_cutoff = frame == 10 ? 1 : .5f;
                    auto scene = f.scene(kind, l, p);
                    scene.fog = {.enabled = true, .density = .025f, .colour = {.2f, .3f, .4f}};
                    scene.taa = {.enabled = samples == 1, .blend = .98f};
                    const auto actual = f.shoot(cached, scene);
                    auto reference = oracle.scene(kind, l, p);
                    reference.lights = scene.lights;
                    reference.lights.cache_shadows = false;
                    reference.fog = scene.fog;
                    reference.taa = scene.taa;
                    const auto expected = oracle.shoot(fresh, reference);
                    for (std::size_t i = 0; i < actual.rgba.size(); ++i)
                        worst = std::max(worst, std::abs(int(actual.rgba[i]) - expected.rgba[i]));
                }
                std::cerr << "cast interactions kind " << kind << " hdr " << hdr << " samples " << samples << ": max "
                          << worst << "\n";
                check(worst <= 1, "cached moving particle shadow differs from fresh maps");
            }
    // Receiving and casting on the same billboard must not darken its own face.
    auto view = f.r3d.create_view({160, 120});
    for (int kind = 0; kind < 5; ++kind) {
        auto p = sprite();
        p.colour = {1, 1, 1};
        auto l = look(f.white);
        l.blend = ParticleBlend3D::Alpha;
        l.lit = 1;
        l.receives_shadow = true;
        l.casts_shadow = false;
        const auto plain = f.shoot(view, f.scene(kind, l, p));
        l.casts_shadow = true;
        const auto both = f.shoot(view, f.scene(kind, l, p));
        check(diff(f.at(plain, p.position), f.at(both, p.position)) <= 2, "billboard shadows its own face");
    }
    // Weather uses the same caster stream, including particles culled from
    // colour rendering. Advancing its clock must update the cached shadow map.
    auto cached = f.r3d.create_view({160, 120});
    auto fresh = oracle.r3d.create_view({160, 120});
    std::size_t affected = 0;
    for (int frame = 0; frame < 8; ++frame) {
        auto scene = f.scene(0, look(f.white), sprite(), false, false);
        auto& layer = scene.weather.snow;
        layer.intensity = 1;
        layer.max_particles = 64;
        layer.box = {12, 6, 10};
        layer.above = .5f;
        layer.size_min = layer.size_max = .8f;
        layer.alpha = 1;
        layer.edge_fade = 0;
        layer.colour = {};
        layer.look = look(f.white);
        layer.look.shadow_cutoff = .1f;
        scene.weather.time = frame * .15;
        const auto actual = f.shoot(cached, scene);
        check(f.r3d.debug_stats().weather_particles > 0 && f.r3d.debug_stats().particle_shadow_submitted > 0 &&
                  f.r3d.debug_stats().particle_shadow_draws > 0,
              "weather omitted from shadow pass");
        auto reference = oracle.scene(0, look(f.white), sprite(), false, false);
        reference.lights = scene.lights;
        reference.lights.cache_shadows = false;
        reference.weather = scene.weather;
        const auto expected = oracle.shoot(fresh, reference);
        check(actual.rgba == expected.rgba, "weather clock left cached particle shadows");
        reference.weather.snow.look.casts_shadow = false;
        const auto plain = oracle.shoot(fresh, reference);
        for (std::size_t i = 0; i < actual.rgba.size(); i += 4)
            affected += int(plain.rgba[i]) - actual.rgba[i] > 20;
    }
    check(affected > 50, "weather casts no visible shadows");
    std::cerr << "weather shadow pixels: " << affected << "\n";
}

void receivers(Fixture& f) {
    // A moving particle can shade a different, stationary lit particle. Its
    // temporal output must follow a spatial reference, including removal.
    for (int kind = 0; kind < 5; ++kind) {
        auto temporal = f.r3d.create_view({160, 120}, {.hdr = true});
        auto spatial = f.r3d.create_view({160, 120}, {.hdr = true});
        int worst = 0;
        for (int frame = 0; frame < 24; ++frame) {
            auto caster = sprite();
            caster.position.y = -.6f + .1f * std::min(frame, 12);
            auto cast = look(f.white);
            cast.casts_shadow = frame < 18;
            auto scene = f.scene(kind, cast, caster);
            auto receiver = sprite();
            receiver.position = {1, 0, -5.8f};
            receiver.size = 3.5f;
            receiver.colour = {1, 1, 1};
            auto receive = look(f.white);
            receive.casts_shadow = false;
            receive.receives_shadow = true;
            receive.lit = 1;
            receive.blend = ParticleBlend3D::Alpha;
            receive.soft = true;
            receive.soft_distance = .3f;
            scene.draw_particles(receive, std::span{&receiver, 1});
            scene.fog = {.enabled = true, .density = .02f, .colour = {.2f, .3f, .4f}};
            scene.taa = {.enabled = true, .blend = 0};
            const auto expected = f.shoot(spatial, scene);
            scene.taa.blend = .98f;
            const auto actual = f.shoot(temporal, scene);
            int frame_error = 0;
            for (int y = 46; y < 74; ++y)
                for (int x = 84; x < 108; ++x)
                    frame_error =
                        std::max(frame_error, diff(actual.at({f32(x), f32(y)}), expected.at({f32(x), f32(y)})));
            // Once motion stops, stable edges intentionally accumulate history.
            if (frame <= 12 || frame == 18)
                worst = std::max(worst, frame_error);
        }
        std::cerr << "particle on particle TAA kind " << kind << ": max " << worst << "\n";
        check(worst <= 2, "moving particle shadow trails on particle receiver");
    }
}

void run(Fixture& f) {
    auto view = f.r3d.create_view({160, 120});
    check(!ParticleLook3D{}.casts_shadow, "casting is not opt-in");
    auto p = sprite();
    auto l = look(f.white);
    for (int kind = 0; kind < 5; ++kind) {
        auto off = l;
        off.casts_shadow = false;
        const auto plain = f.shoot(view, f.scene(kind, off, p));
        if (kind == 0)
            check(!f.r3d.debug_stats().particle_shadow_bytes, "disabled casting allocated a buffer");
        const auto shadow = f.shoot(view, f.scene(kind, l, p));
        const auto first = f.r3d.debug_stats();
        check(first.particle_shadow_submitted == 1 && first.particle_shadow_draws > 0, "caster not submitted to maps");
        const float scale = kind >= 3 ? 1.5f : 1;
        const Vec3f sample{1 + .25f * scale, 0, -5.95f};
        const auto before = f.at(plain, sample), after = f.at(shadow, sample);
        std::cerr << "cast kind " << kind << " sample " << int(before.r) << " -> " << int(after.r) << "\n";
        check(int(before.r) - after.r > 80, "particle did not shade receiver");
        if (const char* dump = std::getenv("KIN_TEST_DUMP"))
            check(save_image(Image{{160, 120}, shadow.rgba},
                             std::filesystem::path(dump) / ("particle_cast_" + std::to_string(kind) + ".png")),
                  "shadow screenshot failed");
        // Same caster inputs must reuse each light's map, then removal clears it.
        for (int frame = 0; frame < 3; ++frame)
            f.shoot(view, f.scene(kind, l, p));
        check(!f.r3d.debug_stats().particle_shadow_draws, "static particle broke shadow cache");
        const auto removed = f.shoot(view, f.scene(kind, off, p));
        check(diff(f.at(plain, sample), f.at(removed, sample)) <= 1, "removed caster left a stale shadow");
        // Intrinsic alpha: transparent RGB, half texture, disc and ring.
        auto mask = l;
        mask.texture = f.transparent;
        check(f.shoot(view, f.scene(kind, mask, p)).rgba == plain.rgba, "transparent texel casts a rectangle");
        mask.texture = f.split;
        const auto half = f.shoot(view, f.scene(kind, mask, p));
        check(int(f.at(plain, sample).r) - f.at(half, sample).r > 60, "opaque texture half failed to cast");
        const Vec3f hole{1 - .25f * scale, 0, -5.95f};
        check(diff(f.at(plain, hole), f.at(half, hole)) <= 2, "transparent texture half casts");
        mask.texture = {};
        mask.softness = 0;
        const auto disc = f.shoot(view, f.scene(kind, mask, p));
        const Vec3f corner{1 + .45f * scale, .45f * scale, -5.95f};
        check(diff(f.at(plain, corner), f.at(disc, corner)) <= 3, "disc shadow uses rectangular coverage");
        mask.ring = 1;
        const auto ring = f.shoot(view, f.scene(kind, mask, p));
        check(diff(f.at(plain, {1, 0, -5.95f}), f.at(ring, {1, 0, -5.95f})) <= 3, "ring hole casts");
        const Vec3f edge{1 + .375f * scale, 0, -5.95f};
        check(int(f.at(plain, edge).r) - f.at(ring, edge).r > 60, "ring silhouette missing");
        // Alpha, cutoff and atlas changes invalidate a previously cached map.
        mask = l;
        p.alpha = .4f;
        check(f.shoot(view, f.scene(kind, mask, p)).rgba == plain.rgba, "alpha below cutoff casts");
        mask.shadow_cutoff = .2f;
        const auto faint = f.shoot(view, f.scene(kind, mask, p));
        check(int(f.at(plain, sample).r) - f.at(faint, sample).r > 80, "cutoff edit did not invalidate map");
        p.alpha = 1;
        mask = l;
        mask.texture = f.split;
        mask.atlas_columns = 2;
        mask.atlas_frames = 2;
        p.frame = 0;
        const auto frame0 = f.shoot(view, f.scene(kind, mask, p));
        p.frame = 1;
        const auto frame1 = f.shoot(view, f.scene(kind, mask, p));
        check(diff(f.at(plain, sample), f.at(frame0, sample)) <= 2 &&
                  int(f.at(plain, sample).r) - f.at(frame1, sample).r > 80,
              "flipbook frame did not update cast coverage");
        p.frame = 0;
        // A caster outside the colour frustum can still shade a visible receiver.
        auto outside = p;
        outside.position.x = -4.5f;
        const auto clean = f.shoot(view, f.scene(kind, off, outside, true));
        const auto cast = f.shoot(view, f.scene(kind, l, outside, true));
        check(f.r3d.debug_stats().particles_drawn == 0 && f.r3d.debug_stats().particle_shadow_submitted == 1,
              "off-camera caster lost or counted as visible");
        const Vec3f target{kind >= 3 ? -1.75f : -3.5f, 0, -5.95f};
        std::cerr << "offscreen kind " << kind << " " << int(f.at(clean, target).r) << " -> "
                  << int(f.at(cast, target).r) << "\n";
        check(int(f.at(clean, target).r) - f.at(cast, target).r > 60, "off-camera particle does not cast");
    }
    // Alpha updates on the same texture handle must invalidate cached masks.
    auto dynamic = f.renderer.create_texture_from_rgba(std::array<u8, 4>{255, 255, 255, 0}.data(), {1, 1});
    auto mask = look(dynamic);
    auto off = mask;
    off.casts_shadow = false;
    const auto plain = f.shoot(view, f.scene(0, off, p));
    for (int i = 0; i < 3; ++i)
        f.shoot(view, f.scene(0, mask, p));
    check(!f.r3d.debug_stats().particle_shadow_draws, "texture test did not warm the shadow cache");
    const u8 opaque[]{255, 255, 255, 255};
    check(f.renderer.update_texture(dynamic, {}, {1, 1}, opaque), "texture update failed");
    const auto changed = f.shoot(view, f.scene(0, mask, p));
    check(int(f.at(plain, {1.25f, 0, -5.95f}).r) - f.at(changed, {1.25f, 0, -5.95f}).r > 80,
          "texture revision left cached shadow");
    // The caster stream has its own hard cap and does not depend on camera culling.
    std::vector<ParticleSprite3D> many((1u << 20) + 3, p);
    for (auto& q : many)
        q.position = {100, 100, 100};
    auto scene = f.scene(0, off, p, false, false);
    scene.draw_particles(l, many);
    f.shoot(view, scene);
    check(f.r3d.debug_stats().particle_shadow_submitted == (1u << 20) &&
              f.r3d.debug_stats().particle_shadow_dropped == 3,
          "caster cap not enforced");
    many.clear();
    scene = f.scene(0, off, p, false, false);
    for (int i = 0; i < 121; ++i)
        f.shoot(view, scene);
    check(!f.r3d.debug_stats().particle_shadow_bytes, "unused caster buffer retained indefinitely");
}
} // namespace
int main() {
    bool gpu = false;
    try {
        App app{};
        auto& window = app.create_window({.title = "particle shadows", .width = 160, .height = 120, .hidden = true});
        auto backend = make_render_backend(window, false, true);
        check(backend && backend->name() == "SDL_GPU", "GPU backend required");
        gpu = true;
        Renderer2D renderer(std::move(backend));
        Fixture f(renderer);
        if (!f.r3d.capabilities().particle_shadows) {
            std::cerr << "Particle shadow shaders unavailable\n";
            const char* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
            return required && std::string_view(required) != "0" ? 1 : 77;
        }
        run(f);
        interactions(f);
        receivers(f);
        std::cout << "Particle casting, silhouettes, cache invalidation, off-camera casters, budgets and idle release "
                     "passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        const char* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
        return gpu || (required && std::string_view(required) != "0") ? 1 : 77;
    }
}
