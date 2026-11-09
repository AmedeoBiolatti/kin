#pragma once
// A world's reflection probes (the ReflectionProbe3D component, kin/ecs/render3d.hpp): captured when they need it and
// placed in the scene's lights (Lights3D::reflection_probes), each frame, after the scene is composed and before the
// pictures that read it (SceneSession3D does it; a game that renders scene() itself runs one).
#include <filesystem>
#include <kin/ecs/world.hpp>
#include <kin/render3d/errors3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <memory>

namespace kin {

class ReflectionProbes3D {
public:
    // `content`: where baked pictures' paths (ReflectionProbe3D::baked) start (empty: as given).
    explicit ReflectionProbes3D(Renderer3D& renderer, std::filesystem::path content = {});
    ~ReflectionProbes3D();
    ReflectionProbes3D(ReflectionProbes3D&&) noexcept;
    ReflectionProbes3D& operator=(ReflectionProbes3D&&) noexcept;

    // Lists `world`'s enabled probes in `scene.lights.reflection_probes` and captures those that need it (first shown,
    // moved, resized, `recapture` changed, their `interval` past by Scene3D::time, or `every_frame`). The captures are
    // six renders each of `scene` as it is (its camera put back after).
    //
    // `lighting`: the view whose global illumination lights the captures (ReflectionCaptureOptions3D's; the scene's own
    // view: SceneSession3D passes its). Its GI takes renders to settle: a probe captured before it had had light for
    // `gi_settle` updates is captured once more when it has.
    Status3D update(EcsWorld& world, Scene3D& scene, const View3D* lighting = nullptr);
    u32 gi_settle = 60;
    // Captures every enabled probe of `world` now (each seeing the others as they stand) and writes its picture to
    // `content` / `folder` / <its entity's authored id, name or id>.hdr, setting its `baked` to that path in the
    // content (save the scene to keep it). The probes then load those pictures instead of capturing at run time.
    Status3D bake(EcsWorld& world, Scene3D& scene, const std::filesystem::path& content,
                  const std::filesystem::path& folder = "probes", const View3D* lighting = nullptr);
    // The probes the last update placed and the captures it made.
    u32 probes() const;
    u32 captures() const;

private:
    struct Data;
    std::unique_ptr<Data> _data;
};

} // namespace kin
