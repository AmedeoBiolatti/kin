#pragma once
#include <filesystem>
#include <kin/assets/asset_handle.hpp>
#include <kin/physics3d/shape3d.hpp>
#include <kin/render3d/errors3d.hpp>
#include <kin/render3d/model_geometry3d.hpp>
#include <optional>
#include <string>
#include <vector>
namespace kin {
class BakeCache;
struct ColliderRecipe3D {
    Shape3DKind kind = Shape3DKind::Box;
    Vec3f half_extents{.5f, .5f, .5f}, offset{}, scale{1, 1, 1};
    Quatf rotation{};
    f32 radius = .5f, half_height = .5f;
    std::filesystem::path mesh;
    std::vector<Vec3f> points;
    std::vector<u32> indices;
    std::vector<ColliderRecipe3D> children;
};
struct ColliderAssetLimits3D {
    u64 source_bytes = 1024u * 1024u;
    ModelGeometryLimits3D geometry;
    std::size_t nodes = 64, depth = 4, hull_points = 256, cache_entries = 128;
};

Result3D<ColliderRecipe3D> parse_collider3d_asset(std::string_view text, ColliderAssetLimits3D limits = {});
struct CookedCollider3D {
    Shape3D shape;
    Vec3f scale{1, 1, 1};
    bool contains_surface = false; // triangle meshes cannot be dynamic
    std::vector<std::filesystem::path> source_files;
};
// CPU-only, synchronous. Holds bounded weak lookup metadata, never geometry or
// owning shape caches. Keep handles in the scene. Root-relative paths and packs
// follow the same content utilities as rendering assets. Explicit invalidation.
class PhysicsAssets3D {
public:
    // cooks: shapes cooked; baked: shapes restored from the bake cache instead; hits: loads served from live entries.
    struct Stats {
        std::size_t entries = 0;
        u64 cooks = 0, hits = 0, baked = 0;
    };
    explicit PhysicsAssets3D(std::filesystem::path root = ".");
    PhysicsAssets3D(std::filesystem::path root, ColliderAssetLimits3D limits);
    ~PhysicsAssets3D();
    PhysicsAssets3D(const PhysicsAssets3D&) = delete;
    PhysicsAssets3D& operator=(const PhysicsAssets3D&) = delete;
    Result3D<AssetHandle<CookedCollider3D>> load_collider(const std::filesystem::path& path, Vec3f scale = {1, 1, 1});
    // Opt-in bake cache (kin/assets/bake_cache.hpp): a recipe with a convex hull or a triangle mesh keeps its cooked
    // shape there (Shape3D::save_binary) and a later run restores it instead of cooking. The recipe and its geometry
    // are still read and checked on every load; the key is what cooking reads (each node's kind, sizes, placement
    // and scale, the hull points and mesh triangles as read from their files, the load's scale, the limits) with
    // Jolt's version, so an edited recipe or model is another key, never a stale shape. Null (the default): off.
    // Not owned: it must outlive the loads made while it is set.
    void set_bake_cache(BakeCache* cache);
    BakeCache* bake_cache() const;
    void invalidate(const std::filesystem::path& source);
    void trim();
    void clear();
    Stats stats() const;
    const std::filesystem::path& root() const;
    // Last load, cleared on success including cache hits. Observation does not clear it.
    const Diagnostics3D& diagnostics() const;

private:
    AssetHandle<CookedCollider3D> load_collider_operation(const std::filesystem::path& path, Vec3f scale = {1, 1, 1});
    struct Data;
    std::unique_ptr<Data> _data;
};
} // namespace kin
