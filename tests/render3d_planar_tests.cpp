// Mirrors and portals (docs/rendering_3d.md): Camera3D::clip_plane, mirror_view, portal_view and kin's planar
// surfaces. The camera arithmetic runs everywhere; the pictures need a GPU (77: skipped without one).
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <kin/ecs/render3d.hpp>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/shader_compiler.hpp>
#include <kin/scene/planar_views3d.hpp>
#include <string_view>
#include <vector>

using namespace kin;

namespace {

constexpr Vec2i extent{160, 120};
constexpr Rectf rect{0, 0, 160, 120};
constexpr f32 aspect = 160.0f / 120.0f;

constexpr f32 pi_v = 3.14159265358979f;
bool near(f32 a, f32 b, f32 eps = 1e-4f) {
    return std::abs(a - b) <= eps;
}
Vec3f ndc(const Mat4f& m, Vec3f p) {
    return m.apply_projective(p);
}
bool same_spot(Vec3f a, Vec3f b) {
    return near(a.x, b.x, 2e-4f) && near(a.y, b.y, 2e-4f);
}

// --- The camera arithmetic ---

void test_plane_through() {
    const Vec4f p = plane_through({0, 2, 0}, {0, 5, 0});
    assert(near(p.x, 0) && near(p.y, 1) && near(p.z, 0) && near(p.w, -2));
    Camera3D camera;
    assert(!camera.clipped());
    camera.clip_plane = p;
    assert(camera.clipped());
}

void test_mirror_view() {
    const Camera3D eye = Camera3D::look_at({1, 3, 6}, {0, 0, 0});
    const Vec4f floor = plane_through({0, 0, 0}, {0, 1, 0});
    const PlanarView3D mirror = mirror_view(eye, floor, aspect);
    // Below the floor as far as the eye is above, looking up through it, and keeping only what is above it.
    assert(near(mirror.camera.position.x, 1) && near(mirror.camera.position.y, -3) &&
           near(mirror.camera.position.z, 6));
    assert(near(mirror.camera.forward().y, -eye.forward().y) && near(mirror.camera.forward().x, eye.forward().x));
    assert(near(mirror.camera.clip_plane.y, 1) && near(mirror.camera.clip_plane.w, -0.005f));
    // A point of the mirror shows in the reflection's picture where the point it reflects does: Q above the floor, its
    // image Q' below, and P where the eye's line to Q' meets the floor.
    const Vec3f q{0.5f, 1.2f, -1.0f};
    const Vec3f q_image{q.x, -q.y, q.z};
    const Vec3f d = q_image - eye.position;
    const Vec3f p = eye.position + d * (-eye.position.y / d.y);
    assert(near(p.y, 0));
    assert(same_spot(ndc(mirror.projector, p), ndc(mirror.camera.view_projection(aspect), q)));
    // The camera's side is the front, whichever way the plane is given.
    const PlanarView3D flipped = mirror_view(eye, Vec4f{0, -1, 0, 0}, aspect);
    assert(near(flipped.camera.position.y, -3) && near(flipped.camera.clip_plane.y, 1));
    // A camera below a ceiling mirror.
    const Camera3D low = Camera3D::look_at({0, 1, 4}, {0, 2, 0});
    const PlanarView3D ceiling = mirror_view(low, plane_through({0, 3, 0}, {0, 1, 0}), aspect);
    assert(near(ceiling.camera.position.y, 5) && near(ceiling.camera.clip_plane.y, -1) &&
           near(ceiling.camera.clip_plane.w, 3 - 0.005f));
}

void test_portal_view() {
    const Camera3D eye = Camera3D::look_at({0.5f, 1, 5}, {0, 0.8f, 0});
    const PortalPose3D entry{{0, 1, 0}, {}};
    const PortalPose3D exit{{20, 1, -4}, Quatf::axis_angle({0, 1, 0}, 90)};
    // In at the entry's front (+Z), out of the exit's front (its +Z: world +X, turned 90 degrees).
    const Vec3f in_front = portal_point(entry, exit, {0, 1, 1});
    assert(near(in_front.x, 19) && near(in_front.y, 1) && near(in_front.z, -4));
    const Vec3f going = portal_rotation(entry, exit).rotate({0, 0, -1});
    assert(near(going.x, 1) && near(going.y, 0) && near(going.z, 0));
    const PlanarView3D through = portal_view(eye, entry, exit, aspect);
    // Behind the exit, as far as the eye is in front of the entry (its 0.5 to the side turned with the portals),
    // keeping what is in front of the exit.
    assert(near(through.camera.position.x, 15, 1e-3f) && near(through.camera.position.z, -3.5f, 1e-3f));
    assert(near(through.camera.clip_plane.x, 1) && near(through.camera.clip_plane.w, -20 - 0.005f, 1e-4f));
    // The entry's points are read where the eye sees them (the same projection, through the portals)...
    const Vec3f p{0.3f, 1.4f, 0};
    assert(same_spot(ndc(through.projector, p), ndc(eye.view_projection(aspect), p)));
    // ...and the picture there shows what lies beyond the exit along that line.
    const Vec3f at_exit = portal_point(entry, exit, p);
    const Vec3f beyond = through.camera.position + (at_exit - through.camera.position) * 3.0f;
    assert(same_spot(ndc(through.projector, p), ndc(through.camera.view_projection(aspect), beyond)));
}

void test_crossing_and_rays() {
    const PortalPose3D entry{{0, 0, 0}, {}, {2, 2}}, exit{{10, 0, 0}, {}, {2, 2}};
    const auto t = portal_crossing(entry, {0, 0, 1}, {0, 0, -1});
    assert(t && near(*t, 0.5f));
    assert(!portal_crossing(entry, {3, 0, 1}, {3, 0, -1}));         // beside the opening
    assert(!portal_crossing(entry, {0, 0, -1}, {0, 0, 1}));         // from behind
    assert(!portal_crossing(entry, {0, 0, 2}, {0, 0, 1}));          // not through
    assert(portal_crossing({{}, {}, {}}, {50, 9, 1}, {50, 9, -1})); // no size: the whole plane
    const auto through = ray_through_portal({{0.5f, 0, 4}, {0, 0, -1}}, entry, exit);
    assert(through && near(through->distance, 4));
    assert(near(through->ray.origin.x, 9.5f) && near(through->ray.origin.z, 0)); // turned half round
    assert(near(through->ray.direction.z, 1));                                   // out of the exit's front
    assert(!ray_through_portal({{5, 0, 4}, {0, 0, -1}}, entry, exit));
    assert(!ray_through_portal({{0, 0, -4}, {0, 0, 1}}, entry, exit));
    // Straddling: closer to the plane than the radius and the views' clip offset (which cuts that far in front of it).
    const auto image = portal_straddle(entry, exit, {0.2f, 0, 0.3f}, 0.5f);
    assert(image && same_spot(image->apply({0.2f, 0, 0.3f}), portal_point(entry, exit, {0.2f, 0, 0.3f})));
    assert(portal_straddle(entry, exit, {0, 0, 0.503f}, 0.5f));
    assert(!portal_straddle(entry, exit, {0, 0, 0.503f}, 0.5f, 0.0f));
    assert(!portal_straddle(entry, exit, {0, 0, 0.6f}, 0.5f));
    assert(!portal_straddle(entry, exit, {1.2f, 0, 0}, 0.5f)); // beside the opening
    assert(!portal_straddle(entry, exit, {0, 0, 0}, 5.0f));    // the wall round it, the floor: too large to go through
    assert(portal_straddle({{}, {}, {}}, exit, {0, 0, 0}, 5.0f)); // no size: anything
}

void test_chain() {
    // Two portals facing each other: each view further back behind the exit, by the gap between them.
    const PortalPose3D a{{-5, 0, 0}, Quatf::axis_angle({0, 1, 0}, 90)}, b{{5, 0, 0}, Quatf::axis_angle({0, 1, 0}, -90)};
    const Camera3D eye = Camera3D::look_at({-3, 0, 0}, {-5, 0, 0});
    const auto chain = portal_chain(eye, a, b, aspect, 3);
    assert(chain.size() == 3);
    assert(near(chain[0].camera.position.x, 7, 1e-3f) && near(chain[1].camera.position.x, 17, 1e-3f) &&
           near(chain[2].camera.position.x, 27, 1e-3f));
    const PlanarView3D second = portal_view(chain[0].camera, a, b, aspect);
    for (int i = 0; i < 16; ++i)
        assert(near(chain[1].projector.m[i], second.projector.m[i], 1e-4f));
}

void test_on_screen() {
    const Camera3D eye = Camera3D::look_at({0, 0, 5}, {0, 0, 0});
    const auto r = planar_on_screen(eye, aspect, {}, {}, {2, 2});
    assert(r && near(r->x + r->w / 2, 0.5f) && near(r->y + r->h / 2, 0.5f));
    const f32 half = 1.0f / (5.0f * std::tan(pi_v / 6) * aspect); // the quad's half width, in clip space
    assert(near(r->w, half, 1e-4f));
    assert(!planar_on_screen(Camera3D::look_at({0, 0, -5}, {0, 0, 0}), aspect, {}, {}, {2, 2})); // from behind
    assert(!planar_on_screen(eye, aspect, {100, 0, 0}, {}, {2, 2}));                             // out of view
    const auto all = planar_on_screen(Camera3D::look_at({0, 0, 1}, {0, 0, -1}), aspect, {}, {}, {100, 100});
    assert(all && near(all->x, 0) && near(all->y, 0) && near(all->w, 1) && near(all->h, 1));
    // Along a wall, beside the camera: cut where it goes behind, on the left half.
    const auto side = planar_on_screen(Camera3D::look_at({0, 0, 0.5f}, {1, 0, 0.5f}), aspect, {}, {}, {100, 100});
    assert(side && side->x + side->w <= 0.5f + 1e-3f);
}

void test_surface_transform() {
    const PortalPose3D entry{{0, 0, 0}, Quatf::axis_angle({0, 1, 0}, 90), {2, 3}};
    const Mat4f m = portal_surface_transform(entry, 0.4f);
    const Vec3f front = m.apply({0.5f, 0.5f, 0.5f}), back = m.apply({0.5f, 0.5f, -0.5f});
    // Its front face in the opening (the portal faces +X), its back 0.4 behind.
    assert(near(front.x, 0) && near(front.y, 1.5f) && near(std::abs(front.z), 1));
    assert(near(back.x, -0.4f) && near(back.y, 1.5f));
}

// A window of the frame shows what the whole frame shows there: a point at (u, v) of the frame is at
// ((u - x) / w, (v - y) / h) of the window, the window's aspect the frame's times w / h. A mirror's cut is turned left
// to right, and its projector still reads where the mirror's point shows.
void test_window() {
    const Camera3D eye = Camera3D::look_at({1, 2, 6}, {0, 0.5f, 0});
    const Rectf window{0.55f, 0.2f, 0.3f, 0.25f};
    Camera3D cut = eye;
    cut.window = window;
    const f32 cut_aspect = aspect * window.w / window.h;
    assert(near(cut.frame_aspect(cut_aspect), aspect));
    const auto screen = [](const Mat4f& vp, Vec3f p) {
        const Vec3f c = vp.apply_projective(p);
        return Vec2f{0.5f + 0.5f * c.x, 0.5f - 0.5f * c.y};
    };
    for (const Vec3f p : {Vec3f{0.8f, 0.9f, 0}, Vec3f{-0.3f, 0.2f, 1}, Vec3f{0.5f, 1.4f, -2}}) {
        const Vec2f whole = screen(eye.view_projection(aspect), p), part = screen(cut.view_projection(cut_aspect), p);
        assert(near(part.x, (whole.x - window.x) / window.w, 1e-3f) &&
               near(part.y, (whole.y - window.y) / window.h, 1e-3f));
    }
    // A mirror's picture cut to where the mirror shows: its projector maps a point of the mirror into the window as the
    // uncut projector maps it into the frame, turned left to right.
    const PlanarView3D whole = mirror_view(eye, plane_through({0, 0, 0}, {0, 1, 0}), aspect);
    PlanarView3D part = whole;
    crop_planar_view(part, window, cut_aspect, true);
    const Vec3f on_mirror{0.4f, 0, 1.5f};
    const Vec2f a = screen(whole.projector, on_mirror), b = screen(part.projector, on_mirror);
    assert(near(b.x, (a.x - (1 - window.x - window.w)) / window.w, 1e-3f) &&
           near(b.y, (a.y - window.y) / window.h, 1e-3f));
    // In the uncut picture, the mirror's point is where the eye sees it, turned left to right.
    const Vec2f seen = screen(eye.view_projection(aspect), on_mirror);
    assert(near(a.x, 1 - seen.x, 1e-3f) && near(a.y, seen.y, 1e-3f));
}

void test_params() {
    Mat4f m;
    for (int i = 0; i < 16; ++i)
        m.m[i] = f32(i + 1);
    Material3D material;
    planar_view_params(material.shader_params, m, 0.1f, 0.75f);
    for (int i = 0; i < 16; ++i)
        assert(material.shader_params[i] == f32(i + 1));
    assert(material.shader_params[16] == 0.1f && material.shader_params[17] == 0.75f);
    assert(material.shader_params[18] == 1.0f && material.shader_params[19] == 1.0f); // the whole picture
    std::array<f32, 8> short_params{};
    planar_view_params(short_params, m); // too short: left alone
    assert(short_params[0] == 0);
    // A picture filling part of its texture (View3D::set_render_size): the projector reads that part, from the top-left
    // (kin_planar_uv's u = 0.5 + 0.5 x / w, v = 0.5 - 0.5 y / w), and the share is in the parameters for the reads.
    const Mat4f identity;
    planar_view_params(material.shader_params, identity, 0.0f, 1.0f, {0.5f, 0.25f});
    Mat4f part;
    std::copy_n(material.shader_params.begin(), 16, part.m);
    const auto uv = [&](f32 x, f32 y) {
        const Vec4f c = part * Vec4f{x, y, 0, 1};
        return Vec2f{0.5f + 0.5f * c.x / c.w, 0.5f - 0.5f * c.y / c.w};
    };
    assert(near(uv(-1, 1).x, 0) && near(uv(-1, 1).y, 0));        // the picture's top-left: the texture's
    assert(near(uv(1, -1).x, 0.5f) && near(uv(1, -1).y, 0.25f)); // its bottom-right: the share's
    assert(material.shader_params[18] == 0.5f && material.shader_params[19] == 0.25f);
}

// --- The pictures ---

struct Rgb {
    int r, g, b;
};

struct Gpu {
    Renderer2D& r;
    Renderer3D& r3d;
    View3D main = r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    View3D other = r3d.create_view(extent, {.samples = 1, .background = Color::rgb(20, 40, 230)}).value_or({});
    Mesh3D box = r3d.create_mesh(meshes::box({1, 1, 1})).value_or({});
    Mesh3D floor = r3d.create_mesh(meshes::plane({20, 20})).value_or({});
    Mesh3D quad = r3d.create_mesh(meshes::plane({2, 2})).value_or({});
    std::vector<u8> pixels;

