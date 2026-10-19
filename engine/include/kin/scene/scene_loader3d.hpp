#pragma once

#include <kin/ecs/scene3d.hpp>
#include <kin/scene/scene_asset_runtime.hpp>

namespace kin {

class PhysicsAssets3D;

struct SceneBindingOptions3D {
    ParticleLimits3D particles;
    PhysicsAssets3D* physics_assets = nullptr;
    // Additive loading leaves the host world's scene header alone by default.
    bool update_scene_header = false;
};

// Finish SceneAssetRuntime::load in a caller-owned world. Binds ONLY the new
// instance, without creating physics, render state or a view. Failure removes
// its new entities; existing entities/bindings stay intact. The caller owns the
// runtime and explicitly calls runtime.clear(world) to remove the instance.
// Rejects scripted scenes; use ScriptScene for Lua lifecycle. Renderer thread.
// False reports the failure through source.diagnostics(), like load/instantiate.
bool instantiate_scene3d(SceneAssetRuntime& source, EcsWorld& world, Assets3D& assets,
                         const SceneBindingOptions3D& options = {});

} // namespace kin
