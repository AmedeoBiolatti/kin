#include <algorithm>
#include <cmath>
#include <kin/core/jobs.hpp>
#include <kin/ecs/scene3d.hpp>
#include <kin/platform/log.hpp>
#include <kin/scene/baked_lighting_scene3d.hpp>
#include <limits>
#include <memory>

namespace kin {

namespace {

// The active SceneEnvironment3D, as apply_scene3d_settings chooses it: the enabled one of the highest priority, ties
// to the lowest id.
flecs::entity active_environment(EcsWorld& world) {
    flecs::entity selected;
    i32 priority = std::numeric_limits<i32>::min();
    world.each([&](flecs::entity entity, const SceneEnvironment3D& settings) {
        if (settings.enabled && (!selected || settings.priority > priority ||
                                 (settings.priority == priority && entity.id() < selected.id()))) {
            selected = entity;
            priority = settings.priority;
        }
    });
    return selected;
}

// Materials' pictures read back once each (their colours as stored), for the bake to read.
struct Pictures {
    Renderer3D& renderer;
    std::vector<std::pair<Texture, std::unique_ptr<Renderer3D::TextureColors3D>>> read;

    BakeTexture3D of(const Texture& texture, const Material3D& m, MaterialMap3D map) {
        if (!texture)
            return {};
        auto found = std::find_if(read.begin(), read.end(), [&](const auto& entry) { return entry.first == texture; });
        if (found == read.end()) {
            auto colors = renderer.read_texture_colors(texture, 256);
            read.emplace_back(texture, std::make_unique<Renderer3D::TextureColors3D>(
                                           colors ? std::move(*colors) : Renderer3D::TextureColors3D{}));
            found = read.end() - 1;
        }
        const auto& slot = found->second;
        if (slot->rgba.empty())
            return {};
        // (A map's own sampler where it has one, else the material's wrap.)
        const auto& sampler = m.texture_samplers[u32(map)];
        const Wrap3D wrap = sampler ? sampler->wrap_u : m.wrap_u;
        return {slot->rgba, slot->size, true, wrap != Wrap3D::Clamp, m.uv_scale, m.uv_offset};
    }
};

// A material's diffuse colour and its light, with their pictures (Material3D: the base colour without what a metal
// reflects; the emission as the stock shaders give it).
void material_light(const Material3D& m, Color tint, Pictures& pictures, BakeSurface3D& surface) {
    const LinearRgb base = to_linear_rgb(m.color);
    const LinearRgb t = to_linear_rgb(tint);
    const f32 diffuse = 1.0f - std::clamp(m.metallic, 0.0f, 1.0f);
    surface.albedo = {base.r * t.r * diffuse, base.g * t.g * diffuse, base.b * t.b * diffuse};
    surface.albedo_texture = pictures.of(m.texture, m, MaterialMap3D::BaseColor);
    if (m.unlit) {
        // An unlit surface shows its colour as it is: light as much as it shows.
        surface.emission = {base.r * t.r, base.g * t.g, base.b * t.b};
        surface.emission_texture = surface.albedo_texture;
        surface.albedo = {};
        return;
    }
    // Independent: the emission colour times the map (white without); else a share of the emissive map, or of the base
    // colour (and its map) without one.
    if (m.emission_mode == EmissionMode3D::Independent) {
        surface.emission = m.emission_color * m.emissive;
        surface.emission_texture = pictures.of(m.emissive_texture, m, MaterialMap3D::Emissive);
    } else if (m.emissive_texture) {
        surface.emission = LinearRgb{1, 1, 1} * m.emissive;
        surface.emission_texture = pictures.of(m.emissive_texture, m, MaterialMap3D::Emissive);
    } else {
        surface.emission = base * m.emissive;
        surface.emission_texture = surface.albedo_texture;
    }
}

} // namespace

SceneBakedLighting3D::SceneBakedLighting3D(Renderer3D& renderer, std::filesystem::path content)
    : _renderer(renderer), _content(std::move(content)) {}

void SceneBakedLighting3D::reload() {
    _tried = false;
    _baked = {};
}

Status3D SceneBakedLighting3D::update(EcsWorld& world, Scene3D& scene) {
    const flecs::entity environment = active_environment(world);
    const SceneEnvironment3D* settings = environment ? environment.get<SceneEnvironment3D>() : nullptr;
    const std::string name = settings ? settings->baked_lighting : std::string{};
    if (name != _name) {
        _name = name;
        _tried = false;
        _baked = {};
    }
    if (!name.empty() && !_tried) {
        _tried = true;
        const std::filesystem::path folder = _content.empty() ? std::filesystem::path(name) : _content / name;
        auto data = load_baked_lighting(folder);
        if (!data) {
            scene.baked_lighting = {};
            return status3d(false, data.diagnostics);
        }
        auto made = _renderer.create_baked_lighting(*data);
        if (!made) {
            scene.baked_lighting = {};
            return status3d(false, made.diagnostics);
        }
        _baked = *made;
    }
    scene.baked_lighting = _baked;
    return status3d(true);
}

Result3D<LightBakeStats3D> bake_scene_lighting(EcsWorld& world, Renderer3D& renderer,
                                               const std::filesystem::path& content,
                                               const std::filesystem::path& folder, const LightBakeControl3D& control,
                                               const Scene3D* scene) {
    const auto fail = [](std::string why) {
        return Result3D<LightBakeStats3D>{std::nullopt, {"bake_scene_lighting: " + std::move(why)}};
    };
    if (folder.empty() || folder.is_absolute()) {
        return fail("the folder must be a relative path in the content");
    }
    const flecs::entity environment = active_environment(world);
    const SceneEnvironment3D defaults{};
    const SceneEnvironment3D& settings = environment ? *environment.get<SceneEnvironment3D>() : defaults;

    // The lights: the environment's sun, fill and ambient light, and every enabled light entity.
    Lights3D lights = settings.lights;
    lights.points.clear();
    lights.spots.clear();
    world.each([&](flecs::entity, const WorldTransform3D& placed, const PointLight3D& light) {
        if (light.enabled)
            lights.points.push_back({.position = placed.position(),
                                     .color = light.color,
                                     .radius = light.radius,
                                     .size = light.size,
                                     .bake = light.bake});
    });
    world.each([&](flecs::entity, const WorldTransform3D& placed, const SpotLight3D& light) {
        if (!light.enabled)
            return;
        const Vec3f axis = placed.matrix.apply_vector({0.0f, 0.0f, -1.0f});
        lights.spots.push_back({.position = placed.position(),
                                .direction = length_squared(axis) > 0.0f ? normalize(axis) : Vec3f{0.0f, -1.0f, 0.0f},
                                .color = light.color,
                                .radius = light.radius,
                                .size = light.size,
                                .inner_angle = light.inner_angle,
                                .outer_angle = light.outer_angle,
                                .bake = light.bake});
    });

    // What stands still: each entity's parts in order, read back as drawn.
    struct Entity {
        flecs::entity entity;
        u32 first_surface = 0; // its parts' surfaces, in part order (those not baked: none)
        std::vector<i32> part_surface;
    };
    std::vector<Entity> entities;
    std::vector<std::unique_ptr<MeshData3D>> meshes;
    std::vector<BakeSurface3D> surfaces;
    Pictures pictures{renderer, {}};
    const auto add = [&](Entity& owner, const Mesh3D& mesh, const Mat4f& transform, const Material3D& material,
                         Color tint, bool lightmapped, f32 texel_scale) -> bool {
        auto data = renderer.read_mesh(mesh);
        if (!data) {
            KIN_LOG_WARN_F(
                "render", "3D bake: a mesh could not be read back: left out",
                (LogFields{{.name = "reason", .value = data.diagnostics.empty() ? "" : data.diagnostics[0]}}));
            owner.part_surface.push_back(-1);
            return false;
        }
        meshes.push_back(std::make_unique<MeshData3D>(std::move(*data)));
        BakeSurface3D surface;
        surface.mesh = meshes.back().get();
        surface.transform = transform;
        material_light(material, tint, pictures, surface);
        surface.lightmapped = lightmapped && surface.mesh->lightmapped();
        surface.texel_scale = texel_scale;
        surface.double_sided = material.double_sided;
        owner.part_surface.push_back(static_cast<i32>(surfaces.size()));
        surfaces.push_back(surface);
        return true;
    };
    world.each([&](flecs::entity entity, const WorldTransform3D& placed, const StaticLighting3D& baked) {
        if (!(baked.texel_scale > 0.0f))
            return;
        Entity owner{entity, static_cast<u32>(surfaces.size()), {}};
        if (const auto* r = entity.get<MeshRenderer3D>(); r && r->visible && r->mesh && !r->mesh.skinned()) {
            add(owner, r->mesh, placed.matrix, r->material, r->tint, baked.lightmapped, baked.texel_scale);
        } else if (const auto* m = entity.get<ModelRenderer3D>(); m && m->visible && m->model) {
            for (const ModelPart3D& part : m->model->parts) {
                if (!part.mesh || part.mesh.skinned() || part.skin >= 0) {
                    owner.part_surface.push_back(-1);
                    continue;
                }
                add(owner, part.mesh, placed.matrix * part.transform, part.material, m->tint, baked.lightmapped,
                    baked.texel_scale);
            }
        } else {
            return;
        }
        entities.push_back(std::move(owner));
    });
    if (surfaces.empty()) {
        return fail("no StaticLighting3D entity with a mesh or model to bake");
    }

    LightBakeControl3D run = control;
    if (!run.jobs)
        run.jobs = &default_job_system();
    // The sky as drawn: the procedural sky's panorama, else the environment's picture, else the hemisphere.
    const Lights3D& drawn = scene ? scene->lights : settings.lights;
    const bool sky_lights = (scene ? scene->sky.enabled : settings.sky.enabled) && drawn.sky_lights_scene;
    BakeSky3D sky = bake_sky(drawn);
    Renderer3D::EnvironmentPicture3D picture;
    if (sky_lights || drawn.environment) {
        auto read = sky_lights ? renderer.read_sky_environment() : renderer.read_environment(drawn.environment);
        if (read) {
            picture = std::move(*read);
            sky.panorama = picture.rgb;
            sky.panorama_size = picture.size;
            sky.intensity = drawn.environment_intensity;
            sky.rotation = sky_lights ? 0.0f : drawn.environment_rotation;
        } else {
            KIN_LOG_WARN("render", "3D bake: the sky's picture could not be read: baked with the ambient light "
                                   "(render the scene once before baking)");
        }
    }
    auto baked = bake_lighting(surfaces, lights, sky, settings.bake, run);
    if (!baked) {
        return Result3D<LightBakeStats3D>{std::nullopt, baked.diagnostics};
    }
    // The placements laid out again by entity: each entity's parts one after another (a part not baked: none), so
    // that StaticLighting3D::lightmap plus the part's index finds its own.
    BakedLightingData3D out;
    out.pages = std::move(baked->pages);
    out.probes = std::move(baked->probes);
    out.stats = baked->stats;
    std::vector<u32> first(entities.size(), 0);
    for (std::size_t e = 0; e < entities.size(); ++e) {
        first[e] = static_cast<u32>(out.placements.size());
        for (const i32 s : entities[e].part_surface)
            out.placements.push_back(s >= 0 ? baked->placements[static_cast<std::size_t>(s)] : LightmapPlacement3D{});
    }
    if (Status3D saved = save_baked_lighting(out, content / folder); !saved) {
        return Result3D<LightBakeStats3D>{std::nullopt, saved.diagnostics};
    }
    for (std::size_t e = 0; e < entities.size(); ++e) {
        bool any = false;
        for (const i32 s : entities[e].part_surface)
            any = any || (s >= 0 && surfaces[static_cast<std::size_t>(s)].lightmapped);
        StaticLighting3D& component = entities[e].entity.ensure<StaticLighting3D>();
        component.lightmap = any ? first[e] + 1 : 0;
        entities[e].entity.modified<StaticLighting3D>();
    }
    if (environment) {
        environment.ensure<SceneEnvironment3D>().baked_lighting = folder.generic_string();
        environment.modified<SceneEnvironment3D>();
    }
    return result3d(out.stats);
}

} // namespace kin