    // Renders `first` into the other view (when given), then `scene` into the main view, and reads the main view.
    void frame(const Scene3D& scene, const Scene3D* first = nullptr) {
        r.clear(colors::black);
        if (first) {
            assert(r3d.render(other, *first));
        }
        assert(r3d.render(main, scene));
        r.draw_texture(main.texture(), rect);
        Vec2i size;
        assert(r.read_rgba(rect, pixels, size) && size == extent);
        r.present();
    }
    Rgb at(Vec2f p) const {
        const int x = std::clamp(int(p.x), 0, extent.x - 1), y = std::clamp(int(p.y), 0, extent.y - 1);
        const u8* c = &pixels[std::size_t(y * extent.x + x) * 4];
        return {c[0], c[1], c[2]};
    }
    Rgb at(const Camera3D& camera, Vec3f world) const {
        const auto p = camera.project(world, rect);
        assert(p);
        return at(p->pos);
    }
    void capture(std::string_view name) const {
        if (const char* folder = std::getenv("KIN_PLANAR_CAPTURE_DIR")) {
            std::filesystem::create_directories(folder);
            std::ofstream file(std::filesystem::path(folder) / (std::string(name) + ".ppm"), std::ios::binary);
            file << "P6\n" << extent.x << ' ' << extent.y << "\n255\n";
            for (std::size_t i = 0; i < pixels.size(); i += 4)
                file.write(reinterpret_cast<const char*>(pixels.data() + i), 3);
        }
    }
};

Material3D unlit(Color c) {
    return {.color = c, .unlit = true};
}
Mat4f at(Vec3f p) {
    return Mat4f::translation(p);
}

// Two boxes, one each side of x = 0; a clip plane facing -X keeps the left one.
void test_clip_plane(Gpu& g) {
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 0, 8}, {0, 0, 0});
    assert(scene.draw(g.box, at({-2, 0, 0}), unlit(Color::rgb(230, 20, 20))));
    assert(scene.draw(g.box, at({2, 0, 0}), unlit(Color::rgb(20, 230, 20))));
    g.frame(scene);
    Rgb left = g.at(scene.camera, {-2, 0, 0.5f}), right = g.at(scene.camera, {2, 0, 0.5f});
    assert(left.r > 200 && right.g > 200);
    assert(g.r3d.stats().instances == 2 && g.r3d.stats().culled == 0);
    scene.camera.clip_plane = plane_through({0, 0, 0}, {-1, 0, 0});
    g.frame(scene);
    // Culled too: a copy wholly behind the plane is not drawn at all.
    assert(g.r3d.stats().instances == 1 && g.r3d.stats().culled == 1);
    left = g.at(scene.camera, {-2, 0, 0.5f});
    right = g.at(scene.camera, {2, 0, 0.5f});
    assert(left.r > 200 && left.g < 40);
    assert(right.r < 10 && right.g < 10 && right.b < 10); // gone: the background
    // A plane through the left box keeps what is behind it: the box is cut open, its front face gone, and its inside
    // (back faces, culled) shows the background.
    scene.camera.clip_plane = plane_through({0, 0, 0.1f}, {0, 0, -1});
    g.frame(scene);
    const Rgb open = g.at(scene.camera, {-2, 0, 0.5f});
    assert(open.r < 10 && open.g < 10);
    std::cout << "planar: clip plane\n";
}

