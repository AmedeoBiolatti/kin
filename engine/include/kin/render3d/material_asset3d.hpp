#pragma once

#include <kin/render3d/scene3d.hpp>
#include <filesystem>
#include <map>
#include <string>

namespace kin {
class Assets3D;

struct MaterialTexture3D {
    std::filesystem::path asset; // empty explicitly clears the imported map
    bool nearest = false;       // otherwise mipmapped
};

// Omitted fields preserve the imported material. Color uses the standard
// component codec (sRGB bytes); texture roles choose sRGB or linear on binding.
struct MaterialPatch3D {
    std::optional<Color> color;
    std::optional<f32> metallic, roughness, emissive, normal_scale, occlusion_strength, alpha_cutoff;
    std::optional<bool> unlit, double_sided, casts_shadow, receives_shadow, wireframe;
    std::optional<Blend3D> blend;
    std::optional<Wrap3D> wrap_u, wrap_v;
    std::optional<MaterialTexture3D> texture, metallic_roughness, normal, occlusion, emissive_texture;
};
struct MaterialPartPatch3D {
    // Exactly one selector: an index or a unique imported part name.
    std::optional<u32> index;
    std::string name;
    MaterialPatch3D material;
};
struct MaterialVariant3D {
    MaterialPatch3D material; // applies to every part, before this layer's parts
    std::vector<MaterialPartPatch3D> parts;
};
struct Materials3DAsset {
    std::filesystem::path source;
    MaterialVariant3D base;
    std::map<std::string, MaterialVariant3D> variants;
};
struct Materials3DLoadResult {
    std::optional<Materials3DAsset> asset;
    std::vector<std::string> diagnostics;
    bool ok() const { return asset.has_value() && diagnostics.empty(); }
};
// CPU-only; 1 MiB per source, 128 variants, 256 part rules per layer.
Materials3DLoadResult parse_materials3d_asset(std::string_view text, const std::filesystem::path& source = {});
Materials3DLoadResult load_materials3d_asset(const std::filesystem::path& source);

// Only materials/textures are owned; meshes and models are never copied or
// pinned. Binding identity prevents stale overrides after a model replacement.
struct MaterialBinding3D {
    std::weak_ptr<const Model> model;
    std::vector<Material3D> materials; // one entry per original model part
    std::vector<std::filesystem::path> source_files;
};
// Renderer-thread-only low-level builder; throws on invalid selectors, variant
// or textures. Assets3D::load_materials wraps it with diagnostics/weak caching.
MaterialBinding3D build_materials3d_binding(const Materials3DAsset& asset, const std::shared_ptr<const Model>& model,
                                         std::string_view variant, Assets3D& assets);
} // namespace kin
