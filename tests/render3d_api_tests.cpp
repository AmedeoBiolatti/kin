#include <cassert>
#include <cmath>
#include <kin/render3d/debug3d.hpp>
#include <kin/render3d/debug_stats3d.hpp>
#include <kin/render3d/scene3d.hpp>
#include <kin/render3d/stats3d.hpp>
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