// A wide box across x = 0 and a box wholly beyond it, under a plane that keeps x < 0, drawn as each DrawClip3D says.
void test_draw_clip(Gpu& g) {
    const auto seen = [&](DrawClip3D clip, bool clipped) {
        Scene3D scene;
        scene.camera = Camera3D::look_at({0, 0, 8}, {0, 0, 0});
        if (clipped)
            scene.camera.clip_plane = plane_through({0, 0, 0}, {-1, 0, 0});
        const std::array copies{MeshInstance3D{.transform = Mat4f::trs({0, 0, 0}, {}, {3, 1, 1})},
                                MeshInstance3D{.transform = at({3, 0, 0})}};
        assert(scene.draw_instanced(g.box, copies, unlit(Color::rgb(230, 20, 20)), clip));
        assert(scene.draws().size() == 1 && scene.draws()[0].clip == clip);
        g.frame(scene);
        const auto red = [&](Vec3f p) { return g.at(scene.camera, p).r > 200; };
        return std::array{red({-1, 0, 0.5f}), red({1, 0, 0.5f}), red({3, 0, 0.5f})};
    };
    using Seen = std::array<bool, 3>;
    assert((seen(DrawClip3D::camera, false) == Seen{true, true, true}));
    assert((seen(DrawClip3D::camera, true) == Seen{true, false, false}));
    // Never cut, nor culled by the plane.
    assert((seen(DrawClip3D::whole, true) == Seen{true, true, true}));
    assert(g.r3d.stats().instances == 2 && g.r3d.stats().culled == 0);
    // Whole, and only where a clip plane is.
    assert((seen(DrawClip3D::whole_if_cut, true) == Seen{true, true, true}));
    assert((seen(DrawClip3D::whole_if_cut, false) == Seen{false, false, false}));
    // Draws one after another merge only when they say the same.
    Scene3D scene;
    const MeshInstance3D copy{};
    assert(scene.draw_instanced(g.box, std::span(&copy, 1), unlit(colors::white)));
    assert(scene.draw_instanced(g.box, std::span(&copy, 1), unlit(colors::white), DrawClip3D::whole));
    assert(scene.draw_instanced(g.box, std::span(&copy, 1), unlit(colors::white), DrawClip3D::whole));
    assert(scene.draws().size() == 2 && scene.draws()[1].count == 2);
    std::cout << "planar: draw clip\n";
}

