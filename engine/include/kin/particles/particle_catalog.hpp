#pragma once

#include <kin/assets/asset_handle.hpp>
#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/render_layer.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kin {

class AssetManager;

enum class ParticlePixelPolicy {
    Free,
    LogicalPixel,
    Subpixel,
    Output,
};

struct ParticleRange {
    f32 min = 0.0f;
    f32 max = 0.0f;
};

struct ParticleRenderStyle {
    std::string sprite_id;
    i32 layer = layer_value(RenderLayer::Effects);
    i32 order = 0;
    bool y_sort = false;
    f32 sort_y_offset = 0.0f;
};

struct ParticleBurst {
    Vec2f position{};
    i32 count = 8;
    ParticleRange speed{20.0f, 80.0f};
    ParticleRange lifetime{0.25f, 0.6f};
    f32 start_size = 2.0f;
    f32 end_size = 0.0f;
    Color start_color{255, 255, 255, 255};
    Color end_color{255, 255, 255, 0};
    Vec2f acceleration{};
    ParticleRenderStyle render{};
};

struct ParticleEffect {
    ParticleBurst burst{};
};

struct ParticleField {
    i32 count = 0;
    u64 seed = 1;
    Rectf area{};
    ParticleRange velocity_x{};
    ParticleRange velocity_y{};
    ParticleRange size{1.0f, 1.0f};
    ParticleRange brightness{255.0f, 255.0f};
    Color color{255, 255, 255, 255};
    bool use_brightness = true;
    bool wrap = true;
    ParticleRenderStyle render{};
};

class ParticleCatalog {
public:
    void clear();
    void set_effect(std::string name, ParticleEffect effect);
    void set_field(std::string name, ParticleField field);

    const ParticleEffect* effect(std::string_view name) const;
    const ParticleField* field(std::string_view name) const;
    bool has_effect(std::string_view name) const;
    bool has_field(std::string_view name) const;

    const std::unordered_map<std::string, ParticleEffect>& effects() const { return _effects; }
    const std::unordered_map<std::string, ParticleField>& fields() const { return _fields; }

private:
    std::unordered_map<std::string, ParticleEffect> _effects;
    std::unordered_map<std::string, ParticleField> _fields;
};

ParticleCatalog default_particle_catalog();
ParticleCatalog load_particle_catalog(const std::filesystem::path& path);
bool save_particle_catalog(const ParticleCatalog& catalog, const std::filesystem::path& path);
void register_particle_catalog_loader(AssetManager& assets);

std::string_view particle_pixel_policy_name(ParticlePixelPolicy policy);
bool parse_particle_pixel_policy(std::string_view value, ParticlePixelPolicy& out);

} // namespace kin
