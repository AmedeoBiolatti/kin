#pragma once

// A camera for kin's experimental 3D (docs/3d_plan.md): where it is, which
// way it looks (down its own -Z, +Y up), and how it projects.

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace kin {

enum class Projection3D : u8 {
    Perspective,
    Orthographic, // parallel: sizes don't shrink with distance (pre-rendered sheets' look)
};

struct Camera3D {
    Vec3f position{0.0f, 0.0f, 5.0f};
    Quatf rotation{};
    Projection3D projection = Projection3D::Perspective;
    f32 fov = 60.0f;                                  // perspective: degrees, top to bottom of the view
    f32 height = 10.0f;                               // orthographic: world units, top to bottom of the view
    f32 z_near = 0.1f;                                // nothing nearer is drawn
    f32 z_far = std::numeric_limits<f32>::infinity(); // nor further (infinity: no limit)
    // A plane the view keeps one side of: a point p of the world is drawn where dot(xyz, p) + w >= 0, not past it.
    // What a mirror's or a portal's view needs (mirror_view, portal_view: what is behind the mirror, or behind the
    // portal's exit, is not in the picture). Clipped in the vertex shaders, so the depth is the camera's own (ambient
    // occlusion, fog and temporal anti-aliasing are as ever); shadows are not clipped, and the sky is drawn where
    // nothing is. The default (0, 0, 0, 1) keeps everything. WebGL2 does not clip (it says so once).
    Vec4f clip_plane{0.0f, 0.0f, 0.0f, 1.0f};
    bool clipped() const { return clip_plane.x != 0.0f || clip_plane.y != 0.0f || clip_plane.z != 0.0f; }
    // The copies this camera does not see, by their pick id (MeshInstance3D::id; 0: none): a shiny thing left out of
    // its own reflection probe's picture (ReflectionCaptureOptions3D::hidden_id). They still cast their shadows.
    u32 hidden_id = 0;
    // The part of the camera's frame a view shows (0..1 across and down; the default all of it): a view of that part
    // alone, as a camera of the same frame would show it, its pixels as dense as the frame's at the view's size over
    // the window's. The frame's aspect is then the view's times window.h / window.w. What a mirror's or a portal's
    // picture of only where it shows on screen takes (PlanarViews3D, crop_planar_view).
    Rectf window{0.0f, 0.0f, 1.0f, 1.0f};
    bool windowed() const { return window.x != 0.0f || window.y != 0.0f || window.w != 1.0f || window.h != 1.0f; }
    // The aspect of the whole frame of a view `aspect` wide over high.
    f32 frame_aspect(f32 aspect) const {
        return windowed() && window.w > 0.0f && window.h > 0.0f ? aspect * window.h / window.w : aspect;
    }

    // At `eye`, looking at `target`.
    static Camera3D look_at(Vec3f eye, Vec3f target, f32 fov = 60.0f, Vec3f up = {0.0f, 1.0f, 0.0f});
    // `distance` from `target`, looking at it: from `yaw` degrees round +Y
    // (0: from +Z, looking toward -Z) and `pitch` degrees above it.
    static Camera3D orbit(Vec3f target, f32 yaw, f32 pitch, f32 distance, f32 fov = 60.0f);

    Vec3f forward() const { return rotation.rotate({0.0f, 0.0f, -1.0f}); }
    Vec3f right() const { return rotation.rotate({1.0f, 0.0f, 0.0f}); }
    Vec3f up() const { return rotation.rotate({0.0f, 1.0f, 0.0f}); }

    // World to the camera's space.
    Mat4f view() const;
    // The camera's space to clip space for a view `aspect` wide over high.
    Mat4f projection_matrix(f32 aspect) const;
    Mat4f view_projection(f32 aspect) const { return projection_matrix(aspect) * view(); }

    // Where `world` shows in a view laid into `rect` (2D draw coordinates),
    // nothing when it is behind the camera.
    std::optional<ScreenPoint> project(Vec3f world, Rectf rect) const;
    // The ray from the camera through `point` of a view laid into `rect`.
    Ray3 ray(Vec2f point, Rectf rect) const;
};

// Where `ray` meets the horizontal plane at height `y`, nothing when it runs
// along it or away from it.
std::optional<Vec3f> hit_plane_y(const Ray3& ray, f32 y = 0.0f);

// Mirrors and portals (docs/rendering_3d.md, Mirrors and portals). A view of the scene in a mirror or through a portal:
// the camera to render it with (into a View3D of its own, of the same aspect as the view it is seen in), and
// `projector`, which takes a point of the mirror's or the entry portal's surface to that view's clip space: where its
// picture shows the point's reflection, or what lies beyond the portal. A surface reads the picture through it
// (kin_planar_reflection and kin_planar_window in kin_material.inc.glsl, with planar_view_params).
struct PlanarView3D {
    Camera3D camera;
    Mat4f projector;
};

// The plane through `point` facing `normal` (any length but zero), as Camera3D::clip_plane takes it: dot(xyz, p) + w
// is how far a point is in front of it.
Vec4f plane_through(Vec3f point, Vec3f normal);

// `camera`'s reflection in the mirror `plane` (plane_through): a camera behind the mirror looking out through it, which
// draws only what is in front of the mirror from `offset` (world units) on, so that the mirror's own surface is not
// drawn into its reflection (a surface may not read the picture it is drawn into). The camera's side of the plane is
// the front, whichever way the plane faces. `aspect`: the views', width over height.
PlanarView3D mirror_view(const Camera3D& camera, Vec4f plane, f32 aspect, f32 offset = 0.005f);

// A portal's placing: its surface in its own XY plane, centred on `position`, its front its +Z (seen from there, gone
// through toward -Z). Rigid: no scale. `size`: its opening, wide (its X) and high (its Y); zero: without bounds (what
// the crossing and ray tests take as the whole plane).
struct PortalPose3D {
    Vec3f position{};
    Quatf rotation{};
    Vec2f size{};
};
// What `camera` sees through the portal `entry`: the camera taken through the portals (in at the entry's front, out of
// the exit's front), drawing only what is in front of `exit` from `offset` on, and the projector for the entry's
// surface. The same projection as `camera`'s; `aspect`: the views'.
PlanarView3D portal_view(const Camera3D& camera, PortalPose3D entry, PortalPose3D exit, f32 aspect,
                         f32 offset = 0.005f);
// What the portals do to a placing (a camera's, a body's, a bullet's): from the entry's space to the exit's, turned
// half round about its +Y, so that what goes into the entry's front comes out of the exit's front. A point, and a
// rotation (a direction: portal_rotation(...).rotate(d)).
Vec3f portal_point(PortalPose3D entry, PortalPose3D exit, Vec3f point);
Quatf portal_rotation(PortalPose3D entry, PortalPose3D exit);

// Whether a step from `from` to `to` (a player's, a body's, this frame's) goes into the portal: through its front,
// within its opening. The share of the step taken before it (0..1): move what is left of it from the exit
// (portal_point, portal_rotation for its facing and its velocity). Nothing for a step that misses it, that comes from
// behind, or that stays on one side.
std::optional<f32> portal_crossing(PortalPose3D entry, Vec3f from, Vec3f to);

// Something half-way through: while a thing whose bounds are the sphere `centre`, `radius` straddles the entry (closer
// to its plane than its radius and the views' clip `offset` (portal_view's), its centre within the opening, and no
// larger than the opening's half diagonal: not the wall the portal is in, nor the floor under it), the transform that
// takes it to its image at the exit (times its own placing: through * transform). Draw it there too, as well as where
// it is: the part that has gone through shows beyond the exit, and the part that has not still shows before the entry.
// Draw neither cut by the pictures' clip planes: the image with DrawClip3D::whole, and the thing itself again with
// DrawClip3D::whole_if_cut (a closed mesh cut open shows its inside, which is not drawn: a rim where the halves meet, a
// hole). Before the thing is moved through (portal_crossing) and after, from either portal of a pair. Nothing when it
// does not straddle it.
std::optional<Mat4f> portal_straddle(PortalPose3D entry, PortalPose3D exit, Vec3f centre, f32 radius,
                                     f32 offset = 0.005f);

// A ray (an aim, a bullet, a line of sight) that goes into the portal `entry`, continued out of `exit`: `ray` starts
// where it comes out, `distance` is how far along the first ray the entry is (its direction's units). Nothing where it
// misses the opening or meets the portal from behind. Rays through a chain of portals: call it again with the new ray.
struct PortalRay3D {
    Ray3 ray;
    f32 distance = 0.0f;
};
std::optional<PortalRay3D> ray_through_portal(const Ray3& ray, PortalPose3D entry, PortalPose3D exit);

// Where to draw the entry's surface so that the camera can go a little way into it: the transform of a unit box
// (meshes::box({1, 1, 1})) that fills the opening and reaches `depth` (world units) behind it. Kin's views clamp depth
// at the near plane rather than clip there, so a flat surface stays whole however close the camera comes; but a camera
// that has passed the plane before the game moves it through (an eye ahead of the body whose crossing moves it) has the
// flat surface behind it and sees what is behind the portal. Inside the box it sees the box's faces (its material
// double_sided), read through the same projector: a portal's is the camera's own projection, so any point reads its
// own pixel. Needs entry.size.
Mat4f portal_surface_transform(PortalPose3D entry, f32 depth = 0.5f);

// Portals seen through portals: `depth` views, each through `entry` again from the one before, the first from `camera`
// (the first is portal_view's). View k (from 0) draws the entry's surface with view k + 1's picture and projector, the
// last with a stand-in; render the last first. A pair of portals facing each other shows its corridor to that depth.
std::vector<PlanarView3D> portal_chain(const Camera3D& camera, PortalPose3D entry, PortalPose3D exit, f32 aspect,
                                       u32 depth, f32 offset = 0.005f);

// Where a planar surface (centred on `centre`, turned by `rotation`, its front its +Z, `size` wide and high: a mirror,
// a portal's opening) shows in `camera`'s view: its bounds (0..1 across and down the view), or nothing when none of it
// is in view or the camera is behind it. A view whose surface is nowhere on screen need not be rendered; the bounds'
// share of the screen says how large its picture needs to be.
std::optional<Rectf> planar_on_screen(const Camera3D& camera, f32 aspect, Vec3f centre, Quatf rotation, Vec2f size);

// A planar-view surface's parameters, Material3D::shader_params' first 20 floats (kin_planar_projector,
// kin_planar_reflection and kin_planar_window read them): the projector's 16, then `distortion` (world units: how far
// the point the picture is read at moves with the bend of the surface's normal map, so that the picture wavers with its
// ripples: 0.25 for water, 0 for a flat mirror) and `strength` (how much of the reflection the picture is, or of a
// window's colour: 1 all). `params` must hold 20 at least.
void planar_view_params(std::span<f32> params, const Mat4f& projector, f32 distortion = 0.0f, f32 strength = 1.0f);

// Water you see into: what is below `plane` (the side its normal does not face, from `offset` under it, so that the
// surface itself is not in the picture), seen by `camera` itself, and the projector to read it where each point of the
// surface shows (crop_planar_view cuts it to the surface's place on the screen, as a mirror's: `mirrored` false). The
// water's surface (planar_reflection.surface.glsl) shows it, bent by its ripples, as the light that comes up through
// it.
PlanarView3D refraction_view(const Camera3D& camera, Vec4f plane, f32 aspect, f32 offset = 0.005f);
// Its picture's projector and how it is read into a planar surface's parameters (after planar_view_params' 20: the
// projector, then `strength` (0: none) and `distortion`, world units the ripples move where it is read, as a
// reflection's).
void planar_refraction_params(std::span<f32> params, const Mat4f& projector, f32 distortion = 0.1f,
                              f32 strength = 1.0f);

// A planar view cut down to `window` of its frame (Camera3D::window: where its surface shows, planar_on_screen's bounds
// for a portal, the same turned left to right for a mirror, whose camera sees its frame flipped), for a picture
// `window` of the frame's size: as sharp as the frame's pixels, for the window's share of the cost. `aspect`: the
// picture's, width over height. Mirrors: `mirrored` true.
void crop_planar_view(PlanarView3D& view, Rectf window, f32 aspect, bool mirrored, const Mat4f& through = {});

// The picture a planar surface needs, where it shows at `bounds` (planar_on_screen's) of a frame `frame` pixels (the
// view's, times how sharp the picture is to be: 1 as sharp): the window to cut its view to (crop_planar_view), two
// pixels wider all round, then widened to whole steps of 64 pixels so that its view keeps its size while the surface
// moves a little, and the picture's size in pixels (the view's: Renderer3D::resize it).
struct PlanarPicture3D {
    Rectf window;
    Vec2i size;
};
PlanarPicture3D planar_picture(Rectf bounds, Vec2i frame);

} // namespace kin