// Rendering part of a view (View3D::set_render_size): a lit scene (the sun, ambient occlusion, screen-space
// reflections on a glossy floor, the sky, fog) drawn into the top-left 96 x 64 of the 160 x 120 view is the picture a
// 96 x 64 view of its own shows, to a level or so; a resize renders the whole again; a scene with temporal
// anti-aliasing cannot render a part.
void test_render_part(Gpu& g) {
    constexpr Vec2i part{96, 64};
    Scene3D scene;
    scene.camera = Camera3D::look_at({3, 3, 6}, {0, 0.5f, 0});
    scene.lights.sun.direction = normalize(Vec3f{-0.4f, -1, -0.3f});
    scene.ambient_occlusion.enabled = true;
    scene.ssr.enabled = true;
    scene.sky.enabled = true;
    scene.fog.enabled = true;
    assert(scene.draw(g.floor, Mat4f{}, Material3D{.color = Color::rgb(90, 90, 100), .roughness = 0.1f}));
    assert(scene.draw(g.box, at({0, 0.5f, 0}), Material3D{.color = Color::rgb(220, 60, 40), .roughness = 0.5f}));
    assert(scene.draw(g.box, at({-1.5f, 0.5f, -1}), Material3D{.color = Color::rgb(40, 160, 220)}));
    View3D own = g.r3d.create_view(part, {.samples = 1, .background = colors::black}).value_or({});
    assert(own && g.r3d.can_render_part(g.main, scene));
    assert(g.main.set_render_size(part) && g.main.render_size() == part && g.main.size() == extent);
    const auto read = [&](View3D& view) {
        g.r.clear(colors::black);
        for (int i = 0; i < 3; ++i) // (the reflections read the frame before's picture)
            assert(g.r3d.render(view, scene));
        const Vec2i size = view.size();
        g.r.draw_texture(view.texture(), {0, 0, f32(size.x), f32(size.y)});
        std::vector<u8> pixels;
        Vec2i read_size;
        assert(g.r.read_rgba({0, 0, f32(part.x), f32(part.y)}, pixels, read_size) && read_size == part);
        g.r.present();
        return pixels;
    };
    const auto whole = read(own), in_part = read(g.main);
    int worst = 0;
    for (std::size_t i = 0; i < whole.size(); ++i)
        worst = std::max(worst, std::abs(int(whole[i]) - int(in_part[i])));
    std::cerr << "render part: the worst channel " << worst << " off its own view's\n";
    assert(worst <= 2);
    // Not left at the corner: the box is in it.
    const auto p = scene.camera.project({0, 0.5f, 0.5f}, {0, 0, f32(part.x), f32(part.y)});
    assert(p);
    const u8* red = &in_part[std::size_t(int(p->pos.y) * part.x + int(p->pos.x)) * 4];
    assert(red[0] > red[2] + 40);
    // A resize renders the whole again.
    assert(g.r3d.resize(g.main, {161, 120}) && g.r3d.resize(g.main, extent) && g.main.render_size() == extent);
    // What cannot render a part says so.
    scene.taa.enabled = true;
    assert(!g.r3d.can_render_part(g.main, scene));
    std::cout << "planar: render part\n";
}

