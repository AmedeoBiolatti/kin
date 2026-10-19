#include <kin/render3d/stats3d.hpp>
#include <kin/render3d/debug3d.hpp>
#include <kin/render3d/debug_stats3d.hpp>
#include <kin/render3d/scene3d.hpp>

#include <cassert>

template<class T> concept HasCacheCounters = requires(T value) { value.retained_gpu_bytes; };
template<class T> concept HasForcedLevel = requires(T value) { value.force_level; };
template<class T> concept HasReadback = requires(T value) { value.stats_readback; };
template<class T> concept HasFrozenJitter = requires(T value) { value.freeze_jitter; };

// Public counters and quality must remain usable without experimental 3D and
// must not grow renderer implementation details or blocking debug controls.
static_assert(sizeof(kin::Render3DStats) <= 128);
static_assert(!HasCacheCounters<kin::Render3DStats>);
static_assert(HasCacheCounters<kin::Render3DDebugStats>);
static_assert(!HasForcedLevel<kin::Lod3D> && !HasReadback<kin::Lod3D>);
static_assert(!HasFrozenJitter<kin::TemporalAA3D>);
static_assert(HasForcedLevel<kin::Render3DDebugOptions> && HasReadback<kin::Render3DDebugOptions>);

int main() {
    const kin::Render3DStats summary;
    const kin::Render3DDebugOptions debug;
    assert(summary.draws == 0 && summary.triangles_complete);
    assert(debug.force_level == -1 && debug.gpu_culling && !debug.stats_readback && !debug.freeze_jitter && !debug.gpu_pass_markers);
}
