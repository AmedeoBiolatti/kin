#include <kin/ecs/scene3d.hpp>

#include <algorithm>
#include <cmath>

namespace kin {

void register_scene3d_settings(EcsWorld& world) {
    auto& registry = world.components();
    if (!registry.find("SceneCamera3D")) registry.native<SceneCamera3D>("SceneCamera3D")
        .field("enabled", &SceneCamera3D::enabled).field("priority", &SceneCamera3D::priority)
        .property<std::string>("projection", [](const auto& value) { return std::string(value.camera.projection == Projection3D::Perspective ? "perspective" : "orthographic"); },
            [](auto& value, const std::string& name) {
                if (name == "perspective") value.camera.projection = Projection3D::Perspective;
                else if (name == "orthographic") value.camera.projection = Projection3D::Orthographic;
                else return false;
                return true;
            })
        .property<f32>("fov", [](const auto& value) { return value.camera.fov; }, [](auto& value, const f32& fov) {
            if (!std::isfinite(fov) || fov <= 0 || fov >= 180) return false;
            value.camera.fov = fov; return true;
        })
        .property<f32>("height", [](const auto& value) { return value.camera.height; }, [](auto& value, const f32& height) {
            if (!std::isfinite(height) || height <= 0) return false;
            value.camera.height = height; return true;
        })
        .property<f32>("near", [](const auto& value) { return value.camera.z_near; }, [](auto& value, const f32& near) {
            if (!std::isfinite(near) || near <= 0) return false;
            value.camera.z_near = near; return true;
        })
        .property<f32>("far", [](const auto& value) { return std::isinf(value.camera.z_far) ? 0.0f : value.camera.z_far; }, [](auto& value, const f32& far) {
            if (!std::isfinite(far) || far < 0) return false;
            value.camera.z_far = far == 0 ? std::numeric_limits<f32>::infinity() : far; return true;
        });

    if (!registry.find("SceneEnvironment3D")) {
        auto builder = registry.native<SceneEnvironment3D>("SceneEnvironment3D");
        builder.field("enabled", &SceneEnvironment3D::enabled).field("priority", &SceneEnvironment3D::priority);
        // These properties use exactly the same field codec/overrides as direct
        // members. Keep the authored names flat for ordinary inspector rows.
#define SETTING(name, member) builder.property<decltype(SceneEnvironment3D{}.member)>(name, \
    [](const auto& value) { return value.member; }, [](auto& value, const auto& field) { value.member = field; return true; })
        SETTING("sun_direction", lights.sun.direction); SETTING("sun_color", lights.sun.color);
        SETTING("fill_direction", lights.fill.direction); SETTING("fill_color", lights.fill.color);
        SETTING("ambient", lights.ambient);
        SETTING("environment_intensity", lights.environment_intensity); SETTING("environment_rotation", lights.environment_rotation);
        SETTING("sky_lights_scene", lights.sky_lights_scene);
        SETTING("shadow_min", lights.shadow_area.min); SETTING("shadow_max", lights.shadow_area.max);
        SETTING("shadow_resolution", lights.shadow_resolution); SETTING("shadow_cascades", lights.shadow_cascades);
        SETTING("shadow_distance", lights.shadow_distance); SETTING("cache_shadows", lights.cache_shadows);
        SETTING("max_shadowed_spots", lights.max_shadowed_spots); SETTING("max_shadowed_points", lights.max_shadowed_points);
        SETTING("local_shadow_resolution", lights.local_shadow_resolution);
        SETTING("sky_enabled", sky.enabled); SETTING("sky_turbidity", sky.turbidity);
        SETTING("sky_intensity", sky.intensity); SETTING("sky_sun_intensity", sky.sun_intensity);
        SETTING("sky_ground_color", sky.ground_color); SETTING("sky_stars", sky.stars);
        SETTING("sky_star_brightness", sky.star_brightness);
        SETTING("fog_enabled", fog.enabled); SETTING("fog_density", fog.density);
        SETTING("fog_height_density", fog.height_density); SETTING("fog_height_falloff", fog.height_falloff);
        SETTING("fog_base_height", fog.base_height); SETTING("fog_start", fog.start);
        SETTING("fog_max_opacity", fog.max_opacity); SETTING("fog_color", fog.colour);
        SETTING("fog_color_from_sky", fog.colour_from_sky); SETTING("fog_sun_scatter", fog.sun_scatter);
        SETTING("fog_sun_anisotropy", fog.sun_anisotropy);
        SETTING("ao_enabled", ambient_occlusion.enabled); SETTING("ao_radius", ambient_occlusion.radius);
        SETTING("ao_intensity", ambient_occlusion.intensity); SETTING("ao_half_resolution", ambient_occlusion.half_resolution);
        SETTING("contact_shadows", contact_shadows.enabled); SETTING("contact_distance", contact_shadows.distance);
        SETTING("contact_thickness", contact_shadows.thickness); SETTING("contact_intensity", contact_shadows.intensity);
        SETTING("taa_enabled", taa.enabled); SETTING("taa_blend", taa.blend); SETTING("taa_sharpness", taa.sharpness);
        SETTING("samples", view.samples); SETTING("background", view.background); SETTING("hdr", view.hdr);
        SETTING("exposure", view.exposure); SETTING("bloom", view.bloom); SETTING("bloom_threshold", view.bloom_threshold);
#undef SETTING
    }
    if (!registry.find("TimeOfDay3D")) registry.native<TimeOfDay3D>("TimeOfDay3D")
        .field("hours", &TimeOfDay3D::hours).field("day_of_year", &TimeOfDay3D::day_of_year)
        .field("latitude", &TimeOfDay3D::latitude).field("axial_tilt", &TimeOfDay3D::axial_tilt)
        .field("moon_phase", &TimeOfDay3D::moon_phase).field("speed", &TimeOfDay3D::speed)
        .field("shadows", &TimeOfDay3D::shadows);
}

