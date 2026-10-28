#pragma once
#include <functional>
#include <kin/core/types.hpp>
#include <kin/renderer/texture.hpp>
#include <span>
#include <string>
#include <vector>

namespace kin {
using GraphResource = u32;
inline constexpr u32 graph_no_slot = ~u32{0};
// Tokens describe backend-owned operations with no graph-managed allocation.
enum class GraphResourceKind : u8 { Token, ColorTarget, StorageTexture, Buffer, RenderAttachment };
struct GraphResourceDesc {
    std::string name;
    GraphResourceKind kind = GraphResourceKind::Token;
    TextureFormat format = TextureFormat::Rgba8;
    Vec2i size{};
    u64 bytes = 0; // buffer capacity; texture payload is derived from size/format
    bool imported = false, exported = false, persistent = false;
    bool hdr = false; // ColorTarget: renderer working space is HDR (RGBA16F payload)
    friend bool operator==(const GraphResourceDesc&, const GraphResourceDesc&) = default;
};
namespace graph_feature {
inline constexpr u64 raster = 1, compute = 2, buffers = 4, rendering3d = 8, shaders = 16;
inline constexpr u64 attachments = 32; // explicit linear float/depth render attachments
} // namespace graph_feature
struct GraphPassDesc {
    std::string name;
    std::vector<GraphResource> reads, writes;
    std::vector<std::string> after;
    u64 required_features = 0;
    bool side_effect = false, enabled = true;
    friend bool operator==(const GraphPassDesc&, const GraphPassDesc&) = default;
};
struct ResourceGraphDesc {
    std::vector<GraphResourceDesc> resources;
    std::vector<GraphPassDesc> passes;
    friend bool operator==(const ResourceGraphDesc&, const ResourceGraphDesc&) = default;
};
struct GraphCompileOptions {
    bool cull_unused = true, reuse_slots = true;
    u64 max_slot_payload_bytes = ~u64{0}; // graph-owned payload only, excluding imports/driver memory
    friend bool operator==(const GraphCompileOptions&, const GraphCompileOptions&) = default;
};
struct GraphResourceLifetime {
    u32 first = graph_no_slot, last = 0, slot = graph_no_slot;
};
struct CompiledResourceGraph {
    ResourceGraphDesc source;
    u64 identity = 0; // stable for one compiled plan; assigned by the compiler
    std::vector<std::string> diagnostics;
    std::vector<u32> order; // indices into the source definition, stable topological order
    std::vector<GraphResourceLifetime> lifetimes;
    std::vector<GraphResourceDesc> slots;
    u64 slot_payload_bytes = 0, peak_live_payload_bytes = 0;
    u32 culled_passes = 0;
    explicit operator bool() const { return diagnostics.empty(); }
};
struct GraphCompilerStats {
    u64 compilations = 0, cache_hits = 0;
    f64 last_compile_ms = 0;
};
// Exact descriptor comparison prevents hash collisions and excludes callbacks,
// resource contents, cameras and time from structural invalidation. One cached
// variant per compiler; use one per view or developer-owned pipeline variant.
class ResourceGraphCompiler {
public:
    const CompiledResourceGraph& compile(const ResourceGraphDesc&, u64 available_features, GraphCompileOptions = {});
    const GraphCompilerStats& stats() const { return _stats; }

private:
    GraphCompileOptions _options;
    u64 _features = 0;
    bool _compiled = false;
    CompiledResourceGraph _plan;
    GraphCompilerStats _stats;
};
// Callbacks record work through existing public renderer APIs. Resource bindings
// are supplied by the caller; compilation does not imply native heap aliasing.
// No callback runs for an invalid plan or an incomplete operation table.
bool execute_resource_graph(const CompiledResourceGraph&, std::span<const std::function<bool()>> operations);
u64 graph_resource_payload(const GraphResourceDesc&);
} // namespace kin
