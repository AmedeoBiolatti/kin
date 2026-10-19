#include <algorithm>
#include <exception>
#include <kin/ecs/physics3d.hpp>
#include <kin/physics3d/assets3d.hpp>
#include <kin/scene/scene_loader3d.hpp>

namespace kin {

bool instantiate_scene3d(SceneAssetRuntime& source, EcsWorld& world, Assets3D& assets,
                         const SceneBindingOptions3D& options) {
    return source.instantiate3d(world, assets, options);
}

bool SceneAssetRuntime::instantiate3d(EcsWorld& world, Assets3D& assets, const SceneBindingOptions3D& options) {
    _diagnostics.clear();
    if (!loaded())
        return fail3d(_diagnostics, "instantiate_scene3d", "scene asset runtime is not loaded");
    if (ecs_is_deferred(world.raw().c_ptr()) || ecs_stage_is_readonly(world.raw().c_ptr()))
        return fail3d(_diagnostics, "instantiate_scene3d", "requires a writable world between ECS iterations");
    if (!scene()->script.empty())
        return fail3d(_diagnostics, "instantiate_scene3d", "requires a C++ scene; use ScriptScene for Lua lifecycle");
    if (!instantiate(world, {.update_scene_header = false}))
        return false;
    try {
        const auto commit = [&]() -> bool {
            if (options.update_scene_header)
                apply_scene_header(world, *scene());
            return true;
        };
        std::vector<EcsId> ids;
        ids.reserve(instance().by_authored_id.size());
        for (const auto& [name, entity] : instance().by_authored_id)
            ids.push_back(entity.id());
        // Empty selections mean ALL entities to the binders, so skip them for
        // an empty instance. Stable order keeps binding diagnostics repeatable.
        std::ranges::sort(ids);
        if (ids.empty())
            return commit();
        // Enforce the world's aggregate capacity before creating any particle
        // storage for this instance; scoped binding alone checks only its share.
        auto errors = validate_particles3d(world, options.particles);
        if (errors.empty())
            errors = resolve_render3d_assets(world, assets, options.particles, ids);
        if (errors.empty() && options.physics_assets)
            errors = resolve_physics3d_assets(world, *options.physics_assets, ids);
        if (errors.empty())
            return commit();
        clear(world);
        _diagnostics = std::move(errors);
        return false;
    } catch (const std::exception& error) {
        clear(world);
        return fail3d(_diagnostics, "instantiate_scene3d", error.what());
    }
}

} // namespace kin