// A mirror floor under a red box: the floor shows the reflection view's picture (the box, and its blue background).
// Scene3D::draw_again (a copy drawn again elsewhere: what is half-way through a portal, at its exit): a skinned copy
// keeps its pose (its joint moves it 1 along X: drawn at x = -2 it shows at -1, again at +1 it shows at +2, not +1); a
// batch's copy is drawn again as a plain one; a copy out of range is refused.
void test_draw_again(Gpu& g) {
    auto skin = meshes::box({0.6f, 0.6f, 0.6f});
    skin.skin.resize(skin.vertices.size());
    const Mesh3D skinned = g.r3d.create_mesh(skin).value_or({});
    assert(skinned);
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 0, 8}, {0, 0, 0});
    const std::array<Mat4f, 1> pose{Mat4f::translation({1, 0, 0})};
    assert(scene.draw_skinned(skinned, pose, Mat4f::translation({-2, 0, 0}), unlit(Color::rgb(230, 20, 20))));
    auto batch = InstanceBatch3D::create(g.box, {MeshInstance3D{.transform = Mat4f::translation({0, 2, 0})}});
    assert(batch);
    assert(scene.draw_instanced(*batch, unlit(Color::rgb(20, 230, 20))));
    assert(scene.draws().size() == 2);
    assert(scene.draw_again(0, 0, Mat4f::translation({1, 0, 0})));
    assert(scene.draw_again(1, 0, Mat4f::translation({0, -2, 0}), DrawClip3D::whole));
    assert(!scene.draw_again(1, 1, Mat4f::identity()) && !scene.draw_again(9, 0, Mat4f::identity()));
    assert(scene.draws().size() == 4 && scene.draws()[2].skinned && !scene.draws()[3].batch &&
           scene.draws()[3].clip == DrawClip3D::whole);
    g.frame(scene);
    const auto red = [](Rgb c) { return c.r > 200 && c.g < 60; };
    const auto green = [](Rgb c) { return c.g > 200 && c.r < 60; };
    assert(red(g.at(scene.camera, {-1, 0, 0.3f})));   // the copy, posed
    assert(red(g.at(scene.camera, {2, 0, 0.3f})));    // again, posed alike
    assert(!red(g.at(scene.camera, {1, 0, 0.3f})));   // (not unposed)
    assert(green(g.at(scene.camera, {0, -2, 0.5f}))); // the batch's copy, again
    std::cout << "planar: draw again\n";
}

void test_mirror(Gpu& g, const SurfaceShader3D& reflection) {
    const Camera3D eye = Camera3D::look_at({0, 2, 6}, {0, 0, 0});
    const PlanarView3D mirror = mirror_view(eye, plane_through({0, 0, 0}, {0, 1, 0}), aspect);
    // The red box, and the floor as `floor` (none: no floor), seen by `camera`.
    const auto scene_of = [&](const Camera3D& camera, const Material3D* floor) {
        Scene3D scene;
        scene.camera = camera;
        scene.lights.sun.color = {0, 0, 0};
        assert(scene.draw(g.box, at({0, 1, 0}), unlit(Color::rgb(230, 20, 20))));
        if (floor)
            assert(scene.draw(g.floor, Mat4f{}, *floor));
        return scene;
    };
    const Vec3f floor_spot{2.5f, 0, 1.5f};
    const Vec3f box_image{0, -1, 0.5f}; // (the box's near face, mirrored)
    Material3D glass{.color = colors::white, .metallic = 1.0f, .roughness = 0.045f};
    // Without its picture: the sky's hemisphere, no blue.
    g.frame(scene_of(eye, &glass));
    Rgb f = g.at(eye, floor_spot);
    assert(!(f.b > f.r + 60));
    Material3D m = glass;
    m.surface = reflection;
    m.emissive_texture = g.other.texture();
    planar_view_params(m.shader_params, mirror.projector);
    // The mirror is in its own view too: in front of the clip plane, never drawn there (and its picture reads white).
    const Scene3D reflected = scene_of(mirror.camera, &m);
    g.frame(scene_of(eye, &m), &reflected);
    f = g.at(eye, floor_spot);
    const Rgb b = g.at(eye, box_image);
    assert(f.b > 150 && f.r < 80);  // the reflection view's background
    assert(b.r > 150 && b.b < 100); // the box, mirrored
    // A rough mirror leaves it to the environment (past 0.7).
    m.roughness = 0.9f;
    g.frame(scene_of(eye, &m), &reflected);
    f = g.at(eye, floor_spot);
    assert(!(f.b > f.r + 60));
    std::cout << "planar: mirror\n";
}

