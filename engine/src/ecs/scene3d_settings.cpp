#include <algorithm>
#include <cmath>
#include <kin/ecs/scene3d.hpp>

namespace kin {

void register_scene3d_settings(EcsWorld& world) {
    auto& registry = world.components();
    if (!registry.find("SceneCamera3D"))
        registry.native<SceneCamera3D>("SceneCamera3D")
            .field("enabled", &SceneCamera3D::enabled)
            .field("priority", &SceneCamera3D::priority)
            .property<std::string>(
                "projection",
                [](const auto& value) {
                    return std::string(value.camera.projection == Projection3D::Perspective ? "perspective"
                                                                                            : "orthographic");
                },
                [](auto& value, const std::string& name) {
                    if (name == "perspective")
                        value.camera.projection = Projection3D::Perspective;
                    else if (name == "orthographic")
                        value.camera.projection = Projection3D::Orthographic;
                    else
                        return false;
                    return true;
                })
            .property<f32>(
                "fov", [](const auto& value) { return value.camera.fov; },
                [](auto& value, const f32& fov) {
                    if (!std::isfinite(fov) || fov <= 0 || fov >= 180)
                        return false;
                    value.camera.fov = fov;
                    return true;
                })
            .property<f32>(
                "height", [](const auto& value) { return value.camera.height; },
                [](auto& value, const f32& height) {
                    if (!std::isfinite(height) || height <= 0)
                        return false;
                    value.camera.height = height;
                    return true;
                })
            .property<f32>(
                "near", [](const auto& value) { return value.camera.z_near; },
                [](auto& value, const f32& near) {
                    if (!std::isfinite(near) || near <= 0)
                        return false;
                    value.camera.z_near = near;
                    return true;
                })
            .property<f32>(
                "far", [](const auto& value) { return std::isinf(value.camera.z_far) ? 0.0f : value.camera.z_far; },
                [](auto& value, const f32& far) {
                    if (!std::isfinite(far) || far < 0)
                        return false;
                    value.camera.z_far = far == 0 ? std::numeric_limits<f32>::infinity() : far;
                    return true;
                });

    if (!registry.find("SceneEnvironment3D")) {
        auto builder = registry.native<SceneEnvironment3D>("SceneEnvironment3D");
        builder.field("enabled", &SceneEnvironment3D::enabled).field("priority", &SceneEnvironment3D::priority);
        // These properties use exactly the same field codec/overrides as direct
        // members. Keep the authored names flat for ordinary inspector rows.
#define SETTING(name, member)                                                                                          \
    builder.property<decltype(SceneEnvironment3D{}.member)>(                                                           \
        name, [](const auto& value) { return value.member; },                                                          \
        [](auto& value, const auto& field) {                                                                           \
            value.member = field;                                                                                      \
            return true;                                                                                               \
        })
#define RGB_SETTING(name, member)                                                                                      \
    builder.property<Vec3f>(                                                                                           \
        name,                                                                                                          \
        [](const auto& value) {                                                                                        \
            const auto c = value.member;                                                                               \
            return Vec3f{c.r, c.g, c.b};                                                                               \
        },                                                                                                             \
        [](auto& value, const Vec3f& c) {                                                                              \
            if (c.x < 0 || c.y < 0 || c.z < 0)                                                                         \
                return false;                                                                                          \
            value.member = {c.x, c.y, c.z};                                                                            \
            return true;                                                                                               \
        })
        SETTING("sun_direction", lights.sun.direction);
        RGB_SETTING("sun_color", lights.sun.color);
        SETTING("fill_direction", lights.fill.direction);
        RGB_SETTING("fill_color", lights.fill.color);
        RGB_SETTING("ambient", lights.ambient);
        RGB_SETTING("ground_ambient", lights.ground_ambient.color);
        SETTING("ground_ambient_enabled", lights.ground_ambient.enabled);
        SETTING("environment_intensity", lights.environment_intensity);
        SETTING("environment_rotation", lights.environment_rotation);
        SETTING("sky_lights_scene", lights.sky_lights_scene);
        SETTING("shadow_min", lights.shadow_area.min);
        SETTING("shadow_max", lights.shadow_area.max);
        SETTING("shadow_resolution", lights.shadow_resolution);
        SETTING("shadow_cascades", lights.shadow_cascades);
        SETTING("shadow_distance", lights.shadow_distance);
        SETTING("sun_shadow_strength", lights.sun_shadow_strength);
        SETTING("shadow_bias", lights.shadow_bias);
        SETTING("shadow_normal_offset", lights.shadow_normal_offset);
        SETTING("shadow_receiver_plane_bias", lights.shadow_receiver_plane_bias);
        SETTING("fill_shadow", lights.fill_shadow.enabled);
        SETTING("fill_shadow_strength", lights.fill_shadow.strength);
        SETTING("fill_shadow_cascades", lights.fill_shadow.cascades);
        SETTING("fill_shadow_resolution", lights.fill_shadow.resolution);
        SETTING("fill_shadow_distance", lights.fill_shadow.distance);
        SETTING("cache_shadows", lights.cache_shadows);
        SETTING("max_shadowed_spots", lights.max_shadowed_spots);
        SETTING("max_shadowed_points", lights.max_shadowed_points);
        SETTING("local_shadow_resolution", lights.local_shadow_resolution);
        SETTING("sky_enabled", sky.enabled);
        builder.property<std::string>(
            "sky_mode",
            [](const auto& v) { return std::string(v.sky.mode == SkyMode3D::Image ? "image" : "procedural"); },
            [](auto& v, const std::string& mode) {
                if (mode == "image")
                    v.sky.mode = SkyMode3D::Image;
                else if (mode == "procedural")
                    v.sky.mode = SkyMode3D::Procedural;
                else
                    return false;
                return true;
            });
        SETTING("sky_turbidity", sky.turbidity);
        SETTING("sky_intensity", sky.intensity);
        SETTING("sky_sun_intensity", sky.sun_intensity);
        RGB_SETTING("sky_ground_color", sky.ground_color);
        SETTING("sky_sun_size", sky.sun_size);
        SETTING("sky_sun_disc", sky.sun_disc);
        RGB_SETTING("sky_night_color", sky.night_color);
        SETTING("sky_moon", sky.moon);
        SETTING("sky_moon_direction", sky.moon_direction);
        SETTING("sky_moon_size", sky.moon_size);
        SETTING("sky_moon_light", sky.moon_light);
        RGB_SETTING("sky_moon_color", sky.moon_color);
        SETTING("sky_light_threshold", sky.light_threshold);
        SETTING("sky_light_interval", sky.light_interval);
        SETTING("sky_stars", sky.stars);
        SETTING("sky_star_brightness", sky.star_brightness);
        SETTING("fog_enabled", fog.enabled);
        SETTING("fog_density", fog.density);
        SETTING("fog_height_density", fog.height_density);
        SETTING("fog_height_falloff", fog.height_falloff);
        SETTING("fog_base_height", fog.base_height);
        SETTING("fog_start", fog.start);
        SETTING("fog_max_opacity", fog.max_opacity);
        RGB_SETTING("fog_color", fog.colour);
        SETTING("fog_color_from_sky", fog.colour_from_sky);
        SETTING("fog_sun_scatter", fog.sun_scatter);
        SETTING("fog_sun_anisotropy", fog.sun_anisotropy);
        SETTING("ao_enabled", ambient_occlusion.enabled);
        SETTING("ao_radius", ambient_occlusion.radius);
        SETTING("ao_intensity", ambient_occlusion.intensity);
        SETTING("ao_half_resolution", ambient_occlusion.half_resolution);
        SETTING("contact_shadows", contact_shadows.enabled);
        SETTING("contact_distance", contact_shadows.distance);
        SETTING("contact_thickness", contact_shadows.thickness);
        SETTING("contact_intensity", contact_shadows.intensity);
        SETTING("contact_steps", contact_shadows.steps);
        SETTING("lod_enabled", lod.enabled);
        SETTING("lod_error_pixels", lod.error_pixels);
        SETTING("lod_bias", lod.bias);
        SETTING("lod_max_cluster_triangles", lod.max_cluster_triangles);
        SETTING("occlusion_culling", lod.occlusion.enabled);
        SETTING("occlusion_depth_margin", lod.occlusion.depth_margin);
        SETTING("occlusion_pixel_margin", lod.occlusion.pixel_margin);
        SETTING("taa_enabled", taa.enabled);
        SETTING("taa_blend", taa.blend);
        SETTING("taa_sharpness", taa.sharpness);
        SETTING("samples", view.samples);
        SETTING("background", view.background);
        SETTING("hdr", view.hdr);
        SETTING("exposure", view.exposure);
        SETTING("bloom", view.bloom);
        SETTING("bloom_threshold", view.bloom_threshold);
#undef SETTING
#undef RGB_SETTING
    }
    if (!registry.find("TimeOfDay3D"))
        registry.native<TimeOfDay3D>("TimeOfDay3D")
            .field("hours", &TimeOfDay3D::hours)
            .field("day_of_year", &TimeOfDay3D::day_of_year)
            .field("latitude", &TimeOfDay3D::latitude)
            .field("axial_tilt", &TimeOfDay3D::axial_tilt)
            .field("moon_phase", &TimeOfDay3D::moon_phase)
            .field("speed", &TimeOfDay3D::speed)
            .field("shadows", &TimeOfDay3D::shadows);
}

static std::vector<std::string> apply_scene3d_settings_impl(EcsWorld& world, Scene3D& scene, ViewOptions3D* view,
                                                            EcsEntity only) {
    std::vector<std::string> errors;
    const auto choose = [&]<class T>() {
        if (only)
            return only.has<T>() ? only : EcsEntity{};
        EcsEntity selected;
        i32 priority = std::numeric_limits<i32>::min();
        world.raw().each([&](flecs::entity entity, const T& settings) {
            if (settings.enabled && (!selected || settings.priority > priority ||
                                     (settings.priority == priority && entity.id() < selected.id()))) {
                selected = EcsEntity{entity};
                priority = settings.priority;
            }
        });
        return selected;
    };
    if (auto entity = choose.operator()<SceneCamera3D>()) {
        auto camera = entity.get<SceneCamera3D>()->camera;
        const auto* transform = entity.get<WorldTransform3D>();
        const bool finite_transform =
            transform && std::ranges::all_of(transform->matrix.m, [](f32 value) { return std::isfinite(value); });
        if (!finite_transform || !std::isfinite(camera.fov) || camera.fov <= 0 || camera.fov >= 180 ||
            !std::isfinite(camera.height) || camera.height <= 0 || !std::isfinite(camera.z_near) ||
            camera.z_near <= 0 || std::isnan(camera.z_far) || camera.z_far <= camera.z_near) {
            errors.push_back("SceneCamera3D entity '" + entity.name() +
                             "': requires a world transform and valid projection/near/far range");
        } else {
            camera.position = transform->position();
            const auto forward = transform->matrix.apply_vector({0, 0, -1});
            const auto up = transform->matrix.apply_vector({0, 1, 0});
            if (length_squared(forward) < 1e-12f || length_squared(cross(forward, up)) < 1e-12f)
                errors.push_back("SceneCamera3D entity '" + entity.name() + "': degenerate transform");
            else {
                camera.rotation = Quatf::look_rotation(forward, up);
                scene.camera = camera;
            }
        }
    }
    if (auto entity = choose.operator()<SceneEnvironment3D>()) {
        const auto& settings = *entity.get<SceneEnvironment3D>();
        // Validate cross-field ranges and direct C++ edits before applying.
        const auto snapshot = world.components().snapshot(entity, "SceneEnvironment3D");
        bool valid = true;
        if (snapshot)
            for (const auto& field : snapshot->fields) {
                if (const auto* scalar = std::get_if<f32>(&field.value))
                    valid &= std::isfinite(*scalar);
                if (const auto* vector = std::get_if<Vec3f>(&field.value))
                    valid &= detail::field_value_matches_kind(*vector, ComponentFieldKind::Vec3f);
            }
        const auto rgb_valid = [](LinearRgb c) {
            return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b) && c.r >= 0 && c.g >= 0 && c.b >= 0;
        };
        valid &= rgb_valid(settings.lights.sun.color) && rgb_valid(settings.lights.fill.color) &&
                 rgb_valid(settings.lights.ambient) && rgb_valid(settings.lights.ground_ambient.color) &&
                 rgb_valid(settings.sky.ground_color) && rgb_valid(settings.sky.moon_color) &&
                 rgb_valid(settings.sky.night_color) && rgb_valid(settings.fog.colour);
        valid &= settings.sky.mode <= SkyMode3D::Image && settings.sky.turbidity >= 1 && settings.sky.turbidity <= 20 &&
                 settings.sky.sun_intensity >= 0 && settings.sky.intensity >= 0 && settings.sky.sun_size >= 0 &&
                 settings.sky.sun_size <= 180 && settings.sky.sun_disc >= 0 && settings.sky.star_brightness >= 0 &&
                 settings.sky.moon_size >= 0 && settings.sky.moon_size <= 180 && settings.sky.moon_light >= 0 &&
                 settings.sky.light_threshold >= 0 &&
                 (!settings.sky.moon || length_squared(settings.sky.moon_direction) > 1e-12f);
        valid &= settings.fog.density >= 0 && settings.fog.height_density >= 0 && settings.fog.height_falloff >= 0 &&
                 settings.fog.start >= 0 && settings.fog.max_opacity >= 0 && settings.fog.max_opacity <= 1 &&
                 std::abs(settings.fog.sun_anisotropy) < 1 && settings.lights.environment_intensity >= 0 &&
                 settings.lights.shadow_cascades >= 1 && settings.lights.shadow_cascades <= 4 &&
                 settings.lights.max_shadowed_spots <= 8 && settings.lights.max_shadowed_points <= 2 &&
                 settings.lights.shadow_resolution >= 64 && settings.lights.shadow_resolution <= 8192 &&
                 settings.lights.local_shadow_resolution >= 64 && settings.lights.local_shadow_resolution <= 8192 &&
                 settings.lights.sun_shadow_strength >= 0 && settings.lights.sun_shadow_strength <= 1 &&
                 settings.lights.shadow_bias >= 0 && settings.lights.shadow_normal_offset >= 0 &&
                 settings.lights.fill_shadow.strength >= 0 && settings.lights.fill_shadow.strength <= 1 &&
                 settings.lights.fill_shadow.cascades >= 1 && settings.lights.fill_shadow.cascades <= 4 &&
                 (settings.lights.fill_shadow.resolution == 0 ||
                  (settings.lights.fill_shadow.resolution >= 64 && settings.lights.fill_shadow.resolution <= 8192)) &&
                 settings.lights.fill_shadow.distance >= 0 && settings.lod.error_pixels > 0 && settings.lod.bias > 0 &&
                 settings.lod.max_cluster_triangles > 0 && settings.lod.occlusion.depth_margin >= 0 &&
                 settings.view.exposure >= 0 && settings.view.bloom >= 0 && settings.view.bloom_threshold >= 0 &&
                 settings.ambient_occlusion.radius > 0 && settings.ambient_occlusion.intensity >= 0 &&
                 settings.contact_shadows.distance >= 0 && settings.contact_shadows.thickness > 0 &&
                 settings.contact_shadows.intensity >= 0 && settings.taa.blend >= 0 && settings.taa.blend <= .98f &&
                 settings.taa.sharpness >= 0 && settings.taa.sharpness <= 1 &&
                 (settings.view.samples == 1 || settings.view.samples == 2 || settings.view.samples == 4 ||
                  settings.view.samples == 8);
        if (!valid)
            errors.push_back("SceneEnvironment3D entity '" + entity.name() + "': invalid environment/view settings");
        else {
            // Preserve local lights when updating scene settings between views.
            auto points = std::move(scene.lights.points);
            auto spots = std::move(scene.lights.spots);
            scene.lights = settings.lights;
            scene.lights.points = std::move(points);
            scene.lights.spots = std::move(spots);
            scene.sky = settings.sky;
            scene.fog = settings.fog;
            scene.ambient_occlusion = settings.ambient_occlusion;
            scene.contact_shadows = settings.contact_shadows;
            scene.taa = settings.taa;
            scene.lod = settings.lod;
            if (view)
                *view = scene3d_view_options(settings.view, *view);
        }
    }
    return errors;
}
Status3D apply_scene3d_settings(EcsWorld& world, Scene3D& scene, ViewOptions3D* view, EcsEntity only) {
    auto diagnostics = apply_scene3d_settings_impl(world, scene, view, only);
    const bool accepted = diagnostics.empty();
    return status3d(accepted, std::move(diagnostics));
}

} // namespace kin
