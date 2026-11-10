#pragma once

// Mirrors and portals on entities (docs/rendering_3d.md, Mirrors and portals): PlanarViews3D draws every Mirror3D and
// Portal3D (kin/ecs/render3d.hpp) of a world into a collected Scene3D and renders the pictures they show, each into a
// view of its own; portal_step moves things through the world's portals. SceneSession3D does both for its scene.

#include <functional>
#include <kin/core/math3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/render3d/camera3d.hpp>
#include <kin/render3d/errors3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

class WorldPhysicsState3D;

// Things going through portals (PortalTraveller3D): after each physics step, each traveller whose point crossed an
// opening since the step before is put at the exit, turned (WorldPhysicsState3D::carry for a character or a body).
// `crossings()` are the last step's: who, through which portal, and the turn (a camera following it turns with it).
class PortalTravel3D {
public:
    struct Crossing {
        EcsEntity traveller, portal, exit;
        Quatf turn{};
    };
    Status3D step(EcsWorld& world, WorldPhysicsState3D* physics);
    std::span<const Crossing> crossings() const { return _crossings; }

private:
    std::unordered_map<u64, Vec3f> _points; // each traveller's point after the step before
    std::vector<Crossing> _crossings;
};

// kin's planar surfaces (engine/shaders/planar_reflection.surface.glsl and planar_window.surface.glsl): not in the
// engine (a surface's variants are about a megabyte), but built by the game that uses them. Without them, mirrors are
// drawn as kin's own metal and portals as their stand-in colour.
struct PlanarSurfaces3D {
    SurfaceShader3D reflection, window;
    bool valid() const { return reflection.valid() && window.valid(); }
};
// From their variants as kin_planar_surfaces(<target>) (cmake/kin_embed.cmake) builds and embeds them: `spirv` gives
// the bytes of a file by name ("planar_reflection.plain.spv" ...; the embedded `planar_surfaces` function).
Result3D<PlanarSurfaces3D> create_planar_surfaces(Renderer3D& renderer,
                                                  const std::function<std::span<const u8>(std::string_view)>& spirv);
// Compiled now by glslc from kin's shader folder: development builds on a machine with glslc only.
Result3D<PlanarSurfaces3D> compile_planar_surfaces(Renderer3D& renderer);

class PlanarViews3D {
public:
    explicit PlanarViews3D(Renderer3D& renderer, PlanarSurfaces3D surfaces = {});
    ~PlanarViews3D();
    PlanarViews3D(PlanarViews3D&&) noexcept;
    PlanarViews3D& operator=(PlanarViews3D&&) noexcept;

    void set_surfaces(PlanarSurfaces3D surfaces);
    // Draws the surfaces of `world`'s enabled Mirror3D and Portal3D entities (placed by their RenderTransform3D when
    // enabled, else WorldTransform3D) into `scene`, after the rest of it is collected and with its camera the view's,
    // and renders the pictures they need: a mirror's when it is on screen, a portal's level by level while the level
    // before sees it (Portal3D::depth), deepest first, each with the other planar surfaces as stand-ins. `scene` is
    // then ready for the view: render it. `view_size` is the view's, `like` its options (the pictures keep its
    // background; one sample, no bloom, no picking, and not HDR: linear light clamped at white, which an HDR view
    // tonemaps once with the rest of its picture). Views are kept from one call to the next,
    // one a mirror and one a portal's level, made again when the size changes, and dropped with their entities.
    // `lit_by`: the view the scene is then rendered into, whose global illumination lights the pictures too (as it
    // stands: Renderer3D::render(view, scene, lit_by)); without it, a scene with GI has its pictures make their own.
    Status3D render(EcsWorld& world, Scene3D& scene, Vec2i view_size, const ViewOptions3D& like = {},
                    const View3D* lit_by = nullptr);
    // The last render's planar views, and what it skipped (surfaces not on screen, or behind the camera).
    u32 views_rendered() const;
    u32 views_skipped() const;
    // Drops every view.
    void clear();

private:
    struct Data;
    std::unique_ptr<Data> _data;
};

// A step from `from` to `to` (a player's, a body's, a frame's) through `world`'s portals: where it ends (the rest of
// the step taken out of the exit) and the turn to give its facing and velocity, when it goes into an enabled Portal3D's
// front within its opening; nothing otherwise. Placed by WorldTransform3D (call after propagating transforms).
struct PortalStep3D {
    EcsEntity portal, exit;
    Vec3f position;
    Quatf turn;
};
std::optional<PortalStep3D> portal_step(EcsWorld& world, Vec3f from, Vec3f to);
// An entity's portal as the helpers of camera3d.hpp take it: its placing and opening (scaled), and its exit's.
std::optional<std::pair<PortalPose3D, PortalPose3D>> portal_poses(EcsEntity portal);

} // namespace kin