// A portal at the origin onto a green box 20 m away; a red box behind the exit is clipped from the view.
void test_portal(Gpu& g, const SurfaceShader3D& window) {
    const Camera3D eye = Camera3D::look_at({0, 0, 5}, {0, 0, 0});
    // Both portals face +Z (their front, as PortalPose3D has it).
    const PortalPose3D entry{{0, 0, 0}, {}}, exit{{20, 0, 0}, {}};
    PlanarView3D through = portal_view(eye, entry, exit, aspect);
    Material3D m = unlit(colors::white);
    m.surface = window;
    m.emissive_texture = g.other.texture();
    planar_view_params(m.shader_params, through.projector);
    const auto scene_of = [&](const Camera3D& camera, bool portal) {
        Scene3D scene;
        scene.camera = camera;
        assert(scene.draw(g.box, at({20, 0, 4}), unlit(Color::rgb(20, 230, 20))));
        assert(scene.draw(g.box, at({20, 0, -2}), unlit(Color::rgb(230, 20, 20))));
        if (portal) // (a plane mesh faces +Y: turned to face +Z)
            assert(scene.draw(g.quad, Mat4f::rotation_x(90), m));
        return scene;
    };
    const Scene3D beyond = scene_of(through.camera, false);
    g.frame(scene_of(eye, true), &beyond);
    const Rgb centre = g.at({80, 60});
    assert(centre.g > 150 && centre.r < 80); // the green box, not the red one between the portal's camera and its exit
    // Without the clip plane, the red box hides the green one.
    through.camera.clip_plane = {0, 0, 0, 1};
    const Scene3D unclipped = scene_of(through.camera, false);
    g.frame(scene_of(eye, true), &unclipped);
    assert(g.at({80, 60}).r > 150);
    // A camera 5 cm into it, not yet moved through: the flat surface is behind it and what is behind the portal shows
    // (a blue box there); inside portal_surface_transform's box it still sees through.
    const Camera3D close = Camera3D::look_at({0, 0, -0.05f}, {0, 0, -1});
    const PortalPose3D sized{{0, 0, 0}, {}, {2, 2}};
    PlanarView3D near_view = portal_view(close, sized, exit, aspect);
    planar_view_params(m.shader_params, near_view.projector);
    const Scene3D beyond_close = scene_of(near_view.camera, false);
    const auto close_scene = [&](bool box) {
        Scene3D scene = scene_of(close, false);
        assert(scene.draw(g.box, at({0, 0, -3}), unlit(Color::rgb(20, 20, 230))));
        Material3D surface = m;
        surface.double_sided = box;
        assert(
            scene.draw(box ? g.box : g.quad, box ? portal_surface_transform(sized) : Mat4f::rotation_x(90), surface));
        return scene;
    };
    g.frame(close_scene(false), &beyond_close);
    const Rgb cut = g.at({80, 60});
    assert(cut.b > 150 && cut.g < 80); // behind the portal
    g.frame(close_scene(true), &beyond_close);
    const Rgb whole = g.at({80, 60});
    assert(whole.g > 150 && whole.b < 80); // through it
    std::cout << "planar: portal\n";
}

#if KIN_ENABLE_PHYSICS3D
// ECS planar-view orchestration is built with the 3D physics components.
// The mirror is outside the main camera's view, but visible through the portal. Its picture must be made for the
// portal's camera: the green box's reflected image is below the floor, apart from the box seen directly above it.
void test_mirror_through_portal(Gpu& g, const PlanarSurfaces3D& surfaces) {
    for (const bool water : {false, true}) {
        kin::EcsWorld world;
        auto exit = world.raw().entity();
        exit.set<WorldTransform3D>({Mat4f::translation({20, 1, 0})});
        auto entry = world.raw().entity();
        entry.set<WorldTransform3D>({Mat4f::translation({0, 1, 0})});
        entry.set<Portal3D>({.size = {4, 4}, .exit = {exit.id()}});
        auto floor = world.raw().entity();
        floor.set<WorldTransform3D>({Mat4f::trs({20, 0, 3}, Quatf::axis_angle({1, 0, 0}, -90), {1, 1, 1})});
        Mirror3D surface{.size = {8, 8}, .color = colors::white};
        surface.metallic = water ? 0.0f : 1.0f;
        surface.refraction = water ? 1.0f : 0.0f;
        surface.refraction_distortion = 0;
        surface.murk_depth = 100;
        floor.set<Mirror3D>(surface);
        PlanarViews3D planar{g.r3d, surfaces};
        Scene3D scene;
        scene.lights.sun.color = scene.lights.ambient = {0, 0, 0};
        const auto compose = [&](float x) {
            scene.clear();
            assert(scene.portals.empty());
            scene.camera = Camera3D::look_at({x, 2, 6}, {0, 0, 0});
            assert(scene.draw(g.box, at({20, water ? -1.0f : 1.0f, 4}), unlit(Color::rgb(20, 230, 20))));
        };
        compose(-0.4f);
        PlanarViews3D stand_ins{g.r3d, {.window = surfaces.window}};
        assert(stand_ins.render(world, scene, extent, {.samples = 1, .background = colors::black}));
        g.frame(scene);
        assert(g.at(scene.camera, {0, -1, -4}).g < 175);
        g.capture(water ? "water-stand-in" : "mirror-stand-in");
        // Move the eye too: a picture made for the main camera or the previous portal camera gives the wrong image.
        for (const float x : {-0.4f, 0.4f}) {
            compose(x);
            assert(planar.render(world, scene, extent, {.samples = 1, .background = colors::black}));
            assert(scene.portals.size() == 1);
            g.frame(scene);
            const Rgb reflected = g.at(scene.camera, {0, -1, -4});
            std::cerr << (water ? "water" : "mirror") << " through portal x=" << x << ": " << reflected.r << ','
                      << reflected.g << ',' << reflected.b << '\n';
            assert(reflected.g > 175 && reflected.g > reflected.r + 80 && reflected.g > reflected.b + 80);
            assert(planar.views_rendered() == (water ? 3u : 2u));
            if (x < 0)
                g.capture(water ? "water-through-portal" : "mirror-through-portal");
        }
        if (g.r3d.capabilities().temporal_upscaling) {
            // The same planar renderer can switch back to native pictures when TAA is disabled. Reusing a
            // scaled cached view here would fail rendering because reconstruction requires TAA.
            PlanarViews3D scaled_planar{g.r3d, surfaces};
            scene.taa.enabled = true;
            // Cached portal/reflection views must adopt a changed history density without changing shading scale.
            for (const float density : {1.f, 1.5f, 2.f, 1.f}) {
                compose(0);
                assert(
                    scaled_planar.render(world, scene, extent,
                                         {.samples = 1, .render_scale = 2.f / 3.f, .temporal_history_scale = density}));
                assert(g.r3d.debug_stats().taa_dispatches == (density > 1.f ? 2u : 1u));
                g.frame(scene);
                assert(g.at(scene.camera, {0, -1, -4}).g > 175);
            }
            scene.taa.enabled = false;
            compose(0);
            assert(scaled_planar.render(world, scene, extent, {.samples = 1}));
            g.frame(scene);
            assert(g.at(scene.camera, {0, -1, -4}).g > 175);
        }
    }
    std::cout << "planar: mirror and water through portal\n";
}
#endif