std::vector<std::string> apply_scene3d_settings(EcsWorld& world, Scene3D& scene, View3DOptions* view, EcsEntity only) {
    std::vector<std::string> errors;
    const auto choose = [&]<class T>() {
        if (only) return only.has<T>() ? only : EcsEntity{};
        EcsEntity selected;
        i32 priority = std::numeric_limits<i32>::min();
        world.raw().each([&](flecs::entity entity, const T& settings) {
            if (settings.enabled && (!selected || settings.priority > priority ||
                (settings.priority == priority && entity.id() < selected.id()))) {
                selected = EcsEntity{entity}; priority = settings.priority;
            }
        });
        return selected;
    };
    if (auto entity = choose.operator()<SceneCamera3D>()) {
        auto camera = entity.get<SceneCamera3D>()->camera;
        const auto* transform = entity.get<WorldTransform3D>();
        const bool finite_transform = transform && std::ranges::all_of(transform->matrix.m, [](f32 value) { return std::isfinite(value); });
        if (!finite_transform || !std::isfinite(camera.fov) || camera.fov <= 0 || camera.fov >= 180 ||
            !std::isfinite(camera.height) || camera.height <= 0 || !std::isfinite(camera.z_near) || camera.z_near <= 0 ||
            std::isnan(camera.z_far) || camera.z_far <= camera.z_near) {
            errors.push_back("SceneCamera3D entity '" + entity.name() + "': requires a world transform and valid projection/near/far range");
        } else {
            camera.position = transform->position();
            const auto forward = transform->matrix.apply_vector({0,0,-1});
            const auto up = transform->matrix.apply_vector({0,1,0});
            if (length_squared(forward) < 1e-12f || length_squared(cross(forward, up)) < 1e-12f)
                errors.push_back("SceneCamera3D entity '" + entity.name() + "': degenerate transform");
            else { camera.rotation = Quatf::look_rotation(forward, up); scene.camera = camera; }
        }
    }
    if (auto entity = choose.operator()<SceneEnvironment3D>()) {
        const auto& settings = *entity.get<SceneEnvironment3D>();
        // Validate cross-field ranges and direct C++ edits before applying.
        const auto snapshot = world.components().snapshot(entity, "SceneEnvironment3D");
        bool valid = true;
        if (snapshot) for (const auto& field : snapshot->fields) {
            if (const auto* scalar = std::get_if<f32>(&field.value)) valid &= std::isfinite(*scalar);
            if (const auto* vector = std::get_if<Vec3f>(&field.value)) valid &= detail::field_value_matches_kind(*vector, ComponentFieldKind::Vec3f);
        }
        valid &= settings.fog.density >= 0 && settings.fog.height_density >= 0 && settings.fog.height_falloff >= 0 &&
            settings.fog.start >= 0 && settings.fog.max_opacity >= 0 && settings.fog.max_opacity <= 1 &&
            std::abs(settings.fog.sun_anisotropy) < 1 && settings.lights.environment_intensity >= 0 &&
            settings.lights.shadow_cascades >= 1 && settings.lights.shadow_cascades <= 4 &&
            settings.lights.max_shadowed_spots <= 8 && settings.lights.max_shadowed_points <= 2 &&
            settings.lights.shadow_resolution >= 64 && settings.lights.shadow_resolution <= 8192 &&
            settings.lights.local_shadow_resolution >= 64 && settings.lights.local_shadow_resolution <= 8192 &&
            settings.view.exposure >= 0 && settings.view.bloom >= 0 && settings.view.bloom_threshold >= 0 &&
            settings.ambient_occlusion.radius > 0 && settings.ambient_occlusion.intensity >= 0 &&
            settings.contact_shadows.distance >= 0 && settings.contact_shadows.thickness > 0 && settings.contact_shadows.intensity >= 0 &&
            settings.taa.blend >= 0 && settings.taa.blend <= .98f && settings.taa.sharpness >= 0 && settings.taa.sharpness <= 1 &&
            (settings.view.samples == 1 || settings.view.samples == 2 || settings.view.samples == 4 || settings.view.samples == 8);
        if (!valid) errors.push_back("SceneEnvironment3D entity '" + entity.name() + "': invalid environment/view settings");
        else {
            // Preserve local lights when updating scene settings between views.
            auto points = std::move(scene.lights.points); auto spots = std::move(scene.lights.spots);
            scene.lights = settings.lights;
            scene.lights.points = std::move(points); scene.lights.spots = std::move(spots);
            scene.sky = settings.sky; scene.fog = settings.fog;
            scene.ambient_occlusion = settings.ambient_occlusion; scene.contact_shadows = settings.contact_shadows; scene.taa = settings.taa;
            if (view) *view = settings.view;
        }
    }
    return errors;
}

} // namespace kin
