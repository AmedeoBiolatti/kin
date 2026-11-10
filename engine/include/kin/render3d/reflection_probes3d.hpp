#pragma once
// Reflection probes (docs/rendering_3d.md, "Reflection probes"): the scene seen all round from a point, captured into a
// picture filtered as the environment is (a level a roughness), which glossy surfaces inside the probe's box reflect in
// place of the environment. The reflection is parallax-corrected: the reflected ray meets the probe's parallax box (a
// room's walls, say) and the probe is read toward that point from where it was captured, so a floor reflects the room's
// far wall where it is, not as if it were infinitely far away.
//
//   kin::ReflectionCapture3D hall = r3d.create_reflection_capture().value_or({});
//   r3d.capture_reflection(hall, scene, {0, 2, 0});          // once, or when the room changes
//   scene.lights.reflection_probes.push_back({.position = {0, 2, 0}, .extent = {8, 3, 6}, .capture = hall});
//
// Scenes made of entities use the ReflectionProbe3D component instead (kin/ecs/render3d.hpp): SceneSession3D captures
// and places them.
#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <memory>
#include <optional>

namespace kin {

class View3D;

// The backend's copy of a capture: the six views' picture, as a panorama.
class IReflectionCaptureBackend3D {
public:
    virtual ~IReflectionCaptureBackend3D() = default;
    // Changes each time it is captured again (the renderer filters it anew).
    virtual u64 generation() const = 0;
};

// A probe's picture (Renderer3D::create_reflection_capture, ::capture_reflection); cheap to copy (copies share it).
// Empty where 3D, or capturing, is not available.
class ReflectionCapture3D {
public:
    ReflectionCapture3D() = default;
    explicit ReflectionCapture3D(std::shared_ptr<IReflectionCaptureBackend3D> backend) : _backend(std::move(backend)) {}
    bool valid() const { return _backend != nullptr; }
    explicit operator bool() const { return valid(); }
    const std::shared_ptr<IReflectionCaptureBackend3D>& backend() const { return _backend; }
    friend bool operator==(const ReflectionCapture3D& a, const ReflectionCapture3D& b) {
        return a._backend == b._backend;
    }

private:
    std::shared_ptr<IReflectionCaptureBackend3D> _backend;
};

// How capture_reflection sees the scene from the probe.
struct ReflectionCaptureOptions3D {
    f32 z_near = 0.05f; // what is nearer to the probe is not in its picture
    f32 z_far = 0.0f;   // 0: the scene camera's
    // The copies left out of its picture (Camera3D::hidden_id): a shiny thing's own, for the probe at its centre.
    u32 hidden_id = 0;
    // A view whose global illumination lights the capture (its probes as they stand, read, not moved: they are round
    // that view's camera, so a probe inside their reach is lit as the view sees the scene lit). Without one, or where
    // that view's GI has no light yet, the capture has none (it is a view's own, and takes renders to settle).
    const View3D* global_illumination = nullptr;
    // The faces rendered (bits 0 to 5: +X, -X, +Y, -Y, +Z, -Z), the others kept as last captured: a live probe can
    // render a few a frame, in turn (ReflectionProbe3D::faces_per_frame). With `publish` false the faces are only kept,
    // and the probe's picture (and its filtering) stays the last published one until a capture publishes them.
    u32 faces = 0x3F;
    bool publish = true;
};

// A probe placed in the scene (Lights3D::reflection_probes). Its boxes share a centre and a rotation:
// `position + box_offset`, turned by `rotation`.
struct ReflectionProbeSource3D {
    Vec3f position{}; // where it was captured from (capture_reflection's)
    Quatf rotation{}; // the boxes'
    Vec3f box_offset{};
    // The influence box (half extents): the surfaces inside it reflect the probe; it fades out over `blend` inside its
    // faces, toward the probes it overlaps or the environment.
    Vec3f extent{5.0f, 5.0f, 5.0f};
    f32 blend = 0.5f;
    // The parallax box (half extents; zero: the influence box's): what the probe saw is taken to lie on its faces. Off:
    // what it saw is taken to be infinitely far, as the environment is (a sphere's own probe, the sky).
    Vec3f parallax_extent{};
    bool parallax = true;
    f32 intensity = 1.0f;
    // Diffuse light too: the surfaces inside the box are lit by what the probe saw all round (its picture's irradiance,
    // as an environment's), in place of the environment's or the hemisphere's ambient light; global illumination's
    // probes still win where they reach. Off: reflections only.
    bool diffuse = true;
    // Where boxes overlap, the higher first; equal, the smaller first.
    i32 priority = 0;
    ReflectionCapture3D capture;
};

// At most this many probes light a render: those whose influence boxes reach the camera's view, by priority, then the
// nearest. A surface blends at most every probe whose box holds it.
constexpr u32 max_reflection_probes3d = 16;

// How much `probe` reflects at `point` (0..1): 1 well inside its influence box, falling to 0 at its faces over `blend`.
f32 reflection_probe_weight(const ReflectionProbeSource3D& probe, Vec3f point);
// The way to read `probe`'s picture for a ray from `point` along `direction` (unit): toward where the ray leaves the
// parallax box, from where the probe was captured (or `direction` itself without parallax). Nothing when `point` is
// outside the parallax box.
std::optional<Vec3f> reflection_probe_direction(const ReflectionProbeSource3D& probe, Vec3f point, Vec3f direction);

} // namespace kin