// No GI or ambient light: a roof blocks ordinary sunlight on the floor. The rotated portal supplies a different
// solar direction from a source opening outside the camera. Its shadow must stop at each opening's plane.
void test_portal_sun(Gpu& g) {
    const auto run = [&](std::string_view name, bool linked, int blocker, float shift = 0) {
        Scene3D scene;
        scene.camera = Camera3D::look_at({0, 2.5f, 6}, {0, 0, 1.5f});
        scene.lights.ambient = {0, 0, 0};
        scene.lights.sun = {.direction = {1, 1, 0}, .color = {.8f, .7f, .6f}};
        scene.lights.shadow_area = {{-6, -1, -4}, {15, 8, 8}};
        scene.lights.shadow_resolution = 256;
        scene.lights.shadow_cascades = 4;
        scene.lights.cache_shadows = true;
        const Material3D black{.color = colors::black};
        assert(scene.draw(g.floor, {}, {.color = colors::white}));
        assert(scene.draw(g.box, Mat4f::trs({0, 3.5f, 2}, {}, {13, .25f, 10}), black));
        if (linked)
            scene.portals.push_back(
                {{{shift, 1.5f, 0}, {}, {2, 3}}, {{8, 1.5f, 0}, Quatf::axis_angle({0, 1, 0}, 90), {2, 3}}});
        if (blocker == 1)
            assert(scene.draw(g.box, Mat4f::trs({9, 3, 0}, {}, {.25f, 4, 3}), black));
        if (blocker == 2)
            assert(scene.draw(g.box, Mat4f::trs({0, .8f, 1.2f}, {}, {3, .25f, .4f}), black));
        if (blocker == 3)
            assert(scene.draw(g.box, Mat4f::trs({7, 1, 0}, {}, {.25f, 2, 3}), black));
        if (blocker == 4)
            assert(scene.draw(g.box, Mat4f::trs({0, 3, -1}, {}, {3, 3, .25f}), black));
        if (blocker == 5 || blocker == 6) {
            const PortalPose3D obstacle = blocker == 5
                                              ? PortalPose3D{{9, 3, 0}, Quatf::axis_angle({0, 1, 0}, 90), {3, 4}}
                                              : PortalPose3D{{0, .8f, 1.2f}, {}, {3, 2}};
            scene.portals.push_back({obstacle, {{40, 2, 0}, Quatf::axis_angle({0, 1, 0}, -90), {3, 4}}});
        }
        g.frame(scene);
        const Rgb first = g.at(scene.camera, {shift, 0, 2});
        g.frame(scene); // cached ordinary maps and the two new clipped passes must agree on the next frame
        if (linked)
            assert(g.r3d.debug_stats().local_shadow_cached >= 2);
        const Rgb inside = g.at(scene.camera, {shift, 0, 2});
        const Rgb outside = g.at(scene.camera, {shift + 1.5f, 0, 2});
        const Rgb past = g.at(scene.camera, {shift, 0, 4});
        std::cerr << name << ": " << inside.r << ',' << inside.g << ',' << inside.b << " outside=" << outside.r
                  << " past=" << past.r << '\n';
        g.capture(name);
        assert(std::abs(first.r - inside.r) <= 2);
        assert(outside.r < 25 && past.r < 25);
        return inside;
    };
    const Rgb closed = run("sun-unlinked", false, 0);
    const Rgb open = run("sun-linked", true, 0);
    const Rgb source = run("sun-source-blocked", true, 1);
    const Rgb target = run("sun-target-blocked", true, 2);
    const Rgb source_behind = run("sun-source-behind", true, 3);
    const Rgb target_behind = run("sun-target-behind", true, 4);
    const Rgb moved = run("sun-moved", true, 0, .75f);
    const Rgb source_portal = run("sun-source-next-portal", true, 5);
    const Rgb target_portal = run("sun-target-next-portal", true, 6);
    assert(closed.r < 25 && open.r > 100);
    assert(source.r < 25 && target.r < 25);
    assert(std::abs(source_behind.r - open.r) <= 8 && std::abs(target_behind.r - open.r) <= 8);
    assert(std::abs(moved.r - open.r) <= 8);
    assert(source_portal.r < 25 && target_portal.r < 25);
    const auto bounce = [&](bool gi, bool blocked) {
        Scene3D scene;
        scene.camera = Camera3D::look_at({0, 1.5f, 3}, {0, 1.5f, 4});
        scene.lights.ambient = {0, 0, 0};
        scene.lights.sun = {.direction = {1, 1, 0}, .color = {2, 2, 2}};
        scene.lights.shadow_area = {{-6, -1, -4}, {15, 8, 8}};
        scene.lights.shadow_resolution = 256;
        scene.global_illumination = {
            .enabled = gi, .area = {{-4, -1, -4}, {14, 6, 8}}, .cell = .25f, .levels = 1, .side = 96, .height = 32};
        scene.global_illumination.probes.per_render = 2048;
        scene.global_illumination.probes.rays = 128;
        const Material3D black{.color = colors::black};
        assert(scene.draw(g.floor, {}, {.color = colors::white}));
        assert(scene.draw(g.box, Mat4f::trs({0, 3.5f, 2}, {}, {13, .25f, 10}), black));
        assert(scene.draw(g.box, Mat4f::trs({4, 3, 0}, {}, {.25f, 8, 20}), black));
        assert(scene.draw(g.box, Mat4f::trs({0, 1.5f, 4}, {}, {3, 3, .25f}), {.color = colors::white}));
        scene.portals.push_back({{{0, 1.5f, 0}, {}, {2, 3}}, {{8, 1.5f, 0}, Quatf::axis_angle({0, 1, 0}, 90), {2, 3}}});
        if (blocked)
            assert(scene.draw(g.box, Mat4f::trs({9, 3, 0}, {}, {.25f, 4, 3}), black));
        for (int frame = 0; frame < (gi ? 128 : 2); ++frame)
            g.frame(scene);
        const Rgb value = g.at({80, 60});
        const char* name = !gi ? "sun-bounce-no-gi" : blocked ? "sun-bounce-blocked" : "sun-bounce-gi";
        g.capture(name);
        std::cerr << name << ": " << value.r << ',' << value.g << ',' << value.b << '\n';
        return value;
    };
    const Rgb no_bounce = bounce(false, false), indirect = bounce(true, false), blocked_bounce = bounce(true, true);
    assert(no_bounce.r < 25 && indirect.r > no_bounce.r + 20);
    assert(blocked_bounce.r + 15 < indirect.r);
    std::cout << "planar: direct sun through portal\n";
}

