#include <kin/render3d/shadow_cascades.hpp>

#include <algorithm>
#include <cmath>

namespace kin {

namespace {

// The portion of a cascade's range, at its end, over which it fades into the next.
constexpr f32 blend_share = 0.1f;

} // namespace

std::array<f32, max_shadow_cascades + 1> cascade_splits(f32 near, f32 far, u32 count, f32 lambda) {
    std::array<f32, max_shadow_cascades + 1> splits{};
    count = std::clamp(count, 1u, max_shadow_cascades);
    near = std::max(near, 1e-4f);
    far = std::max(far, near);
    for (u32 i = 0; i <= count; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(count);
        const f32 log = near * std::pow(far / near, t);
        const f32 linear = near + (far - near) * t;
        splits[i] = lambda * log + (1.0f - lambda) * linear;
    }
    splits[0] = near;
    splits[count] = far;
    return splits;
}

Camera3D slice_of(const Camera3D& camera, f32 near, f32 far) {
    Camera3D slice = camera;
    slice.z_near = near;
    slice.z_far = far;
    return slice;
}

ShadowCascades fit_shadow_cascades(const Camera3D& camera, f32 aspect, Vec3f to_sun, const Bounds3& area, u32 cascades,
                                   f32 distance, u32 resolution) {
    ShadowCascades out;
    const Vec3f area_size = area.size();
    if (!(area_size.x > 0 && area_size.y >= 0 && area_size.z > 0) || length_squared(to_sun) <= 0.0f || resolution == 0) {
        return out;
    }
    to_sun = normalize(to_sun);
    const f32 near = std::max(camera.z_near, 1e-4f);
    const f32 far = std::max(std::min(distance, camera.z_far), near * 1.001f);
    out.count = std::clamp(cascades, 1u, max_shadow_cascades);
    const auto splits = cascade_splits(near, far, out.count);

    // The sun's space is fixed by the area, not the camera: a texel grid in
    // it stays put in the world, which is what snapping needs.
    const Vec3f center = area.center();
    const f32 radius = std::max(area.radius(), 0.01f);
    const Vec3f up = std::abs(to_sun.y) > 0.99f ? Vec3f{0.0f, 0.0f, -1.0f} : Vec3f{0.0f, 1.0f, 0.0f};
    const Mat4f light_view = Mat4f::look_at(center + to_sun * (radius + 1.0f), center, up);
    Vec3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < 8; ++i) {
        const Vec3f corner{(i & 1) ? area.max.x : area.min.x, (i & 2) ? area.max.y : area.min.y, (i & 4) ? area.max.z : area.min.z};
        const Vec3f p = light_view.apply(corner);
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const f32 z_near = std::max(0.01f, -hi.z - 0.5f), z_far = -lo.z + 0.5f;

    const bool ortho = camera.projection == Projection3D::Orthographic;
    const f32 half_tan = std::tan(camera.fov * 0.5f * 3.14159265f / 180.0f);
    // The half height and width of the view at depth `d`.
    const auto half_height = [&](f32 d) { return ortho ? camera.height * 0.5f : d * half_tan; };

    for (u32 k = 0; k < out.count; ++k) {
        ShadowCascade& c = out.cascade[k];
        // Each fades into the next over its last tenth, so the next must
        // cover from where that starts.
        c.slice_far = splits[k + 1];
        c.blend_from = c.slice_far - (c.slice_far - splits[k]) * blend_share;
        if (k == out.count - 1) {
            c.blend_from = c.slice_far; // nothing after it
        }
        c.slice_near = k == 0 ? splits[0] : out.cascade[k - 1].blend_from;

        // The sphere about the slice, centred on the view's axis: equally far
        // from a corner of the near end and of the far end (or at the nearer
        // end if the far end's corner is the closest it can be).
        const f32 n = c.slice_near, f = c.slice_far;
        const f32 hn = half_height(n), hf = half_height(f);
        const f32 rn2 = hn * hn * (1.0f + aspect * aspect), rf2 = hf * hf * (1.0f + aspect * aspect);
        const f32 axis = std::clamp((rf2 + f * f - rn2 - n * n) / (2.0f * (f - n)), n, f);
        f32 r = std::sqrt(std::max(rf2 + (f - axis) * (f - axis), rn2 + (axis - n) * (axis - n)));
        // Rounded up so tiny changes (a fov that breathes) don't remake the grid each frame.
        r = std::ceil(r * 16.0f) / 16.0f;
        const Vec3f at = light_view.apply(camera.position + camera.forward() * axis);

        // Per axis: the sphere's box snapped to texels, unless the area is
        // smaller than it (then the area itself, which never moves).
        f32 box_lo[2], box_hi[2], texel[2];
        const f32 centre[2] = {at.x, at.y}, area_lo[2] = {lo.x, lo.y}, area_hi[2] = {hi.x, hi.y};
        for (int a = 0; a < 2; ++a) {
            if (area_hi[a] - area_lo[a] <= 2.0f * r) {
                box_lo[a] = area_lo[a];
                box_hi[a] = area_hi[a];
                texel[a] = (box_hi[a] - box_lo[a]) / static_cast<f32>(resolution);
            } else {
                texel[a] = 2.0f * r / static_cast<f32>(resolution);
                const f32 snapped = std::floor(centre[a] / texel[a] + 0.5f) * texel[a];
                box_lo[a] = snapped - r;
                box_hi[a] = snapped + r;
            }
        }
        c.view_projection = Mat4f::orthographic(box_lo[0], box_hi[0], box_lo[1], box_hi[1], z_near, z_far) * light_view;
        c.texel_world = std::max(texel[0], texel[1]);
        c.depth_range = std::max(z_far - z_near, 0.01f);
    }
    return out;
}

} // namespace kin
