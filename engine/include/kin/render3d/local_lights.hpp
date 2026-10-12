#pragma once

// Spot lights and the shadows of lights that are not the sun (docs/3d_plan.md, P3b), the maths apart from the GPU so that
// it can be tested: the cone's falloff (the CPU twin of mesh.frag's), the view a spot's or a point light's shadow map is
// drawn from, how important a light is to the picture, and which lights hold the few shadow maps (with a hysteresis).

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/render3d/scene3d.hpp>

#include <span>
#include <vector>

namespace kin {

constexpr u32 max_spot_lights = 16;     // spot lights a frame (as Lights3D::points' 16)
constexpr u32 max_spot_shadow_maps = 8; // Lights3D::max_shadowed_spots is clamped to this
constexpr u32 max_point_shadow_maps = 2;
constexpr u32 cube_faces = 6;
// The layers of the shared array: a layer a spot's map, then six a point light's.
constexpr u32 max_local_shadow_layers = max_spot_shadow_maps + cube_faces * max_point_shadow_maps;
// A map's guard band: the view reaches this many texels past the cone's edge (or a cube face's), so that the filter's
// taps at the edge read the map and not the clamp.
constexpr f32 shadow_guard_texels = 2.0f;
// How much more important a light that held a shadow map last frame is taken to be, against one that did not.
constexpr f32 shadow_hysteresis = 1.5f;

// A cone as the shader holds it: the unit axis, the cosine of the outer angle and 1 / (cos inner - cos outer).
struct SpotCone {
    Vec3f axis{0.0f, -1.0f, 0.0f};
    f32 cos_outer = 0.0f;
    f32 cos_inner = 1.0f;
    f32 scale = 1.0f;
};

SpotCone spot_cone(const SpotLight& light);
// The cone's angular term for the cosine of the angle between its axis and the way to the surface point: 0 at and
// beyond the outer angle, 1 at and within the inner one, between, the square of the linear step in the cosine (glTF's).
f32 spot_cone_factor(const SpotCone& cone, f32 cos_angle);
// What a surface point is lit by, as a multiple of the light's colour (before the cosine of the surface's angle and the
// BRDF): the radius's window, the inverse square (at least the light's size), and the cone. The CPU twin of mesh.frag.
f32 spot_attenuation(const SpotLight& light, Vec3f point);
f32 point_attenuation(Vec3f position, f32 radius, f32 size, Vec3f point);

// A sphere that holds all the light of a spot (its cone out to its radius): for the view's culling and the importance.
void spot_bounding_sphere(const SpotLight& light, Vec3f& centre, f32& radius);

// The view a shadow map is drawn from (reverse Z, as the camera's and the sun's), and what the shader needs of it.
struct LocalShadowView {
    Mat4f view_projection{};
    f32 tan_half = 1.0f;   // the half angle's tangent, guard band included (a texel at distance d is 2 d tan_half / resolution)
    f32 near_plane = 0.05f;
    f32 far_plane = 10.0f;
};
// The view a spot light's map (`resolution` texels a side) is drawn from: from its position along its axis, a cone
// of the outer angle and the guard band. `far` and `near` come from LightShadow3D (see spot_shadow_far).
LocalShadowView spot_shadow_view(const SpotLight& light, u32 resolution);
// The far plane of a spot's or point light's map: its shadow distance, or the radius (never beyond it).
f32 local_shadow_far(const LightShadow3D& shadow, f32 radius);
// One face (0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z) of a point light's cube map.
LocalShadowView cube_face_view(const PointLight& light, u32 face, u32 resolution);
// The face a direction from the light falls in: the axis it is longest along, 2 * axis + (1 if negative).
u32 cube_face_of(Vec3f direction);

// How much a light matters to this picture: its intensity (the brightest component) times its reach (the radius) times
// the share of the view it fills (the square of its bounding sphere's angular radius over a right angle, at most 1;
// 1 from inside the sphere). For the choice of which lights get a shadow map.
f32 shadow_importance(f32 intensity, f32 reach, Vec3f centre, f32 sphere_radius, Vec3f eye);

// Which requests hold the slots, across frames. `key` names a light (0 is not a name); a light that held a slot last
// frame keeps it while it is among the most important `slots.held.size()` (its importance counted `hysteresis` times
// over), and the slots of lights that are not chosen are free again. Deterministic: ties go to the first request.
struct ShadowSlots {
    std::vector<u64> held; // by slot, 0: free
};
struct ShadowRequest {
    u64 key = 0;
    f32 importance = 0.0f;
};
// Fills `slot_of` with each request's slot, -1 for those not chosen.
void assign_shadow_slots(ShadowSlots& slots, std::span<const ShadowRequest> requests, f32 hysteresis, std::vector<i32>& slot_of);

} // namespace kin
