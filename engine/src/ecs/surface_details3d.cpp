// The ECS side of decals and morph targets (docs/rendering_3d.md, Decals and Morph targets): DecalRenderer3D and
// MorphWeights3D in the component registry (scene files, prefabs, inspection, Lua), and the binding of decal images.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <kin/ecs/scene3d.hpp>
#include <kin/render3d/assets3d.hpp>
#include <stdexcept>

namespace kin {

std::string format_morph_weights3d(std::span<const MorphWeight3D> weights) {
    std::string out;
    for (const MorphWeight3D& w : weights) {
        if (!out.empty())
            out += ", ";
        char number[32];
        const auto end = std::to_chars(number, number + sizeof(number), w.weight).ptr;
        out += w.target + "=" + std::string(number, end);
    }
    return out;
}

std::optional<std::vector<MorphWeight3D>> parse_morph_weights3d(std::string_view text) {
    std::vector<MorphWeight3D> out;
    const auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
            s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
            s.remove_suffix(1);
        return s;
    };
    while (!trim(text).empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view item = trim(text.substr(0, comma));
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        const std::size_t equals = item.rfind('=');
        if (equals == std::string_view::npos)
            return std::nullopt;
        const std::string_view name = trim(item.substr(0, equals)), value = trim(item.substr(equals + 1));
        f32 weight = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), weight);
        if (name.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            !std::isfinite(weight) || weight < -1 || weight > 1)
            return std::nullopt;
        out.push_back({std::string(name), weight});
    }
    return out;
}

void register_surface_details3d_components(EcsWorld& world) {
    auto& registry = world.components();
    if (!registry.find("MorphWeights3D"))
        registry.native<MorphWeights3D>("MorphWeights3D")
            .property<std::string>(
                "weights", [](const MorphWeights3D& m) { return format_morph_weights3d(m.weights); },
                [](MorphWeights3D& m, const std::string& text) {
                    auto parsed = parse_morph_weights3d(text);
                    if (!parsed)
                        return false;
                    m.weights = std::move(*parsed);
                    return true;
                })
            .description("Morph target weights by name, \"smile=0.8, blink=1\", each within [-1, 1].")
            .field("animated", &MorphWeights3D::animated)
            .description("The animator's clips set the targets not named here.");
    if (!registry.find("DecalRenderer3D")) {
        auto b = registry.native<DecalRenderer3D>("DecalRenderer3D");
        b.field("texture", &DecalRenderer3D::texture_asset)
            .asset_type("Image")
            .description("The picture (colour, and coverage in its alpha).")
            .field("normal", &DecalRenderer3D::normal_asset)
            .asset_type("Image")
            .description("An optional tangent-space normal map, green up.")
            .field("size", &DecalRenderer3D::size)
            .units("m")
            .description("The box: the picture's width (X), how deep it reaches (Y), its height (Z).")
            .field("visible", &DecalRenderer3D::visible);
        b.property<Color>(
            "color", [](const DecalRenderer3D& d) { return d.decal.color; },
            [](DecalRenderer3D& d, const Color& c) {
                d.decal.color = c;
                return true;
            });
        const auto scalar = [&](const char* name, f32 Decal3D::*member, f32 low, f32 high) {
            b.property<f32>(
                name, [member](const DecalRenderer3D& d) { return d.decal.*member; },
                [member, low, high](DecalRenderer3D& d, const f32& v) {
                    if (!std::isfinite(v) || v < low || v > high)
                        return false;
                    d.decal.*member = v;
                    return true;
                });
        };
        scalar("roughness", &Decal3D::roughness, 0, 1);
        scalar("metallic", &Decal3D::metallic, 0, 1);
        scalar("normal_strength", &Decal3D::normal_strength, 0, 8);
        scalar("emission", &Decal3D::emission, 0, 1e4f);
        scalar("normal_fade", &Decal3D::normal_fade, 0, 1);
        scalar("depth_fade", &Decal3D::depth_fade, 0, 1);
    }
}

static std::vector<std::string> resolve_decals3d_assets_impl(EcsWorld& world, Assets3D& assets,
                                                             std::span<const EcsId> selected) {
    std::vector<std::string> errors;
    std::vector<EcsEntity> entities;
    if (selected.empty())
        world.each([&](flecs::entity entity, const DecalRenderer3D&) { entities.emplace_back(entity); });
    else
        for (auto id : selected)
            if (auto e = world.entity_by_id(id); e && e.has<DecalRenderer3D>())
                entities.push_back(e);
    std::ranges::sort(entities, {}, &EcsEntity::id);
    for (auto entity : entities) {
        const DecalRenderer3D reference = *entity.get<DecalRenderer3D>(); // copy-on-write may move the table
        Texture textures[2];
        bool failed = false;
        const std::string* paths[2] = {&reference.texture_asset.id, &reference.normal_asset.id};
        for (int i = 0; i < 2; ++i) {
            if (paths[i]->empty())
                continue;
            std::string error;
            try {
                textures[i] = assets.load_texture(*paths[i], false, /*srgb=*/i == 0).value_or({});
                error = diagnostics3d_text(assets.diagnostics());
            } catch (const std::exception& failure) {
                error = failure.what();
            }
            if (!textures[i]) {
                errors.push_back("entity '" + entity.name() + "', DecalRenderer3D " + (i == 0 ? "texture" : "normal") +
                                 " '" + *paths[i] + "': " + error);
                failed = true;
            }
        }
        if (failed)
            continue; // the last good pictures stay
        auto* d = entity.get_mut<DecalRenderer3D>();
        bool changed = false;
        const auto bind = [&](const std::string& path, std::string& bound, Texture& slot, Texture& texture) {
            // An empty reference clears only a picture it bound; a C++ one stays.
            if (path.empty() && bound.empty())
                return;
            if (bound != path || slot != texture) {
                slot = std::move(texture);
                bound = path;
                changed = true;
            }
        };
        bind(reference.texture_asset.id, d->bound_texture, d->decal.texture, textures[0]);
        bind(reference.normal_asset.id, d->bound_normal, d->decal.normal, textures[1]);
        if (changed)
            entity.modified<DecalRenderer3D>();
    }
    return errors;
}

Status3D resolve_decals3d_assets(EcsWorld& world, Assets3D& assets, std::span<const EcsId> selected) {
    auto diagnostics = resolve_decals3d_assets_impl(world, assets, selected);
    const bool accepted = diagnostics.empty();
    return status3d(accepted, std::move(diagnostics));
}

} // namespace kin
