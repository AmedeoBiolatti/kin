#include <cassert>
#include <cmath>
#include <kin/render3d/debug3d.hpp>
#include <kin/render3d/debug_stats3d.hpp>
#include <kin/render3d/particles3d.hpp>
#include <kin/render3d/scene3d.hpp>
#include <kin/render3d/sky3d.hpp>
#include <kin/render3d/stats3d.hpp>
#include <kin/render3d/timeofday3d.hpp>
#ifdef KIN_EXPERIMENTAL_3D
#include <kin/ecs/render3d.hpp>
#include <kin/physics3d/assets3d.hpp>
#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#endif
#include <type_traits>

template <class T>
concept HasCacheCounters = requires(T value) { value.retained_gpu_bytes; };
template <class T>
concept HasForcedLevel = requires(T value) { value.force_level; };
template <class T>
concept HasReadback = requires(T value) { value.stats_readback; };
template <class T>
concept HasFrozenJitter = requires(T value) { value.freeze_jitter; };

// Public counters and quality must remain usable without experimental 3D and
// must not grow renderer implementation details or blocking debug controls.
static_assert(sizeof(kin::RenderStats3D) <= 128);
static_assert(!HasCacheCounters<kin::RenderStats3D>);
static_assert(HasCacheCounters<kin::RenderDebugStats3D>);
static_assert(!HasForcedLevel<kin::Lod3D> && !HasReadback<kin::Lod3D>);
static_assert(!HasFrozenJitter<kin::TemporalAA3D>);
static_assert(HasForcedLevel<kin::RenderDebugOptions3D> && HasReadback<kin::RenderDebugOptions3D>);

// Colours of lights cannot silently become geometric directions or sRGB bytes.
static_assert(sizeof(kin::LinearRgb) == 3 * sizeof(kin::f32));
static_assert(std::is_standard_layout_v<kin::LinearRgb>);
static_assert(!std::is_convertible_v<kin::Vec3f, kin::LinearRgb>);
static_assert(!std::is_convertible_v<kin::Color, kin::LinearRgb>);
static_assert(!std::is_convertible_v<kin::LinearColor, kin::LinearRgb>);
static_assert(!std::is_assignable_v<decltype(kin::DirectionalLight3D::color)&, kin::Vec3f>);
static_assert(std::is_same_v<decltype(kin::PointLightSource3D::color), kin::LinearRgb>);
static_assert(std::is_same_v<decltype(kin::SpotLightSource3D::color), kin::LinearRgb>);
static_assert(std::is_same_v<decltype(kin::Lights3D::ambient), kin::LinearRgb>);

static_assert(std::is_same_v<decltype(kin::Sky3D::ground_color), kin::LinearRgb>);
static_assert(std::is_same_v<decltype(kin::Sky3D::moon_color), kin::LinearRgb>);
static_assert(std::is_same_v<decltype(kin::Fog3D::colour), kin::LinearRgb>);
static_assert(std::is_same_v<decltype(kin::SkyState3D::fog_color), kin::LinearRgb>);
static_assert(std::is_same_v<decltype(kin::ParticleDesc3D::colour_start), kin::LinearRgb>);
static_assert(std::is_same_v<decltype(kin::ParticleSprite3D::colour), kin::LinearRgb>);
static_assert(std::is_same_v<decltype(kin::WeatherLayer3D::colour), kin::LinearRgb>);
static_assert(std::is_same_v<decltype(kin::sky_radiance({}, {}, {})), kin::LinearRgb>);
static_assert(!std::is_assignable_v<decltype(kin::Sky3D::ground_color)&, kin::Vec3f>);
#ifdef KIN_EXPERIMENTAL_3D
template <class T>
concept LocalLightFields = requires(T light) {
    light.color;
    light.radius;
    light.size;
    light.shadow_enabled;
    light.shadow_resolution;
    light.shadow_bias;
    light.shadow_normal_offset;
    light.shadow_near_plane;
    light.shadow_distance;
    light.shadow_softness;
};
static_assert(LocalLightFields<kin::PointLight3D> && LocalLightFields<kin::PointLightSource3D>);
static_assert(LocalLightFields<kin::SpotLight3D> && LocalLightFields<kin::SpotLightSource3D>);
static_assert(std::is_same_v<decltype(std::declval<kin::Renderer3D&>().create_mesh(kin::MeshData3D{})),
                             kin::Result3D<kin::Mesh3D>>);
static_assert(std::is_same_v<decltype(std::declval<kin::Assets3D&>().load_model("a")),
                             kin::Result3D<kin::AssetHandle<kin::Model3D>>>);
static_assert(std::is_same_v<decltype(std::declval<kin::PhysicsAssets3D&>().load_collider("a")),
                             kin::Result3D<kin::AssetHandle<kin::CookedCollider3D>>>);
static_assert(std::is_same_v<decltype(kin::MeshData3D{}.validate()), kin::Status3D>);
static_assert(!std::is_convertible_v<kin::Status3D, bool>);
static_assert(std::is_same_v<decltype(kin::parse_materials3d_asset("")), kin::Result3D<kin::MaterialsAsset3D>>);
static_assert(std::is_same_v<decltype(kin::parse_collider3d_asset("")), kin::Result3D<kin::ColliderRecipe3D>>);

template <class T>
concept LegacyModelError = requires(T& owner, std::string* error) { owner.load_model("a", error).value_or({}); };
template <class T>
concept LegacyColliderError =
    requires(T& owner, std::string* error) { owner.load_collider("a", kin::Vec3f{1, 1, 1}, error).value_or({}); };
static_assert(!LegacyModelError<kin::Assets3D> && !LegacyModelError<kin::Renderer3D>);
static_assert(!LegacyColliderError<kin::PhysicsAssets3D>);
template <class T>
concept LegacyErrorAccessor = requires(T& owner) { owner.error(); };
static_assert(!LegacyErrorAccessor<kin::ModelFile3D>);

#endif

static_assert(std::is_same_v<decltype(kin::mix({0, 0, 0}, {255, 255, 255}, 0.5f)), kin::Color>);

int main() {
    assert(kin::mix({0, 0, 0}, {255, 255, 255}, 0.5f).r == 128);
    const auto encoded = kin::Color::rgba(128, 64, 32, 17);
    const auto light = kin::to_linear_rgb(encoded);
    assert(std::abs(light.r - 0.2158605f) < 1e-6f);
    assert(kin::to_srgb(light.with_alpha(17.0f / 255.0f)) == encoded);
    const kin::LinearRgb hdr{4.0f, 2.0f, 1.0f};
    assert((hdr * 2.0f + hdr == kin::LinearRgb{12.0f, 6.0f, 3.0f}));
    assert((kin::mix(kin::LinearRgb{}, hdr, 0.5f) == kin::LinearRgb{2.0f, 1.0f, 0.5f}));
    const kin::RenderStats3D summary;
    const kin::RenderDebugOptions3D debug;
    assert(summary.draws == 0 && summary.triangles_complete);
    assert(debug.force_level == -1 && debug.gpu_culling && !debug.stats_readback && !debug.freeze_jitter &&
           !debug.gpu_pass_markers);
}
