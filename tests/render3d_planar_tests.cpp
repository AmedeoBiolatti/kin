// Mirrors and portals (docs/rendering_3d.md): Camera3D::clip_plane, mirror_view, portal_view and kin's planar
// surfaces. The camera arithmetic runs everywhere; the pictures need a GPU (77: skipped without one).
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/shader_compiler.hpp>
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
    std::array<f32, 8> short_params{};
    planar_view_params(short_params, m); // too short: left alone
    assert(short_params[0] == 0);
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
    test_mirror(g, reflection);
    test_portal(g, portal);
}