#if KIN_ENABLE_PHYSICS3D
// A black partition separates two places. Only the linked aperture connects a white receiver to the green emitter
// in the other place. A blocker on either segment must stop that light; reducing the opening must reduce it too.
void test_portal_gi(Gpu& g) {
    const auto run = [&](std::string_view name, bool linked, int blocker, f32 opening, bool front = true,
                         GiEmitters3D emitters = GiEmitters3D::Exact) {
        Scene3D scene;
        scene.camera = Camera3D::look_at({0, 1.5f, 1}, {0, 1.5f, 3});
        scene.lights.sun.color = scene.lights.ambient = {0, 0, 0};
        scene.global_illumination = {
            .enabled = true, .area = {{-4, -1, -4}, {12, 5, 5}}, .cell = 0.25f, .levels = 1, .side = 64, .height = 32};
        scene.global_illumination.probes.per_render = 2048;
        scene.global_illumination.probes.rays = 128;
        scene.global_illumination.emitters = emitters;
        const Material3D black{.color = colors::black};
        const Material3D green{.color = Color::rgb(10, 255, 10), .emissive = 2};
        assert(scene.draw(g.box, Mat4f::trs({4, 1.5f, 0}, {}, {0.5f, 8, 14}), black));
        assert(scene.draw(g.box, Mat4f::trs({0, 1.5f, 3}, {}, {3, 3, 0.25f}), {.color = colors::white}));
        assert(scene.draw(g.box, Mat4f::trs({8, 1.5f, 3}, {}, {4, 4, 0.25f}), green));
        if (blocker)
            assert(scene.draw(g.box, Mat4f::trs({blocker == 1 ? 0.0f : 8.0f, 1.5f, 0.75f}, {}, {5, 5, 0.25f}), black));
        kin::EcsWorld world;
        auto exit = world.raw().entity();
        exit.set<WorldTransform3D>({Mat4f::translation({8, 1.5f, 0})});
        exit.set<Portal3D>({.enabled = false, .size = {opening, opening}});
        auto entry = world.raw().entity();
        entry.set<WorldTransform3D>(
            {Mat4f::trs({0, 1.5f, 0}, front ? Quatf{} : Quatf::axis_angle({0, 1, 0}, 180), {1, 1, 1})});
        entry.set<Portal3D>({.enabled = linked, .size = {3, 3}, .exit = {exit.id()}});
        PlanarViews3D planar{g.r3d};
        assert(planar.render(world, scene, extent));
        assert(scene.portals.size() == (linked ? 1u : 0u));
        assert(planar.views_rendered() == 0); // lighting crosses an off-screen portal without needing its picture
        auto view = g.r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
        assert(view);
        for (int frame = 0; frame < 128; ++frame) {
            g.r.clear(colors::black);
            assert(g.r3d.render(view, scene));
            if (frame == 127) {
                g.r.draw_texture(view.texture(), rect);
                Vec2i size;
                assert(g.r.read_rgba(rect, g.pixels, size) && size == extent);
            }
            g.r.present();
        }
        const Rgb value = g.at({80, 60});
        g.capture(name);
        std::cerr << "portal GI " << name << ": " << value.r << ',' << value.g << ',' << value.b << '\n';
        return value;
    };
    const Rgb closed = run("gi-unlinked", false, 0, 3);
    const Rgb open = run("gi-linked", true, 0, 3);
    const Rgb entry = run("gi-entry-blocked", true, 1, 3);
    const Rgb exit = run("gi-exit-blocked", true, 2, 3);
    const Rgb narrow = run("gi-narrow", true, 0, 0.25f);
    const Rgb back = run("gi-back-facing", true, 0, 3, false);
    const Rgb probes = run("gi-probes", true, 0, 3, true, GiEmitters3D::Probes);
    assert(open.g > closed.g + 25 && open.g > open.r + 20);
    assert(entry.g < open.g / 3 && exit.g < open.g / 3);
    assert(narrow.g < open.g / 2);
    assert(back.g < open.g / 3);
    assert(std::abs(probes.g - open.g) <= 4); // the portal's emitter path is counted once with either strategy
    std::cout << "planar: GI through portals\n";
}
#endif

} // namespace

int main() {
    test_plane_through();
    test_mirror_view();
    test_portal_view();
    test_params();
    test_crossing_and_rays();
    test_chain();
    test_on_screen();
    test_surface_transform();
    test_window();
    std::cout << "planar: camera arithmetic\n";

    App app;
    auto& window = app.create_window({.title = "planar views", .width = 160, .height = 120, .hidden = true});
    Renderer2D r{make_render_backend(window, false, true)};
    Renderer3D r3d{r};
    const bool require = std::getenv("KIN_REQUIRE_GPU_TESTS") != nullptr;
    if (!r3d.available() || !shader_compiler_available())
        return require ? 1 : 77;
    const std::filesystem::path shaders{KIN_ENGINE_SHADERS};
    const SurfaceShader3D reflection =
        r3d.compile_surface_shader(shaders / "planar_reflection.surface.glsl").value_or({});
    const SurfaceShader3D portal = r3d.compile_surface_shader(shaders / "planar_window.surface.glsl").value_or({});
    if (!reflection || !portal)
        return require ? 1 : 77;
    Gpu g{r, r3d};
    assert(g.main && g.other && g.box && g.floor && g.quad);
    test_clip_plane(g);
    test_draw_clip(g);
    test_draw_again(g);
    test_render_part(g);
    test_mirror(g, reflection);
    test_portal(g, portal);
#if KIN_ENABLE_PHYSICS3D
    test_mirror_through_portal(g, {reflection, portal});
#endif
    test_portal_sun(g);
#if KIN_ENABLE_PHYSICS3D
    test_portal_gi(g);
#endif
}
